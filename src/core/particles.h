// Particle systems (Spray, Snow, Super Spray), space warps (forces, deflectors and geometric
// deformers) and the binding modifier that connects nodes to space warps.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "modifier.h"
#include "object.h"

namespace veraa {

class Scene;
class Node;

struct Particle {
  Vec3 pos, vel;        // world space, units and units per frame
  float age = 0.0f;     // frames since birth
  float life = 30.0f;   // frames
  float size = 1.0f;
  uint32_t id = 0;
  Vec3 spinAxis{0, 0, 1};
  float spinPhase = 0.0f;
};

// Base of the particle systems. The mesh is the particles at the evaluation time, placed in the
// emitter's object space. Forces come from the space warps bound to the node (Space Warp
// Binding modifiers in its stack).
class ParticleSystemObject : public GeometryObject {
 public:
  ParticleSystemObject() = default;
  // Copies get their own simulation cache.
  ParticleSystemObject(const ParticleSystemObject& o) : GeometryObject(o) {}
  ParticleSystemObject& operator=(const ParticleSystemObject& o) {
    GeometryObject::operator=(o);
    cache_ = std::make_shared<Cache>();
    return *this;
  }
  bool timeDependent() const override { return true; }
  PolyMesh build() const override { return PolyMesh(); }
  PolyMesh buildAt(const GeomState& context) const override;

  // Live particles (world space) of a node at a frame.
  std::vector<Particle> simulate(const Scene& scene, uint32_t nodeId, double frame) const;

  // Emitter description used by the simulation.
  struct Emission {
    double start = 0.0, stop = 30.0;  // emission frames
    double rate = 10.0;               // particles per frame
    int maxAlive = 100000;
    double life = 30.0, lifeVariation = 0.0;
    int seed = 12345;
  };
  virtual Emission emission() const = 0;
  // A new particle in emitter object space (position, velocity in units per frame, size).
  virtual void spawn(uint32_t id, Vec3& pos, Vec3& vel, float& size, Vec3& spinAxis) const = 0;
  // Size multiplier over the particle's life (grow / fade).
  virtual float sizeScale(const Particle& p) const { return 1.0f; }
  // 0 tetrahedron, 1 cube, 2 facing quad, 3 six point star, 4 triangle, 5 sphere, 6 drop
  virtual int shape() const = 0;
  virtual float spinPeriod() const { return 0.0f; }

 private:
  struct Cache {
    size_t key = 0;
    uint32_t nodeId = 0;
    double time = -1e30;
    double birthAccum = 0.0;
    uint32_t nextId = 0;
    std::vector<Particle> particles;
    bool valid = false;
  };
  mutable std::shared_ptr<Cache> cache_ = std::make_shared<Cache>();
};

class SprayObject : public ParticleSystemObject {
 public:
  SprayObject();
  std::string type() const override { return "Spray"; }
  std::unique_ptr<SceneObject> clone() const override;
  Emission emission() const override;
  void spawn(uint32_t id, Vec3& pos, Vec3& vel, float& size, Vec3& spinAxis) const override;
  int shape() const override;
};

class SnowObject : public ParticleSystemObject {
 public:
  SnowObject();
  std::string type() const override { return "Snow"; }
  std::unique_ptr<SceneObject> clone() const override;
  Emission emission() const override;
  void spawn(uint32_t id, Vec3& pos, Vec3& vel, float& size, Vec3& spinAxis) const override;
  int shape() const override;
  float spinPeriod() const override;
};

class SuperSprayObject : public ParticleSystemObject {
 public:
  SuperSprayObject();
  std::string type() const override { return "SuperSpray"; }
  std::unique_ptr<SceneObject> clone() const override;
  Emission emission() const override;
  void spawn(uint32_t id, Vec3& pos, Vec3& vel, float& size, Vec3& spinAxis) const override;
  float sizeScale(const Particle& p) const override;
  int shape() const override;
  float spinPeriod() const override;
};

// Space warps. Forces act on bound particle systems; deflectors bounce them; Ripple and Wave
// deform bound geometry.
class ForceWarpObject : public SpaceWarpObject {
 public:
  // Acceleration (units per frame^2) at a world position, or a velocity change for deflectors.
  // warpWorld: the warp's world matrix at `frame`.
  virtual Vec3 acceleration(const Scene& scene, const Mat4& warpWorld, const Particle& p, double frame,
                            float fps) const {
    return Vec3(0.0f);
  }
  // Velocity damping per frame (Drag), 0 = none.
  virtual float damping(const Mat4& warpWorld, const Particle& p, double frame) const { return 0.0f; }
  // Collision with the step from p0 to p1 (world). Adjusts pos/vel and returns true on a hit.
  virtual bool deflect(const Scene& scene, const Mat4& warpWorld, const Vec3& p0, Particle& p, uint32_t seed) const {
    return false;
  }
  // Geometric deformation of a world-space point (Ripple, Wave).
  virtual bool deforms() const { return false; }
  virtual Vec3 deform(const Mat4& warpWorld, const Mat4& warpInverse, const Vec3& worldPoint, float flexibility) const {
    return worldPoint;
  }
};

// Connects a node to a space warp (param "warp" = warp node id). Geometric warps deform the
// node's mesh; force warps are read by particle systems.
class SpaceWarpBindingModifier : public Modifier {
 public:
  SpaceWarpBindingModifier();
  std::string type() const override { return "SpaceWarpBinding"; }
  std::unique_ptr<Modifier> clone() const override { return std::make_unique<SpaceWarpBindingModifier>(*this); }
  void applyState(GeomState& state) const override;
  std::vector<uint32_t> referencedNodes() const override;
};

// Whether a node holds a space warp that can be bound to `target`.
bool canBindToSpaceWarp(const Node& target, const Node& warp);
// Adds a binding modifier for `warp` to `target` (no-op if already bound). Returns true if added.
bool bindToSpaceWarp(Node& target, const Node& warp);

}  // namespace veraa
