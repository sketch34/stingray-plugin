
#include <editor_plugin_api/editor_plugin_api.h>
#include <string>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_GIF

#include <stb_image.h>
#include <stb_image_write.h>

namespace PLUGIN_NAMESPACE {

ConfigDataApi* config_data_api = nullptr;
EditorLoggingApi* logging_api = nullptr;
EditorEvalApi* eval_api = nullptr;
EditorAllocatorApi* allocator_api = nullptr;
EditorAllocator plugin_allocator = nullptr;

/**
 * Return plugin extension name.
 */
const char* get_name() { return "Editor Plugin Extension"; }

/**
 * Return plugin version.
 */
const char* get_version() { return "1.0.0"; }

/**
 * Log received arguments arguments and return a copy of the first one.
 */
ConfigValue test_log_arguments(ConfigValueArgs args, int num)
{
	if (num < 1)
		return nullptr;

	// Log all string arguments to the editor console.
	for (auto i = 0; i < num; ++i) {
		auto argument = &args[i];
		if (config_data_api->type(argument) == CD_TYPE_STRING)
			logging_api->info(config_data_api->to_string(argument));
	}

	// Evaluate some JavaScript from the editor extension.
	eval_api->eval("console.warn(`The answer to life the universe and everything equals ${21 + 21}`);", nullptr, nullptr);

	// Clone first argument and return it.
	auto first_arg = &args[0];
	auto copy_cv = config_data_api->make(nullptr);
	auto type = config_data_api->type(first_arg);
	switch (type) {
		case CD_TYPE_NULL: return config_data_api->nil();
		case CD_TYPE_FALSE: config_data_api->set_bool(copy_cv, false); break;
		case CD_TYPE_TRUE: config_data_api->set_bool(copy_cv, true); break;
		case CD_TYPE_NUMBER: config_data_api->set_number(copy_cv, config_data_api->to_number(first_arg)); break;
		case CD_TYPE_STRING: config_data_api->set_string(copy_cv, config_data_api->to_string(first_arg)); break;
		case CD_TYPE_ARRAY:
		{
			auto length = config_data_api->array_size(first_arg);
			for (auto i = 0; i < length; ++i) {
				auto origin_item = config_data_api->array_item(first_arg, i);
				config_data_api->push(copy_cv, origin_item);
			}
			break;
		}
		case CD_TYPE_OBJECT:
		{
			auto size = config_data_api->object_size(first_arg);
			config_data_api->set_object(copy_cv);
			for (auto i = 0; i < size; ++i) {
				auto object_item_key = config_data_api->object_key(first_arg, i);
				auto object_item_value = config_data_api->object_value(first_arg, i);
				config_data_api->set(copy_cv, object_item_key, object_item_value);
			}
			break;
		}
		default: break;
	}

	return copy_cv;
}

/*
 * Create a new config value using a custom allocator.
 */
ConfigValue test_custom_allocator(ConfigValueArgs args, int num)
{
	auto cvca = config_data_api->make(plugin_allocator);
	config_data_api->set_string(cvca, "This was allocator with a custom plugin allocator.");
	return cvca;
}

/*
 * Allocator function used by our test plugin custom allocator.
 * We use as an example a static buffer to write up to 1024 bytes of data.
 */
void* test_custom_allocate(size_t size, size_t align, void *param)
{
	static unsigned char small_buffer[1024] = { 0 };
	static size_t small_buffer_cursor = 0;

	if (small_buffer_cursor+size > sizeof small_buffer) {
		logging_api->error("We do not have enough space!");
		return nullptr;
	}
	
	logging_api->info("We have enough space!");
	auto pi = small_buffer_cursor;
	small_buffer_cursor += size;
	return small_buffer + pi;
}

/*
 * Deallocation function used by our test plugin allocator.
 * We do not release any memory since the allocator writes to a static buffer.
 */
size_t test_custom_deallocate(void* ptr, void *param)
{
	logging_api->debug("We do not deallocate in the fixed small_buffer.");
	return 0;
}


/**
* Load GIF animations frames from the specified file path..
*/
STBIDEF unsigned char *gif_load_frames(char const *filename, int *x, int *y, int *frames)
{
	typedef struct gif_result_t {
		int delay;
		unsigned char *data;
		struct gif_result_t *next;
	} gif_result;

	FILE *f;
	stbi__context s;
	unsigned char *result;

	if (!((f = stbi__fopen(filename, "rb"))))
		return stbi__errpuc("can't fopen", "Unable to open file");

	stbi__start_file(&s, f);

	if (stbi__gif_test(&s)) {
		int c;
		stbi__gif g;
		gif_result head;
		gif_result *prev = nullptr, *gr = &head;

		memset(&g, 0, sizeof(g));
		memset(&head, 0, sizeof(head));

		*frames = 0;

		while ((gr->data = stbi__gif_load_next(&s, &g, &c, 4))) {
			if (gr->data == (unsigned char*)&s) {
				gr->data = nullptr;
				break;
			}

			if (prev) prev->next = gr;
			gr->delay = g.delay;
			prev = gr;
			gr = (gif_result*)stbi__malloc(sizeof(gif_result));
			memset(gr, 0, sizeof(gif_result));
			++(*frames);
		}

		STBI_FREE(g.out);

		if (gr != &head)
			STBI_FREE(gr);

		if (*frames > 0) {
			*x = g.w;
			*y = g.h;
		}

		result = head.data;

		if (*frames > 1) {
			unsigned int size = 4 * g.w * g.h;
			unsigned char *p;

			result = (unsigned char*)stbi__malloc(*frames * (size + 2));
			gr = &head;
			p = result;

			while (gr) {
				prev = gr;
				memcpy(p, gr->data, size);
				p += size;
				*p++ = gr->delay & 0xFF;
				*p++ = (gr->delay & 0xFF00) >> 8;
				gr = gr->next;

				STBI_FREE(prev->data);
				if (prev != &head) STBI_FREE(prev);
			}
		}
	}
	else {
		stbi__result_info result_info;
		result = (unsigned char*)stbi__load_main(&s, x, y, frames, 4, &result_info, 0);
		*frames = !!result;
	}

	fclose(f);
	return result;
}

/**
* Extract GIF frames and save them to disk as PNG files.
*/
ConfigValue extract_frames(ConfigValueArgs args, int num)
{
	if (num != 1)
		return nullptr;
	auto file_path_cv = &args[0];
	std::string file_path = config_data_api->to_string(file_path_cv);

	int w, h, frames;
	auto frames_data = gif_load_frames(file_path.c_str(), &w, &h, &frames);

	// Create config data array to return all generated PNG file paths.
	auto result_file_paths = config_data_api->make(nullptr);
	config_data_api->set_array(result_file_paths, frames);

	unsigned int frame_size = 4 * w * h;
	for (int i = 0; i < frames; ++i) {
		char png_filename[256];
		auto frame_data = frames_data + i * (frame_size + 2);
		auto delay = (stbi__uint16)*(frame_data + frame_size);

		size_t dot_index = file_path.find_last_of(".");
		auto raw_name = file_path.substr(0, dot_index);

		sprintf(png_filename, "%s_%02d.png", raw_name.c_str(), i);
		auto image_written = stbi_write_png(png_filename, w, h, 4, frame_data, 0);
		if (!image_written)
			continue;

		char generation_log_info[1024];
		sprintf(generation_log_info, "Generated `%s` with frame delay %d", png_filename, delay);
		logging_api->info(generation_log_info);

		auto file_path_item = config_data_api->array_item(result_file_paths, i);
		config_data_api->set_string(file_path_item, png_filename);
	}

	return result_file_paths;
}

/**
 * Setup plugin resources and define client JavaScript APIs.
 */
void plugin_loaded(GetEditorApiFunction get_editor_api)
{
	auto api = static_cast<EditorApi*>(get_editor_api(EDITOR_API_ID));
	config_data_api = static_cast<ConfigDataApi*>(get_editor_api(CONFIGDATA_API_ID));
	logging_api = static_cast<EditorLoggingApi*>(get_editor_api(EDITOR_LOGGING_API_ID));
	eval_api = static_cast<EditorEvalApi*>(get_editor_api(EDITOR_EVAL_API_ID));
	allocator_api = static_cast<EditorAllocatorApi*>(get_editor_api(EDITOR_ALLOCATOR_ID));

	plugin_allocator = allocator_api->create("example_allocator", test_custom_allocate, test_custom_deallocate, nullptr);

	api->register_native_function("example", "test_log_arguments", &test_log_arguments);
	api->register_native_function("example", "test_custom_allocator", &test_custom_allocator);

	api->register_native_function("nativeGiphy", "extractFrames", &extract_frames);
}

/**
 * Release plugin resources and exposed APIs.
 */
void plugin_unloaded(GetEditorApiFunction get_editor_api)
{
	auto api = static_cast<EditorApi*>(get_editor_api(EDITOR_API_ID));
	api->unregister_native_function("example", "test_log_arguments");
	api->unregister_native_function("example", "test_custom_allocator");

	api->unregister_native_function("nativeGiphy", "extractFrames");

	allocator_api->destroy(plugin_allocator);
}

} // end namespace

/**
 * Setup plugin APIs.
 */
extern "C" __declspec(dllexport) void *get_editor_plugin_api(unsigned api)
{
	if (api == EDITOR_PLUGIN_SYNC_API_ID) {
		static struct EditorPluginSyncApi editor_api = {nullptr};
		editor_api.get_name = &PLUGIN_NAMESPACE::get_name;
		editor_api.get_version = &PLUGIN_NAMESPACE::get_version;
		editor_api.plugin_loaded = &PLUGIN_NAMESPACE::plugin_loaded;
		editor_api.shutdown = &PLUGIN_NAMESPACE::plugin_unloaded;
		return &editor_api;
	}

	return nullptr;
}
