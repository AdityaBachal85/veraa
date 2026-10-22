// Image textures for the renderer (8-bit or float), with bilinear, nearest and mipmapped
// sampling, and a thread-safe cache.
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "common.h"

namespace veraa {
namespace render {

enum class TexFilter { Bilinear, Mipmap, Nearest };

struct TexSample {
  Vec3 rgb;        // linear (8-bit data decoded from sRGB when requested)
  float a = 1.0f;  // alpha
};

class Texture {
 public:
  // Loads an image file (PNG, JPG, BMP, TGA, HDR, EXR, ...). 8-bit images keep 8 bits per channel.
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
  // Filtered lookup. footprint is the texture-space width of the pixel (Mipmap filtering).
  // wrapU/V false clamps to the edge.
  TexSample sample(const Vec2& uv, TexFilter filter, float footprint, bool srgb, bool wrapU = true,
                   bool wrapV = true) const;
  // Raw texel fetch (integer coordinates wrap), linear float RGB.
  Vec3 texel(int x, int y, bool srgb) const;
  float texelAlpha(int x, int y) const;

 private:
  struct Level {
    int w = 0, h = 0;
    std::vector<uint8_t> b;  // RGBA8 (8-bit textures)
    std::vector<float> f;    // RGB float (HDR textures)
  };
  TexSample fetch(const Level& L, int x, int y, bool srgb, bool wrapU, bool wrapV) const;
  TexSample bilinearAt(const Level& L, const Vec2& uv, bool srgb, bool wrapU, bool wrapV) const;
  const std::vector<Level>& mips() const;
  template <class Fetch>
  auto bilinear(const Vec2& uv, Fetch fetch) const;

  int w_ = 0, h_ = 0;
  bool hasAlpha_ = false;
  std::vector<uint8_t> bdata_;  // RGBA8
  std::vector<float> fdata_;    // RGB float
  mutable std::once_flag mipOnce_;
  mutable std::vector<Level> mips_;  // level 0 aliases the base data (empty vectors)
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

// Reads an OpenEXR file as linear RGBA floats. Returns false and sets error on failure.
bool loadExr(const std::string& path, int& w, int& h, std::vector<float>& rgba, std::string* error);
// Writes linear RGB (3 floats per pixel) or RGBA (4) as a half-float, ZIP-compressed OpenEXR file.
bool saveExr(const std::string& path, int w, int h, int channels, const float* data, std::string* error);

}  // namespace render
}  // namespace veraa
