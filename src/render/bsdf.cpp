#include "bsdf.h"

#include "material.h"
#include "scene.h"

namespace veraa {
namespace render {

namespace {

constexpr float kDeltaGloss = 0.999f;
constexpr float kMinAlpha = 0.0015f;

float glossToAlpha(float gloss) {
  float r = 1.0f - clampf(gloss, 0.0f, 1.0f);
  return std::max(r * r, kMinAlpha);
}

// GGX (Trowbridge-Reitz), isotropic.
float ggxD(const Vec3& h, float a) {
  float c2 = h.z * h.z;
  if (c2 <= 0.0f) return 0.0f;
  float a2 = a * a;
  float d = c2 * (a2 - 1.0f) + 1.0f;
  return a2 / (kPi * d * d);
}

float ggxLambda(const Vec3& w, float a) {
  float c2 = w.z * w.z;
  if (c2 >= 1.0f) return 0.0f;
  float t2 = (1.0f - c2) / std::max(c2, 1e-12f);
  return 0.5f * (-1.0f + std::sqrt(1.0f + a * a * t2));
}

float ggxG1(const Vec3& w, float a) { return 1.0f / (1.0f + ggxLambda(w, a)); }
float ggxG2(const Vec3& wo, const Vec3& wi, float a) { return 1.0f / (1.0f + ggxLambda(wo, a) + ggxLambda(wi, a)); }

// Visible-normal sampling (Heitz 2018). wo must be in the upper hemisphere.
Vec3 sampleGgxVndf(const Vec3& wo, float a, float u1, float u2) {
  Vec3 vh = normalize(Vec3(a * wo.x, a * wo.y, wo.z));
  float lensq = vh.x * vh.x + vh.y * vh.y;
  Vec3 t1 = lensq > 0.0f ? Vec3(-vh.y, vh.x, 0.0f) / std::sqrt(lensq) : Vec3(1.0f, 0.0f, 0.0f);
  Vec3 t2 = cross(vh, t1);
  float r = std::sqrt(u1);
  float phi = kTwoPi * u2;
  float p1 = r * std::cos(phi);
  float p2 = r * std::sin(phi);
  float s = 0.5f * (1.0f + vh.z);
  p2 = (1.0f - s) * std::sqrt(std::max(0.0f, 1.0f - p1 * p1)) + s * p2;
  Vec3 nh = t1 * p1 + t2 * p2 + vh * std::sqrt(std::max(0.0f, 1.0f - p1 * p1 - p2 * p2));
  return normalize(Vec3(a * nh.x, a * nh.y, std::max(1e-6f, nh.z)));
}

Vec3 one() { return Vec3(1.0f); }

}  // namespace

MaterialData MaterialData::fromColor(const Vec3& diffuse) {
  MaterialData m;
  m.diffuse = diffuse;
  m.finalize();
  return m;
}

MaterialData MaterialData::fromMaterial(const Material& mat, const Scene* scene, TextureCache& cache,
                                        std::vector<std::string>* warnings) {
  const ParamBlock& p = mat.params;
  auto tex = [&](const char* key) -> const Texture* {
    const std::string& path = p.getString(key);
    if (path.empty()) return nullptr;
    // ":name" are built-in procedural textures (e.g. ":checker").
    bool builtin = path[0] == ':';
    return cache.get(scene && !builtin ? scene->resolvePath(path) : path, warnings);
  };
  MaterialData m;
  m.diffuse = p.getColor("diffuse");
  m.diffuseMap = tex("diffuseMap");
  m.diffuseMapAmount = p.getFloat("diffuseMapAmount");
  m.roughness = p.getFloat("roughness");
  m.reflect = p.getColor("reflect");
  m.reflectMap = tex("reflectMap");
  m.reflectGlossiness = p.getFloat("reflectGlossiness");
  m.fresnel = p.getBool("fresnel");
  m.fresnelIor = p.getFloat("fresnelIor");
  m.metalness = p.getFloat("metalness");
  m.refract = p.getColor("refract");
  m.refractGlossiness = p.getFloat("refractGlossiness");
  m.ior = p.getFloat("ior");
  // Fog color is the transmittance after 10 scene units (10 cm) at multiplier 1.
  Vec3 fog = p.getColor("fogColor");
  float mult = p.getFloat("fogMultiplier");
  m.absorption = Vec3(-std::log(clampf(fog.x, 1e-4f, 1.0f)), -std::log(clampf(fog.y, 1e-4f, 1.0f)),
                      -std::log(clampf(fog.z, 1e-4f, 1.0f))) *
                 (mult / 10.0f);
  m.thinWalled = p.getBool("thinWalled");
  m.affectShadows = p.getBool("affectShadows");
  m.emission = p.getColor("selfIllum") * p.getFloat("selfIllumMultiplier");
  m.opacity = p.getFloat("opacity");
  m.opacityMap = tex("opacityMap");
  m.bumpMap = tex("bumpMap");
  m.bumpAmount = p.getFloat("bumpAmount");
  m.tileU = p.getFloat("mapTileU");
  m.tileV = p.getFloat("mapTileV");
  m.finalize();
  return m;
}

void MaterialData::finalize() {
  diffuse = clampColor(diffuse, 0.0f, 1.0f);
  reflect = clampColor(reflect, 0.0f, 1.0f);
  refract = clampColor(refract, 0.0f, 1.0f);
  metalness = clampf(metalness, 0.0f, 1.0f);
  fresnelIor = std::max(fresnelIor, 1.0001f);
  ior = std::max(ior, 1.0001f);
  hasRefraction = maxComponent(refract) > 0.0f && metalness < 1.0f;
  hasEmission = maxComponent(emission) > 0.0f;
  hasOpacity = opacity < 1.0f || opacityMap != nullptr;
  shadowTransparent = hasOpacity || (hasRefraction && affectShadows);
}

Vec3 MaterialData::shadowTransmission(const Vec2& tiledUv, float cosAbs) const {
  float a = alphaAt(tiledUv);
  Vec3 pass(1.0f - a);
  if (hasRefraction && affectShadows) {
    Vec3 refl = reflectMap ? reflectMap->color(tiledUv) : reflect;
    Vec3 F = fresnel ? refl * fresnelDielectric(cosAbs, fresnelIor) : refl;
    pass += (one() - F) * refract * ((1.0f - metalness) * a);
  }
  return pass;
}

void Bsdf::init(const MaterialData& m, const Vec2& uv, const Vec3& wo, bool entering) {
  // Only solid refractive objects have an inside; everything else looks the same from both sides.
  if (!m.hasRefraction || m.thinWalled) entering = true;
  diffuseColor_ = m.diffuse;
  if (m.diffuseMap) diffuseColor_ = lerp(m.diffuse, m.diffuseMap->color(uv), m.diffuseMapAmount);
  reflColor_ = m.reflectMap ? m.reflectMap->color(uv) : m.reflect;
  metal_ = m.metalness;
  metalF0_ = diffuseColor_ * reflColor_;
  fresnel_ = m.fresnel;
  etaS_ = entering ? m.fresnelIor : 1.0f / m.fresnelIor;
  deltaS_ = m.reflectGlossiness >= kDeltaGloss;
  alphaS_ = glossToAlpha(m.reflectGlossiness);

  thin_ = m.thinWalled;
  etaT_ = thin_ ? 1.0f : (entering ? m.ior : 1.0f / m.ior);
  deltaT_ = thin_ || m.refractGlossiness >= kDeltaGloss;
  alphaT_ = glossToAlpha(m.refractGlossiness);
  refrColor_ = m.hasRefraction ? m.refract * (1.0f - metal_) : Vec3(0.0f);

  float cosO = std::max(wo.z, 1e-4f);
  s0_ = specF(cosO);
  Vec3 rest = vmax(one() - s0_, Vec3(0.0f));
  wt_ = rest * refrColor_;
  wd_ = rest * (one() - refrColor_) * diffuseColor_ * (1.0f - metal_);
  // Inside a refractive object only the interface matters; no diffuse layer underneath.
  if (!entering && m.hasRefraction && !thin_) wd_ = Vec3(0.0f);

  oren_ = m.roughness > 0.0f;
  if (oren_) {
    float sigma = m.roughness * 0.5f;  // radians
    float s2 = sigma * sigma;
    orenA_ = 1.0f - 0.5f * s2 / (s2 + 0.33f);
    orenB_ = 0.45f * s2 / (s2 + 0.09f);
  }

  float wD = luminance(wd_), wS = luminance(s0_), wT = luminance(wt_);
  // Sample reflections a bit more often than their energy suggests; they carry sharp detail.
  if (wS > 0.0f) wS = std::max(wS, 0.15f * (wD + wT));
  float sum = wD + wS + wT;
  if (sum <= 0.0f) {
    pD_ = pS_ = pT_ = 0.0f;
    nonDelta_ = false;
    return;
  }
  pD_ = wD / sum;
  pS_ = wS / sum;
  pT_ = wT / sum;
  nonDelta_ = pD_ > 0.0f || (pS_ > 0.0f && !deltaS_) || (pT_ > 0.0f && !deltaT_);
}

Vec3 Bsdf::specF(float c) const {
  Vec3 diel = fresnel_ ? reflColor_ * fresnelDielectric(c, etaS_) : reflColor_;
  if (metal_ <= 0.0f) return diel;
  return lerp(diel, schlick(metalF0_, c), metal_);
}

Vec3 Bsdf::albedo() const { return clampColor(wd_ + s0_ + wt_, 0.0f, 1.0f); }

Vec3 Bsdf::eval(const Vec3& wo, const Vec3& wi, float& pdf) const {
  pdf = 0.0f;
  Vec3 f(0.0f);
  float cosO = wo.z, cosI = wi.z;
  if (cosO <= 0.0f) return f;
  if (cosI > 0.0f) {
    if (pD_ > 0.0f) {
      float factor = 1.0f;
      if (oren_) {
        float sinI = std::sqrt(std::max(0.0f, 1.0f - cosI * cosI));
        float sinO = std::sqrt(std::max(0.0f, 1.0f - cosO * cosO));
        float maxCos = 0.0f;
        if (sinI > 1e-4f && sinO > 1e-4f) maxCos = std::max(0.0f, (wi.x * wo.x + wi.y * wo.y) / (sinI * sinO));
        float sinA, tanB;
        if (cosI > cosO) {  // theta_i < theta_o
          sinA = sinO;
          tanB = sinI / cosI;
        } else {
          sinA = sinI;
          tanB = sinO / cosO;
        }
        factor = orenA_ + orenB_ * maxCos * sinA * tanB;
      }
      f += wd_ * (kInvPi * factor);
      pdf += pD_ * cosI * kInvPi;
    }
    if (pS_ > 0.0f && !deltaS_) {
      Vec3 h = normalize(wo + wi);
      float D = ggxD(h, alphaS_);
      Vec3 F = specF(dot(wo, h));
      f += F * (D * ggxG2(wo, wi, alphaS_) / (4.0f * cosO * cosI));
      pdf += pS_ * ggxG1(wo, alphaS_) * D / (4.0f * cosO);
    }
  } else if (cosI < 0.0f && pT_ > 0.0f && !deltaT_) {
    float eta = etaT_;
    Vec3 h = normalize(wo + wi * eta);
    if (h.z < 0.0f) h = -h;
    float dO = dot(wo, h), dI = dot(wi, h);
    if (dO > 0.0f && dI < 0.0f) {
      float denom = dO + eta * dI;
      if (std::fabs(denom) > 1e-6f) {
        float D = ggxD(h, alphaT_);
        float G = ggxG2(wo, Vec3(wi.x, wi.y, -wi.z), alphaT_);
        Vec3 F = specF(dO);
        float common = D * eta * eta * std::fabs(dI) / (denom * denom);
        f += vmax(one() - F, Vec3(0.0f)) * refrColor_ * (common * G * dO / (std::fabs(cosI) * cosO));
        pdf += pT_ * ggxG1(wo, alphaT_) * dO / cosO * common;
      }
    }
  }
  return f;
}

bool Bsdf::sample(const Vec3& wo, float uLobe, float u1, float u2, BsdfSample& s) const {
  s = BsdfSample();
  if (wo.z <= 0.0f || isBlack()) return false;
  if (uLobe < pD_) {
    s.wi = sampleCosineHemisphere(u1, u2);
    if (s.wi.z <= 0.0f) return false;
  } else if (uLobe < pD_ + pS_) {
    if (deltaS_) {
      s.wi = reflectLocal(wo);
      s.weight = s0_ / pS_;
      s.pdf = pS_;
      s.delta = true;
      return true;
    }
    Vec3 h = sampleGgxVndf(wo, alphaS_, u1, u2);
    s.wi = reflectAbout(wo, h);
    if (s.wi.z <= 0.0f) return false;
  } else {
    if (pT_ <= 0.0f) return false;
    if (thin_) {
      s.wi = -wo;
      s.weight = wt_ / pT_;
      s.pdf = pT_;
      s.delta = true;
      s.transmission = true;
      return true;
    }
    if (deltaT_) {
      if (refractDir(wo, Vec3(0, 0, 1), etaT_, s.wi)) {
        s.transmission = true;
      } else {
        s.wi = reflectLocal(wo);  // total internal reflection
      }
      s.weight = wt_ / pT_;
      s.pdf = pT_;
      s.delta = true;
      return true;
    }
    Vec3 h = sampleGgxVndf(wo, alphaT_, u1, u2);
    if (!refractDir(wo, h, etaT_, s.wi)) return false;
    if (s.wi.z >= 0.0f) return false;
    s.transmission = true;
  }
  Vec3 f = eval(wo, s.wi, s.pdf);
  if (s.pdf <= 0.0f) return false;
  s.weight = f * (std::fabs(s.wi.z) / s.pdf);
  return isFiniteColor(s.weight);
}

}  // namespace render
}  // namespace veraa
