#include "renderer.h"

#if defined(_WIN32)
#define STBIW_WINDOWS_UTF8
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>

namespace veraa {

bool writePng(const std::string& path, int width, int height, const std::vector<uint8_t>& rgba) {
  if (width <= 0 || height <= 0 || rgba.size() < static_cast<size_t>(width) * height * 4) return false;
  return stbi_write_png(path.c_str(), width, height, 4, rgba.data(), width * 4) != 0;
}

bool writeHdr(const std::string& path, int width, int height, const std::vector<float>& rgb) {
  if (width <= 0 || height <= 0 || rgb.size() < static_cast<size_t>(width) * height * 3) return false;
  return stbi_write_hdr(path.c_str(), width, height, 3, rgb.data()) != 0;
}

}  // namespace veraa
