// OpenEXR reading and writing through the vendored TinyEXR (BSD-3), using stb's zlib.
#include "texture.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wclass-memaccess"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wtype-limits"
#endif
#define TINYEXR_USE_MINIZ 0
#define TINYEXR_USE_STB_ZLIB 1
#define TINYEXR_USE_THREAD 0
#define TINYEXR_USE_OPENMP 0
#define TINYEXR_IMPLEMENTATION
#include <tinyexr/tinyexr.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <cstdio>
#include <cstdlib>

namespace veraa {
namespace render {

bool loadExr(const std::string& path, int& w, int& h, std::vector<float>& rgba, std::string* error) {
  float* out = nullptr;
  const char* err = nullptr;
  int ret = LoadEXR(&out, &w, &h, path.c_str(), &err);
  if (ret != TINYEXR_SUCCESS) {
    if (error) *error = "Cannot load EXR " + path + ": " + (err ? err : "unknown error");
    if (err) FreeEXRErrorMessage(err);
    return false;
  }
  rgba.assign(out, out + static_cast<size_t>(w) * h * 4);
  std::free(out);
  return true;
}

bool saveExr(const std::string& path, int w, int h, int channels, const float* data, std::string* error) {
  if (w <= 0 || h <= 0 || (channels != 3 && channels != 4) || !data) {
    if (error) *error = "Nothing to save";
    return false;
  }
  EXRHeader header;
  InitEXRHeader(&header);
  EXRImage image;
  InitEXRImage(&image);
  size_t n = static_cast<size_t>(w) * h;
  std::vector<std::vector<float>> planes(channels, std::vector<float>(n));
  for (size_t i = 0; i < n; ++i)
    for (int c = 0; c < channels; ++c) planes[c][i] = data[i * channels + c];
  // Channels are stored in alphabetical order: (A)BGR.
  std::vector<float*> ptrs;
  std::vector<EXRChannelInfo> infos(channels);
  const char* names4[] = {"A", "B", "G", "R"};
  const char* names3[] = {"B", "G", "R"};
  for (int c = 0; c < channels; ++c) {
    const char* name = channels == 4 ? names4[c] : names3[c];
    int src = channels == 4 ? (c == 0 ? 3 : 3 - c) : 2 - c;
    ptrs.push_back(planes[src].data());
    std::snprintf(infos[c].name, sizeof(infos[c].name), "%s", name);
  }
  image.images = reinterpret_cast<unsigned char**>(ptrs.data());
  image.width = w;
  image.height = h;
  image.num_channels = channels;
  header.num_channels = channels;
  header.channels = infos.data();
  std::vector<int> pixelTypes(channels, TINYEXR_PIXELTYPE_FLOAT), requested(channels, TINYEXR_PIXELTYPE_HALF);
  header.pixel_types = pixelTypes.data();
  header.requested_pixel_types = requested.data();
  header.compression_type = TINYEXR_COMPRESSIONTYPE_ZIP;
  const char* err = nullptr;
  int ret = SaveEXRImageToFile(&image, &header, path.c_str(), &err);
  if (ret != TINYEXR_SUCCESS) {
    if (error) *error = std::string("Cannot save EXR: ") + (err ? err : "unknown error");
    if (err) FreeEXRErrorMessage(err);
    return false;
  }
  return true;
}

}  // namespace render
}  // namespace veraa
