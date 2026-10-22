// Surface scattering as a weighted list of lobes (diffuse, Oren-Nayar, diffuse transmission,
// sheen, GGX reflection/refraction with anisotropy, perfect specular, thin transmission and
// subsurface entry). Each lobe has its own shading frame, so blended materials can keep their
// own bump-mapped normals. Lobes are tagged with a category for render elements.
#pragma once

#include <cstdint>
#include <new>
#include <type_traits>

#include "common.h"

namespace veraa {
namespace render {

enum class LobeKind : uint8_t {
  Lambert,
  OrenNayar,
  DiffuseTransmission,
  Sheen,
  GgxReflect,
  SpecularReflect,
  GgxRefract,
  SpecularRefract,
  ThinTransmit,
  Subsurface,  // entry into a random-walk medium (handled by the integrator)
};

enum class LobeCategory : uint8_t { Diffuse = 0, Specular = 1, Transmission = 2, Subsurface = 3 };
constexpr int kLobeCategories = 4;

enum class FresnelMode : uint8_t { None, Dielectric, Schlick, Metalness };

struct Lobe {
  LobeKind kind = LobeKind::Lambert;
  LobeCategory category = LobeCategory::Diffuse;
  Frame frame;          // z = normal on the viewer's side
  Vec3 weight{1, 1, 1};
  float ax = 0.0f, ay = 0.0f;  // microfacet roughness (alpha) along frame.t and frame.b
  FresnelMode fresnel = FresnelMode::None;
  float eta = 1.5f;     // refraction IOR across the surface as seen from wo (n_far / n_near)
  float fresnelEta = 1.5f;  // IOR used by the Fresnel term (0 = no Fresnel falloff)
  Vec3 tint{1, 1, 1};   // reflection color (dielectric) or F0 (Schlick)
  Vec3 metalF0;         // Metalness: F0 of the metal part
  float metal = 0.0f;
  Vec3 etaRgb;          // per-channel refraction IOR (dispersion); zero when unused
  float orenA = 1.0f, orenB = 0.0f;
  bool flipped = false;   // Two-sided back material: light from the back side
  float select = 0.0f;  // sampling probability (set by Bsdf::finalize)
  Vec3 woL;             // view direction in the lobe frame (set by Bsdf::finalize)

  // Fresnel reflectance at a microfacet cosine.
  Vec3 fresnelAt(float cosI) const;
};

struct BsdfEval {
  Vec3 f[kLobeCategories];  // f * |cos| per category (sum = total)
  float pdf = 0.0f;         // solid-angle pdf of Bsdf::sample for this direction
  Vec3 total() const { return f[0] + f[1] + f[2] + f[3]; }
};

struct BsdfSample {
  Vec3 wi;              // world space
  Vec3 weight;          // f * |cos| / pdf
  float pdf = 0.0f;
  bool delta = false;
  bool transmission = false;  // crossed the surface
  bool subsurface = false;    // entered the subsurface medium (integrator runs a random walk)
  LobeCategory category = LobeCategory::Diffuse;
  int lobe = -1;
  int channel = -1;           // dispersion: the color channel the path continues with
};

class Bsdf {
 public:
  static constexpr int kMaxLobes = 24;

  void clear() { count_ = 0; }
  bool empty() const { return count_ == 0; }
  int count() const { return count_; }
  Lobe* add(const Lobe& l);
  Lobe& lobe(int i) { return lobes()[i]; }
  const Lobe& lobe(int i) const { return lobes()[i]; }
  // Multiplies every lobe weight (layering, blending).
  void scale(const Vec3& s);
  // Appends another BSDF's lobes multiplied by s.
  void append(const Bsdf& other, const Vec3& s, bool flipped = false);
  // Computes sampling probabilities from the lobe weights and the view direction.
  void finalize(const Vec3& wo);

  // Non-delta lobes only. geomSide: +1 when wi is on the viewer's side of the geometric
  // surface, -1 behind it, 0 to skip the check (lobes that disagree are ignored, so bumped
  // normals cannot leak light through the surface).
  BsdfEval eval(const Vec3& wo, const Vec3& wi, int geomSide = 0) const;
  bool sample(const Vec3& wo, float uLobe, float u1, float u2, BsdfSample& s) const;

  bool hasNonDelta() const { return nonDelta_; }
  bool isBlack() const { return count_ == 0 || totalSelect_ <= 0.0f; }
  // Directional albedo estimate for the denoiser guide.
  Vec3 albedo() const { return albedo_; }
  bool hasSubsurface() const;

 private:
  Vec3 evalLobe(const Lobe& l, const Vec3& woL, const Vec3& wiL, float& pdf) const;
  Lobe* lobes() { return reinterpret_cast<Lobe*>(storage_); }
  const Lobe* lobes() const { return reinterpret_cast<const Lobe*>(storage_); }
  // Raw storage: lobes are copied in by add(), so building a BSDF does not initialize all slots.
  alignas(Lobe) unsigned char storage_[kMaxLobes * sizeof(Lobe)];
  int count_ = 0;
  bool nonDelta_ = false;
  float totalSelect_ = 0.0f;
  Vec3 albedo_;
};

// Shared microfacet helpers (also used by tests).
float ggxD(const Vec3& h, float ax, float ay);
float ggxLambda(const Vec3& w, float ax, float ay);
Vec3 sampleGgxVndf(const Vec3& wo, float ax, float ay, float u1, float u2);
float glossToAlpha(float glossiness, bool isRoughness = false);
// Directional albedo of the sheen lobe (for energy-conserving layering).
float sheenAlbedo(float cosO, float alpha);

}  // namespace render
}  // namespace veraa
