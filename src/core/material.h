// Physically based material with V-Ray style parameters (Diffuse, Reflect, Refract, ...).
#pragma once

#include <cstdint>
#include <string>

#include "params.h"

namespace veraa {

class Material {
 public:
  Material();
  static std::string type() { return "VeraaMtl"; }

  json toJson() const;
  void fromJson(const json& j);

  uint32_t id = 0;  // stable within a scene; nodes refer to materials by id
  std::string name = "Material";
  ParamBlock params;
};

// Parameter keys (also used by the renderer):
//  Diffuse:    diffuse (color), diffuseMap (file), diffuseMapAmount, roughness
//  Reflection: reflect (color), reflectGlossiness, fresnel (bool), fresnelIor, metalness, reflectMap
//  Refraction: refract (color), refractGlossiness, ior, fogColor, fogMultiplier, thinWalled, affectShadows
//  Self-illum: selfIllum (color), selfIllumMultiplier
//  Maps:       opacity, opacityMap, bumpMap, bumpAmount, mapTileU, mapTileV

}  // namespace veraa
