// Image textures for the renderer (8-bit or float), with bilinear wrap sampling and a cache.
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "common.h"

namespace veraa {
namespace render {

class Texture {
 public:
  // Loads an image file. 8-bit images keep 8 bits per channel; .hdr loads as float.
  static std::unique_ptr<Texture> load(const std::string& path, std::string* error);
  // Procedural checkerboard (used by material previews).
  static std::unique_ptr<Texture> checker(int size, int checks, float a, float b);
  // Float RGB image (used for HDR environments built in memory).
  static std::unique_ptr<Texture> fromFloatRgb(int w, int h, std::vector<float> rgb);

  int width() const { return w_; }
  int height() const { return h_; }
  bool isHdr() const { return !fdata_.empty(); }
  bool hasAlpha() const { return hasAlpha_; }

  // Linear color. 8-bit data is treated as sRGB-encoded when srgb is true.
  Vec3 color(const Vec2& uv, bool srgb = true) const;
  // Scalar value in [0, 1] (luminance of the raw, non-decoded values) for bump maps.
  float value(const Vec2& uv) const;
  // Opacity: the alpha channel when the image has one, otherwise the raw luminance.
  float opacity(const Vec2& uv) const;
  // Raw texel fetch (integer coordinates wrap), linear float RGB.
  Vec3 texel(int x, int y, bool srgb) const;
  float texelAlpha(int x, int y) const;

 private:
  template <class Fetch>
  auto bilinear(const Vec2& uv, Fetch fetch) const;

  int w_ = 0, h_ = 0;
  bool hasAlpha_ = false;
  std::vector<uint8_t> bdata_;  // RGBA8
  std::vector<float> fdata_;    // RGB float
};

// Loads each file once. Thread-safe.
class TextureCache {
 public:
  // Returns null (and records a warning) when the file cannot be loaded.
  const Texture* get(const std::string& resolvedPath, std::vector<std::string>* warnings);

 private:
  std::mutex mutex_;
  std::map<std::string, std::unique_ptr<Texture>> textures_;
};

}  // namespace render
}  // namespace veraa
