#include "postprocess.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>

#include "common.h"
#include "photometry.h"
#include "texmaps.h"

#ifdef VERAA_HAVE_OIDN
#include <OpenImageDenoise/oidn.hpp>
#endif

namespace veraa {
namespace render {

namespace {

// ACES filmic curve, Stephen Hill's fit of the RRT + sRGB ODT.
Vec3 acesFitted(const Vec3& c) {
  Vec3 v(0.59719f * c.x + 0.35458f * c.y + 0.04823f * c.z, 0.07600f * c.x + 0.90834f * c.y + 0.01566f * c.z,
         0.02840f * c.x + 0.13383f * c.y + 0.83777f * c.z);
  auto rrt = [](float x) { return (x * (x + 0.0245786f) - 0.000090537f) / (x * (0.983729f * x + 0.4329510f) + 0.238081f); };
  v = Vec3(rrt(v.x), rrt(v.y), rrt(v.z));
  return {1.60475f * v.x - 0.53108f * v.y - 0.07367f * v.z, -0.10208f * v.x + 1.10813f * v.y - 0.00605f * v.z,
          -0.00327f * v.x - 0.07276f * v.y + 1.07602f * v.z};
}

// ---- AgX-style view transform ----
// Built from the published description of AgX (T. Sobotka): the scene color is expressed in a
// slightly wider, rotated set of primaries (an "inset" that pulls colors toward the achromatic
// axis), encoded logarithmically around middle grey, passed per channel through a sigmoid and
// brought back with the inverse matrix. Per-channel compression of the inset values gives
// saturated highlights a smooth path to white without the hue skews of plain per-channel curves.
// The primaries, log range and sigmoid below are Veraa's own choices.

struct M3 {
  float m[3][3];
  Vec3 operator*(const Vec3& v) const {
    return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z, m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
            m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z};
  }
};

M3 mul(const M3& a, const M3& b) {
  M3 r{};
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      for (int k = 0; k < 3; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
  return r;
}

M3 inverse(const M3& a) {
  const auto& m = a.m;
  double det = m[0][0] * (static_cast<double>(m[1][1]) * m[2][2] - static_cast<double>(m[1][2]) * m[2][1]) -
               m[0][1] * (static_cast<double>(m[1][0]) * m[2][2] - static_cast<double>(m[1][2]) * m[2][0]) +
               m[0][2] * (static_cast<double>(m[1][0]) * m[2][1] - static_cast<double>(m[1][1]) * m[2][0]);
  double inv = det != 0.0 ? 1.0 / det : 0.0;
  M3 r;
  r.m[0][0] = static_cast<float>((m[1][1] * m[2][2] - m[1][2] * m[2][1]) * inv);
  r.m[0][1] = static_cast<float>((m[0][2] * m[2][1] - m[0][1] * m[2][2]) * inv);
  r.m[0][2] = static_cast<float>((m[0][1] * m[1][2] - m[0][2] * m[1][1]) * inv);
  r.m[1][0] = static_cast<float>((m[1][2] * m[2][0] - m[1][0] * m[2][2]) * inv);
  r.m[1][1] = static_cast<float>((m[0][0] * m[2][2] - m[0][2] * m[2][0]) * inv);
  r.m[1][2] = static_cast<float>((m[0][2] * m[1][0] - m[0][0] * m[1][2]) * inv);
  r.m[2][0] = static_cast<float>((m[1][0] * m[2][1] - m[1][1] * m[2][0]) * inv);
  r.m[2][1] = static_cast<float>((m[0][1] * m[2][0] - m[0][0] * m[2][1]) * inv);
  r.m[2][2] = static_cast<float>((m[0][0] * m[1][1] - m[0][1] * m[1][0]) * inv);
  return r;
}

// RGB -> XYZ for primaries given as xy chromaticities, with white (1,1,1) -> the white point.
M3 primariesToXyz(const Vec2 prim[3], const Vec2& white) {
  M3 P;
  for (int i = 0; i < 3; ++i) {
    P.m[0][i] = prim[i].x / prim[i].y;
    P.m[1][i] = 1.0f;
    P.m[2][i] = (1.0f - prim[i].x - prim[i].y) / prim[i].y;
  }
  Vec3 W(white.x / white.y, 1.0f, (1.0f - white.x - white.y) / white.y);
  Vec3 S = inverse(P) * W;
  for (int r = 0; r < 3; ++r)
    for (int i = 0; i < 3; ++i) P.m[r][i] *= S[i];
  return P;
}

struct AgxMatrices {
  M3 inset, outset;
};

const AgxMatrices& agxMatrices() {
  static const AgxMatrices mats = [] {
    const Vec2 white(0.3127f, 0.3290f);
    const Vec2 rec709[3] = {Vec2(0.64f, 0.33f), Vec2(0.30f, 0.60f), Vec2(0.15f, 0.06f)};
    // Primaries pushed 15% away from white and rotated a little (degrees, counter-clockwise in xy):
    // red toward magenta, green toward cyan and blue toward magenta keep fire, foliage and skies
    // on their hue when they burn out.
    const float expand = 0.15f;
    const float rotDeg[3] = {-1.0f, 2.0f, 1.5f};
    Vec2 wide[3];
    for (int i = 0; i < 3; ++i) {
      Vec2 d = rec709[i] - white;
      float a = radians(rotDeg[i]), cs = std::cos(a), sn = std::sin(a);
      wide[i] = white + Vec2(cs * d.x - sn * d.y, sn * d.x + cs * d.y) * (1.0f + expand);
    }
    AgxMatrices r;
    r.inset = mul(inverse(primariesToXyz(wide, white)), primariesToXyz(rec709, white));
    r.outset = inverse(r.inset);
    return r;
  }();
  return mats;
}

// Log range (stops around middle grey 0.18) and display-encoded sigmoid.
constexpr float kAgxMinEv = -10.0f, kAgxMaxEv = 6.5f;
constexpr float kAgxPivotY = 0.48f, kAgxSlope = 2.6f;

float agxSigmoidRaw(float x) {
  const float x0 = -kAgxMinEv / (kAgxMaxEv - kAgxMinEv);
  float d = kAgxSlope * (x - x0);
  float L = d > 0.0f ? 1.0f - kAgxPivotY : kAgxPivotY;
  float r = std::fabs(d / L);
  return kAgxPivotY + d / std::cbrt(1.0f + r * r * r);
}

float agxCurve(float v) {
  static const float lo = agxSigmoidRaw(0.0f), hi = agxSigmoidRaw(1.0f);
  float x = (std::log2(std::max(v, 1e-10f) / 0.18f) - kAgxMinEv) / (kAgxMaxEv - kAgxMinEv);
  float y = (agxSigmoidRaw(clampf(x, 0.0f, 1.0f)) - lo) / (hi - lo);
  return srgbToLinear(clampf(y, 0.0f, 1.0f));
}

Vec3 agxView(const Vec3& c) {
  const AgxMatrices& M = agxMatrices();
  Vec3 v = M.inset * vmax(c, Vec3(0.0f));
  v = Vec3(agxCurve(v.x), agxCurve(v.y), agxCurve(v.z));
  return clampColor(M.outset * v, 0.0f, 1.0f);
}

// Dark/bright multipliers: the slope blends from `dark` (black) to `bright` (white and above),
// so the mapping stays monotonic for any positive multipliers.
float multiplyCurve(float x, float dark, float bright) {
  if (x <= 0.0f) return 0.0f;
  float s = x < 1.0f ? x * x * x * (1.0f - 0.5f * x) : x - 0.5f;
  return dark * x + (bright - dark) * s;
}

Vec3 scaleTo(const Vec3& c, float from, float to) { return from > 1e-20f ? c * (to / from) : Vec3(0.0f); }

// ---- 3D LUTs (.cube) ----
struct Lut3D {
  int n = 0;
  Vec3 domainMin{0, 0, 0}, domainMax{1, 1, 1};
  std::vector<Vec3> data;  // r fastest
  Vec3 at(int r, int g, int b) const { return data[(static_cast<size_t>(b) * n + g) * n + r]; }
  Vec3 lookup(const Vec3& c) const {
    Vec3 t;
    for (int k = 0; k < 3; ++k)
      t[k] = clampf((c[k] - domainMin[k]) / std::max(domainMax[k] - domainMin[k], 1e-6f), 0.0f, 1.0f) * (n - 1);
    int r0 = std::min(static_cast<int>(t.x), n - 2), g0 = std::min(static_cast<int>(t.y), n - 2),
        b0 = std::min(static_cast<int>(t.z), n - 2);
    float fr = t.x - r0, fg = t.y - g0, fb = t.z - b0;
    Vec3 c00 = lerp(at(r0, g0, b0), at(r0 + 1, g0, b0), fr), c10 = lerp(at(r0, g0 + 1, b0), at(r0 + 1, g0 + 1, b0), fr),
         c01 = lerp(at(r0, g0, b0 + 1), at(r0 + 1, g0, b0 + 1), fr),
         c11 = lerp(at(r0, g0 + 1, b0 + 1), at(r0 + 1, g0 + 1, b0 + 1), fr);
    return lerp(lerp(c00, c10, fg), lerp(c01, c11, fg), fb);
  }
};

std::shared_ptr<const Lut3D> loadLut(const std::string& path) {
  static std::mutex m;
  static std::map<std::string, std::shared_ptr<const Lut3D>> cache;
  std::lock_guard<std::mutex> lock(m);
  auto it = cache.find(path);
  if (it != cache.end()) return it->second;
  auto lut = std::make_shared<Lut3D>();
  std::ifstream f(path);
  std::string line;
  while (f && std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream in(line);
    std::string key;
    in >> key;
    if (key == "LUT_3D_SIZE") {
      in >> lut->n;
      if (lut->n < 2 || lut->n > 256) lut->n = 0;
      else lut->data.reserve(static_cast<size_t>(lut->n) * lut->n * lut->n);
    } else if (key == "DOMAIN_MIN") {
      in >> lut->domainMin.x >> lut->domainMin.y >> lut->domainMin.z;
    } else if (key == "DOMAIN_MAX") {
      in >> lut->domainMax.x >> lut->domainMax.y >> lut->domainMax.z;
    } else if (!key.empty() && (std::isdigit(static_cast<unsigned char>(key[0])) || key[0] == '-' || key[0] == '.')) {
      std::istringstream row(line);
      Vec3 c;
      if (row >> c.x >> c.y >> c.z) lut->data.push_back(c);
    }
  }
  if (lut->n == 0 || lut->data.size() != static_cast<size_t>(lut->n) * lut->n * lut->n) lut.reset();
  cache[path] = lut;
  return lut;
}

// Monotone cubic (Fritsch-Carlson) curve table with 1024 entries.
std::vector<float> curveTable(const std::vector<Vec2>& pts) {
  std::vector<float> table(1024);
  if (pts.size() < 2) {
    for (int i = 0; i < 1024; ++i) table[i] = i / 1023.0f;
    return table;
  }
  size_t n = pts.size();
  std::vector<float> dx(n - 1), dy(n - 1), m(n - 1), t(n);
  for (size_t i = 0; i + 1 < n; ++i) {
    dx[i] = std::max(pts[i + 1].x - pts[i].x, 1e-6f);
    dy[i] = pts[i + 1].y - pts[i].y;
    m[i] = dy[i] / dx[i];
  }
  t[0] = m[0];
  t[n - 1] = m[n - 2];
  for (size_t i = 1; i + 1 < n; ++i) t[i] = (m[i - 1] * m[i] <= 0.0f) ? 0.0f : (m[i - 1] + m[i]) * 0.5f;
  for (size_t i = 0; i + 1 < n; ++i) {
    if (m[i] == 0.0f) {
      t[i] = t[i + 1] = 0.0f;
      continue;
    }
    float a = t[i] / m[i], b = t[i + 1] / m[i];
    float s = a * a + b * b;
    if (s > 9.0f) {
      float k = 3.0f / std::sqrt(s);
      t[i] = k * a * m[i];
      t[i + 1] = k * b * m[i];
    }
  }
  for (int i = 0; i < 1024; ++i) {
    float x = i / 1023.0f;
    float y;
    if (x <= pts.front().x) {
      y = pts.front().y;
    } else if (x >= pts.back().x) {
      y = pts.back().y;
    } else {
      size_t k = 0;
      while (k + 2 < n && x > pts[k + 1].x) ++k;
      float h = dx[k], s = (x - pts[k].x) / h;
      float h00 = 2 * s * s * s - 3 * s * s + 1, h10 = s * s * s - 2 * s * s + s, h01 = -2 * s * s * s + 3 * s * s,
            h11 = s * s * s - s * s;
      y = h00 * pts[k].y + h10 * h * t[k] + h01 * pts[k + 1].y + h11 * h * t[k + 1];
    }
    table[i] = clampf(y, 0.0f, 1.0f);
  }
  return table;
}

// Approximate Gaussian blur of an RGB image with three box blurs per axis.
void boxBlur(std::vector<float>& img, int w, int h, int r) {
  if (r <= 0) return;
  std::vector<float> tmp(img.size());
  auto pass = [&](const std::vector<float>& src, std::vector<float>& dst, bool horizontal) {
    int len = horizontal ? w : h, lines = horizontal ? h : w;
    float inv = 1.0f / (2 * r + 1);
    for (int l = 0; l < lines; ++l) {
      auto idx = [&](int i) {
        i = std::clamp(i, 0, len - 1);
        return horizontal ? (static_cast<size_t>(l) * w + i) * 3 : (static_cast<size_t>(i) * w + l) * 3;
      };
      float acc[3] = {0, 0, 0};
      for (int i = -r; i <= r; ++i)
        for (int c = 0; c < 3; ++c) acc[c] += src[idx(i) + c];
      for (int i = 0; i < len; ++i) {
        size_t o = idx(i);
        for (int c = 0; c < 3; ++c) dst[o + c] = acc[c] * inv;
        size_t add = idx(i + r + 1), rem = idx(i - r);
        for (int c = 0; c < 3; ++c) acc[c] += src[add + c] - src[rem + c];
      }
    }
  };
  for (int k = 0; k < 3; ++k) {
    pass(img, tmp, true);
    pass(tmp, img, false);
  }
}

}  // namespace

ColorMappingParams colorMappingParams(const DisplaySettings& d) {
  ColorMappingParams p;
  p.type = (d.tonemap >= 0 && d.tonemap < kMapCount) ? d.tonemap : kMapFilmicAces;
  p.dark = std::max(d.mapDark, 0.0f);
  p.bright = std::max(d.mapBright, 0.0f);
  p.gamma = std::max(d.mapGamma, 0.01f);
  p.burn = clampf(d.mapBurn, 0.0f, 1.0f);
  return p;
}

Vec3 applyColorMapping(const Vec3& cIn, const ColorMappingParams& p) {
  Vec3 c = vmax(cIn, Vec3(0.0f));
  const bool unit = p.dark == 1.0f && p.bright == 1.0f;
  auto g = [&](float x) { return unit ? std::max(x, 0.0f) : multiplyCurve(x, p.dark, p.bright); };
  auto expo = [&](float x) { return 1.0f - std::exp(-g(x)); };
  const float invGamma = 1.0f / p.gamma;
  Vec3 out;
  switch (p.type) {
    case kMapReinhard: {
      float L = luminance(c), Lm = g(L);
      out = scaleTo(c, L, Lm * (1.0f + p.burn * Lm) / (1.0f + Lm));
      break;
    }
    case kMapLinear: out = Vec3(g(c.x), g(c.y), g(c.z)); break;
    case kMapExponential: out = Vec3(expo(c.x), expo(c.y), expo(c.z)); break;
    case kMapHsvExponential: {
      float V = maxComponent(c);
      out = scaleTo(c, V, expo(V));
      break;
    }
    case kMapIntensityExp: {
      float I = luminance(c);
      out = scaleTo(c, I, expo(I));
      break;
    }
    case kMapGamma: {
      auto gc = [&](float x) { return std::pow(std::max(x * p.bright, 0.0f), invGamma); };
      return Vec3(gc(c.x), gc(c.y), gc(c.z));
    }
    case kMapIntensityGamma: {
      float I = luminance(c);
      return scaleTo(c, I, std::pow(std::max(I * p.bright, 0.0f), invGamma));
    }
    case kMapAgx:
    case kMapFilmicAces:
    default: {
      // The multipliers shape the luminance before the filmic curve.
      if (!unit) {
        float L = luminance(c);
        c = scaleTo(c, L, g(L));
      }
      // The filmic curve maps scene middle grey (0.18) to roughly display middle grey with this pre-gain.
      out = p.type == kMapAgx ? agxView(c) : clampColor(acesFitted(c * 1.8f), 0.0f, 1.0f);
      break;
    }
  }
  if (p.gamma != 1.0f)
    out = Vec3(std::pow(std::max(out.x, 0.0f), invGamma), std::pow(std::max(out.y, 0.0f), invGamma),
               std::pow(std::max(out.z, 0.0f), invGamma));
  return vmax(out, Vec3(0.0f));
}

std::vector<Vec2> parseCurve(const std::string& text) {
  std::vector<Vec2> pts;
  std::string item;
  std::istringstream in(text);
  while (std::getline(in, item, ';')) {
    float x, y;
    char comma;
    std::istringstream p(item);
    if (p >> x >> comma >> y) pts.push_back(Vec2(clampf(x, 0.0f, 1.0f), clampf(y, 0.0f, 1.0f)));
  }
  std::sort(pts.begin(), pts.end(), [](const Vec2& a, const Vec2& b) { return a.x < b.x; });
  return pts;
}

void postProcess(std::vector<float>& rgb, int w, int h, const DisplaySettings& d) {
  size_t n = static_cast<size_t>(w) * h;
  if (rgb.size() < n * 3 || n == 0) return;
  if (d.lensEffects) {
    float diag = std::sqrt(static_cast<float>(w * w + h * h));
    // Bloom: a wide soft glow of the whole image.
    if (d.bloomIntensity > 0.0f) {
      std::vector<float> glow = rgb;
      boxBlur(glow, w, h, std::max(1, static_cast<int>(d.bloomSize * diag / 3.0f)));
      for (size_t i = 0; i < n * 3; ++i) rgb[i] = rgb[i] * (1.0f - d.bloomIntensity * 0.3f) + glow[i] * d.bloomIntensity;
    }
    // Glare: star streaks from highlights, computed at quarter resolution.
    if (d.glareIntensity > 0.0f && d.glareRays > 0) {
      int sw = std::max(1, w / 4), sh = std::max(1, h / 4);
      std::vector<float> bright(static_cast<size_t>(sw) * sh * 3, 0.0f), streak(bright.size(), 0.0f);
      for (int y = 0; y < sh; ++y)
        for (int x = 0; x < sw; ++x) {
          Vec3 acc(0.0f);
          for (int dy = 0; dy < 4; ++dy)
            for (int dx = 0; dx < 4; ++dx) {
              int sx = std::min(x * 4 + dx, w - 1), sy = std::min(y * 4 + dy, h - 1);
              const float* p = &rgb[(static_cast<size_t>(sy) * w + sx) * 3];
              acc += Vec3(p[0], p[1], p[2]);
            }
          acc /= 16.0f;
          float l = luminance(acc);
          Vec3 b = l > 1.0f ? acc * ((l - 1.0f) / l) : Vec3(0.0f);
          size_t i = (static_cast<size_t>(y) * sw + x) * 3;
          bright[i] = b.x;
          bright[i + 1] = b.y;
          bright[i + 2] = b.z;
        }
      int len = std::max(2, static_cast<int>(d.glareSize * diag / 4.0f));
      int lines = std::max(1, d.glareRays / 2 + (d.glareRays % 2));
      float decay = 4.0f / len;
      for (int li = 0; li < lines; ++li) {
        float a = radians(d.glareRotation) + kPi * li / lines;
        float ddx = std::cos(a), ddy = std::sin(a);
        for (int y = 0; y < sh; ++y)
          for (int x = 0; x < sw; ++x) {
            Vec3 acc(0.0f);
            for (int k = -len; k <= len; k += 1) {
              int sx = static_cast<int>(std::lround(x + ddx * k)), sy = static_cast<int>(std::lround(y + ddy * k));
              if (sx < 0 || sy < 0 || sx >= sw || sy >= sh) continue;
              const float* p = &bright[(static_cast<size_t>(sy) * sw + sx) * 3];
              if (p[0] + p[1] + p[2] <= 0.0f) continue;
              acc += Vec3(p[0], p[1], p[2]) * std::exp(-std::fabs(static_cast<float>(k)) * decay);
            }
            size_t i = (static_cast<size_t>(y) * sw + x) * 3;
            streak[i] += acc.x;
            streak[i + 1] += acc.y;
            streak[i + 2] += acc.z;
          }
      }
      float k = d.glareIntensity * 0.25f;
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          float fx = std::min((x + 0.5f) / 4.0f - 0.5f, sw - 1.0f), fy = std::min((y + 0.5f) / 4.0f - 0.5f, sh - 1.0f);
          fx = std::max(fx, 0.0f);
          fy = std::max(fy, 0.0f);
          int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
          int x1 = std::min(x0 + 1, sw - 1), y1 = std::min(y0 + 1, sh - 1);
          float tx = fx - x0, ty = fy - y0;
          for (int c = 0; c < 3; ++c) {
            float a = streak[(static_cast<size_t>(y0) * sw + x0) * 3 + c], b = streak[(static_cast<size_t>(y0) * sw + x1) * 3 + c];
            float cc = streak[(static_cast<size_t>(y1) * sw + x0) * 3 + c], dd = streak[(static_cast<size_t>(y1) * sw + x1) * 3 + c];
            rgb[(static_cast<size_t>(y) * w + x) * 3 + c] += ((a * (1 - tx) + b * tx) * (1 - ty) + (cc * (1 - tx) + dd * tx) * ty) * k;
          }
        }
    }
  }
  if (d.vignetting > 0.0f) {
    float cx = w * 0.5f, cy = h * 0.5f, inv = 1.0f / (cx * cx + cy * cy);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        float r2 = ((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy)) * inv;
        float v = std::max(0.0f, 1.0f - d.vignetting * r2 * (1.2f - 0.2f * r2));
        for (int c = 0; c < 3; ++c) rgb[(static_cast<size_t>(y) * w + x) * 3 + c] *= v;
      }
  }
}

