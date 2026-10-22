// Analytic lights: plane (rect) and sphere area lights, omni, spot and sun.
#pragma once

#include <string>
#include <vector>

#include "common.h"

namespace veraa {
class Scene;
class Node;

namespace render {

struct LightSample {
  Vec3 wi;            // direction toward the light
  Vec3 radiance;      // incoming radiance (area lights) or irradiance (delta lights)
  float dist = 0.0f;  // distance to the sampled point (infinity for the sun)
  float pdf = 1.0f;   // solid-angle pdf (1 for delta lights)
  bool delta = false;
};

struct Light {
  enum class Type { Rect, Sphere, Omni, Spot, Sun };
  Type type = Type::Omni;
  Vec3 color;  // radiance for area lights; see sample() for the others
  bool castShadows = true;
  bool invisible = false;
  bool doubleSided = false;

  Vec3 position;    // rect center / sphere center / point position
  Vec3 ex, ey, n;   // rect half-extent vectors and emission normal
  float area = 0.0f;
  float radius = 0.0f;
  Vec3 dir;         // spot: emission direction; sun: direction toward the sun
  float cosHot = 1.0f, cosFall = 0.0f;
  bool decay = true;
  float cosSun = 1.0f;

  bool isArea() const { return type == Type::Rect || type == Type::Sphere; }
  bool sample(const Vec3& p, float u1, float u2, LightSample& s) const;
  // Ray hit test for area lights. Returns the distance, emitted radiance toward the ray origin
  // (zero when seen from the back) and the solid-angle pdf sample() would have used.
  bool intersect(const Vec3& o, const Vec3& d, float tMax, float& t, Vec3& Le, float& pdf) const;
  // Approximate emitted power, used to pick lights when there are many.
  float power() const;
};

// Collects enabled lights. Sun lights are reported through sunDir/sunIrradiance (first sun found).
std::vector<Light> collectLights(const Scene& scene, bool* hasSun, Vec3* sunDir, float* sunIrradiance);

}  // namespace render
}  // namespace veraa
