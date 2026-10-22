#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "renderer.h"
#include "texture.h"

#if defined(_WIN32)
#define STBIW_WINDOWS_UTF8
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>

namespace veraa {

namespace {

std::string lowerExt(const std::string& path) {
  size_t dot = path.find_last_of('.');
  if (dot == std::string::npos) return {};
  std::string e = path.substr(dot + 1);
  for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return e;
}

// Baseline uncompressed TIFF (8 bits per channel, RGB or RGBA, little endian).
bool writeTiff(const std::string& path, int w, int h, int channels, const std::vector<uint8_t>& data) {
  std::ofstream f(std::filesystem::u8path(path), std::ios::binary);
  if (!f) return false;
  auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
  auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
  uint32_t imageBytes = static_cast<uint32_t>(w) * h * channels;
  const int entries = channels == 4 ? 12 : 11;
  uint32_t ifdOffset = 8;
  uint32_t ifdSize = 2 + entries * 12 + 4;
  uint32_t bitsOffset = ifdOffset + ifdSize;
  uint32_t dataOffset = bitsOffset + 8;
  f.write("II", 2);
  u16(42);
  u32(ifdOffset);
  u16(static_cast<uint16_t>(entries));
  auto entry = [&](uint16_t tag, uint16_t type, uint32_t count, uint32_t value) {
    u16(tag);
    u16(type);
    u32(count);
    u32(value);
  };
  entry(256, 4, 1, static_cast<uint32_t>(w));                 // ImageWidth
  entry(257, 4, 1, static_cast<uint32_t>(h));                 // ImageLength
  entry(258, 3, static_cast<uint32_t>(channels), bitsOffset); // BitsPerSample
  entry(259, 3, 1, 1);                                        // Compression: none
  entry(262, 3, 1, 2);                                        // Photometric: RGB
  entry(273, 4, 1, dataOffset);                               // StripOffsets
  entry(277, 3, 1, static_cast<uint32_t>(channels));          // SamplesPerPixel
  entry(278, 4, 1, static_cast<uint32_t>(h));                 // RowsPerStrip
  entry(279, 4, 1, imageBytes);                               // StripByteCounts
  entry(284, 3, 1, 1);                                        // PlanarConfiguration: chunky
  entry(296, 3, 1, 1);                                        // ResolutionUnit: none
  if (channels == 4) entry(338, 3, 1, 2);                     // ExtraSamples: unassociated alpha
  u32(0);
  for (int i = 0; i < 4; ++i) u16(8);
  if (channels == 3) {
    std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
      for (int c = 0; c < 3; ++c) rgb[i * 3 + c] = data[i * 4 + c];
    f.write(reinterpret_cast<const char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
  } else {
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(imageBytes));
  }
  return static_cast<bool>(f);
}

}  // namespace

bool writePng(const std::string& path, int width, int height, const std::vector<uint8_t>& rgba) {
  if (width <= 0 || height <= 0 || rgba.size() < static_cast<size_t>(width) * height * 4) return false;
  return stbi_write_png(path.c_str(), width, height, 4, rgba.data(), width * 4) != 0;
}

bool writeHdr(const std::string& path, int width, int height, const std::vector<float>& rgb) {
  if (width <= 0 || height <= 0 || rgb.size() < static_cast<size_t>(width) * height * 3) return false;
  return stbi_write_hdr(path.c_str(), width, height, 3, rgb.data()) != 0;
}

bool saveImage(const std::string& path, int w, int h, const std::vector<uint8_t>& rgba8, const std::vector<float>* hdr,
               const std::vector<float>* alpha, bool withAlpha, std::string* error) {
  auto fail = [&](const std::string& msg) {
    if (error) *error = msg;
    return false;
  };
  if (w <= 0 || h <= 0) return fail("The image is empty");
  size_t n = static_cast<size_t>(w) * h;
  std::string ext = lowerExt(path);
  if (ext == "exr" || ext == "hdr") {
    if (!hdr || hdr->size() < n * 3) return fail("No floating-point image to save");
    if (ext == "hdr") return writeHdr(path, w, h, *hdr) ? true : fail("Cannot write " + path);
    if (withAlpha && alpha && alpha->size() >= n) {
      std::vector<float> rgba(n * 4);
      for (size_t i = 0; i < n; ++i) {
        for (int c = 0; c < 3; ++c) rgba[i * 4 + c] = (*hdr)[i * 3 + c];
        rgba[i * 4 + 3] = (*alpha)[i];
      }
      return render::saveExr(path, w, h, 4, rgba.data(), error);
    }
    return render::saveExr(path, w, h, 3, hdr->data(), error);
  }
  if (rgba8.size() < n * 4) return fail("No image to save");
  std::vector<uint8_t> px = rgba8;
  if (withAlpha && alpha && alpha->size() >= n)
    for (size_t i = 0; i < n; ++i) px[i * 4 + 3] = static_cast<uint8_t>(std::lround(std::min(std::max((*alpha)[i], 0.0f), 1.0f) * 255.0f));
  bool ok = false;
  if (ext == "png") {
    ok = stbi_write_png(path.c_str(), w, h, 4, px.data(), w * 4) != 0;
  } else if (ext == "jpg" || ext == "jpeg") {
    std::vector<uint8_t> rgb(n * 3);
    for (size_t i = 0; i < n; ++i)
      for (int c = 0; c < 3; ++c) rgb[i * 3 + c] = px[i * 4 + c];
    ok = stbi_write_jpg(path.c_str(), w, h, 3, rgb.data(), 95) != 0;
  } else if (ext == "bmp") {
    ok = stbi_write_bmp(path.c_str(), w, h, 4, px.data()) != 0;
  } else if (ext == "tga") {
    ok = stbi_write_tga(path.c_str(), w, h, 4, px.data()) != 0;
  } else if (ext == "tif" || ext == "tiff") {
    ok = writeTiff(path, w, h, withAlpha ? 4 : 3, px);
  } else {
    return fail("Unsupported image format: ." + ext);
  }
  return ok ? true : fail("Cannot write " + path);
}

bool isDataElement(const std::string& name) {
  return name == "Normals" || name == "ZDepth" || name == "ObjectID" || name == "MaterialID" || name == "WireColor" ||
         name == "Alpha" || name == "DiffuseFilter";
}

int saveRenderElements(const Renderer& renderer, const DisplaySettings& beauty, const std::string& dir,
                       const std::string& prefix, const std::vector<std::string>& extensions,
                       std::vector<std::string>* savedPaths, std::string* error) {
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::u8path(dir), ec);
  int w = renderer.width(), h = renderer.height(), saved = 0;
  // Lighting parts use the beauty's tone curve (without denoising or Light Mix); data
  // elements are written linearly.
  DisplaySettings lit = beauty;
  lit.denoise = false;
  lit.useLightMix = false;
  lit.element.clear();
  DisplaySettings data;
  data.tonemap = 2;
  auto save = [&](const std::string& name, const std::vector<float>& rgb, bool isData) {
    std::vector<uint8_t> rgba8;
    toneMapImage(rgb, w, h, isData ? data : lit, rgba8);
    for (const std::string& ext : extensions) {
      std::string path = (std::filesystem::u8path(dir) / std::filesystem::u8path(prefix + name + ext)).u8string();
      if (saveImage(path, w, h, rgba8, &rgb, nullptr, false, error)) {
        ++saved;
        if (savedPaths) savedPaths->push_back(path);
      }
    }
  };
  for (const auto& name : renderer.elementNames()) {
    std::vector<float> rgb;
    if (renderer.resolveElement(name, rgb)) save(name, rgb, isDataElement(name));
  }
  std::vector<std::string> groups = renderer.lightGroupNames();
  for (const auto& g : groups) {
    std::vector<LightMixGroup> solo;
    for (const auto& other : groups) solo.push_back({other, Vec3(1.0f), 1.0f, other == g});
    std::vector<float> rgb;
    if (renderer.resolveLightMix(solo, rgb)) save("LightMix_" + g, rgb, false);
  }
  return saved;
}

}  // namespace veraa
