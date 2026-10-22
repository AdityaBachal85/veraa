// Layered V-Ray style surface model: Fresnel-weighted GGX reflection over diffuse, with
// optional metalness, GGX refraction (or thin-walled transmission) and emission.
#pragma once

#include <string>
#include <vector>

#include "common.h"
#include "texture.h"

namespace veraa {
class Material;
class Scene;

namespace render {

struct MaterialData {
  Vec3 diffuse{0.5f, 0.5f, 0.5f};
  const Texture* diffuseMap = nullptr;
  float diffuseMapAmount = 1.0f;
  float roughness = 0.0f;

  Vec3 reflect;
  const Texture* reflectMap = nullptr;
  float reflectGlossiness = 1.0f;
  bool fresnel = true;
  float fresnelIor = 1.6f;
  float metalness = 0.0f;

  Vec3 refract;
  float refractGlossiness = 1.0f;
  float ior = 1.6f;
  Vec3 absorption;  // Beer-Lambert coefficient per scene unit inside the object
  bool thinWalled = false;
  bool affectShadows = true;

  Vec3 emission;

  float opacity = 1.0f;
  const Texture* opacityMap = nullptr;
  const Texture* bumpMap = nullptr;
  float bumpAmount = 1.0f;
  float tileU = 1.0f, tileV = 1.0f;

  // Derived flags.
  bool hasRefraction = false;
  bool hasEmission = false;
  bool hasOpacity = false;       // can be cut out (opacity < 1 or an opacity map)
  bool shadowTransparent = false;

  static MaterialData fromMaterial(const Material& m, const Scene* scene, TextureCache& cache,
                                   std::vector<std::string>* warnings);
  static MaterialData fromColor(const Vec3& diffuse);
  void finalize();

  Vec2 tiled(const Vec2& uv) const { return {uv.x * tileU, uv.y * tileV}; }
  float alphaAt(const Vec2& tiledUv) const {
    float a = opacity;
    if (opacityMap) a *= opacityMap->opacity(tiledUv);
    return a;
  }
  // Fraction of light a shadow ray keeps when crossing this surface (cut-outs and
  // "affect shadows" refraction). cosAbs is |cos| between the ray and the normal.
  Vec3 shadowTransmission(const Vec2& tiledUv, float cosAbs) const;
};

struct BsdfSample {
  Vec3 wi;        // local frame
  Vec3 weight;    // f * |cos| / pdf
  float pdf = 0.0f;
  bool delta = false;
  bool transmission = false;
};

// Evaluated at one shading point. Works in a local frame whose +Z is the shading normal on
// the side the light leaves toward (wo.z > 0).
class Bsdf {
 public:
  // entering: wo is on the outside of the surface (the mesh's outward side).
  void init(const MaterialData& m, const Vec2& tiledUv, const Vec3& wo, bool entering);

  // Non-delta lobes only. Returns f (without the cosine) and the combined sampling pdf.
  Vec3 eval(const Vec3& wo, const Vec3& wi, float& pdf) const;
  bool sample(const Vec3& wo, float uLobe, float u1, float u2, BsdfSample& s) const;
  bool hasNonDelta() const { return nonDelta_; }
  bool isBlack() const { return pD_ + pS_ + pT_ <= 0.0f; }
  Vec3 albedo() const;
  float etaT() const { return etaT_; }
  bool thin() const { return thin_; }

 private:
  Vec3 specF(float cosI) const;

  Vec3 diffuseColor_;
  Vec3 wd_;          // diffuse weight (includes the Fresnel/refraction energy split)
  float orenA_ = 1.0f, orenB_ = 0.0f;
  bool oren_ = false;

  Vec3 reflColor_;
  float alphaS_ = 0.0f;
  bool deltaS_ = true;
  bool fresnel_ = true;
  float etaS_ = 1.6f;
  float metal_ = 0.0f;
  Vec3 metalF0_;
  Vec3 s0_;          // specular weight at wo

  Vec3 refrColor_;
  float alphaT_ = 0.0f;
  bool deltaT_ = true;
  float etaT_ = 1.6f;
  bool thin_ = false;
  Vec3 wt_;          // transmission weight at wo

  float pD_ = 0.0f, pS_ = 0.0f, pT_ = 0.0f;
  bool nonDelta_ = false;
};

}  // namespace render
}  // namespace veraa
