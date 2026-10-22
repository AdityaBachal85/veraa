// Shared helpers for the path tracer: random numbers, sampling, color and ray offsets.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

#include "vmath.h"

namespace veraa {
namespace render {

// PCG32 random number generator (O'Neill), small and fast with good statistics.
class Rng {
 public:
  Rng() = default;
  Rng(uint64_t seed, uint64_t stream) { seedWith(seed, stream); }
  void seedWith(uint64_t seed, uint64_t stream) {
    state_ = 0u;
    inc_ = (stream << 1u) | 1u;
    nextU32();
    state_ += seed;
    nextU32();
  }
  uint32_t nextU32() {
    uint64_t old = state_;
    state_ = old * 6364136223846793005ULL + inc_;
    uint32_t xorshifted = static_cast<uint32_t>(((old >> 18u) ^ old) >> 27u);
    uint32_t rot = static_cast<uint32_t>(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((~rot + 1u) & 31u));
  }
  // Uniform float in [0, 1).
  float next() { return static_cast<float>(nextU32() >> 8) * (1.0f / 16777216.0f); }

 private:
  uint64_t state_ = 0x853c49e6748fea9bULL;
  uint64_t inc_ = 0xda3e39cb94b95bdbULL;
};

inline uint64_t hash64(uint64_t x) {
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ULL;
  x ^= x >> 33;
  return x;
}

inline Vec3 sampleCosineHemisphere(float u1, float u2) {
  // Concentric disk mapping, then project up.
  float a = 2.0f * u1 - 1.0f, b = 2.0f * u2 - 1.0f;
  float r, phi;
  if (a == 0.0f && b == 0.0f) {
    r = 0.0f;
    phi = 0.0f;
  } else if (a * a > b * b) {
    r = a;
    phi = (kPi / 4.0f) * (b / a);
  } else {
    r = b;
    phi = (kPi / 2.0f) - (kPi / 4.0f) * (a / b);
  }
  float x = r * std::cos(phi), y = r * std::sin(phi);
  return {x, y, std::sqrt(std::max(0.0f, 1.0f - x * x - y * y))};
}

inline Vec2 sampleConcentricDisk(float u1, float u2) {
  Vec3 h = sampleCosineHemisphere(u1, u2);
  return {h.x, h.y};
}

inline Vec3 sampleUniformSphere(float u1, float u2) {
  float z = 1.0f - 2.0f * u1;
  float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
  float phi = kTwoPi * u2;
  return {r * std::cos(phi), r * std::sin(phi), z};
}

// Uniform direction inside a cone around +Z with cosine of the half-angle cosMax.
inline Vec3 sampleUniformCone(float u1, float u2, float cosMax) {
  float cosT = (1.0f - u1) + u1 * cosMax;
  float sinT = std::sqrt(std::max(0.0f, 1.0f - cosT * cosT));
  float phi = kTwoPi * u2;
  return {std::cos(phi) * sinT, std::sin(phi) * sinT, cosT};
}

inline float powerHeuristic(float a, float b) {
  float a2 = a * a, b2 = b * b;
  return a2 + b2 > 0.0f ? a2 / (a2 + b2) : 0.0f;
}

// Tent filter offset in [-1, 1].
inline float sampleTent(float u) {
  u *= 2.0f;
  return u < 1.0f ? std::sqrt(u) - 1.0f : 1.0f - std::sqrt(2.0f - u);
}

// Local shading frame: z is the normal.
struct Frame {
  Vec3 t, b, n;
  Frame() = default;
  explicit Frame(const Vec3& normal) : n(normal) { makeBasis(n, t, b); }
  Frame(const Vec3& tangent, const Vec3& bitangent, const Vec3& normal) : t(tangent), b(bitangent), n(normal) {}
  Vec3 toLocal(const Vec3& v) const { return {dot(v, t), dot(v, b), dot(v, n)}; }
  Vec3 toWorld(const Vec3& v) const { return t * v.x + b * v.y + n * v.z; }
};

inline bool isFiniteColor(const Vec3& c) { return std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z); }

// Offsets a ray origin along the geometric normal by a few ulps (Waechter & Binder, Ray Tracing Gems ch. 6),
// so new rays do not re-hit the surface they start on, independent of scene scale.
inline Vec3 offsetRayOrigin(const Vec3& p, const Vec3& n) {
  constexpr float kOrigin = 1.0f / 32.0f;
  constexpr float kFloatScale = 1.0f / 65536.0f;
  constexpr float kIntScale = 256.0f;
  Vec3 out;
  for (int i = 0; i < 3; ++i) {
    int offset = static_cast<int>(kIntScale * n[i]);
    float pi = p[i];
    int32_t bits;
    std::memcpy(&bits, &pi, sizeof(bits));
    bits += (pi < 0.0f) ? -offset : offset;
    float moved;
    std::memcpy(&moved, &bits, sizeof(moved));
    out[i] = std::fabs(pi) < kOrigin ? pi + kFloatScale * n[i] : moved;
  }
  return out;
}

inline Vec3 reflectLocal(const Vec3& w) { return {-w.x, -w.y, w.z}; }
inline Vec3 reflectAbout(const Vec3& w, const Vec3& h) { return h * (2.0f * dot(w, h)) - w; }

// Refracts w (pointing away from the surface) through a surface with normal n on w's side.
// eta = n_transmitted / n_incident. Returns false on total internal reflection.
inline bool refractDir(const Vec3& w, const Vec3& n, float eta, Vec3& wt) {
  float cosI = dot(n, w);
  float sin2I = std::max(0.0f, 1.0f - cosI * cosI);
  float sin2T = sin2I / (eta * eta);
  if (sin2T >= 1.0f) return false;
  float cosT = std::sqrt(1.0f - sin2T);
  wt = -w / eta + n * (cosI / eta - cosT);
  return true;
}

// Unpolarized Fresnel reflectance for a dielectric. cosI > 0 on the incident side, eta = n_t / n_i.
inline float fresnelDielectric(float cosI, float eta) {
  cosI = clampf(cosI, 0.0f, 1.0f);
  float sin2T = (1.0f - cosI * cosI) / (eta * eta);
  if (sin2T >= 1.0f) return 1.0f;
  float cosT = std::sqrt(1.0f - sin2T);
  float rs = (cosI - eta * cosT) / (cosI + eta * cosT);
  float rp = (eta * cosI - cosT) / (eta * cosI + cosT);
  return 0.5f * (rs * rs + rp * rp);
}

inline Vec3 schlick(const Vec3& f0, float cosI) {
  float m = clampf(1.0f - cosI, 0.0f, 1.0f);
  float m5 = m * m * m * m * m;
  return f0 + (Vec3(1.0f) - f0) * m5;
}

inline float srgbToLinear(float c) {
  return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
inline float linearToSrgb(float c) {
  c = clampf(c, 0.0f, 1.0f);
  return c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

inline Vec3 clampLuminance(const Vec3& c, float maxLum) {
  if (maxLum <= 0.0f) return c;
  float l = luminance(c);
  return l > maxLum ? c * (maxLum / l) : c;
}

}  // namespace render
}  // namespace veraa
