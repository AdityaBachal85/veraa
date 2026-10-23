// Materials compiled for rendering: inputs with their maps, bump mapping, opacity and the
// construction of the BSDF lobes for every material type.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "bsdf.h"
#include "material.h"
#include "texmaps.h"

namespace veraa {
namespace render {

struct SubsurfaceParams {
  Vec3 albedo{0.8f, 0.8f, 0.8f};  // multiple-scattering albedo (the color the surface ends up with)
  Vec3 radius{1.0f, 1.0f, 1.0f};  // mean free path per channel (scene units)
  float anisotropy = 0.0f;
  float ior = 1.4f;
};

// Matte/shadow catcher settings (VeraaWrapperMtl).
struct MatteParams {
  bool matte = false;
  float alphaContribution = 1.0f;
  bool shadows = true;
  bool shadowsAffectAlpha = true;
  Vec3 shadowColor;
  float shadowBrightness = 1.0f;
  float reflectionAmount = 1.0f;
};

struct SurfaceShade {
  Bsdf bsdf;
  Vec3 emission;           // radiance toward the viewer
  bool emissionAffectsGI = true;
  Vec3 absorption;         // interior medium (refraction fog), per scene unit
  bool hasExitColor = false;
  Vec3 exitColor;
  bool thin = false;
  bool hasSubsurface = false;
  SubsurfaceParams sss;
  float generateGI = 1.0f;  // VeraaWrapperMtl
  float receiveGI = 1.0f;
  const MatteParams* matte = nullptr;
  Vec3 shadingNormal;       // final normal on the viewer's side (denoiser guide)
};

class ShadeMaterial {
 public:
  virtual ~ShadeMaterial() = default;
  // Concrete material for this hit (Multi/Sub-Object and Override pick a sub-material).
  virtual const ShadeMaterial* resolve(const ShadeContext& sc) const { return this; }
  // Cut-out opacity in [0, 1].
  virtual float opacity(const ShadeContext& sc) const { return 1.0f; }
  virtual bool hasOpacity() const { return false; }
  // Light a shadow ray keeps when crossing the surface (cut-outs, transparent shadows).
  virtual Vec3 shadowTransmission(const ShadeContext& sc, float cosAbs) const {
    return Vec3(1.0f - opacity(sc));
  }
  virtual bool shadowTransparent() const { return hasOpacity(); }
  // True when the light shadow rays pass through this surface beyond its cut-out opening stands
  // in for a perfectly smooth refraction that bends rays (solid glass), which scattered paths do
  // not connect to lights through while refractive caustics are off.
  virtual bool smoothShadowRefraction(const ShadeContext& sc) const { return false; }
  // Builds the BSDF. sc.ns is bump-mapped in place.
  virtual void shade(ShadeContext& sc, SurfaceShade& out) const = 0;
  // Render-time displacement map of the material (null when none).
  virtual const MapNode* displacement(float& amount, float& shift) const { return nullptr; }
  // Emits light that should be importance-sampled as a mesh light.
  virtual bool isMeshLight() const { return false; }
  virtual bool emits() const { return false; }
  // A refractive solid with an inside (for entering/leaving logic).
  virtual bool isRefractiveSolid() const { return false; }
};

struct MaterialCompileContext {
  MapCompileContext maps;
  float exposureCompensation = 1.0f;  // VeraaLightMtl "compensate camera exposure"
};

std::unique_ptr<ShadeMaterial> compileMaterial(const Material& m, MaterialCompileContext& ctx);
// Plain diffuse material for objects without a material (wire color).
std::unique_ptr<ShadeMaterial> compileColorMaterial(const Vec3& diffuse);

// Height-field or normal-map bump. amount is the material's bump amount times the slot amount.
void applyBump(ShadeContext& sc, const MapNode* bump, float amount);

// Single-scattering albedo for a desired multiple-scattering albedo (random walk inversion).
Vec3 subsurfaceSingleScatterAlbedo(const Vec3& multiScatter);

}  // namespace render
}  // namespace veraa
