#include "dynamics.h"

#include <algorithm>
#include <cmath>

#include "registry.h"
#include "scene.h"

namespace veraa {
namespace {

Mat4 rotationOnly(const Mat4& m) {
  Mat4 r;
  for (int c = 0; c < 3; ++c) {
    Vec3 col = m.column(c);
    float l = length(col);
    if (l > 1e-12f) col /= l;
    r.m[0][c] = col.x;
    r.m[1][c] = col.y;
    r.m[2][c] = col.z;
  }
  return r;
}

// World inverse inertia applied to a vector.
Vec3 applyInvInertia(const RigidBody& b, const Vec3& v) {
  if (b.type != BodyType::Dynamic) return Vec3(0.0f);
  Mat4 r = b.rot.toMatrix();
  Vec3 local = r.transposed().transformDir(v);
  Vec3 ii = b.invInertiaLocal();
  return r.transformDir(local * ii);
}

Vec3 velocityAt(const RigidBody& b, const Vec3& p) { return b.vel + cross(b.angVel, p - b.pos); }

// Point inside an oriented box: penetration and outward normal (world).
bool pointInBox(const RigidBody& box, const Vec3& p, Vec3& normal, float& depth) {
  Mat4 r = box.rot.toMatrix();
  Vec3 l = r.transposed().transformDir(p - box.pos);
  Vec3 h = box.halfExtents;
  if (std::fabs(l.x) > h.x || std::fabs(l.y) > h.y || std::fabs(l.z) > h.z) return false;
  float best = kInfinity;
  int axis = 0;
  for (int k = 0; k < 3; ++k) {
    float d = h[k] - std::fabs(l[k]);
    if (d < best) {
      best = d;
      axis = k;
    }
  }
  Vec3 n(0.0f);
  n[axis] = l[axis] >= 0.0f ? 1.0f : -1.0f;
  normal = r.transformDir(n);
  depth = best;
  return true;
}

// Sphere against an oriented box: contact on the box surface.
bool sphereBox(const Vec3& c, float radius, const RigidBody& box, Vec3& point, Vec3& normal, float& depth) {
  Mat4 r = box.rot.toMatrix();
  Vec3 l = r.transposed().transformDir(c - box.pos);
  Vec3 h = box.halfExtents;
  Vec3 q(clampf(l.x, -h.x, h.x), clampf(l.y, -h.y, h.y), clampf(l.z, -h.z, h.z));
  Vec3 d = l - q;
  float dist = length(d);
  if (dist > radius) return false;
  if (dist > 1e-6f) {
    normal = r.transformDir(d / dist);
    depth = radius - dist;
    point = box.pos + r.transformDir(q);
    return true;
  }
  // Centre inside the box.
  Vec3 n;
  float pen;
  if (!pointInBox(box, c, n, pen)) return false;
  normal = n;
  depth = pen + radius;
  point = c - n * pen;
  return true;
}

}  // namespace

Vec3 RigidBody::invInertiaLocal() const {
  if (type != BodyType::Dynamic || mass <= 0.0f) return Vec3(0.0f);
  if (shape == BodyShape::Sphere) {
    float i = 0.4f * mass * radius * radius;
    return Vec3(1.0f / std::max(i, 1e-9f));
  }
  Vec3 s = halfExtents * 2.0f;
  float ix = mass / 12.0f * (s.y * s.y + s.z * s.z), iy = mass / 12.0f * (s.x * s.x + s.z * s.z),
        iz = mass / 12.0f * (s.x * s.x + s.y * s.y);
  return Vec3(1.0f / std::max(ix, 1e-9f), 1.0f / std::max(iy, 1e-9f), 1.0f / std::max(iz, 1e-9f));
}

Mat4 RigidBody::matrix() const { return Mat4::translate(pos) * rot.toMatrix(); }

std::vector<Vec3> RigidBody::worldPoints() const {
  std::vector<Vec3> out;
  Mat4 m = matrix();
  if (shape == BodyShape::Convex && !points.empty()) {
    for (const auto& p : points) out.push_back(m.transformPoint(p));
    return out;
  }
  Vec3 h = halfExtents;
  // Corners, edge midpoints and face centres.
  for (int x = -1; x <= 1; ++x)
    for (int y = -1; y <= 1; ++y)
      for (int z = -1; z <= 1; ++z) {
        if (x == 0 && y == 0 && z == 0) continue;
        out.push_back(m.transformPoint(Vec3(h.x * x, h.y * y, h.z * z)));
      }
  return out;
}

void RigidWorld::collide(std::vector<Contact>& out) const {
  const auto& s = settings;
  int n = static_cast<int>(bodies.size());
  for (int i = 0; i < n; ++i) {
    const RigidBody& a = bodies[static_cast<size_t>(i)];
    if (a.type != BodyType::Dynamic) continue;
    if (s.groundPlane) {
      if (a.shape == BodyShape::Sphere) {
        float d = a.pos.z - a.radius - s.groundZ;
        if (d < 0.0f) {
          Contact c;
          c.a = i;
          c.normal = Vec3(0, 0, 1);
          c.point = Vec3(a.pos.x, a.pos.y, s.groundZ);
          c.depth = -d;
          out.push_back(c);
        }
      } else {
        for (const Vec3& p : a.worldPoints())
          if (p.z < s.groundZ) {
            Contact c;
            c.a = i;
            c.normal = Vec3(0, 0, 1);
            c.point = p;
            c.depth = s.groundZ - p.z;
            out.push_back(c);
          }
      }
    }
  }
  for (int i = 0; i < n; ++i)
    for (int j = i + 1; j < n; ++j) {
      const RigidBody& a = bodies[static_cast<size_t>(i)];
      const RigidBody& b = bodies[static_cast<size_t>(j)];
      if (a.type != BodyType::Dynamic && b.type != BodyType::Dynamic) continue;
      // Broad phase: bounding spheres.
      float ra = a.shape == BodyShape::Sphere ? a.radius : length(a.halfExtents);
      float rb = b.shape == BodyShape::Sphere ? b.radius : length(b.halfExtents);
      if (length(a.pos - b.pos) > ra + rb) continue;
      auto add = [&](int ia, int ib, const Vec3& point, const Vec3& normal, float depth) {
        Contact c;
        c.a = ia;
        c.b = ib;
        c.point = point;
        c.normal = normal;
        c.depth = depth;
        out.push_back(c);
      };
      if (a.shape == BodyShape::Sphere && b.shape == BodyShape::Sphere) {
        Vec3 d = a.pos - b.pos;
        float dist = length(d);
        if (dist < a.radius + b.radius && dist > 1e-6f) {
          Vec3 nrm = d / dist;
          add(i, j, b.pos + nrm * b.radius, nrm, a.radius + b.radius - dist);
        }
      } else if (a.shape == BodyShape::Sphere || b.shape == BodyShape::Sphere) {
        bool aIsSphere = a.shape == BodyShape::Sphere;
        const RigidBody& sph = aIsSphere ? a : b;
        const RigidBody& box = aIsSphere ? b : a;
        Vec3 point, nrm;
        float depth;
        if (sphereBox(sph.pos, sph.radius, box, point, nrm, depth)) {
          // nrm points from the box towards the sphere.
          if (aIsSphere) add(i, j, point, nrm, depth);
          else add(i, j, point, -nrm, depth);
        }
      } else {
        // Points of each body inside the other's box.
        for (const Vec3& p : a.worldPoints()) {
          Vec3 nrm;
          float depth;
          if (pointInBox(b, p, nrm, depth)) add(i, j, p, nrm, depth);
        }
        for (const Vec3& p : b.worldPoints()) {
          Vec3 nrm;
          float depth;
          if (pointInBox(a, p, nrm, depth)) add(i, j, p, -nrm, depth);
        }
      }
    }
}

void RigidWorld::step(double dtD) {
  float dt = static_cast<float>(dtD);
  if (dt <= 0.0f) return;
  // Kinematic bodies follow their animation.
  for (auto& b : bodies) {
    if (b.type != BodyType::Kinematic || !kinematicPose) continue;
    Vec3 p0 = b.pos;
    Quat q0 = b.rot;
    kinematicPose(b, time_ + dtD);
    b.vel = (b.pos - p0) / dt;
    Quat dq = b.rot * q0.conjugate();
    if (dq.w < 0) dq = Quat{-dq.x, -dq.y, -dq.z, -dq.w};
    Quat lg = Quat::log(dq.normalized());
    b.angVel = Vec3(static_cast<float>(lg.x), static_cast<float>(lg.y), static_cast<float>(lg.z)) * (2.0f / dt);
  }
  // Gravity.
  for (auto& b : bodies)
    if (b.type == BodyType::Dynamic) b.vel += settings.gravity * dt;

  std::vector<Contact> contacts;
  collide(contacts);
  const float restThreshold = std::max(1.0f, length(settings.gravity) * dt * 2.0f);
  for (auto& c : contacts) {
    RigidBody& a = bodies[static_cast<size_t>(c.a)];
    Vec3 vb = c.b >= 0 ? velocityAt(bodies[static_cast<size_t>(c.b)], c.point) : Vec3(0.0f);
    float vn = dot(velocityAt(a, c.point) - vb, c.normal);
    float e = c.b >= 0 ? 0.5f * (a.bounce + bodies[static_cast<size_t>(c.b)].bounce) : 0.5f * (a.bounce + settings.groundBounce);
    c.bounceTarget = vn < -restThreshold ? -e * vn : 0.0f;
  }
  for (int it = 0; it < settings.iterations; ++it) {
    for (auto& c : contacts) {
      RigidBody& a = bodies[static_cast<size_t>(c.a)];
      RigidBody* b = c.b >= 0 ? &bodies[static_cast<size_t>(c.b)] : nullptr;
      Vec3 ra = c.point - a.pos, rb = b ? c.point - b->pos : Vec3(0.0f);
      Vec3 rel = velocityAt(a, c.point) - (b ? velocityAt(*b, c.point) : Vec3(0.0f));
      float vn = dot(rel, c.normal);
      float imA = a.invMass(), imB = b ? b->invMass() : 0.0f;
      auto effMass = [&](const Vec3& dir) {
        float k = imA + imB + dot(dir, cross(applyInvInertia(a, cross(ra, dir)), ra));
        if (b) k += dot(dir, cross(applyInvInertia(*b, cross(rb, dir)), rb));
        return k;
      };
      float kn = effMass(c.normal);
      if (kn < 1e-12f) continue;
      float dj = (c.bounceTarget - vn) / kn;
      float jn = std::max(0.0f, c.jn + dj);
      dj = jn - c.jn;
      c.jn = jn;
      Vec3 imp = c.normal * dj;
      auto applyImpulse = [&](const Vec3& J) {
        a.vel += J * imA;
        a.angVel += applyInvInertia(a, cross(ra, J));
        if (b) {
          b->vel -= J * imB;
          b->angVel -= applyInvInertia(*b, cross(rb, J));
        }
      };
      applyImpulse(imp);
      // Friction.
      rel = velocityAt(a, c.point) - (b ? velocityAt(*b, c.point) : Vec3(0.0f));
      Vec3 tv = rel - c.normal * dot(rel, c.normal);
      float tl = length(tv);
      if (tl > 1e-6f) {
        Vec3 t = tv / tl;
        float kt = effMass(t);
        if (kt > 1e-12f) {
          float mu = b ? 0.5f * (a.friction + b->friction) : 0.5f * (a.friction + settings.groundFriction);
          float djt = -tl / kt;
          float jt = clampf(c.jt + djt, -mu * c.jn, mu * c.jn);
          djt = jt - c.jt;
          c.jt = jt;
          applyImpulse(t * djt);
        }
      }
    }
  }
  // Integrate.
  for (auto& b : bodies) {
    if (b.type != BodyType::Dynamic) continue;
    b.pos += b.vel * dt;
    Quat w{b.angVel.x, b.angVel.y, b.angVel.z, 0.0};
    Quat dq = w * b.rot;
    b.rot = Quat{b.rot.x + 0.5 * dt * dq.x, b.rot.y + 0.5 * dt * dq.y, b.rot.z + 0.5 * dt * dq.z, b.rot.w + 0.5 * dt * dq.w}
                .normalized();
    // Light damping keeps resting contacts calm.
    b.angVel *= 0.999f;
  }
  // Positional correction (split from velocities).
  for (const auto& c : contacts) {
    RigidBody& a = bodies[static_cast<size_t>(c.a)];
    RigidBody* b = c.b >= 0 ? &bodies[static_cast<size_t>(c.b)] : nullptr;
    float imA = a.invMass(), imB = b ? b->invMass() : 0.0f;
    float sum = imA + imB;
    if (sum <= 0.0f) continue;
    float corr = std::max(0.0f, c.depth - 0.01f) * 0.6f / sum;
    // Points of one body produce several contacts; spread the correction.
    corr /= 4.0f;
    a.pos += c.normal * (corr * imA);
    if (b) b->pos -= c.normal * (corr * imB);
  }
  time_ += dtD;
}

void RigidWorld::stepFrame() {
  int n = std::max(1, settings.substeps);
  double dt = 1.0 / (std::max(1.0f, settings.fps) * n);
  for (int i = 0; i < n; ++i) step(dt);
}

double RigidWorld::kineticEnergy() const {
  double e = 0.0;
  for (const auto& b : bodies)
    if (b.type == BodyType::Dynamic) e += 0.5 * b.mass * length2(b.vel);
  return e;
}

// ---------------------------------------------------------------------------------------------

RigidBodyModifier::RigidBodyModifier() {
  params.beginGroup("Rigid Body Properties");
  params.addEnum("bodyType", "Rigid Body Type", {"Dynamic", "Kinematic", "Static"}, 0);
  params.addEnum("shape", "Shape Type", {"Sphere", "Box", "Convex"}, 1);
  params.beginGroup("Physical Material");
  params.addFloat("mass", "Mass", 1.0, 0.0001, 1e9, 0.1, 3);
  params.addFloat("friction", "Friction", 0.5, 0.0, 2.0, 0.05);
  params.addFloat("bounce", "Bounciness", 0.5, 0.0, 1.0, 0.05);
  params.beginGroup("Initial Motion");
  params.addFloat("velX", "Velocity X", 0.0, -1e6, 1e6, 1.0);
  params.addFloat("velY", "Velocity Y", 0.0, -1e6, 1e6, 1.0);
  params.addFloat("velZ", "Velocity Z", 0.0, -1e6, 1e6, 1.0);
  params.addFloat("spinX", "Spin X (deg/s)", 0.0, -1e6, 1e6, 1.0);
  params.addFloat("spinY", "Spin Y (deg/s)", 0.0, -1e6, 1e6, 1.0);
  params.addFloat("spinZ", "Spin Z (deg/s)", 0.0, -1e6, 1e6, 1.0);
}

bool rigidBodyFromNode(const Scene& scene, const Node& n, double frame, RigidBody& out) {
  const Modifier* mod = nullptr;
  for (const auto& m : n.modifiers)
    if (m && m->enabled && m->type() == "RigidBody") mod = m.get();
  if (!mod) return false;
  const ParamBlock& p = mod->params;
  const PolyMesh& mesh = n.mesh();
  Aabb box = mesh.bounds();
  if (!box.valid()) return false;
  Mat4 world = nodeWorldMatrixAt(scene, n.id, frame);
  Mat4 objWorld = world * n.objectOffset.matrix();
  Vec3 scale(length(objWorld.column(0)), length(objWorld.column(1)), length(objWorld.column(2)));
  Mat4 rot = rotationOnly(objWorld);
  RigidBody b;
  b.nodeId = n.id;
  b.type = static_cast<BodyType>(std::clamp(p.getInt("bodyType"), 0, 2));
  b.shape = static_cast<BodyShape>(std::clamp(p.getInt("shape"), 0, 2));
  b.mass = p.getFloat("mass");
  b.bounce = p.getFloat("bounce");
  b.friction = p.getFloat("friction");
  b.halfExtents = box.extent() * 0.5f * scale;
  b.radius = maxComponent(b.halfExtents);
  b.pos = objWorld.transformPoint(box.center());
  b.rot = Quat::fromMatrix(rot);
  if (b.shape == BodyShape::Convex) {
    size_t stride = std::max<size_t>(1, mesh.positions.size() / 96);
    for (size_t i = 0; i < mesh.positions.size(); i += stride)
      b.points.push_back((mesh.positions[i] - box.center()) * scale);
  }
  b.vel = Vec3(p.getFloat("velX"), p.getFloat("velY"), p.getFloat("velZ"));
  b.angVel = Vec3(radians(p.getFloat("spinX")), radians(p.getFloat("spinY")), radians(p.getFloat("spinZ")));
  b.nodeOffset = b.matrix().inverse() * world;
  out = b;
  return true;
}

namespace {
bool isPosRotPath(const std::string& path, uint32_t id) {
  std::string prefix = "node:" + std::to_string(id) + "/";
  if (path.rfind(prefix, 0) != 0) return false;
  std::string ch = path.substr(prefix.size());
  return ch.rfind("pos.", 0) == 0 || ch.rfind("rot.", 0) == 0;
}
}  // namespace

BakeResult bakeRigidBodies(Scene& scene, int startFrame, int endFrame, const RigidWorldSettings& settings,
                           const std::function<bool(int)>& progress) {
  BakeResult r;
  Animation* anim = Animation::of(scene);
  if (!anim || endFrame <= startFrame) return r;
  RigidWorld world;
  world.settings = settings;
  for (const auto& up : scene.nodes) {
    RigidBody b;
    if (rigidBodyFromNode(scene, *up, startFrame, b)) world.bodies.push_back(b);
  }
  if (world.bodies.empty()) return r;
  world.kinematicPose = [&scene, startFrame, fps = settings.fps](RigidBody& b, double t) {
    const Node* n = scene.findNode(b.nodeId);
    if (!n) return;
    Mat4 w = nodeWorldMatrixAt(scene, b.nodeId, startFrame + t * fps);
    Mat4 m = w * b.nodeOffset.inverse();
    b.pos = m.translation();
    b.rot = Quat::fromMatrix(rotationOnly(m));
  };
  // Clear the old baked keys of dynamic bodies.
  for (const auto& b : world.bodies) {
    if (b.type != BodyType::Dynamic) continue;
    for (auto it = anim->tracks.begin(); it != anim->tracks.end();)
      it = isPosRotPath(it->first, b.nodeId) ? anim->tracks.erase(it) : std::next(it);
    ++r.bodies;
  }
  struct Rec {
    std::vector<Transform> frames;
  };
  std::vector<Rec> recs(world.bodies.size());
  auto record = [&](int frame) {
    for (size_t i = 0; i < world.bodies.size(); ++i) {
      const RigidBody& b = world.bodies[i];
      if (b.type != BodyType::Dynamic) continue;
      const Node* n = scene.findNode(b.nodeId);
      Mat4 nodeWorld = b.matrix() * b.nodeOffset;
      Mat4 parent = n && n->parentId ? nodeWorldMatrixAt(scene, n->parentId, frame) : Mat4::identity();
      Transform t = decomposeTransform(parent.inverse() * nodeWorld);
      if (n) t.scale = n->transform.scale;
      if (!recs[i].frames.empty()) {
        // Keep Euler angles continuous.
        const Vec3& prev = recs[i].frames.back().rotation;
        for (int k = 0; k < 3; ++k) t.rotation[k] += 360.0f * std::round((prev[k] - t.rotation[k]) / 360.0f);
      }
      recs[i].frames.push_back(t);
    }
  };
  record(startFrame);
  for (int f = startFrame + 1; f <= endFrame; ++f) {
    world.stepFrame();
    record(f);
    ++r.frames;
    if (progress && !progress(f)) break;
  }
  for (size_t i = 0; i < world.bodies.size(); ++i) {
    const RigidBody& b = world.bodies[i];
    if (b.type != BodyType::Dynamic) continue;
    for (int c = 0; c < 6; ++c) {
      Track tr;
      for (size_t k = 0; k < recs[i].frames.size(); ++k) {
        Key key;
        key.time = startFrame + static_cast<double>(k);
        const Transform& t = recs[i].frames[k];
        key.value = c < 3 ? t.position[c] : t.rotation[c - 3];
        key.inType = key.outType = TangentType::Linear;
        tr.keys.push_back(key);
      }
      anim->tracks[Animation::nodePath(b.nodeId, Animation::transformChannels()[static_cast<size_t>(c)])] = tr;
    }
  }
  anim->touch();
  anim->evaluate(scene, scene.time.currentFrame);
  return r;
}

int unbakeRigidBodies(Scene& scene) {
  Animation* anim = Animation::of(scene);
  if (!anim) return 0;
  int count = 0;
  for (const auto& up : scene.nodes) {
    bool dynamic = false;
    for (const auto& m : up->modifiers)
      if (m && m->type() == "RigidBody" && m->params.getInt("bodyType") == 0) dynamic = true;
    if (!dynamic) continue;
    size_t before = anim->tracks.size();
    for (auto it = anim->tracks.begin(); it != anim->tracks.end();)
      it = isPosRotPath(it->first, up->id) ? anim->tracks.erase(it) : std::next(it);
    if (anim->tracks.size() != before) ++count;
  }
  anim->touch();
  return count;
}

void registerDynamicsModifiers() {
  registerModifierType({"RigidBody", "Rigid Body", "Animation", true, false,
                        [] { return std::make_unique<RigidBodyModifier>(); }});
}

}  // namespace veraa