void tonemapToRgba(const float* hdr, int w, int h, const DisplaySettings& d, std::vector<uint8_t>& out,
                   const float* alpha) {
  size_t n = static_cast<size_t>(w) * h;
  out.resize(n * 4);
  float scale = std::pow(2.0f, d.exposureEV);
  ColorMappingParams cm = colorMappingParams(d);
  const bool mapOn = !d.mapAdaptationOnly;
  const bool protectBackground = mapOn && !d.mapAffectBackground && alpha != nullptr;
  // Color corrections on the display-linear (color mapped) image.
  Vec3 wb = kelvinToRgb(d.whiteBalanceK);
  Vec3 wbMul(1.0f / std::max(wb.x, 1e-3f), 1.0f / std::max(wb.y, 1e-3f), 1.0f / std::max(wb.z, 1e-3f));
  wbMul /= std::max(luminance(wbMul), 1e-6f);
  Vec3 tintMul(1.0f + 0.15f * d.tint, 1.0f - 0.2f * d.tint, 1.0f + 0.15f * d.tint);
  float burn = clampf(d.highlightBurn, 0.0f, 1.0f);
  bool levels = d.corrections && (d.levelsBlack != 0.0f || d.levelsWhite != 1.0f || d.levelsGamma != 1.0f);
  std::vector<float> curve;
  if (d.corrections && d.curve.size() >= 2) {
    bool identity = d.curve.size() == 2 && d.curve[0] == Vec2(0, 0) && d.curve[1] == Vec2(1, 1);
    if (!identity) curve = curveTable(d.curve);
  }
  std::shared_ptr<const Lut3D> lut;
  if (d.corrections && !d.lutFile.empty() && d.lutStrength > 0.0f) lut = loadLut(d.lutFile);
  float lb = d.levelsBlack, lw = std::max(d.levelsWhite, lb + 1e-3f), lg = 1.0f / std::max(d.levelsGamma, 0.01f);
  for (size_t i = 0; i < n; ++i) {
    Vec3 c(hdr[i * 3], hdr[i * 3 + 1], hdr[i * 3 + 2]);
    if (!isFiniteColor(c)) c = Vec3(0.0f);
    c = vmax(c * scale, Vec3(0.0f));  // exposure
    if (mapOn) {
      Vec3 mapped = applyColorMapping(c, cm);
      // Background pixels (alpha 0) keep the linear values when the mapping must not affect them.
      c = protectBackground ? lerp(c, mapped, clampf(alpha[i], 0.0f, 1.0f)) : mapped;
    }
    if (d.corrections) {
      c = c * wbMul * tintMul;
      if (d.hueShift != 0.0f) {
        Vec3 hsv = rgbToHsv(vmax(c, Vec3(0.0f)));
        hsv.x += d.hueShift / 360.0f;
        c = hsvToRgb(hsv);
      }
      if (burn < 1.0f) {
        float l = luminance(c);
        if (l > 0.0f) c *= 1.0f / (1.0f + (1.0f - burn) * l);
      }
    }
    if (d.saturation != 1.0f) c = vmax(lerp(Vec3(luminance(c)), c, d.saturation), Vec3(0.0f));
    // Display transform.
    Vec3 s(linearToSrgb(c.x), linearToSrgb(c.y), linearToSrgb(c.z));
    for (int k = 0; k < 3; ++k) {
      float v = s[k];
      if (d.contrast != 1.0f) v = clampf(0.5f + (v - 0.5f) * d.contrast, 0.0f, 1.0f);
      if (levels) v = std::pow(clampf((v - lb) / (lw - lb), 0.0f, 1.0f), lg);
      if (!curve.empty()) v = curve[static_cast<size_t>(std::lround(clampf(v, 0.0f, 1.0f) * 1023.0f))];
      s[k] = v;
    }
    if (lut) s = lerp(s, clampColor(lut->lookup(s), 0.0f, 1.0f), clampf(d.lutStrength, 0.0f, 1.0f));
    for (int k = 0; k < 3; ++k) out[i * 4 + k] = static_cast<uint8_t>(std::lround(clampf(s[k], 0.0f, 1.0f) * 255.0f));
    out[i * 4 + 3] = 255;
  }
}

