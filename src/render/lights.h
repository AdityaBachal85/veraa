// Analytic and mesh lights: plane (optionally textured), disc, sphere, omni, spot, IES, sun,
// mesh lights and skylight portals, with light linking, affect flags and light groups.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "common.h"
#include "photometry.h"
#include "texture.h"

namespace veraa {
class Scene;
class Node;

namespace render {

class Environment;

struct LightSample {
  Vec3 wi;            // direction toward the light
  Vec3 radiance;      // incoming radiance (area lights) or irradiance (delta lights)
  float dist = 0.0f;  // distance to the sampled point (infinity for the sun)
  float pdf = 1.0f;   // solid-angle pdf (1 for delta lights)
  bool delta = false;
};

// Include/exclude list of a light.
struct LightLink {
  bool include = false;          // true: only the listed nodes are affected
  int affects = 0;               // 0 illumination and shadows, 1 illumination, 2 shadows
  std::vector<uint32_t> nodes;   // sorted node ids
  bool active() const { return include || !nodes.empty(); }
  bool listed(uint32_t node) const;
  bool illuminates(uint32_t node) const {
    if (!active() || affects == 2) return true;
    return listed(node) == include;
  }
  bool castsShadowFrom(uint32_t node) const {
    if (!active() || affects == 1) return true;
    return listed(node) == include;
  }
};

struct Light {
  enum class Type { Rect, Disc, Sphere, Omni, Spot, Sun, Mesh, Ies, Portal };
  Type type = Type::Omni;
  Vec3 color;  // radiance (area lights), intensity scale (point lights), irradiance (sun)
  bool castShadows = true;
  bool invisible = false;
  bool doubleSided = false;

  Vec3 position;    // rect/disc center, sphere center, point position
  Vec3 ex, ey, n;   // rect half-extent vectors (disc: radius vectors) and emission normal
  float area = 0.0f;
  float radius = 0.0f;
  Vec3 dir;         // spot: emission direction; sun: direction toward the sun
  float cosHot = 1.0f, cosFall = 0.0f;
  bool decay = true;
  float cosSun = 1.0f;
  float spreadK = 0.0f;              // directional emission exponent (0 = Lambertian)
  const Texture* texture = nullptr;  // rect/disc texture

  // IES: profile and the light's world-to-local rotation (photometric nadir = local -Z).
  std::shared_ptr<IesProfile> ies;
  Vec3 lx, ly, lz;
  float iesScale = 1.0f;

  // Mesh lights: world-space triangles and their area distribution.
  std::vector<Vec3> tri;          // 3 corners per triangle
  std::vector<float> triCdf;      // cumulative area / total
  int meshGeom = -1;              // renderer geometry holding the triangles
  // Emitted radiance at a point of triangle t with barycentrics b, seen from the front (or back).
  std::function<Vec3(int t, const Vec2& b, bool front)> meshRadiance;

  const Environment* env = nullptr;  // portals sample the environment through their opening

  // Affect flags and linking.
  bool affectDiffuse = true, affectSpecular = true, affectReflections = true;
  float diffuseMult = 1.0f, specularMult = 1.0f;
  LightLink link;
  int group = 0;
  uint32_t nodeId = 0;
  std::string name;

  bool isArea() const { return type == Type::Rect || type == Type::Disc || type == Type::Sphere || type == Type::Portal; }
  bool isDelta() const { return type == Type::Omni || type == Type::Spot || type == Type::Ies || type == Type::Sun; }
  bool sample(const Vec3& p, float u1, float u2, LightSample& s) const;
  // Ray hit test for analytic area lights. Returns the distance, emitted radiance toward the ray
  // origin (zero when seen from the back) and the solid-angle pdf sample() would have used.
  bool intersect(const Vec3& o, const Vec3& d, float tMax, float& t, Vec3& Le, float& pdf) const;
  // Solid-angle pdf of sample() for a mesh-light point (distance and cosine at the light).
  float meshPdf(float dist2, float cosLight) const;
  // Approximate emitted power, used to pick lights when there are many.
  float power() const;

 private:
  Vec3 areaRadiance(const Vec3& localUv, float cosL) const;
};

// Names used for the non-light groups of Light Mix.
constexpr const char* kEnvironmentGroup = "Environment";
constexpr const char* kSelfIllumGroup = "Self-illumination";

struct LightBuildResult {
  std::vector<Light> lights;
  // Physical sun found (VeraaSun/Sun Positioner first, then the standard Sun).
  bool hasSun = false;
  bool physicalSun = false;  // VeraaSun: photometric units
  Vec3 sunDir{0, 0, 1};
  float sunIrradiance = 0.0f;  // luminance irradiance of the sun (for the sky)
  float sunTurbidity = 2.5f, sunOzone = 0.35f, sunIntensity = 1.0f, sunSize = 1.0f;
  int sunSkyModel = 0;
  Vec3 sunGroundAlbedo{0.2f, 0.2f, 0.2f};
  Vec3 sunFilter{1, 1, 1};
  bool sunInvisible = false;
  float sunCos = 1.0f;          // cosine of the sun disk's angular radius
  Vec3 sunColor;                // irradiance color of the sun at the ground
  // Dome light (replaces the environment for lighting).
  bool hasDome = false;
  std::string domeTexture;
  Vec3 domeColor{1, 1, 1};
  float domeRotation = 0.0f;
  bool domeSpherical = true;
  bool domeInvisible = false;
  int domeGroup = -1;
  uint32_t domeNode = 0;
  // Mesh lights to build from scene nodes (geometry is added by the renderer).
  struct MeshLightRequest {
    int lightIndex;
    uint32_t sourceNode;
  };
  std::vector<MeshLightRequest> meshRequests;
  std::vector<uint32_t> hiddenByMeshLights;  // nodes replaced by mesh lights
  std::vector<std::string> groupNames;        // light mix groups
};

// Collects enabled lights. Light groups are assigned by name; group 0 is the environment and
// group 1 is self-illumination.
LightBuildResult collectLights(const Scene& scene, TextureCache& cache, std::vector<std::string>* warnings);

// Physical sun illuminance (lux) at the ground for a sun elevation, with a simple atmosphere.
Vec3 physicalSunIrradiance(float elevationRad, float turbidity, float ozone);

}  // namespace render
}  // namespace veraa
