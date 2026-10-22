#include "texmaps.h"

#include <algorithm>

#include "modifier.h"  // perlinNoise
#include "scene.h"

namespace veraa {
namespace render {

// ---------------------------------------------------------------------------------------------
// Helpers

uint32_t hashU32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}

float hashFloat(uint32_t x) { return static_cast<float>(hashU32(x) >> 8) * (1.0f / 16777216.0f); }

Vec3 rgbToHsv(const Vec3& c) {
  float mx = maxComponent(c), mn = std::min(c.x, std::min(c.y, c.z));
  float d = mx - mn;
  float h = 0.0f;
  if (d > 1e-8f) {
    if (mx == c.x) h = (c.y - c.z) / d + (c.y < c.z ? 6.0f : 0.0f);
    else if (mx == c.y) h = (c.z - c.x) / d + 2.0f;
    else h = (c.x - c.y) / d + 4.0f;
    h /= 6.0f;
  }
  return {h, mx > 0.0f ? d / mx : 0.0f, mx};
}

Vec3 hsvToRgb(const Vec3& hsv) {
  float h = hsv.x - std::floor(hsv.x), s = clampf(hsv.y, 0.0f, 1.0f), v = hsv.z;
  float f = h * 6.0f;
  int i = static_cast<int>(std::floor(f)) % 6;
  float fr = f - std::floor(f);
  float p = v * (1 - s), q = v * (1 - s * fr), t = v * (1 - s * (1 - fr));
  switch (i) {
    case 0: return {v, t, p};
    case 1: return {q, v, p};
    case 2: return {p, v, t};
    case 3: return {p, q, v};
    case 4: return {t, p, v};
    default: return {v, p, q};
  }
}

namespace {

inline float smooth01(float t) {
  t = clampf(t, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

inline Vec3 vabs(const Vec3& v) { return {std::fabs(v.x), std::fabs(v.y), std::fabs(v.z)}; }

float fractalNoise(const Vec3& p, int type, float levels) {
  // type: 0 regular, 1 fractal, 2 turbulence. Returns [0, 1].
  if (type == 0) return clampf(perlinNoise(p.x, p.y, p.z) * 0.5f + 0.5f, 0.0f, 1.0f);
  float sum = 0.0f, amp = 1.0f, norm = 0.0f, freq = 1.0f;
  int n = std::max(1, static_cast<int>(std::ceil(levels)));
  for (int i = 0; i < n; ++i) {
    float w = (i + 1 > levels) ? levels - i : 1.0f;  // fractional last level
    float v = perlinNoise(p.x * freq + i * 17.13f, p.y * freq - i * 9.71f, p.z * freq + i * 3.37f);
    if (type == 2) v = std::fabs(v);
    sum += v * amp * w;
    norm += amp * w;
    amp *= 0.5f;
    freq *= 2.0f;
  }
  float r = sum / std::max(norm, 1e-6f);
  return type == 2 ? clampf(r * 1.6f, 0.0f, 1.0f) : clampf(r * 0.5f + 0.5f, 0.0f, 1.0f);
}

// ---- Output group ----
struct OutputParams {
  bool active = false;
  float amount = 1.0f, offset = 0.0f, level = 1.0f;
  bool invert = false, clamp = false;
  void read(const ParamBlock& p) {
    if (!p.has("outputAmount")) return;
    amount = p.getFloat("outputAmount");
    offset = p.getFloat("rgbOffset");
    level = p.getFloat("rgbLevel");
    invert = p.getBool("invert");
    clamp = p.getBool("clampOutput");
    active = amount != 1.0f || offset != 0.0f || level != 1.0f || invert || clamp;
  }
  Vec3 apply(Vec3 c) const {
    if (!active) return c;
    if (invert) c = Vec3(1.0f) - c;
    c = (c * level + Vec3(offset)) * amount;
    return clamp ? clampColor(c, 0.0f, 1.0f) : c;
  }
};

// ---- 2D placement ----
struct Coords2D {
  int mapping = 0;
  float uOff = 0, vOff = 0, uTile = 1, vTile = 1, cosA = 1, sinA = 0;
  bool uWrap = true, vWrap = true, uMirror = false, vMirror = false;
  bool realWorld = false;
  float realW = 100, realH = 100;
  void read(const ParamBlock& p) {
    if (!p.has("mapping")) return;
    mapping = p.getInt("mapping");
    uOff = p.getFloat("uOffset");
    vOff = p.getFloat("vOffset");
    uTile = p.getFloat("uTiling");
    vTile = p.getFloat("vTiling");
    float a = radians(p.getFloat("wAngle"));
    cosA = std::cos(a);
    sinA = std::sin(a);
    uWrap = p.getBool("uWrap");
    vWrap = p.getBool("vWrap");
    uMirror = p.getBool("uMirror");
    vMirror = p.getBool("vMirror");
    realWorld = p.getBool("realWorld");
    realW = std::max(p.getFloat("realWidth"), 1e-4f);
    realH = std::max(p.getFloat("realHeight"), 1e-4f);
  }
  // Base coordinates before placement.
  Vec2 source(const ShadeContext& sc) const {
    switch (mapping) {
      case 1: return {sc.pObj.x * 0.01f, sc.pObj.y * 0.01f};
      case 2: return {sc.p.x * 0.01f, sc.p.y * 0.01f};
      case 3: {
        Vec3 d = normalize(-sc.wo);
        float u = 0.5f + std::atan2(d.x, d.y) / kTwoPi;
        float v = 1.0f - std::acos(clampf(d.z, -1.0f, 1.0f)) / kPi;
        return {u, v};
      }
      case 4: return {0.5f + std::atan2(sc.pObj.y, sc.pObj.x) / kTwoPi, sc.pObj.z * 0.01f};
      default: return sc.uv;
    }
  }
  // Returns false when the point falls outside an untiled map.
  bool apply(const ShadeContext& sc, Vec2& uv, float& scale) const {
    Vec2 s = source(sc);
    if (realWorld) s = Vec2(s.x * 100.0f / realW, s.y * 100.0f / realH);
    float cu = s.x - 0.5f - uOff, cv = s.y - 0.5f - vOff;
    float ru = cu * cosA + cv * sinA, rv = -cu * sinA + cv * cosA;
    uv = Vec2(ru * uTile + 0.5f, rv * vTile + 0.5f);
    scale = std::max(std::fabs(uTile), std::fabs(vTile)) * (realWorld ? 100.0f / std::min(realW, realH) : 1.0f);
    if (!uWrap && (uv.x < 0.0f || uv.x > 1.0f)) return false;
    if (!vWrap && (uv.y < 0.0f || uv.y > 1.0f)) return false;
    if (uMirror) {
      float f = uv.x - 2.0f * std::floor(uv.x * 0.5f);
      uv.x = f > 1.0f ? 2.0f - f : f;
    }
    if (vMirror) {
      float f = uv.y - 2.0f * std::floor(uv.y * 0.5f);
      uv.y = f > 1.0f ? 2.0f - f : f;
    }
    return true;
  }
};

// ---- 3D placement ----
struct Coords3D {
  int source = 0;
  Vec3 offset, tiling{1, 1, 1};
  void read(const ParamBlock& p) {
    if (!p.has("source")) return;
    source = p.getInt("source");
    offset = Vec3(p.getFloat("offsetX"), p.getFloat("offsetY"), p.getFloat("offsetZ"));
    tiling = Vec3(p.getFloat("tilingX"), p.getFloat("tilingY"), p.getFloat("tilingZ"));
  }
  Vec3 apply(const ShadeContext& sc) const {
    Vec3 q = source == 1 ? sc.p : (source == 2 ? Vec3(sc.uv.x * 100.0f, sc.uv.y * 100.0f, 0.0f) : sc.pObj);
    q = q - offset;
    return {q.x * tiling.x, q.y * tiling.y, q.z * tiling.z};
  }
};

struct Ctx {
  MapCompileContext& c;
  ColorInput color(const TexMap& m, const std::string& key) { return colorInput(m.params, m.maps, key, c); }
  FloatInput fl(const TexMap& m, const std::string& key) { return floatInput(m.params, m.maps, key, c); }
};

// ---------------------------------------------------------------------------------------------
// Map nodes

class ConstantMap : public MapNode {
 public:
  explicit ConstantMap(const Vec3& c) : c_(c) {}
  Vec3 color(const ShadeContext&) const override { return c_; }

 private:
  Vec3 c_;
};

class BitmapMap : public MapNode {
 public:
  BitmapMap(const TexMap& m, Ctx& ctx) {
    const ParamBlock& p = m.params;
    coords_.read(p);
    out_.read(p);
    std::string file = p.getString("file");
    if (!file.empty()) {
      bool builtin = file[0] == ':';
      std::string path = ctx.c.scene && !builtin ? ctx.c.scene->resolvePath(file) : file;
      tex_ = ctx.c.cache ? ctx.c.cache->get(path, ctx.c.warnings) : nullptr;
    }
    int cs = p.getInt("colorSpace");
    srgb_ = cs == 1 || (cs == 0 && !(tex_ && tex_->isHdr()));
    alphaSource_ = p.getInt("alphaSource");
    mono_ = p.getInt("monoOutput");
    int f = p.getInt("filtering");
    filter_ = f == 2 ? TexFilter::Nearest : (f == 1 ? TexFilter::Mipmap : TexFilter::Bilinear);
    blur_ = p.getFloat("blur");
  }
  TexSample fetch(const ShadeContext& sc, bool srgb, bool& inside) const {
    Vec2 uv;
    float scale = 1.0f;
    inside = coords_.apply(sc, uv, scale);
    if (!inside || !tex_) return TexSample{Vec3(0.0f), 0.0f};
    float fp = sc.footprint * scale * blur_;
    return tex_->sample(uv, filter_, fp, srgb, coords_.uWrap, coords_.vWrap);
  }
  Vec3 color(const ShadeContext& sc) const override {
    if (!tex_) return Vec3(0.5f);
    bool inside;
    TexSample s = fetch(sc, srgb_, inside);
    return out_.apply(s.rgb);
  }
  float value(const ShadeContext& sc) const override {
    if (!tex_) return 0.5f;
    bool inside;
    TexSample s = fetch(sc, false, inside);
    if (mono_ == 2) return out_.apply(Vec3(s.a)).x;
    Vec3 c = out_.apply(srgb_ && tex_->isHdr() ? s.rgb : s.rgb);
    return (c.x + c.y + c.z) * (1.0f / 3.0f);
  }
  float opacity(const ShadeContext& sc) const override {
    if (!tex_) return 1.0f;
    bool inside;
    TexSample s = fetch(sc, false, inside);
    if (!inside) return 0.0f;
    bool useAlpha = alphaSource_ == 0 && tex_->hasAlpha();
    if (mono_ == 2) useAlpha = true;
    if (mono_ == 1) useAlpha = false;
    float v = useAlpha ? s.a : (s.rgb.x + s.rgb.y + s.rgb.z) * (1.0f / 3.0f);
    return clampf(out_.apply(Vec3(v)).x, 0.0f, 1.0f);
  }

 private:
  Coords2D coords_;
  OutputParams out_;
  const Texture* tex_ = nullptr;
  bool srgb_ = true;
  int alphaSource_ = 0, mono_ = 0;
  TexFilter filter_ = TexFilter::Mipmap;
  float blur_ = 1.0f;
};

class CheckerMap : public MapNode {
 public:
  CheckerMap(const TexMap& m, Ctx& ctx) : c1_(ctx.color(m, "color1")), c2_(ctx.color(m, "color2")) {
    coords_.read(m.params);
    out_.read(m.params);
    soften_ = m.params.getFloat("soften");
  }
  Vec3 color(const ShadeContext& sc) const override {
    Vec2 uv;
    float scale;
    if (!coords_.apply(sc, uv, scale)) return Vec3(0.0f);
    float fu = uv.x * 2.0f, fv = uv.y * 2.0f;
    float t;
    if (soften_ <= 0.0f) {
      int iu = static_cast<int>(std::floor(fu)), iv = static_cast<int>(std::floor(fv));
      t = ((iu + iv) & 1) ? 1.0f : 0.0f;
    } else {
      // Smoothed square waves multiplied together.
      auto wave = [&](float x) {
        float f = x - std::floor(x);
        float w = soften_ * 0.5f;
        float a = smooth01(f / std::max(w, 1e-4f)) - smooth01((f - 0.5f) / std::max(w, 1e-4f));
        return 2.0f * a - 1.0f;
      };
      t = 0.5f - 0.5f * wave(fu * 0.5f) * wave(fv * 0.5f);
    }
    return out_.apply(lerp(c1_.eval(sc), c2_.eval(sc), t));
  }

 private:
  ColorInput c1_, c2_;
  Coords2D coords_;
  OutputParams out_;
  float soften_ = 0.0f;
};

class GradientMap : public MapNode {
 public:
  GradientMap(const TexMap& m, Ctx& ctx)
      : c1_(ctx.color(m, "color1")), c2_(ctx.color(m, "color2")), c3_(ctx.color(m, "color3")) {
    const ParamBlock& p = m.params;
    coords_.read(p);
    out_.read(p);
    pos2_ = clampf(p.getFloat("color2Pos"), 0.001f, 0.999f);
    radial_ = p.getInt("gradientType") == 1;
    noise_ = p.getFloat("noiseAmount");
    noiseSize_ = std::max(p.getFloat("noiseSize"), 1e-4f);
  }
  Vec3 color(const ShadeContext& sc) const override {
    Vec2 uv;
    float scale;
    coords_.apply(sc, uv, scale);
    float t = radial_ ? 1.0f - std::min(1.0f, length(Vec2(uv.x - 0.5f, uv.y - 0.5f)) * 2.0f) : uv.y;
    if (noise_ > 0.0f) t += noise_ * perlinNoise(uv.x / noiseSize_ * 4.0f, uv.y / noiseSize_ * 4.0f, 0.37f);
    t = clampf(t, 0.0f, 1.0f);
    // Color 3 at the bottom (t = 0), color 1 at the top, like the classic Gradient map.
    Vec3 c = t < pos2_ ? lerp(c3_.eval(sc), c2_.eval(sc), t / pos2_)
                       : lerp(c2_.eval(sc), c1_.eval(sc), (t - pos2_) / (1.0f - pos2_));
    return out_.apply(c);
  }

 private:
  ColorInput c1_, c2_, c3_;
  Coords2D coords_;
  OutputParams out_;
  float pos2_ = 0.5f;
  bool radial_ = false;
  float noise_ = 0.0f, noiseSize_ = 1.0f;
};

class GradientRampMap : public MapNode {
 public:
  GradientRampMap(const TexMap& m, Ctx& ctx) {
    const ParamBlock& p = m.params;
    coords_.read(p);
    out_.read(p);
    type_ = p.getInt("gradientType");
    interp_ = p.getInt("interpolation");
    int n = p.getInt("keyCount");
    for (int i = 1; i <= n; ++i) {
      std::string k = std::to_string(i);
      keys_.push_back({p.getFloat("pos" + k), ctx.color(m, "color" + k)});
    }
    std::stable_sort(keys_.begin(), keys_.end(), [](const Key& a, const Key& b) { return a.pos < b.pos; });
    source_ = ctx.fl(m, "source");
    hMin_ = p.getFloat("heightMin");
    hMax_ = p.getFloat("heightMax");
    noise_ = p.getFloat("noiseAmount");
    noiseSize_ = std::max(p.getFloat("noiseSize"), 1e-4f);
  }
  Vec3 color(const ShadeContext& sc) const override {
    Vec2 uv;
    float scale;
    coords_.apply(sc, uv, scale);
    float u = uv.x - std::floor(uv.x), v = uv.y - std::floor(uv.y);
    float t = 0.0f;
    float cu = u - 0.5f, cv = v - 0.5f;
    switch (type_) {
      case 0: t = u; break;
      case 1: t = v; break;
      case 2: t = std::min(1.0f, std::sqrt(cu * cu + cv * cv) * 2.0f); break;
      case 3: t = (u + v) * 0.5f; break;
      case 4: t = std::max(std::fabs(cu), std::fabs(cv)) * 2.0f; break;
      case 5: t = 0.5f + std::atan2(cv, cu) / kTwoPi; break;
      case 6: t = 1.0f - std::fabs(dot(sc.ns, sc.wo)); break;
      case 7: t = (sc.p.z - hMin_) / std::max(hMax_ - hMin_, 1e-4f); break;
      case 8: t = source_.eval(sc); break;
      default: break;
    }
    if (noise_ > 0.0f) t += noise_ * perlinNoise(u / noiseSize_ * 4.0f, v / noiseSize_ * 4.0f, 0.71f);
    t = clampf(t, 0.0f, 1.0f);
    if (keys_.empty()) return Vec3(0.0f);
    if (t <= keys_.front().pos) return out_.apply(keys_.front().c.eval(sc));
    for (size_t i = 0; i + 1 < keys_.size(); ++i) {
      const Key &a = keys_[i], &b = keys_[i + 1];
      if (t <= b.pos) {
        if (interp_ == 2) return out_.apply(a.c.eval(sc));
        float f = (t - a.pos) / std::max(b.pos - a.pos, 1e-6f);
        if (interp_ == 1) f = smooth01(f);
        return out_.apply(lerp(a.c.eval(sc), b.c.eval(sc), f));
      }
    }
    return out_.apply(keys_.back().c.eval(sc));
  }

 private:
  struct Key {
    float pos;
    ColorInput c;
  };
  std::vector<Key> keys_;
  FloatInput source_;
  Coords2D coords_;
  OutputParams out_;
  int type_ = 0, interp_ = 0;
  float hMin_ = 0, hMax_ = 100, noise_ = 0, noiseSize_ = 1;
};

class TilesMap : public MapNode {
 public:
  TilesMap(const TexMap& m, Ctx& ctx) : tile_(ctx.color(m, "tileColor")), grout_(ctx.color(m, "groutColor")) {
    const ParamBlock& p = m.params;
    coords_.read(p);
    out_.read(p);
    pattern_ = p.getInt("pattern");
    nh_ = std::max(p.getInt("tilesH"), 1);
    nv_ = std::max(p.getInt("tilesV"), 1);
    colorVar_ = p.getFloat("colorVariance");
    fadeVar_ = p.getFloat("fadeVariance");
    gh_ = p.getFloat("groutH") * 0.01f;
    gv_ = p.getFloat("groutV") * 0.01f;
    shift_ = p.getFloat("rowShift");
    randShift_ = p.getFloat("randomShift");
    seed_ = static_cast<uint32_t>(p.getInt("seed"));
  }
  Vec3 color(const ShadeContext& sc) const override {
    Vec2 uv;
    float scale;
    coords_.apply(sc, uv, scale);
    float y = uv.y * nv_;
    int row = static_cast<int>(std::floor(y));
    float fy = y - row;
    int across = nh_;
    float rowShift = 0.0f;
    switch (pattern_) {
      case 1: rowShift = 0.0f; break;                                   // stack bond
      case 2: rowShift = (row & 1) ? 0.5f : 0.0f; break;                // half running bond
      case 3: rowShift = (row & 1) ? 0.25f : 0.0f; across = (row & 1) ? nh_ : nh_ * 2; break;  // Flemish-like
      case 4: across = (row & 1) ? nh_ * 2 : nh_; rowShift = (row & 1) ? 0.25f : 0.0f; break;  // English bond
      case 5: rowShift = std::fmod(row * 0.25f, 1.0f); break;          // fine running bond
      default: rowShift = (row & 1) ? shift_ : 0.0f; break;           // running bond
    }
    if (randShift_ > 0.0f) rowShift += randShift_ * hashFloat(static_cast<uint32_t>(row) * 9781u + seed_);
    float x = uv.x * across + rowShift;
    int col = static_cast<int>(std::floor(x));
    float fx = x - col;
    float gx = gh_ * across * 0.5f, gy = gv_ * nv_ * 0.5f;
    bool groutHit = fx < gx || fx > 1.0f - gx || fy < gy || fy > 1.0f - gy;
    if (groutHit) return out_.apply(grout_.eval(sc));
    uint32_t id = hashU32(static_cast<uint32_t>(col) * 73856093u ^ static_cast<uint32_t>(row) * 19349663u ^ seed_);
    Vec3 c = tile_.eval(sc);
    if (colorVar_ > 0.0f) {
      Vec3 hsv = rgbToHsv(c);
      hsv.x += (hashFloat(id) - 0.5f) * colorVar_ * 0.3f;
      hsv.y = clampf(hsv.y * (1.0f + (hashFloat(id + 1) - 0.5f) * colorVar_), 0.0f, 1.0f);
      c = hsvToRgb(hsv);
    }
    if (fadeVar_ > 0.0f) c *= 1.0f - fadeVar_ * hashFloat(id + 2);
    return out_.apply(c);
  }

 private:
  ColorInput tile_, grout_;
  Coords2D coords_;
  OutputParams out_;
  int pattern_ = 0, nh_ = 4, nv_ = 8;
  float colorVar_ = 0, fadeVar_ = 0, gh_ = 0.02f, gv_ = 0.02f, shift_ = 0.5f, randShift_ = 0;
  uint32_t seed_ = 1;
};

class NoiseMap : public MapNode {
 public:
  NoiseMap(const TexMap& m, Ctx& ctx) : c1_(ctx.color(m, "color1")), c2_(ctx.color(m, "color2")) {
    const ParamBlock& p = m.params;
    coords_.read(p);
    out_.read(p);
    type_ = p.getInt("noiseType");
    size_ = std::max(p.getFloat("size"), 1e-4f);
    levels_ = p.getFloat("levels");
    phase_ = p.getFloat("phase");
    lo_ = p.getFloat("lowThreshold");
    hi_ = std::max(p.getFloat("highThreshold"), lo_ + 1e-4f);
  }
  float noise(const ShadeContext& sc) const {
    Vec3 q = coords_.apply(sc) / size_ + Vec3(0.0f, 0.0f, phase_ * 0.37f);
    float n = fractalNoise(q, type_, levels_);
    return clampf((n - lo_) / (hi_ - lo_), 0.0f, 1.0f);
  }
  Vec3 color(const ShadeContext& sc) const override { return out_.apply(lerp(c1_.eval(sc), c2_.eval(sc), noise(sc))); }
  float value(const ShadeContext& sc) const override {
    if (!c1_.mapped() && !c2_.mapped() && !out_.active) {
      Vec3 a = c1_.value, b = c2_.value;
      float va = (a.x + a.y + a.z) / 3.0f, vb = (b.x + b.y + b.z) / 3.0f;
      return va + (vb - va) * noise(sc);
    }
    return MapNode::value(sc);
  }

 private:
  ColorInput c1_, c2_;
  Coords3D coords_;
  OutputParams out_;
  int type_ = 0;
  float size_ = 25, levels_ = 3, phase_ = 0, lo_ = 0, hi_ = 1;
};

class CellularMap : public MapNode {
 public:
  CellularMap(const TexMap& m, Ctx& ctx)
      : cell_(ctx.color(m, "cellColor")), d1_(ctx.color(m, "divColor1")), d2_(ctx.color(m, "divColor2")) {
    const ParamBlock& p = m.params;
    coords_.read(p);
    out_.read(p);
    chips_ = p.getInt("cellType") == 1;
    size_ = std::max(p.getFloat("size"), 1e-4f);
    spread_ = std::max(p.getFloat("spread"), 1e-3f);
    var_ = p.getFloat("colorVariance");
    fractal_ = p.getBool("fractal");
    iterations_ = p.getFloat("iterations");
    rough_ = p.getFloat("roughness");
  }
  static void worley(const Vec3& q, float& f1, float& f2, uint32_t& id) {
    int cx = static_cast<int>(std::floor(q.x)), cy = static_cast<int>(std::floor(q.y)), cz = static_cast<int>(std::floor(q.z));
    f1 = f2 = 1e9f;
    id = 0;
    for (int dz = -1; dz <= 1; ++dz)
      for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
          uint32_t h = hashU32(static_cast<uint32_t>(cx + dx) * 73856093u ^ static_cast<uint32_t>(cy + dy) * 19349663u ^
                               static_cast<uint32_t>(cz + dz) * 83492791u);
          Vec3 fp(cx + dx + hashFloat(h), cy + dy + hashFloat(h + 1), cz + dz + hashFloat(h + 2));
          float d = length(fp - q);
          if (d < f1) {
            f2 = f1;
            f1 = d;
            id = h;
          } else if (d < f2) {
            f2 = d;
          }
        }
  }
  Vec3 color(const ShadeContext& sc) const override {
    Vec3 q = coords_.apply(sc) / size_;
    float f1, f2;
    uint32_t id;
    worley(q, f1, f2, id);
    float edge = chips_ ? (f2 - f1) : f1;
    if (fractal_) {
      float amp = 0.5f, freq = 2.0f;
      for (int i = 1; i < static_cast<int>(iterations_); ++i) {
        float a, b;
        uint32_t tmp;
        worley(q * freq, a, b, tmp);
        edge += (chips_ ? (b - a) : a) * amp * (0.5f + rough_);
        amp *= 0.5f;
        freq *= 2.0f;
      }
    }
    Vec3 cellC = cell_.eval(sc);
    if (var_ > 0.0f) {
      Vec3 hsv = rgbToHsv(cellC);
      hsv.z *= 1.0f - var_ * hashFloat(id + 7);
      hsv.x += (hashFloat(id + 8) - 0.5f) * var_ * 0.2f;
      cellC = hsvToRgb(hsv);
    }
    float t;
    if (chips_) {
      t = clampf(edge / spread_, 0.0f, 1.0f);  // 0 at the borders, 1 inside a chip
      if (t >= 1.0f) return out_.apply(cellC);
      return out_.apply(t < 0.5f ? lerp(d2_.eval(sc), d1_.eval(sc), t * 2.0f) : lerp(d1_.eval(sc), cellC, t * 2.0f - 1.0f));
    }
    t = clampf(edge / (spread_ * 1.5f), 0.0f, 1.0f);  // distance from the feature point
    if (t < 0.5f) return out_.apply(lerp(cellC, d1_.eval(sc), t * 2.0f));
    return out_.apply(lerp(d1_.eval(sc), d2_.eval(sc), t * 2.0f - 1.0f));
  }

 private:
  ColorInput cell_, d1_, d2_;
  Coords3D coords_;
  OutputParams out_;
  bool chips_ = false, fractal_ = false;
  float size_ = 5, spread_ = 0.5f, var_ = 0, iterations_ = 3, rough_ = 0;
};

class FalloffMap : public MapNode {
 public:
  FalloffMap(const TexMap& m, Ctx& ctx) : front_(ctx.color(m, "frontColor")), side_(ctx.color(m, "sideColor")) {
    const ParamBlock& p = m.params;
    type_ = p.getInt("falloffType");
    dir_ = p.getInt("direction");
    ior_ = std::max(p.getFloat("ior"), 1.0001f);
    near_ = p.getFloat("nearDistance");
    far_ = p.getFloat("farDistance");
    curve_ = std::max(p.getFloat("curve"), 0.01f);
  }
  float factor(const ShadeContext& sc) const {
    Vec3 v = dir_ == 1 ? sc.objZ : (dir_ == 2 ? Vec3(0, 0, 1) : sc.wo);
    Vec3 n = sc.outside ? sc.ns : -sc.ns;
    float c = dot(n, v);
    float t = 0.0f;
    switch (type_) {
      case 0: t = 0.5f * (1.0f - c); break;
      case 1: t = 1.0f - std::fabs(c); break;
      case 2: t = fresnelDielectric(std::fabs(dot(n, sc.wo)), ior_); break;
      case 3: t = 0.5f * (1.0f - dot(n, sc.lightDir)); break;
      case 4: t = (sc.cameraDistance - near_) / std::max(far_ - near_, 1e-4f); break;
      default: break;
    }
    t = clampf(t, 0.0f, 1.0f);
    return curve_ != 1.0f ? std::pow(t, curve_) : t;
  }
  Vec3 color(const ShadeContext& sc) const override { return lerp(front_.eval(sc), side_.eval(sc), factor(sc)); }

 private:
  ColorInput front_, side_;
  int type_ = 1, dir_ = 0;
  float ior_ = 1.6f, near_ = 0, far_ = 1000, curve_ = 1;
};

class MixMap : public MapNode {
 public:
  MixMap(const TexMap& m, Ctx& ctx) : c1_(ctx.color(m, "color1")), c2_(ctx.color(m, "color2")), amt_(ctx.fl(m, "mixAmount")) {
    useCurve_ = m.params.getBool("useCurve");
    lo_ = m.params.getFloat("curveLower");
    hi_ = m.params.getFloat("curveUpper");
  }
  Vec3 color(const ShadeContext& sc) const override {
    float t = clampf(amt_.eval(sc), 0.0f, 1.0f);
    if (useCurve_) t = hi_ > lo_ ? smooth01((t - lo_) / (hi_ - lo_)) : (t >= lo_ ? 1.0f : 0.0f);
    return lerp(c1_.eval(sc), c2_.eval(sc), t);
  }

 private:
  ColorInput c1_, c2_;
  FloatInput amt_;
  bool useCurve_ = false;
  float lo_ = 0.3f, hi_ = 0.7f;
};

Vec3 blendMode(int mode, const Vec3& a, const Vec3& b) {
  auto per = [&](auto f) { return Vec3(f(a.x, b.x), f(a.y, b.y), f(a.z, b.z)); };
  switch (mode) {
    case 0: return b;                                                          // Normal
    case 1: return (a + b) * 0.5f;                                             // Average
    case 2: return a + b;                                                      // Add
    case 3: return vmax(a - b, Vec3(0.0f));                                    // Subtract
    case 4: return vmin(a, b);                                                 // Darken
    case 5: return a * b;                                                      // Multiply
    case 6: return per([](float x, float y) { return y <= 0.0f ? 0.0f : std::max(0.0f, 1.0f - (1.0f - x) / y); });
    case 7: return vmax(a + b - Vec3(1.0f), Vec3(0.0f));                       // Linear burn
    case 8: return vmax(a, b);                                                 // Lighten
    case 9: return Vec3(1.0f) - (Vec3(1.0f) - a) * (Vec3(1.0f) - b);           // Screen
    case 10: return per([](float x, float y) { return y >= 1.0f ? 1.0f : std::min(1.0f, x / (1.0f - y)); });
    case 11: return a + b;                                                     // Linear dodge
    case 12: return per([](float x, float y) { return x < 0.5f ? 2 * x * y : 1 - 2 * (1 - x) * (1 - y); });
    case 13: return per([](float x, float y) { return (1 - 2 * y) * x * x + 2 * y * x; });
    case 14: return per([](float x, float y) { return y < 0.5f ? 2 * x * y : 1 - 2 * (1 - x) * (1 - y); });
    case 15: return vabs(a - b);                                               // Difference
    case 16: return a + b - a * b * 2.0f;                                      // Exclusion
    case 17: {
      Vec3 ha = rgbToHsv(a), hb = rgbToHsv(b);
      return hsvToRgb(Vec3(hb.x, ha.y, ha.z));
    }
    case 18: {
      Vec3 ha = rgbToHsv(a), hb = rgbToHsv(b);
      return hsvToRgb(Vec3(ha.x, hb.y, ha.z));
    }
    case 19: {
      Vec3 ha = rgbToHsv(a), hb = rgbToHsv(b);
      return hsvToRgb(Vec3(hb.x, hb.y, ha.z));
    }
    case 20: {
      Vec3 ha = rgbToHsv(a), hb = rgbToHsv(b);
      return hsvToRgb(Vec3(ha.x, ha.y, hb.z));
    }
    default: return b;
  }
}

class CompositeMap : public MapNode {
 public:
  CompositeMap(const TexMap& m, Ctx& ctx) {
    int n = m.params.getInt("layerCount");
    for (int i = 1; i <= n; ++i) {
      std::string k = "layer" + std::to_string(i);
      if (!m.params.getBool(k + "Enabled")) continue;
      layers_.push_back({ctx.color(m, k + "Color"), ctx.fl(m, k + "Mask"), m.params.getInt(k + "Mode"),
                         m.params.getFloat(k + "Opacity") * 0.01f});
    }
  }
  Vec3 color(const ShadeContext& sc) const override {
    Vec3 r(0.0f);
    bool first = true;
    for (const auto& L : layers_) {
      Vec3 c = L.color.eval(sc);
      float a = clampf(L.mask.eval(sc), 0.0f, 1.0f) * L.opacity;
      if (first) {
        r = c * a;
        first = false;
        continue;
      }
      r = lerp(r, blendMode(L.mode, r, c), a);
    }
    return r;
  }

 private:
  struct Layer {
    ColorInput color;
    FloatInput mask;
    int mode;
    float opacity;
  };
  std::vector<Layer> layers_;
};

class ColorCorrectionMap : public MapNode {
 public:
  ColorCorrectionMap(const TexMap& m, Ctx& ctx) : in_(ctx.color(m, "color")) {
    const ParamBlock& p = m.params;
    hue_ = p.getFloat("hueShift") / 360.0f;
    sat_ = p.getFloat("saturation") * 0.01f;
    tint_ = p.getColor("tint");
    tintStrength_ = p.getFloat("tintStrength") * 0.01f;
    bright_ = p.getFloat("brightness") * 0.01f;
    contrast_ = p.getFloat("contrast") * 0.01f;
    gamma_ = std::max(p.getFloat("gamma"), 0.01f);
    exposure_ = std::pow(2.0f, p.getFloat("exposure"));
    invert_ = p.getBool("invert");
  }
  Vec3 color(const ShadeContext& sc) const override {
    Vec3 c = in_.eval(sc) * exposure_;
    if (hue_ != 0.0f || sat_ != 0.0f) {
      Vec3 hsv = rgbToHsv(c);
      hsv.x += hue_;
      hsv.y = clampf(hsv.y * (1.0f + sat_), 0.0f, 1.0f);
      c = hsvToRgb(hsv);
    }
    if (tintStrength_ > 0.0f) c = lerp(c, tint_ * luminance(c), tintStrength_);
    c += Vec3(bright_);
    if (contrast_ != 0.0f) c = (c - Vec3(0.5f)) * (1.0f + contrast_) + Vec3(0.5f);
    c = vmax(c, Vec3(0.0f));
    if (gamma_ != 1.0f) c = Vec3(std::pow(c.x, 1.0f / gamma_), std::pow(c.y, 1.0f / gamma_), std::pow(c.z, 1.0f / gamma_));
    if (invert_) c = Vec3(1.0f) - c;
    return c;
  }

 private:
  ColorInput in_;
  float hue_ = 0, sat_ = 0, tintStrength_ = 0, bright_ = 0, contrast_ = 0, gamma_ = 1, exposure_ = 1;
  Vec3 tint_{1, 1, 1};
  bool invert_ = false;
};

class OutputMap : public MapNode {
 public:
  OutputMap(const TexMap& m, Ctx& ctx) : in_(ctx.color(m, "color")) { out_.read(m.params); }
  Vec3 color(const ShadeContext& sc) const override { return out_.apply(in_.eval(sc)); }

 private:
  ColorInput in_;
  OutputParams out_;
};

class AmbientOcclusionMap : public MapNode {
 public:
  AmbientOcclusionMap(const TexMap& m, Ctx& ctx)
      : unocc_(ctx.color(m, "unoccludedColor")), occ_(ctx.color(m, "occludedColor")), radius_(ctx.fl(m, "radius")) {
    const ParamBlock& p = m.params;
    dist_ = p.getFloat("distribution");
    falloff_ = p.getFloat("falloff");
    samples_ = std::max(p.getInt("samples"), 1);
    mode_ = p.getInt("mode");
    sameOnly_ = p.getBool("onlySameObject");
    ignoreSelf_ = p.getBool("ignoreSelf");
    ctx.c.needsTracer = true;
  }
  Vec3 color(const ShadeContext& sc) const override {
    if (!sc.tracer || !sc.rng) return unocc_.eval(sc);
    float r = radius_.eval(sc);
    if (r <= 0.0f) return unocc_.eval(sc);
    Vec3 n = sc.outside ? sc.ns : -sc.ns;
    Vec3 axis = n;
    if (mode_ == 1) axis = normalize(n * (2.0f * dot(n, sc.wo)) - sc.wo);
    if (mode_ == 2) axis = -n;
    Frame f(axis);
    int count = sc.depth == 0 ? samples_ : std::min(samples_, 2);
    float unocc = 0.0f;
    Rng& rng = *sc.rng;
    for (int i = 0; i < count; ++i) {
      float u1 = rng.next(), u2 = rng.next();
      // cos^distribution lobe around the axis.
      float cosT = std::pow(u1, 1.0f / (dist_ + 1.0f));
      float sinT = std::sqrt(std::max(0.0f, 1.0f - cosT * cosT));
      float phi = kTwoPi * u2;
      Vec3 d = f.toWorld(Vec3(std::cos(phi) * sinT, std::sin(phi) * sinT, cosT));
      Vec3 o = offsetRayOrigin(sc.p, dot(d, sc.ng) > 0.0f ? sc.ng : -sc.ng);
      uint32_t hitNode = 0;
      float t = sc.tracer->traceDistance(o, d, r, &hitNode);
      bool hit = hitNode != 0 && t < r;
      if (hit && sameOnly_ && hitNode != sc.nodeId) hit = false;
      if (hit && ignoreSelf_ && hitNode == sc.nodeId) hit = false;
      if (!hit) {
        unocc += 1.0f;
      } else if (falloff_ > 0.0f) {
        unocc += std::pow(t / r, 1.0f / std::max(falloff_, 1e-3f)) * falloff_;
      }
    }
    return lerp(occ_.eval(sc), unocc_.eval(sc), unocc / count);
  }

 private:
  ColorInput unocc_, occ_;
  FloatInput radius_;
  float dist_ = 1, falloff_ = 0;
  int samples_ = 8, mode_ = 0;
  bool sameOnly_ = false, ignoreSelf_ = false;
};

class NormalBumpMap : public MapNode {
 public:
  NormalBumpMap(const TexMap& m, Ctx& ctx) : normal_(ctx.color(m, "normal")) {
    const ParamBlock& p = m.params;
    amount_ = p.getFloat("normalAmount");
    method_ = p.getInt("method");
    flipR_ = p.getBool("flipRed");
    flipG_ = p.getBool("flipGreen");
    swap_ = p.getBool("swapRG");
    if (const TexMap* b = m.mapFor("bump")) bump_ = compileMap(b, ctx.c);
    bumpAmount_ = p.getFloat("bumpAmount");
  }
  bool isNormalMap() const override { return true; }
  Vec3 color(const ShadeContext& sc) const override { return normal_.eval(sc); }
  Vec3 perturbNormal(const ShadeContext& sc) const override {
    Vec3 c = normal_.eval(sc);
    // Normal maps hold linear data; an sRGB-decoded Bitmap is re-encoded here.
    Vec3 t(c.x * 2.0f - 1.0f, c.y * 2.0f - 1.0f, c.z * 2.0f - 1.0f);
    if (normal_.map) {
      // Bitmaps decode 8-bit sRGB by default: undo it so (128,128,255) maps to (0,0,1).
      t = Vec3(linearToSrgb(c.x) * 2.0f - 1.0f, linearToSrgb(c.y) * 2.0f - 1.0f, linearToSrgb(c.z) * 2.0f - 1.0f);
    }
    if (swap_) std::swap(t.x, t.y);
    if (flipR_) t.x = -t.x;
    if (flipG_) t.y = -t.y;
    t.x *= amount_;
    t.y *= amount_;
    Vec3 n = sc.ns;
    Vec3 out;
    if (method_ == 0) {
      Vec3 T = sc.dpdu - n * dot(n, sc.dpdu);
      if (length2(T) < 1e-20f) {
        Vec3 b;
        makeBasis(n, T, b);
      }
      T = normalize(T);
      Vec3 B = cross(n, T);
      if (dot(B, sc.dpdv) < 0.0f) B = -B;
      out = T * t.x + B * t.y + n * std::max(t.z, 1e-3f);
    } else {
      out = t;  // object/world space normal (object rotation is not available: treated as world)
    }
    if (length2(out) < 1e-20f) return n;
    out = normalize(out);
    if (dot(out, sc.ng) < 0.0f && dot(n, sc.ng) > 0.0f) out = normalize(out - sc.ng * (dot(out, sc.ng) * 1.01f));
    return out;
  }
  const MapNode* extraBump() const { return bump_.get(); }
  float extraBumpAmount() const { return bumpAmount_; }

 private:
  ColorInput normal_;
  float amount_ = 1;
  int method_ = 0;
  bool flipR_ = false, flipG_ = false, swap_ = false;
  MapPtr bump_;
  float bumpAmount_ = 1;
};

class TriplanarMap : public MapNode {
 public:
  TriplanarMap(const TexMap& m, Ctx& ctx) {
    const ParamBlock& p = m.params;
    tx_ = ctx.color(m, "texture");
    same_ = p.getBool("sameOnAll");
    ty_ = same_ ? tx_ : ctx.color(m, "textureY");
    tz_ = same_ ? tx_ : ctx.color(m, "textureZ");
    size_ = std::max(p.getFloat("size"), 1e-3f);
    blend_ = p.getFloat("blend");
    world_ = p.getInt("space") == 1;
    randOffset_ = p.getBool("randomOffset");
    randRot_ = p.getBool("randomRotation");
    seed_ = static_cast<uint32_t>(p.getInt("seed"));
  }
  Vec3 color(const ShadeContext& sc) const override {
    Vec3 q = world_ ? sc.p : sc.pObj;
    Vec3 n = world_ ? sc.ns : sc.nObj;
    if (length2(n) < 1e-12f) n = sc.ns;
    Vec3 w = vabs(normalize(n));
    float k = 1.0f / std::max(blend_ * 0.5f, 0.02f);
    w = Vec3(std::pow(w.x, k), std::pow(w.y, k), std::pow(w.z, k));
    float s = w.x + w.y + w.z;
    w = s > 0.0f ? w / s : Vec3(0, 0, 1);
    uint32_t h = hashU32(sc.nodeId * 2654435761u + seed_);
    Vec2 off = randOffset_ ? Vec2(hashFloat(h), hashFloat(h + 1)) : Vec2();
    float ang = randRot_ ? hashFloat(h + 2) * kTwoPi : 0.0f;
    float ca = std::cos(ang), sa = std::sin(ang);
    auto project = [&](float a, float b) {
      float u = a / size_, v = b / size_;
      return Vec2(u * ca - v * sa + off.x, u * sa + v * ca + off.y);
    };
    Vec3 c(0.0f);
    ShadeContext s2 = sc;
    s2.footprint = sc.footprint;
    if (w.x > 1e-4f) {
      s2.uv = project(q.y, q.z);
      c += tx_.eval(s2) * w.x;
    }
    if (w.y > 1e-4f) {
      s2.uv = project(q.x, q.z);
      c += ty_.eval(s2) * w.y;
    }
    if (w.z > 1e-4f) {
      s2.uv = project(q.x, q.y);
      c += tz_.eval(s2) * w.z;
    }
    return c;
  }

 private:
  ColorInput tx_, ty_, tz_;
  bool same_ = true, world_ = false, randOffset_ = false, randRot_ = false;
  float size_ = 100, blend_ = 0.2f;
  uint32_t seed_ = 0;
};

class EdgesMap : public MapNode {
 public:
  EdgesMap(const TexMap& m, Ctx& ctx) : edge_(ctx.color(m, "edgeColor")), face_(ctx.color(m, "faceColor")) {
    thickness_ = m.params.getFloat("thickness");
    hidden_ = m.params.getBool("hiddenEdges");
    ctx.c.needsEdges = true;
  }
  Vec3 color(const ShadeContext& sc) const override {
    if (!sc.tracer || sc.geom < 0) return face_.eval(sc);
    Vec3 v[3];
    unsigned mask = 0;
    if (!sc.tracer->triangle(sc.geom, sc.prim, v, mask)) return face_.eval(sc);
    if (hidden_) mask = 7u;
    float b[3] = {1.0f - sc.bary.x - sc.bary.y, sc.bary.x, sc.bary.y};
    float area2 = length(cross(v[1] - v[0], v[2] - v[0]));
    float best = 1e30f;
    for (int i = 0; i < 3; ++i) {
      if (!(mask & (1u << i))) continue;
      // Edge i joins corners i and i+1; the opposite corner is i+2.
      int opp = (i + 2) % 3;
      float len = length(v[(i + 1) % 3] - v[i]);
      if (len <= 0.0f) continue;
      best = std::min(best, b[opp] * area2 / len);
    }
    float half = thickness_ * 0.5f;
    float t = best >= 1e29f ? 1.0f : smooth01((best - half * 0.8f) / std::max(half * 0.4f, 1e-5f));
    return lerp(edge_.eval(sc), face_.eval(sc), t);
  }

 private:
  ColorInput edge_, face_;
  float thickness_ = 0.5f;
  bool hidden_ = false;
};

class ColorVariationMap : public MapNode {
 public:
  ColorVariationMap(const TexMap& m, Ctx& ctx) {
    const ParamBlock& p = m.params;
    mode_ = p.getInt("mode");
    int n = p.getInt("colorCount");
    for (int i = 1; i <= n; ++i) colors_.push_back(ctx.color(m, "color" + std::to_string(i)));
    hueVar_ = p.getFloat("hueVariation") / 360.0f;
    satVar_ = p.getFloat("satVariation");
    valVar_ = p.getFloat("valueVariation");
    seed_ = static_cast<uint32_t>(p.getInt("seed"));
    if (mode_ == 1) ctx.c.needsElements = true;
  }
  Vec3 color(const ShadeContext& sc) const override {
    uint32_t key;
    switch (mode_) {
      case 1: key = sc.nodeId * 7919u + static_cast<uint32_t>(sc.elementId) * 104729u; break;
      case 2: key = static_cast<uint32_t>(sc.faceMatId) * 15485863u; break;
      case 3: key = sc.instanceId * 32452843u + sc.nodeId; break;
      default: key = sc.nodeId * 2654435761u; break;
    }
    uint32_t h = hashU32(key ^ (seed_ * 0x9E3779B9u));
    if (colors_.empty()) return Vec3(0.5f);
    Vec3 c = colors_[h % colors_.size()].eval(sc);
    if (hueVar_ > 0.0f || satVar_ > 0.0f || valVar_ > 0.0f) {
      Vec3 hsv = rgbToHsv(c);
      hsv.x += (hashFloat(h + 11) * 2.0f - 1.0f) * hueVar_;
      hsv.y = clampf(hsv.y * (1.0f + (hashFloat(h + 12) * 2.0f - 1.0f) * satVar_), 0.0f, 1.0f);
      hsv.z = std::max(0.0f, hsv.z * (1.0f + (hashFloat(h + 13) * 2.0f - 1.0f) * valVar_));
      c = hsvToRgb(hsv);
    }
    return c;
  }

 private:
  int mode_ = 0;
  std::vector<ColorInput> colors_;
  float hueVar_ = 0, satVar_ = 0, valVar_ = 0;
  uint32_t seed_ = 0;
};

class DistanceMap : public MapNode {
 public:
  DistanceMap(const TexMap& m, Ctx& ctx) : near_(ctx.color(m, "nearColor")), far_(ctx.color(m, "farColor")) {
    dist_ = std::max(m.params.getFloat("distance"), 1e-4f);
    std::vector<std::string> names;
    std::string list = m.params.getString("objects");
    size_t start = 0;
    while (start <= list.size()) {
      size_t comma = list.find(',', start);
      std::string n = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
      while (!n.empty() && std::isspace(static_cast<unsigned char>(n.front()))) n.erase(n.begin());
      while (!n.empty() && std::isspace(static_cast<unsigned char>(n.back()))) n.pop_back();
      if (!n.empty()) names.push_back(n);
      if (comma == std::string::npos) break;
      start = comma + 1;
    }
    if (!names.empty()) {
      field_ = static_cast<int>(ctx.c.distanceFields.size());
      ctx.c.distanceFields.push_back(names);
    }
  }
  Vec3 color(const ShadeContext& sc) const override {
    if (field_ < 0 || !sc.tracer) return far_.eval(sc);
    float d = sc.tracer->distanceTo(field_, sc.p, dist_);
    return lerp(near_.eval(sc), far_.eval(sc), clampf(d / dist_, 0.0f, 1.0f));
  }

 private:
  ColorInput near_, far_;
  float dist_ = 50;
  int field_ = -1;
};

}  // namespace

// ---------------------------------------------------------------------------------------------
// Compilation

MapPtr compileMap(const TexMap* map, MapCompileContext& c) {
  if (!map) return nullptr;
  Ctx ctx{c};
  const std::string& t = map->type;
  if (t == "Bitmap") return std::make_shared<BitmapMap>(*map, ctx);
  if (t == "Checker") return std::make_shared<CheckerMap>(*map, ctx);
  if (t == "Gradient") return std::make_shared<GradientMap>(*map, ctx);
  if (t == "GradientRamp") return std::make_shared<GradientRampMap>(*map, ctx);
  if (t == "Tiles") return std::make_shared<TilesMap>(*map, ctx);
  if (t == "Noise") return std::make_shared<NoiseMap>(*map, ctx);
  if (t == "Cellular") return std::make_shared<CellularMap>(*map, ctx);
  if (t == "Falloff") return std::make_shared<FalloffMap>(*map, ctx);
  if (t == "Mix") return std::make_shared<MixMap>(*map, ctx);
  if (t == "Composite") return std::make_shared<CompositeMap>(*map, ctx);
  if (t == "ColorCorrection") return std::make_shared<ColorCorrectionMap>(*map, ctx);
  if (t == "Output") return std::make_shared<OutputMap>(*map, ctx);
  if (t == "AmbientOcclusion") return std::make_shared<AmbientOcclusionMap>(*map, ctx);
  if (t == "NormalBump") return std::make_shared<NormalBumpMap>(*map, ctx);
  if (t == "Triplanar") return std::make_shared<TriplanarMap>(*map, ctx);
  if (t == "Edges") return std::make_shared<EdgesMap>(*map, ctx);
  if (t == "ColorVariation") return std::make_shared<ColorVariationMap>(*map, ctx);
  if (t == "Distance") return std::make_shared<DistanceMap>(*map, ctx);
  if (c.warnings) c.warnings->push_back("Unsupported map type: " + t);
  return std::make_shared<ConstantMap>(Vec3(0.5f));
}

ColorInput colorInput(const ParamBlock& p, const MapSlots& maps, const std::string& key, MapCompileContext& ctx) {
  ColorInput in;
  const ParamDef* d = p.def(key);
  if (d && d->type == ParamType::Color) in.value = p.getColor(key);
  else if (d) in.value = Vec3(p.getFloat(key));
  auto it = maps.find(key);
  if (it != maps.end() && it->second.active()) {
    in.map = compileMap(it->second.map.get(), ctx);
    in.amount = clampf(it->second.amount, 0.0f, 1.0f);
  }
  return in;
}

FloatInput floatInput(const ParamBlock& p, const MapSlots& maps, const std::string& key, MapCompileContext& ctx) {
  FloatInput in;
  const ParamDef* d = p.def(key);
  if (d && d->type == ParamType::Color) in.value = luminance(p.getColor(key));
  else if (d) in.value = p.getFloat(key);
  auto it = maps.find(key);
  if (it != maps.end() && it->second.active()) {
    in.map = compileMap(it->second.map.get(), ctx);
    in.amount = clampf(it->second.amount, 0.0f, 1.0f);
  }
  return in;
}

MapPtr slotMap(const MapSlots& maps, const std::string& key, MapCompileContext& ctx, float* amount) {
  auto it = maps.find(key);
  if (it == maps.end() || !it->second.active()) return nullptr;
  if (amount) *amount = it->second.amount;
  return compileMap(it->second.map.get(), ctx);
}

std::vector<uint8_t> renderMapSwatch(const TexMap& map, const Scene* sceneForPaths, int size) {
  size = std::max(size, 4);
  std::vector<uint8_t> out(static_cast<size_t>(size) * size * 4, 255);
  TextureCache cache;
  MapCompileContext ctx;
  ctx.scene = sceneForPaths;
  ctx.cache = &cache;
  MapPtr m = compileMap(&map, ctx);
  if (!m) return out;
  Rng rng(1, 1);
  for (int y = 0; y < size; ++y)
    for (int x = 0; x < size; ++x) {
      ShadeContext sc;
      float u = (x + 0.5f) / size, v = 1.0f - (y + 0.5f) / size;
      sc.uv = Vec2(u, v);
      // A 100 x 100 cm patch facing the viewer for 3D maps.
      sc.p = sc.pObj = Vec3(u * 100.0f, v * 100.0f, 0.0f);
      sc.ng = sc.ns = sc.nObj = Vec3(0, 0, 1);
      // Bend the normal like a sphere so Falloff-type maps show their range.
      float cx = u * 2.0f - 1.0f, cy = v * 2.0f - 1.0f, r2 = cx * cx + cy * cy;
      if (r2 < 1.0f) sc.ns = normalize(Vec3(cx, cy, std::sqrt(1.0f - r2)));
      sc.wo = Vec3(0, 0, 1);
      sc.dpdu = Vec3(100, 0, 0);
      sc.dpdv = Vec3(0, 100, 0);
      sc.footprint = 1.0f / size;
      sc.rng = &rng;
      Vec3 c = m->color(sc);
      uint8_t* px = &out[(static_cast<size_t>(y) * size + x) * 4];
      px[0] = static_cast<uint8_t>(std::lround(linearToSrgb(c.x) * 255.0f));
      px[1] = static_cast<uint8_t>(std::lround(linearToSrgb(c.y) * 255.0f));
      px[2] = static_cast<uint8_t>(std::lround(linearToSrgb(c.z) * 255.0f));
      px[3] = 255;
    }
  return out;
}

}  // namespace render
}  // namespace veraa