#ifdef VERAA_HAVE_OIDN
namespace {
std::mutex& oidnMutex() {
  static std::mutex m;
  return m;
}
oidn::DeviceRef* oidnDevice() {
  static oidn::DeviceRef device;
  static bool ok = [] {
    // Prefer the fastest device (a GPU when available), but fall back to the CPU if the
    // GPU backend cannot start (missing driver, unsupported card, ...).
    for (int attempt = 0; attempt < 2; ++attempt) {
      device = attempt == 0 ? oidn::newDevice() : oidn::newDevice(oidn::DeviceType::CPU);
      if (!device) continue;
      device.commit();
      const char* msg = nullptr;
      if (device.getError(msg) == oidn::Error::None) return true;
    }
    return false;
  }();
  return ok ? &device : nullptr;
}
}  // namespace

bool oidnAvailable() {
  std::lock_guard<std::mutex> lock(oidnMutex());
  return oidnDevice() != nullptr;
}

bool denoiseImage(const std::vector<float>& color, const std::vector<float>& albedo, const std::vector<float>& normal,
                  int w, int h, std::vector<float>& out, std::string* error) {
  std::lock_guard<std::mutex> lock(oidnMutex());
  oidn::DeviceRef* dev = oidnDevice();
  if (!dev) {
    if (error) *error = "Denoiser device unavailable";
    return false;
  }
  size_t bytes = static_cast<size_t>(w) * h * 3 * sizeof(float);
  oidn::BufferRef colorBuf = dev->newBuffer(bytes);
  oidn::BufferRef albedoBuf = dev->newBuffer(bytes);
  oidn::BufferRef normalBuf = dev->newBuffer(bytes);
  oidn::BufferRef outBuf = dev->newBuffer(bytes);
  colorBuf.write(0, bytes, color.data());
  albedoBuf.write(0, bytes, albedo.data());
  normalBuf.write(0, bytes, normal.data());
  oidn::FilterRef filter = dev->newFilter("RT");
  filter.setImage("color", colorBuf, oidn::Format::Float3, w, h);
  filter.setImage("albedo", albedoBuf, oidn::Format::Float3, w, h);
  filter.setImage("normal", normalBuf, oidn::Format::Float3, w, h);
  filter.setImage("output", outBuf, oidn::Format::Float3, w, h);
  filter.set("hdr", true);
  filter.commit();
  filter.execute();
  const char* msg = nullptr;
  if (dev->getError(msg) != oidn::Error::None) {
    if (error) *error = msg ? msg : "Denoiser failed";
    return false;
  }
  out.resize(static_cast<size_t>(w) * h * 3);
  outBuf.read(0, bytes, out.data());
  return true;
}
#else
bool oidnAvailable() { return false; }
bool denoiseImage(const std::vector<float>&, const std::vector<float>&, const std::vector<float>&, int, int,
                  std::vector<float>&, std::string* error) {
  if (error) *error = "Built without Intel Open Image Denoise";
  return false;
}
#endif

}  // namespace render
}  // namespace veraa
