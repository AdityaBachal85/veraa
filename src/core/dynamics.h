// Rigid body dynamics: bodies with sphere, box or convex shapes, gravity, bounce and friction,
// simulated with sequential impulses and baked into keyframes.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "animation.h"
#include "modifier.h"

namespace veraa {

enum class BodyType { Dynamic, Kinematic, Static };
enum class BodyShape { Sphere, Box, Convex };

struct RigidBody {
  BodyType type = BodyType::Dynamic;
  BodyShape shape = BodyShape::Box;
  float mass = 1.0f, bounce = 0.5f, friction = 0.5f;
  float radius = 1.0f;         // sphere
  Vec3 halfExtents{1, 1, 1};   // box, and the bounding box of convex shapes
  std::vector<Vec3> points;    // convex: hull sample points (body space)
  Vec3 pos;                    // centre of mass (world)
  Quat rot;
  Vec3 vel, angVel;            // units per second, radians per second (world)
  uint32_t nodeId = 0;
  Mat4 nodeOffset;             // node world = body matrix * nodeOffset

  float invMass() const { return type == BodyType::Dynamic && mass > 0.0f ? 1.0f / mass : 0.0f; }
  Vec3 invInertiaLocal() const;  // diagonal
  Mat4 matrix() const;           // body to world (rotation + translation)
  // Corner/surface sample points in world space (boxes and convex shapes).
  std::vector<Vec3> worldPoints() const;
};

struct RigidWorldSettings {
  Vec3 gravity{0.0f, 0.0f, -980.0f};  // units per second^2
  bool groundPlane = true;
  float groundZ = 0.0f;
  float groundBounce = 0.5f, groundFriction = 0.5f;
  int substeps = 4;     // per frame
  int iterations = 12;  // solver iterations per substep
  float fps = 30.0f;
};

class RigidWorld {
 public:
  std::vector<RigidBody> bodies;
  RigidWorldSettings settings;
  // Kinematic bodies: called before each step with the step's end time (seconds from start)
  // to place them; their velocities are derived from the motion.
  std::function<void(RigidBody& body, double time)> kinematicPose;

  // Advances by dt seconds.
  void step(double dt);
  // Advances one frame (settings.substeps steps).
  void stepFrame();
  double time() const { return time_; }
  double kineticEnergy() const;

 private:
  struct Contact {
    int a = -1, b = -1;  // b = -1 for the ground
    Vec3 point, normal;  // normal points from b towards a
    float depth = 0.0f;
    float bounceTarget = 0.0f;
    float jn = 0.0f, jt = 0.0f;
  };
  void collide(std::vector<Contact>& out) const;
  double time_ = 0.0;
};

// Rigid Body modifier: marks a node as a body (parameters: bodyType, shape, mass, bounce,
// friction, initial velocity and spin).
class RigidBodyModifier : public Modifier {
 public:
  RigidBodyModifier();
  std::string type() const override { return "RigidBody"; }
  std::unique_ptr<Modifier> clone() const override { return std::make_unique<RigidBodyModifier>(*this); }
};

// Builds a body from a node (its Rigid Body modifier, mesh and pose at `frame`).
bool rigidBodyFromNode(const Scene& scene, const Node& n, double frame, RigidBody& out);

struct BakeResult {
  int frames = 0;
  int bodies = 0;
};

// Simulates every node with a Rigid Body modifier from startFrame to endFrame and stores the
// motion of dynamic bodies as position and rotation keys. progress(frame) returns false to stop.
BakeResult bakeRigidBodies(Scene& scene, int startFrame, int endFrame, const RigidWorldSettings& settings,
                           const std::function<bool(int)>& progress = {});
// Removes the baked position/rotation keys of dynamic bodies.
int unbakeRigidBodies(Scene& scene);

}  // namespace veraa
