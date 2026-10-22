#include "texture.h"

#include <array>
#include <cctype>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_GIF
#if defined(_WIN32)
#define STBI_WINDOWS_UTF8
#endif
#include <stb/stb_image.h>

namespace veraa {
namespace render {

namespace {

const std::array<float, 256>& srgbTable() {
  static const std::array<float, 256> table = [] {
    std::array<float, 256> t{};
    for (int i = 0; i < 256; ++i) t[i] = srgbToLinear(i / 255.0f);
    return t;
  }();
  return table;
}

inline int wrapIndex(int i, int n) {
  int m = i % n;
  return m < 0 ? m + n : m;
}

inline int clampIndex(int i, int n) { return i < 0 ? 0 : (i >= n ? n - 1 : i); }

bool endsWithIgnoreCase(const std::string& s, const std::string& suffix) {
  if (s.size() < suffix.size()) return false;
  for (size_t i = 0; i < suffix.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(s[s.size() - suffix.size() + i])) != suffix[i]) return false;
  return true;
}

}  // namespace

std::unique_ptr<Texture> Texture::load(const std::string& path, std::string* error) {
  auto tex = std::make_unique<Texture>();
  if (endsWithIgnoreCase(path, ".exr")) {
    int w = 0, h = 0;
    std::vector<float> rgba;
    if (!loadExr(path, w, h, rgba, error)) return nullptr;
    tex->w_ = w;
    tex->h_ = h;
    tex->fdata_.resize(static_cast<size_t>(w) * h * 3);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
      for (int c = 0; c < 3; ++c) tex->fdata_[i * 3 + c] = rgba[i * 4 + c];
    return tex;
  }
  if (stbi_is_hdr(path.c_str())) {
    int w = 0, h = 0, n = 0;
    float* data = stbi_loadf(path.c_str(), &w, &h, &n, 3);
    if (!data) {
      if (error) *error = std::string("Cannot load image ") + path + ": " + stbi_failure_reason();
      return nullptr;
    }
    tex->w_ = w;
    tex->h_ = h;
    tex->fdata_.assign(data, data + static_cast<size_t>(w) * h * 3);
    stbi_image_free(data);
    return tex;
  }
  int w = 0, h = 0, n = 0;
  unsigned char* data = stbi_load(path.c_str(), &w, &h, &n, 4);
  if (!data) {
    if (error)
      *error = std::string("Cannot load image ") + path + ": " +
               (stbi_failure_reason() ? stbi_failure_reason() : "unknown error");
    return nullptr;
  }
  tex->w_ = w;
  tex->h_ = h;
  tex->bdata_.assign(data, data + static_cast<size_t>(w) * h * 4);
  stbi_image_free(data);
  if (n == 2 || n == 4) {
    for (size_t i = 3; i < tex->bdata_.size(); i += 4)
      if (tex->bdata_[i] != 255) {
        tex->hasAlpha_ = true;
        break;
      }
  }
  return tex;
}

std::unique_ptr<Texture> Texture::checker(int size, int checks, float a, float b) {
  auto tex = std::make_unique<Texture>();
  tex->w_ = tex->h_ = size;
  tex->fdata_.resize(static_cast<size_t>(size) * size * 3);
  int cell = std::max(1, size / checks);
  for (int y = 0; y < size; ++y)
    for (int x = 0; x < size; ++x) {
      float v = (((x / cell) + (y / cell)) & 1) ? a : b;
      size_t i = (static_cast<size_t>(y) * size + x) * 3;
      tex->fdata_[i] = tex->fdata_[i + 1] = tex->fdata_[i + 2] = v;
    }
  return tex;
}

std::unique_ptr<Texture> Texture::fromFloatRgb(int w, int h, std::vector<float> rgb) {
  auto tex = std::make_unique<Texture>();
  tex->w_ = w;
  tex->h_ = h;
  tex->fdata_ = std::move(rgb);
  return tex;
}

Vec3 Texture::texel(int x, int y, bool srgb) const {
  x = wrapIndex(x, w_);
  y = wrapIndex(y, h_);
  size_t i = static_cast<size_t>(y) * w_ + x;
  if (!fdata_.empty()) return {fdata_[i * 3], fdata_[i * 3 + 1], fdata_[i * 3 + 2]};
  const uint8_t* p = &bdata_[i * 4];
  if (srgb) {
    const auto& t = srgbTable();
    return {t[p[0]], t[p[1]], t[p[2]]};
  }
  return {p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f};
}

float Texture::texelAlpha(int x, int y) const {
  if (bdata_.empty()) return 1.0f;
  x = wrapIndex(x, w_);
  y = wrapIndex(y, h_);
  return bdata_[(static_cast<size_t>(y) * w_ + x) * 4 + 3] / 255.0f;
}

template <class Fetch>
auto Texture::bilinear(const Vec2& uv, Fetch fetch) const {
  // v = 0 is the bottom of the image (OBJ / 3ds Max convention); row 0 is the top.
  float fx = uv.x * w_ - 0.5f;
  float fy = (1.0f - uv.y) * h_ - 0.5f;
  if (!std::isfinite(fx) || !std::isfinite(fy)) {
    fx = 0;
    fy = 0;
  }
  float flx = std::floor(fx), fly = std::floor(fy);
  int x0 = static_cast<int>(flx), y0 = static_cast<int>(fly);
  float tx = fx - flx, ty = fy - fly;
  auto a = fetch(x0, y0), b = fetch(x0 + 1, y0), c = fetch(x0, y0 + 1), d = fetch(x0 + 1, y0 + 1);
  return (a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty;
}

Vec3 Texture::color(const Vec2& uv, bool srgb) const {
  if (w_ == 0) return Vec3(1.0f);
  return bilinear(uv, [&](int x, int y) { return texel(x, y, srgb); });
}

float Texture::value(const Vec2& uv) const {
  if (w_ == 0) return 1.0f;
  return bilinear(uv, [&](int x, int y) {
    Vec3 c = texel(x, y, false);
    return (c.x + c.y + c.z) * (1.0f / 3.0f);
  });
}

float Texture::opacity(const Vec2& uv) const {
  if (w_ == 0) return 1.0f;
  if (hasAlpha_) return bilinear(uv, [&](int x, int y) { return texelAlpha(x, y); });
  return clampf(value(uv), 0.0f, 1.0f);
}

// ---- Filtered sampling ----

const std::vector<Texture::Level>& Texture::mips() const {
  std::call_once(mipOnce_, [this] {
    // Level 0 is the base image (kept in bdata_/fdata_); levels 1.. are box-filtered halves.
    mips_.reserve(40);  // keeps `prev` valid while levels are appended
    mips_.push_back(Level{w_, h_, {}, {}});
    const Level* prev = nullptr;
    int pw = w_, ph = h_;
    bool hdr = !fdata_.empty();
    while (pw > 1 || ph > 1) {
      Level L;
      L.w = std::max(1, pw / 2);
      L.h = std::max(1, ph / 2);
      if (hdr) L.f.resize(static_cast<size_t>(L.w) * L.h * 3);
      else L.b.resize(static_cast<size_t>(L.w) * L.h * 4);
      for (int y = 0; y < L.h; ++y)
        for (int x = 0; x < L.w; ++x) {
          float acc[4] = {0, 0, 0, 0};
          int n = 0;
          for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx) {
              int sx = std::min(x * 2 + dx, pw - 1), sy = std::min(y * 2 + dy, ph - 1);
              size_t si = static_cast<size_t>(sy) * pw + sx;
              if (hdr) {
                const float* src = prev ? &prev->f[si * 3] : &fdata_[si * 3];
                for (int c = 0; c < 3; ++c) acc[c] += src[c];
              } else {
                const uint8_t* src = prev ? &prev->b[si * 4] : &bdata_[si * 4];
                for (int c = 0; c < 4; ++c) acc[c] += src[c];
              }
              ++n;
            }
          size_t di = static_cast<size_t>(y) * L.w + x;
          if (hdr)
            for (int c = 0; c < 3; ++c) L.f[di * 3 + c] = acc[c] / n;
          else
            for (int c = 0; c < 4; ++c) L.b[di * 4 + c] = static_cast<uint8_t>(std::lround(acc[c] / n));
        }
      mips_.push_back(std::move(L));
      prev = &mips_.back();
      pw = prev->w;
      ph = prev->h;
    }
  });
  return mips_;
}

TexSample Texture::fetch(const Level& L, int x, int y, bool srgb, bool wrapU, bool wrapV) const {
  x = wrapU ? wrapIndex(x, L.w) : clampIndex(x, L.w);
  y = wrapV ? wrapIndex(y, L.h) : clampIndex(y, L.h);
  size_t i = static_cast<size_t>(y) * L.w + x;
  bool base = L.b.empty() && L.f.empty();
  TexSample s;
  if (!fdata_.empty()) {
    const float* p = base ? &fdata_[i * 3] : &L.f[i * 3];
    s.rgb = Vec3(p[0], p[1], p[2]);
    return s;
  }
  const uint8_t* p = base ? &bdata_[i * 4] : &L.b[i * 4];
  if (srgb) {
    const auto& t = srgbTable();
    s.rgb = Vec3(t[p[0]], t[p[1]], t[p[2]]);
  } else {
    s.rgb = Vec3(p[0], p[1], p[2]) * (1.0f / 255.0f);
  }
  s.a = p[3] * (1.0f / 255.0f);
  return s;
}

TexSample Texture::bilinearAt(const Level& L, const Vec2& uv, bool srgb, bool wrapU, bool wrapV) const {
  float fx = uv.x * L.w - 0.5f;
  float fy = (1.0f - uv.y) * L.h - 0.5f;
  if (!std::isfinite(fx) || !std::isfinite(fy)) fx = fy = 0.0f;
  float flx = std::floor(fx), fly = std::floor(fy);
  int x0 = static_cast<int>(flx), y0 = static_cast<int>(fly);
  float tx = fx - flx, ty = fy - fly;
  TexSample a = fetch(L, x0, y0, srgb, wrapU, wrapV), b = fetch(L, x0 + 1, y0, srgb, wrapU, wrapV),
            c = fetch(L, x0, y0 + 1, srgb, wrapU, wrapV), d = fetch(L, x0 + 1, y0 + 1, srgb, wrapU, wrapV);
  TexSample r;
  r.rgb = (a.rgb * (1 - tx) + b.rgb * tx) * (1 - ty) + (c.rgb * (1 - tx) + d.rgb * tx) * ty;
  r.a = (a.a * (1 - tx) + b.a * tx) * (1 - ty) + (c.a * (1 - tx) + d.a * tx) * ty;
  return r;
}

TexSample Texture::sample(const Vec2& uv, TexFilter filter, float footprint, bool srgb, bool wrapU,
                          bool wrapV) const {
  if (w_ == 0) return TexSample{Vec3(1.0f), 1.0f};
  if (filter == TexFilter::Nearest) {
    Level base{w_, h_, {}, {}};
    int x = static_cast<int>(std::floor(uv.x * w_)), y = static_cast<int>(std::floor((1.0f - uv.y) * h_));
    return fetch(base, x, y, srgb, wrapU, wrapV);
  }
  if (filter == TexFilter::Mipmap && footprint > 0.0f) {
    const auto& levels = mips();
    float lod = std::log2(std::max(footprint * static_cast<float>(std::max(w_, h_)), 1e-8f));
    lod = clampf(lod, 0.0f, static_cast<float>(levels.size() - 1));
    int l0 = static_cast<int>(std::floor(lod));
    float t = lod - l0;
    TexSample a = bilinearAt(levels[l0], uv, srgb, wrapU, wrapV);
    if (t <= 1e-3f || l0 + 1 >= static_cast<int>(levels.size())) return a;
    TexSample b = bilinearAt(levels[l0 + 1], uv, srgb, wrapU, wrapV);
    a.rgb = lerp(a.rgb, b.rgb, t);
    a.a = a.a + (b.a - a.a) * t;
    return a;
  }
  Level base{w_, h_, {}, {}};
  return bilinearAt(base, uv, srgb, wrapU, wrapV);
}

const Texture* TextureCache::get(const std::string& path, std::vector<std::string>* warnings) {
  if (path.empty()) return nullptr;
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = textures_.find(path);
  if (it != textures_.end()) return it->second.get();
  std::unique_ptr<Texture> tex;
  if (path == ":checker") {
    tex = Texture::checker(256, 8, 0.75f, 0.25f);
  } else {
    std::string err;
    tex = Texture::load(path, &err);
    if (!tex && warnings) warnings->push_back(err);
  }
  const Texture* raw = tex.get();
  textures_.emplace(path, std::move(tex));
  return raw;
}

}  // namespace render
}  // namespace veraa
