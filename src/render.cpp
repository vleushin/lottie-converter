#include "render.h"

#define lp_COLOR_DEPTH 8
#define lp_COLOR_BYTES 4

void write_png(
	unsigned char* buffer,
	const size_t width,
	const size_t height,
	const std::filesystem::path& out_file_path
) {
	png_structp png_ptr = nullptr;
	png_infop info_ptr = nullptr;

	png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
	if (png_ptr == nullptr) {
		throw std::runtime_error("PNG export failed: unable to create structure");
	}

	info_ptr = png_create_info_struct(png_ptr);
	if (info_ptr == nullptr) {
		png_destroy_write_struct(&png_ptr, &info_ptr);
		throw std::runtime_error("PNG export failed: unable to create info data");
	}

	if (setjmp(png_jmpbuf(png_ptr))) {
		png_destroy_write_struct(&png_ptr, &info_ptr);
		throw std::runtime_error("PNG export failed: longjump error");
	}

	// Changed to RGB (no alpha) since we're replacing transparency with white
	png_set_IHDR(
		png_ptr,
		info_ptr,
		width,
		height,
		lp_COLOR_DEPTH,
		PNG_COLOR_TYPE_RGB,  // Changed from PNG_COLOR_TYPE_RGB_ALPHA
		PNG_INTERLACE_NONE,
		PNG_COMPRESSION_TYPE_DEFAULT,
		PNG_FILTER_TYPE_DEFAULT
	);

	// Replace transparent pixels with white background
	size_t total_bytes = width * height * lp_COLOR_BYTES;
	unsigned char* rgb_buffer = new unsigned char[width * height * 3]; // RGB only (no alpha)
	
	size_t rgb_index = 0;
	for (size_t i = 0; i < total_bytes; i += lp_COLOR_BYTES) {
		unsigned char r = buffer[i];
		unsigned char g = buffer[i + 1];
		unsigned char b = buffer[i + 2];
		unsigned char a = buffer[i + 3];
		
		if (a == 0) {
			// Fully transparent - replace with white
			rgb_buffer[rgb_index] = 255;     // R
			rgb_buffer[rgb_index + 1] = 255; // G
			rgb_buffer[rgb_index + 2] = 255; // B
		} else if (a == 255) {
			// Fully opaque - keep original colors
			rgb_buffer[rgb_index] = r;       // R
			rgb_buffer[rgb_index + 1] = g;   // G
			rgb_buffer[rgb_index + 2] = b;   // B
		} else {
			// Semi-transparent - blend with white background
			float alpha_f = a / 255.0f;
			float inv_alpha = 1.0f - alpha_f;
			
			rgb_buffer[rgb_index] = (unsigned char)(r * alpha_f + 255 * inv_alpha);     // R
			rgb_buffer[rgb_index + 1] = (unsigned char)(g * alpha_f + 255 * inv_alpha); // G
			rgb_buffer[rgb_index + 2] = (unsigned char)(b * alpha_f + 255 * inv_alpha); // B
		}
		
		rgb_index += 3;
	}

	unsigned char** row_pointers = (unsigned char**)png_malloc(png_ptr, height * sizeof(png_byte*));
	if (row_pointers == nullptr) {
		png_destroy_write_struct(&png_ptr, &info_ptr);
		delete[] rgb_buffer;
		throw std::runtime_error("PNG export failed");
	}
	
	// Point to RGB buffer instead of original RGBA buffer
	for (unsigned int y = 0; y < height; ++y) {
		row_pointers[y] = rgb_buffer + width * y * 3; // 3 bytes per pixel (RGB)
	}
	png_set_rows(png_ptr, info_ptr, row_pointers);

	FILE* out_file = fopen((const char*)out_file_path.generic_string().c_str(), "wb");

	png_init_io(png_ptr, out_file);

	png_write_png(png_ptr, info_ptr, PNG_TRANSFORM_BGR, nullptr);

	png_free(png_ptr, row_pointers);
	png_destroy_write_struct(&png_ptr, &info_ptr);
	
	delete[] rgb_buffer; // Clean up RGB buffer
	fclose(out_file);
}

// Alternative version if you want to keep the original function signature
// and modify the buffer in-place before calling the original write_png
void apply_white_background(unsigned char* buffer, const size_t width, const size_t height) {
	size_t total_bytes = width * height * lp_COLOR_BYTES;
	
	for (size_t i = 0; i < total_bytes; i += lp_COLOR_BYTES) {
		unsigned char& r = buffer[i];
		unsigned char& g = buffer[i + 1];
		unsigned char& b = buffer[i + 2];
		unsigned char& a = buffer[i + 3];
		
		if (a == 0) {
			// Fully transparent - replace with white
			r = 255;
			g = 255;
			b = 255;
			a = 255; // Make fully opaque
		} else if (a != 255) {
			// Semi-transparent - blend with white background
			float alpha_f = a / 255.0f;
			float inv_alpha = 1.0f - alpha_f;
			
			r = (unsigned char)(r * alpha_f + 255 * inv_alpha);
			g = (unsigned char)(g * alpha_f + 255 * inv_alpha);
			b = (unsigned char)(b * alpha_f + 255 * inv_alpha);
			a = 255; // Make fully opaque
		}
	}
}

void render(
	const std::string& lottie_data,
	const size_t width,
	const size_t height,
	const std::filesystem::path& output_directory,
	double fps,
	size_t threads_count
) {
	static unsigned int cache_counter = 0;
	const auto cache_counter_str = std::to_string(++cache_counter);
	auto player = rlottie::Animation::loadFromData(lottie_data, cache_counter_str);
	if (!player) throw std::runtime_error("can not load lottie animation");

	const size_t player_frame_count = player->totalFrame();
	const double player_fps = player->frameRate();
	if (fps == 0.0) fps = player_fps;
	const double duration = player_frame_count / (double)player_fps;
	const double step = player_fps / fps;
	const double output_frame_count = fps * duration;

	if (threads_count == 0) {
		threads_count = std::thread::hardware_concurrency();
	}
	auto threads = std::vector<std::thread>(threads_count);
	for (int i = 0; i < threads_count; ++i) {
		threads.push_back(std::thread([i, output_frame_count, step, width, height, threads_count, &output_directory, &lottie_data, cache_counter_str]() {
			auto local_player = rlottie::Animation::loadFromData(lottie_data, cache_counter_str);
			char file_name[8];
			uint32_t* const buffer = new uint32_t[width * height];
			for (size_t j = i; j < output_frame_count; j += threads_count) {
				rlottie::Surface surface(buffer, width, height, width * lp_COLOR_BYTES);
				local_player->renderSync(round(j * step), surface);

				// Apply white background to transparent pixels
				apply_white_background((unsigned char*)buffer, width, height);

				sprintf(file_name, "%03zu.png", j);
				write_png(
					(unsigned char*)buffer,
					width,
					height,
					output_directory / std::filesystem::path(file_name)
				);
			}
			delete[] buffer;
		}));
	}

	for (auto& thread : threads) {
		if (thread.joinable()) {
			thread.join();
		}
	}
}