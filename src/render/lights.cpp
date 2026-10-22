#include "lights.h"

#include "scene.h"

namespace veraa {
namespace render {

namespace {

float smoothstepf(float a, float b, float x) {
  if (a == b) return x >= b ? 1.0f : 0.0f;
  float t = clampf((x - a) / (b - a), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

// Distance along the ray to the sphere (nearest positive root), or -1.
float raySphere(const Vec3& o, const Vec3& d, const Vec3& c, float r) {
  Vec3 oc = o - c;
  float b = dot(oc, d);
  float cc = dot(oc, oc) - r * r;
  float disc = b * b - cc;
  if (disc < 0.0f) return -1.0f;
  float s = std::sqrt(disc);
  float t0 = -b - s, t1 = -b + s;
  if (t0 > 1e-4f) return t0;
  if (t1 > 1e-4f) return t1;
  return -1.0f;
}

float sphereConePdf(const Vec3& p, const Vec3& c, float r) {
  float d2 = length2(c - p);
  if (d2 <= r * r) return 0.0f;
  float sin2 = r * r / d2;
  float oneMinusCos = sin2 < 1e-4f ? 0.5f * sin2 : 1.0f - std::sqrt(1.0f - sin2);
  return 1.0f / (kTwoPi * oneMinusCos);
}

}  // namespace

bool Light::sample(const Vec3& p, float u1, float u2, LightSample& s) const {
  s = LightSample();
  switch (type) {
    case Type::Rect: {
      Vec3 q = position + ex * (2.0f * u1 - 1.0f) + ey * (2.0f * u2 - 1.0f);
      Vec3 d = q - p;
      float d2 = length2(d);
      if (d2 <= 1e-12f) return false;
      float dist = std::sqrt(d2);
      s.wi = d / dist;
      float cosL = -dot(s.wi, n);
      if (cosL <= 0.0f) {
        if (!doubleSided) return false;
        cosL = -cosL;
      }
      if (cosL < 1e-6f) return false;
      s.dist = dist;
      s.pdf = d2 / (cosL * area);
      s.radiance = color;
      return true;
    }
    case Type::Sphere: {
      Vec3 toC = position - p;
      float dc2 = length2(toC);
      if (dc2 <= radius * radius * 1.0001f) return false;  // inside the light
      float dc = std::sqrt(dc2);
      Vec3 axis = toC / dc;
      float sin2 = radius * radius / dc2;
      float cosMax = std::sqrt(std::max(0.0f, 1.0f - sin2));
      Frame f(axis);
      s.wi = normalize(f.toWorld(sampleUniformCone(u1, u2, cosMax)));
      float t = raySphere(p, s.wi, position, radius);
      if (t < 0.0f) t = dc - radius;  // grazing numerical miss
      s.dist = t;
      s.pdf = sphereConePdf(p, position, radius);
      s.radiance = color;
      return s.pdf > 0.0f;
    }
    case Type::Omni:
    case Type::Spot: {
      Vec3 lp = position;
      if (radius > 0.0f) lp += sampleUniformSphere(u1, u2) * radius;
      Vec3 d = lp - p;
      float d2 = length2(d);
      if (d2 <= 1e-12f) return false;
      float dist = std::sqrt(d2);
      s.wi = d / dist;
      s.dist = dist;
      s.delta = true;
      // "Multiplier" is the irradiance at 100 scene units from the light.
      s.radiance = decay ? color * (1e4f / d2) : color;
      if (type == Type::Spot) {
        float cosA = -dot(s.wi, dir);
        float f = smoothstepf(cosFall, cosHot, cosA);
        if (f <= 0.0f) return false;
        s.radiance *= f;
      }
      return true;
    }
    case Type::Sun: {
      Frame f(dir);
      s.wi = cosSun < 1.0f ? normalize(f.toWorld(sampleUniformCone(u1, u2, cosSun))) : dir;
      s.dist = kInfinity;
      s.delta = true;
      s.radiance = color;
      return true;
    }
  }
  return false;
}

bool Light::intersect(const Vec3& o, const Vec3& d, float tMax, float& t, Vec3& Le, float& pdf) const {
  if (type == Type::Rect) {
    float denom = dot(d, n);
    if (std::fabs(denom) < 1e-8f) return false;
    float tt = dot(position - o, n) / denom;
    if (tt <= 1e-4f || tt >= tMax) return false;
    Vec3 q = o + d * tt - position;
    float a = dot(q, ex) / length2(ex), b = dot(q, ey) / length2(ey);
    if (std::fabs(a) > 1.0f || std::fabs(b) > 1.0f) return false;
    float cosL = -denom;  // > 0 when the ray hits the emitting side
    if (cosL <= 0.0f) {
      if (!doubleSided) return false;
      cosL = -cosL;
    }
    t = tt;
    Le = color;
    pdf = tt * tt / (cosL * area);
    return true;
  }
  if (type == Type::Sphere) {
    float tt = raySphere(o, d, position, radius);
    if (tt <= 0.0f || tt >= tMax) return false;
    t = tt;
    Le = color;
    pdf = sphereConePdf(o, position, radius);
    return true;
  }
  return false;
}

float Light::power() const {
  float l = luminance(color);
  switch (type) {
    case Type::Rect: return l * area * kPi * (doubleSided ? 2.0f : 1.0f);
    case Type::Sphere: return l * 4.0f * kPi * radius * radius * kPi;
    case Type::Omni: return l * 4.0f * kPi * 1e4f;
    case Type::Spot: return l * kTwoPi * (1.0f - 0.5f * (cosHot + cosFall)) * 1e4f;
    case Type::Sun: return l * 1e6f;
  }
  return l;
}

std::vector<Light> collectLights(const Scene& scene, bool* hasSun, Vec3* sunDir, float* sunIrradiance) {
  std::vector<Light> lights;
  if (hasSun) *hasSun = false;
  for (const auto& node : scene.nodes) {
    const LightObject* lo = node->light();
    if (!lo) continue;
    const ParamBlock& p = lo->params;
    if (!p.getBool("enabled")) continue;
    float intensity = p.getFloat("intensity");
    Vec3 color = p.getColor("color") * intensity;
    if (maxComponent(color) <= 0.0f) continue;
    Mat4 M = node->worldMatrix();
    std::string type = lo->type();
    Light l;
    l.color = color;
    l.castShadows = p.getBool("castShadows");
    l.position = M.translation();
    Vec3 minusZ = normalize(M.transformDir(Vec3(0, 0, -1)));
    float maxScale = std::max(length(M.column(0)), std::max(length(M.column(1)), length(M.column(2))));
    if (type == "RectLight") {
      l.type = Light::Type::Rect;
      l.ex = M.transformDir(Vec3(p.getFloat("width") * 0.5f, 0, 0));
      l.ey = M.transformDir(Vec3(0, p.getFloat("length") * 0.5f, 0));
      l.n = minusZ;
      l.area = 4.0f * length(l.ex) * length(l.ey);
      l.doubleSided = p.getBool("doubleSided");
      l.invisible = p.getBool("invisible");
      if (l.area <= 0.0f) continue;
    } else if (type == "SphereLight") {
      l.type = Light::Type::Sphere;
      l.radius = p.getFloat("radius") * maxScale;
      l.invisible = p.getBool("invisible");
      if (l.radius <= 0.0f) continue;
    } else if (type == "OmniLight" || type == "SpotLight") {
      l.type = type == "OmniLight" ? Light::Type::Omni : Light::Type::Spot;
      l.radius = p.getFloat("radius");
      l.decay = p.getInt("decay") == 1;
      if (l.type == Light::Type::Spot) {
        l.dir = minusZ;
        float hot = p.getFloat("hotspot"), fall = std::max(p.getFloat("falloff"), hot + 0.01f);
        l.cosHot = std::cos(radians(hot * 0.5f));
        l.cosFall = std::cos(radians(fall * 0.5f));
      }
    } else if (type == "SunLight") {
      l.type = Light::Type::Sun;
      l.dir = -minusZ;
      l.cosSun = std::cos(radians(0.265f * p.getFloat("sizeMultiplier")));
      if (hasSun && !*hasSun) {
        *hasSun = true;
        if (sunDir) *sunDir = l.dir;
        if (sunIrradiance) *sunIrradiance = luminance(color);
      }
    } else {
      continue;
    }
    lights.push_back(l);
  }
  return lights;
}

}  // namespace render
}  // namespace veraa
