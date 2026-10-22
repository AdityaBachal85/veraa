// What texture maps and materials see at a shading point.
#pragma once

#include <cstdint>

#include "common.h"

namespace veraa {
namespace render {

// Scene queries for maps that look around (Ambient Occlusion, Edges, Distance).
class Tracer {
 public:
  virtual ~Tracer() = default;
  // Distance to the nearest surface along the ray (or tMax when nothing is hit) and the hit
  // node id (0 when nothing is hit).
  virtual float traceDistance(const Vec3& o, const Vec3& d, float tMax, uint32_t* nodeId) const = 0;
  // Triangle corners in world space and polygon-edge flags (bit i: edge from corner i to i+1).
  virtual bool triangle(int geom, unsigned prim, Vec3 corners[3], unsigned& edgeMask) const = 0;
  // Distance from p to the surfaces of a distance field (maxDist when farther).
  virtual float distanceTo(int field, const Vec3& p, float maxDist) const = 0;
};

// Kind of ray that reached a surface (Override material, light linking of reflections, ...).
enum class RayKind : uint8_t { Camera, Diffuse, Reflect, Refract, Shadow };

struct ShadeContext {
  Vec3 p;               // world position
  Vec3 pObj;            // object-space position
  Vec3 ng;              // geometric normal (outward side of the mesh)
  Vec3 ns;              // shading normal (outward side; bump-mapped after Material::perturb)
  Vec3 nObj;            // object-space shading normal
  Vec3 wo;              // toward the viewer
  Vec2 uv;
  Vec3 dpdu, dpdv;      // world-space derivatives of position with respect to uv (may be zero)
  Vec3 objZ{0, 0, 1};   // object's local Z axis in world space
  float footprint = 0.0f;  // approximate texture-space filter width (0 = sharpest)
  float cameraDistance = 0.0f;
  Vec3 lightDir{0, 0, 1};  // direction toward the brightest light (Falloff Shadow/Light)

  uint32_t nodeId = 0;
  uint32_t instanceId = 0;
  int faceMatId = 0;       // material ID of the face (0-based)
  int elementId = 0;       // connected mesh element
  int geom = -1;           // renderer geometry record
  unsigned prim = 0;
  Vec2 bary;               // barycentric (u, v) of the hit in its triangle
  bool outside = true;     // the viewer is on the outward side
  RayKind rayKind = RayKind::Camera;
  int depth = 0;

  const Tracer* tracer = nullptr;
  Rng* rng = nullptr;
  float time = 0.0f;
};

}  // namespace render
}  // namespace veraa
