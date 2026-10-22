#include "particles.h"

#include <algorithm>
#include <cmath>
#include <functional>

#include "animation.h"
#include "registry.h"
#include "scene.h"

namespace veraa {
namespace {

float hash01(uint32_t a, uint32_t b, uint32_t c) {
  uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u ^ (c + 0x165667B1u) * 0xC2B2AE3Du;
  h ^= h >> 15;
  h *= 0x2C1B3C6Du;
  h ^= h >> 12;
  h *= 0x297A2D39u;
  h ^= h >> 15;
  return static_cast<float>(h >> 8) * (1.0f / 16777216.0f);
}

// Symmetric random in [-1, 1].
float hashS(uint32_t a, uint32_t b, uint32_t c) { return hash01(a, b, c) * 2.0f - 1.0f; }

Vec3 randomUnit(uint32_t seed, uint32_t id, uint32_t k) {
  float z = hashS(seed, id, k), a = hash01(seed, id, k + 1) * kTwoPi;
  float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
  return Vec3(r * std::cos(a), r * std::sin(a), z);
}

// Gravity-like acceleration for strength 1, in units per frame^2 (980 units/s^2).
float baseAccel(float fps) { return 980.0f / std::max(1.0f, fps * fps); }

Mat4 emitterMatrix(const Scene& s, const Node& n, double frame) {
  return nodeWorldMatrixAt(s, n.id, frame) * n.objectOffset.matrix();
}

bool inTime(const ParamBlock& p, double frame) {
  return frame >= p.getFloat("timeOn") && frame <= p.getFloat("timeOff");
}

void addTimeParams(ParamBlock& p) {
  p.beginGroup("Timing");
  p.addFloat("timeOn", "Time On", 0.0, -1e6, 1e6, 1.0, 0);
  p.addFloat("timeOff", "Time Off", 100.0, -1e6, 1e6, 1.0, 0);
}

void addDeflectorParams(ParamBlock& p) {
  p.beginGroup("Parameters");
  p.addFloat("bounce", "Bounce", 1.0, 0.0, 100.0, 0.05, 2);
  p.addFloat("variation", "Variation", 0.0, 0.0, 100.0, 1.0, 1);
  p.addFloat("chaos", "Chaos", 0.0, 0.0, 100.0, 1.0, 1);
  p.addFloat("friction", "Friction", 0.0, 0.0, 100.0, 1.0, 1);
}

// Reflects a velocity on a surface with normal n (unit, world), with bounce, variation, chaos and
// friction (percent).
Vec3 bounceVelocity(const ParamBlock& p, const Vec3& v, const Vec3& n, uint32_t seed, uint32_t id) {
  float vn = dot(v, n);
  Vec3 normal = n * vn, tangent = v - normal;
  float bounce = p.getFloat("bounce") * (1.0f + hashS(seed, id, 91) * p.getFloat("variation") / 100.0f);
  Vec3 out = tangent * (1.0f - p.getFloat("friction") / 100.0f) - normal * bounce;
  float chaos = p.getFloat("chaos") / 100.0f;
  if (chaos > 0.0f) {
    float speed = length(out);
    Vec3 dir = normalize(lerp(normalize(out), randomUnit(seed, id, 93), chaos));
    if (dot(dir, n) * vn > 0.0f) dir = dir - n * (2.0f * dot(dir, n));  // keep it on the outgoing side
    out = dir * speed;
  }
  return out;
}

// ---- Force warps ----

class GravityWarp : public ForceWarpObject {
 public:
  GravityWarp() {
    params.beginGroup("Force");
    params.addFloat("strength", "Strength", 1.0, -1e6, 1e6, 0.01, 2);
    params.addFloat("decay", "Decay", 0.0, 0.0, 1e6, 0.001, 3);
    params.addEnum("mode", "Shape", {"Planar", "Spherical"}, 0);
    params.beginGroup("Display");
    params.addFloat("iconSize", "Icon Size", 10.0, 0.0, 1e6, 0.5);
  }
  std::string type() const override { return "Gravity"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<GravityWarp>(*this); }
  Vec3 acceleration(const Scene&, const Mat4& w, const Particle& p, double, float fps) const override {
    Vec3 o = w.translation();
    Vec3 dir;
    float dist;
    if (params.getInt("mode") == 1) {
      Vec3 d = o - p.pos;
      dist = length(d);
      if (dist < 1e-6f) return Vec3(0.0f);
      dir = d / dist;
    } else {
      dir = normalize(-w.column(2));
      dist = std::fabs(dot(p.pos - o, dir));
    }
    float decay = std::exp(-params.getFloat("decay") * dist);
    return dir * (params.getFloat("strength") * baseAccel(fps) * decay);
  }
};

class WindWarp : public ForceWarpObject {
 public:
  WindWarp() {
    params.beginGroup("Force");
    params.addFloat("strength", "Strength", 1.0, -1e6, 1e6, 0.01, 2);
    params.addFloat("decay", "Decay", 0.0, 0.0, 1e6, 0.001, 3);
    params.addEnum("mode", "Shape", {"Planar", "Spherical"}, 0);
    params.beginGroup("Wind");
    params.addFloat("turbulence", "Turbulence", 0.0, 0.0, 1e6, 0.01, 2);
    params.addFloat("frequency", "Frequency", 0.0, 0.0, 1e6, 0.01, 2);
    params.addFloat("scale", "Scale", 1.0, 0.0, 1e6, 0.01, 2);
    params.beginGroup("Display");
    params.addFloat("iconSize", "Icon Size", 10.0, 0.0, 1e6, 0.5);
  }
  std::string type() const override { return "Wind"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<WindWarp>(*this); }
  Vec3 acceleration(const Scene&, const Mat4& w, const Particle& p, double frame, float fps) const override {
    Vec3 o = w.translation();
    Vec3 dir;
    float dist;
    if (params.getInt("mode") == 1) {
      Vec3 d = p.pos - o;
      dist = length(d);
      if (dist < 1e-6f) return Vec3(0.0f);
      dir = d / dist;
    } else {
      dir = normalize(-w.column(2));
      dist = std::fabs(dot(p.pos - o, dir));
    }
    float g = baseAccel(fps);
    float decay = std::exp(-params.getFloat("decay") * dist);
    Vec3 a = dir * (params.getFloat("strength") * g * decay);
    float turb = params.getFloat("turbulence");
    if (turb > 0.0f) {
      float sc = 0.01f * params.getFloat("scale");
      float ph = static_cast<float>(frame) * params.getFloat("frequency") * 0.1f;
      Vec3 q = p.pos * sc;
      a += Vec3(perlinNoise(q.x + ph, q.y, q.z), perlinNoise(q.x, q.y + ph + 17.3f, q.z),
                perlinNoise(q.x, q.y, q.z + ph + 41.9f)) *
           (turb * g * decay);
    }
    return a;
  }
};

class DragWarp : public ForceWarpObject {
 public:
  DragWarp() {
    addTimeParams(params);
    params.beginGroup("Damping Characteristics");
    params.addFloat("damping", "Linear Damping", 5.0, 0.0, 100.0, 0.5, 1);
    params.addFloat("range", "Range (0 = Unlimited)", 0.0, 0.0, 1e6, 1.0);
    params.beginGroup("Display");
    params.addFloat("iconSize", "Icon Size", 10.0, 0.0, 1e6, 0.5);
  }
  std::string type() const override { return "Drag"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<DragWarp>(*this); }
  float damping(const Mat4& w, const Particle& p, double frame) const override {
    if (!inTime(params, frame)) return 0.0f;
    float range = params.getFloat("range");
    if (range > 0.0f && length(p.pos - w.translation()) > range) return 0.0f;
    return params.getFloat("damping") / 100.0f;
  }
};

class VortexWarp : public ForceWarpObject {
 public:
  VortexWarp() {
    addTimeParams(params);
    params.beginGroup("Vortex Shape");
    params.addFloat("taperLength", "Taper Length", 100.0, 0.0, 1e6, 1.0);
    params.addFloat("taperShape", "Taper Curve", 1.0, 0.001, 4.0, 0.05);
    params.beginGroup("Capture and Motion");
    params.addFloat("axialDrop", "Axial Drop", 0.1, -1e6, 1e6, 0.01);
    params.addFloat("orbitalSpeed", "Orbital Speed", 0.3, -1e6, 1e6, 0.01);
    params.addFloat("radialPull", "Radial Pull", 0.5, -1e6, 1e6, 0.01);
    params.addEnum("rotation", "Rotation", {"Counter-Clockwise", "Clockwise"}, 0);
    params.beginGroup("Display");
    params.addFloat("iconSize", "Icon Size", 10.0, 0.0, 1e6, 0.5);
  }
  std::string type() const override { return "Vortex"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<VortexWarp>(*this); }
  Vec3 acceleration(const Scene&, const Mat4& w, const Particle& p, double frame, float) const override {
    if (!inTime(params, frame)) return Vec3(0.0f);
    Vec3 axis = normalize(w.column(2)), o = w.translation();
    Vec3 rel = p.pos - o;
    float h = dot(rel, axis);
    Vec3 radial = rel - axis * h;
    float r = length(radial);
    if (r < 1e-5f) return -axis * params.getFloat("axialDrop");
    Vec3 rh = radial / r;
    Vec3 tang = cross(axis, rh) * (params.getInt("rotation") == 1 ? -1.0f : 1.0f);
    // Taper: the vortex narrows towards its base; the pull is stronger near the axis.
    float tl = std::max(1e-3f, params.getFloat("taperLength"));
    float taper = std::pow(clampf(std::fabs(h) / tl, 0.05f, 1.0f), params.getFloat("taperShape"));
    float targetR = tl * 0.25f * taper;
    // Orbital speed is a target tangential velocity; steer towards it.
    float vt = dot(p.vel, tang);
    Vec3 a = tang * ((params.getFloat("orbitalSpeed") * 10.0f - vt) * 0.2f);
    a -= rh * (params.getFloat("radialPull") * 0.05f * (r - targetR) / std::max(targetR, 1e-3f));
    a -= axis * params.getFloat("axialDrop");
    return a;
  }
};

class PushWarp : public ForceWarpObject {
 public:
  PushWarp() {
    addTimeParams(params);
    params.beginGroup("Strength Control");
    params.addFloat("strength", "Basic Force", 1.0, -1e6, 1e6, 0.01, 2);
    params.addFloat("range", "Range (0 = Unlimited)", 0.0, 0.0, 1e6, 1.0);
    params.beginGroup("Display");
    params.addFloat("iconSize", "Icon Size", 10.0, 0.0, 1e6, 0.5);
  }
  std::string type() const override { return "Push"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<PushWarp>(*this); }
  Vec3 acceleration(const Scene&, const Mat4& w, const Particle& p, double frame, float) const override {
    if (!inTime(params, frame)) return Vec3(0.0f);
    float range = params.getFloat("range");
    if (range > 0.0f && length(p.pos - w.translation()) > range) return Vec3(0.0f);
    return normalize(w.column(2)) * (params.getFloat("strength") * 0.1f);
  }
};

class MotorWarp : public ForceWarpObject {
 public:
  MotorWarp() {
    addTimeParams(params);
    params.beginGroup("Strength Control");
    params.addFloat("strength", "Basic Torque", 1.0, -1e6, 1e6, 0.01, 2);
    params.addFloat("range", "Range (0 = Unlimited)", 0.0, 0.0, 1e6, 1.0);
    params.beginGroup("Display");
    params.addFloat("iconSize", "Icon Size", 10.0, 0.0, 1e6, 0.5);
  }
  std::string type() const override { return "Motor"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<MotorWarp>(*this); }
  Vec3 acceleration(const Scene&, const Mat4& w, const Particle& p, double frame, float) const override {
    if (!inTime(params, frame)) return Vec3(0.0f);
    Vec3 axis = normalize(w.column(2));
    Vec3 rel = p.pos - w.translation();
    float range = params.getFloat("range");
    if (range > 0.0f && length(rel) > range) return Vec3(0.0f);
    Vec3 radial = rel - axis * dot(rel, axis);
    if (length2(radial) < 1e-10f) return Vec3(0.0f);
    return normalize(cross(axis, radial)) * (params.getFloat("strength") * 0.1f);
  }
};

class PBombWarp : public ForceWarpObject {
 public:
  PBombWarp() {
    params.beginGroup("Blast Symmetry");
    params.addEnum("mode", "Symmetry", {"Spherical", "Cylindrical", "Planar"}, 0);
    params.addFloat("chaos", "Chaos", 0.0, 0.0, 100.0, 1.0, 1);
    params.beginGroup("Explosion Parameters");
    params.addFloat("startTime", "Start Time", 30.0, -1e6, 1e6, 1.0, 0);
    params.addFloat("duration", "Duration", 1.0, 0.0, 1e6, 1.0, 0);
    params.addFloat("strength", "Strength", 0.3, -1e6, 1e6, 0.01, 2);
    params.addFloat("range", "Range (0 = Unlimited)", 0.0, 0.0, 1e6, 1.0);
    params.addEnum("decayType", "Decay", {"Unlimited", "Linear", "Exponential"}, 0);
    params.beginGroup("Display");
    params.addFloat("iconSize", "Icon Size", 10.0, 0.0, 1e6, 0.5);
  }
  std::string type() const override { return "PBomb"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<PBombWarp>(*this); }
  Vec3 acceleration(const Scene&, const Mat4& w, const Particle& p, double frame, float) const override {
    double t0 = params.getFloat("startTime");
    double dur = std::max(1.0f, params.getFloat("duration"));
    if (frame < t0 || frame > t0 + dur) return Vec3(0.0f);
    Vec3 rel = p.pos - w.translation(), axis = normalize(w.column(2));
    Vec3 dir;
    switch (params.getInt("mode")) {
      case 1: dir = rel - axis * dot(rel, axis); break;
      case 2: dir = axis * (dot(rel, axis) >= 0.0f ? 1.0f : -1.0f); break;
      default: dir = rel; break;
    }
    float dist = length(dir);
    if (dist < 1e-6f) return Vec3(0.0f);
    dir /= dist;
    float range = params.getFloat("range"), falloff = 1.0f;
    int decay = params.getInt("decayType");
    if (range > 0.0f && decay != 0) {
      if (dist > range) return Vec3(0.0f);
      falloff = decay == 1 ? 1.0f - dist / range : std::exp(-3.0f * dist / range);
    }
    float chaos = params.getFloat("chaos") / 100.0f;
    if (chaos > 0.0f) dir = normalize(lerp(dir, randomUnit(7, p.id, 3), chaos));
    return dir * (params.getFloat("strength") * 10.0f * falloff / static_cast<float>(dur));
  }
};

class DeflectorWarp : public ForceWarpObject {
 public:
  DeflectorWarp() {
    addDeflectorParams(params);
    params.beginGroup("Display Icon");
    params.addFloat("width", "Width", 50.0, 0.0, 1e6, 1.0);
    params.addFloat("length", "Length", 50.0, 0.0, 1e6, 1.0);
  }
  std::string type() const override { return "Deflector"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<DeflectorWarp>(*this); }
  bool deflect(const Scene&, const Mat4& w, const Vec3& p0, Particle& p, uint32_t seed) const override {
    Mat4 inv = w.inverse();
    Vec3 a = inv.transformPoint(p0), b = inv.transformPoint(p.pos);
    if ((a.z > 0.0f) == (b.z > 0.0f) || std::fabs(a.z - b.z) < 1e-12f) return false;
    float t = a.z / (a.z - b.z);
    Vec3 hit = lerp(a, b, t);
    if (std::fabs(hit.x) > params.getFloat("width") * 0.5f || std::fabs(hit.y) > params.getFloat("length") * 0.5f)
      return false;
    Vec3 n = normalize(w.column(2));
    if (a.z < 0.0f) n = -n;
    Vec3 wh = w.transformPoint(hit);
    p.vel = bounceVelocity(params, p.vel, n, seed, p.id);
    p.pos = wh + n * 1e-3f + p.vel * ((1.0f - t) * 0.0f);
    return true;
  }
};

class SDeflectorWarp : public ForceWarpObject {
 public:
  SDeflectorWarp() {
    addDeflectorParams(params);
    params.beginGroup("Display Icon");
    params.addFloat("diameter", "Diameter", 50.0, 0.0, 1e6, 1.0);
  }
  std::string type() const override { return "SDeflector"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<SDeflectorWarp>(*this); }
  bool deflect(const Scene&, const Mat4& w, const Vec3& p0, Particle& p, uint32_t seed) const override {
    Vec3 c = w.translation();
    float r = params.getFloat("diameter") * 0.5f * length(w.column(0));
    bool in0 = length(p0 - c) < r, in1 = length(p.pos - c) < r;
    if (in0 == in1) return false;
    // Segment / sphere intersection.
    Vec3 d = p.pos - p0, m = p0 - c;
    float A = dot(d, d), B = 2.0f * dot(m, d), C = dot(m, m) - r * r;
    float disc = B * B - 4.0f * A * C;
    if (A < 1e-12f || disc < 0.0f) return false;
    float sq = std::sqrt(disc);
    float t = in0 ? (-B + sq) / (2.0f * A) : (-B - sq) / (2.0f * A);
    t = clampf(t, 0.0f, 1.0f);
    Vec3 hit = p0 + d * t;
    Vec3 n = normalize(hit - c);
    if (in0) n = -n;
    p.vel = bounceVelocity(params, p.vel, n, seed, p.id);
    p.pos = hit + n * 1e-3f;
    return true;
  }
};

class UDeflectorWarp : public ForceWarpObject {
 public:
  UDeflectorWarp() {
    params.beginGroup("Object-Based Deflector");
    params.addInt("object", "Object", 0, 0, 2147483647);
    addDeflectorParams(params);
    params.beginGroup("Display Icon");
    params.addFloat("iconSize", "Icon Size", 10.0, 0.0, 1e6, 0.5);
  }
  std::string type() const override { return "UDeflector"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<UDeflectorWarp>(*this); }
  bool deflect(const Scene& s, const Mat4&, const Vec3& p0, Particle& p, uint32_t seed) const override {
    int id = params.getInt("object");
    const Node* n = id > 0 ? s.findNode(static_cast<uint32_t>(id)) : nullptr;
    if (!n) return false;
    const TriMesh& tri = n->triMesh();
    if (tri.empty()) return false;
    Mat4 m = n->objectWorldMatrix(), inv = m.inverse();
    Vec3 a = inv.transformPoint(p0), b = inv.transformPoint(p.pos), d = b - a;
    Aabb seg;
    seg.expand(a);
    seg.expand(b);
    if (seg.hi.x < tri.bounds.lo.x || seg.lo.x > tri.bounds.hi.x || seg.hi.y < tri.bounds.lo.y ||
        seg.lo.y > tri.bounds.hi.y || seg.hi.z < tri.bounds.lo.z || seg.lo.z > tri.bounds.hi.z)
      return false;
    float best = 2.0f;
    Vec3 bestN;
    for (size_t i = 0; i + 2 < tri.indices.size(); i += 3) {
      const Vec3& v0 = tri.positions[tri.indices[i]];
      const Vec3& v1 = tri.positions[tri.indices[i + 1]];
      const Vec3& v2 = tri.positions[tri.indices[i + 2]];
      Vec3 e1 = v1 - v0, e2 = v2 - v0, pv = cross(d, e2);
      float det = dot(e1, pv);
      if (std::fabs(det) < 1e-12f) continue;
      float invDet = 1.0f / det;
      Vec3 tv = a - v0;
      float u = dot(tv, pv) * invDet;
      if (u < 0.0f || u > 1.0f) continue;
      Vec3 qv = cross(tv, e1);
      float v = dot(d, qv) * invDet;
      if (v < 0.0f || u + v > 1.0f) continue;
      float t = dot(e2, qv) * invDet;
      if (t < 0.0f || t > 1.0f || t >= best) continue;
      best = t;
      bestN = cross(e1, e2);
    }
    if (best > 1.0f) return false;
    Vec3 hit = m.transformPoint(a + d * best);
    Vec3 nw = normalize(inv.transformNormalWithInverse(bestN));
    if (dot(nw, p.pos - p0) > 0.0f) nw = -nw;
    p.vel = bounceVelocity(params, p.vel, nw, seed, p.id);
    p.pos = hit + nw * 1e-3f;
    return true;
  }
};

// ---- Geometric warps ----

class RippleWarp : public ForceWarpObject {
 public:
  RippleWarp() {
    params.beginGroup("Ripple");
    params.addFloat("amplitude1", "Amplitude 1", 5.0, -1e6, 1e6, 0.1);
    params.addFloat("amplitude2", "Amplitude 2", 5.0, -1e6, 1e6, 0.1);
    params.addFloat("waveLength", "Wave Length", 50.0, 0.001, 1e6, 1.0);
    params.addFloat("phase", "Phase", 0.0, -1e6, 1e6, 0.01);
    params.addFloat("decay", "Decay", 0.0, 0.0, 1e6, 0.001, 3);
    params.beginGroup("Display");
    params.addInt("circles", "Circles", 10, 3, 200);
    params.addInt("segments", "Segments", 16, 3, 200);
  }
  std::string type() const override { return "Ripple"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<RippleWarp>(*this); }
  bool deforms() const override { return true; }
  Vec3 deform(const Mat4& w, const Mat4& inv, const Vec3& wp, float flex) const override {
    Vec3 l = inv.transformPoint(wp);
    float r = std::sqrt(l.x * l.x + l.y * l.y);
    float c = r > 1e-6f ? l.x / r : 1.0f, s = r > 1e-6f ? l.y / r : 0.0f;
    float amp = params.getFloat("amplitude1") * c * c + params.getFloat("amplitude2") * s * s;
    float dz = amp * std::sin(kTwoPi * (r / params.getFloat("waveLength") - params.getFloat("phase"))) *
               std::exp(-params.getFloat("decay") * r);
    l.z += dz * flex;
    return w.transformPoint(l);
  }
};

class WaveWarp : public ForceWarpObject {
 public:
  WaveWarp() {
    params.beginGroup("Wave");
    params.addFloat("amplitude1", "Amplitude 1", 5.0, -1e6, 1e6, 0.1);
    params.addFloat("amplitude2", "Amplitude 2", 5.0, -1e6, 1e6, 0.1);
    params.addFloat("waveLength", "Wave Length", 50.0, 0.001, 1e6, 1.0);
    params.addFloat("phase", "Phase", 0.0, -1e6, 1e6, 0.01);
    params.addFloat("decay", "Decay", 0.0, 0.0, 1e6, 0.001, 3);
    params.beginGroup("Display");
    params.addInt("sides", "Sides", 5, 1, 200);
    params.addInt("segments", "Segments", 4, 1, 200);
    params.addFloat("width", "Width", 100.0, 0.001, 1e6, 1.0);
  }
  std::string type() const override { return "Wave"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<WaveWarp>(*this); }
  bool deforms() const override { return true; }
  Vec3 deform(const Mat4& w, const Mat4& inv, const Vec3& wp, float flex) const override {
    Vec3 l = inv.transformPoint(wp);
    float half = std::max(1e-3f, params.getFloat("width") * 0.5f);
    float amp = lerpf(params.getFloat("amplitude1"), params.getFloat("amplitude2"), clampf(std::fabs(l.x) / half, 0.0f, 1.0f));
    float dz = amp * std::sin(kTwoPi * (l.y / params.getFloat("waveLength") - params.getFloat("phase"))) *
               std::exp(-params.getFloat("decay") * std::fabs(l.y));
    l.z += dz * flex;
    return w.transformPoint(l);
  }
};

// ---- Particle shapes ----

void addTri(PolyMesh& m, const Vec3& a, const Vec3& b, const Vec3& c) {
  int i = m.addVertex(a), j = m.addVertex(b), k = m.addVertex(c);
  m.addFace({i, j, k}, {}, 0);
}

void addParticleShape(PolyMesh& m, int shape, const Vec3& c, float size, const Vec3& dir, const Vec3& toEye,
                      const Mat4& spin) {
  float h = size * 0.5f;
  switch (shape) {
    case 1: {  // cube
      Vec3 ax = spin.transformDir(Vec3(h, 0, 0)), ay = spin.transformDir(Vec3(0, h, 0)), az = spin.transformDir(Vec3(0, 0, h));
      int base = static_cast<int>(m.positions.size());
      for (int i = 0; i < 8; ++i)
        m.addVertex(c + ax * ((i & 1) ? 1.f : -1.f) + ay * ((i & 2) ? 1.f : -1.f) + az * ((i & 4) ? 1.f : -1.f));
      const int f[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
      for (const auto& q : f) m.addFace({base + q[0], base + q[1], base + q[2], base + q[3]}, {}, 1);
      break;
    }
    case 2:    // facing quad
    case 3:    // six point star
    case 4: {  // triangle
      Vec3 n = length2(toEye) > 1e-12f ? normalize(toEye) : Vec3(0, -1, 0);
      Vec3 u, v;
      makeBasis(n, u, v);
      if (shape != 2) {
        Vec3 su = spin.transformDir(u), sv = cross(n, su);
        if (length2(sv) > 1e-12f) {
          u = normalize(su - n * dot(su, n));
          v = cross(n, u);
        }
      }
      if (shape == 2) {
        int a = m.addVertex(c - u * h - v * h), b = m.addVertex(c + u * h - v * h), d = m.addVertex(c + u * h + v * h),
            e = m.addVertex(c - u * h + v * h);
        m.addFace({a, b, d, e}, {}, 0);
      } else {
        auto pt = [&](float ang) { return c + (u * std::cos(ang) + v * std::sin(ang)) * h; };
        addTri(m, pt(kPi * 0.5f), pt(kPi * 0.5f + kTwoPi / 3), pt(kPi * 0.5f + 2 * kTwoPi / 3));
        if (shape == 3) addTri(m, pt(-kPi * 0.5f), pt(-kPi * 0.5f + kTwoPi / 3), pt(-kPi * 0.5f + 2 * kTwoPi / 3));
      }
      break;
    }
    case 5: {  // sphere (octahedron)
      Vec3 ax[6] = {Vec3(h, 0, 0), Vec3(-h, 0, 0), Vec3(0, h, 0), Vec3(0, -h, 0), Vec3(0, 0, h), Vec3(0, 0, -h)};
      int base = static_cast<int>(m.positions.size());
      for (const auto& a : ax) m.addVertex(c + a);
      const int f[8][3] = {{0, 2, 4}, {2, 1, 4}, {1, 3, 4}, {3, 0, 4}, {2, 0, 5}, {1, 2, 5}, {3, 1, 5}, {0, 3, 5}};
      for (const auto& t : f) m.addFace({base + t[0], base + t[1], base + t[2]}, {}, 1);
      break;
    }
    default: {  // tetrahedron / drop, pointing along the velocity
      Vec3 d = length2(dir) > 1e-12f ? normalize(dir) : Vec3(0, 0, -1);
      Vec3 u, v;
      makeBasis(d, u, v);
      float len = shape == 6 ? size * 2.0f : size;
      float r = size * 0.3f;
      Vec3 tip = c + d * (len * 0.5f), back = c - d * (len * 0.5f);
      int t = m.addVertex(tip);
      int b0 = m.addVertex(back + u * r), b1 = m.addVertex(back + (u * -0.5f + v * 0.866f) * r),
          b2 = m.addVertex(back + (u * -0.5f - v * 0.866f) * r);
      m.addFace({t, b0, b1}, {}, 0);
      m.addFace({t, b1, b2}, {}, 0);
      m.addFace({t, b2, b0}, {}, 0);
      m.addFace({b0, b2, b1}, {}, 0);
      break;
    }
  }
}

// Bound force warps of a node, from its Space Warp Binding modifiers.
std::vector<std::pair<const Node*, const ForceWarpObject*>> boundWarps(const Scene& s, const Node& n) {
  std::vector<std::pair<const Node*, const ForceWarpObject*>> out;
  for (const auto& m : n.modifiers) {
    if (!m || !m->enabled || m->type() != "SpaceWarpBinding") continue;
    int id = m->params.getInt("warp");
    const Node* w = id > 0 ? s.findNode(static_cast<uint32_t>(id)) : nullptr;
    if (!w || !w->object) continue;
    auto* f = dynamic_cast<const ForceWarpObject*>(w->object.get());
    if (f && !f->deforms()) out.emplace_back(w, f);
  }
  return out;
}

void addSystemTiming(ParamBlock& p, double life, double rate) {
  p.beginGroup("Timing");
  p.addFloat("start", "Start", 0.0, -1e6, 1e6, 1.0, 0);
  p.addFloat("life", "Life", life, 1.0, 1e6, 1.0, 0);
  p.addFloat("birthRate", "Birth Rate", rate, 0.0, 1e6, 0.1, 1);
  p.addBool("constant", "Constant", true);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Simulation

std::vector<Particle> ParticleSystemObject::simulate(const Scene& scene, uint32_t nodeId, double frame) const {
  const Node* self = scene.findNode(nodeId);
  if (!self) return {};
  Emission em = emission();
  auto warps = boundWarps(scene, *self);
  float fps = std::max(1.0f, scene.time.fps);

  // Anything that changes the simulation invalidates the cache.
  std::string sig = params.toJson().dump();
  sig += self->transform.toJson().dump();
  sig += self->objectOffset.toJson().dump();
  sig += std::to_string(self->parentId);
  for (const auto& w : warps) {
    sig += std::to_string(w.first->id);
    sig += w.first->object->params.toJson().dump();
    sig += w.first->transform.toJson().dump();
  }
  for (const auto& m : self->modifiers)
    if (m && m->type() == "SpaceWarpBinding") sig += std::to_string(m->params.getInt("warp")) + (m->enabled ? "1" : "0");
  if (const Animation* a = Animation::of(scene)) sig += "#" + std::to_string(a->revision());
  sig += "#" + std::to_string(fps);
  size_t key = std::hash<std::string>()(sig);

  Cache& c = *cache_;
  if (!c.valid || c.key != key || c.nodeId != nodeId || frame < c.time - 1e-6) {
    c = Cache();
    c.key = key;
    c.nodeId = nodeId;
    c.time = em.start;
    c.valid = true;
  }
  if (frame < em.start) return {};

  const int substeps = 2;
  const double step = 1.0 / substeps;
  uint32_t seed = static_cast<uint32_t>(em.seed);
  std::vector<Mat4> warpW(warps.size());
  while (c.time < frame - 1e-9) {
    double dt = std::min(step, frame - c.time);
    double t = c.time, tm = t + dt * 0.5;
    for (size_t i = 0; i < warps.size(); ++i) warpW[i] = nodeWorldMatrixAt(scene, warps[i].first->id, tm);
    // Integrate.
    for (auto& p : c.particles) {
      Vec3 a(0.0f);
      float damp = 0.0f;
      for (size_t i = 0; i < warps.size(); ++i) {
        a += warps[i].second->acceleration(scene, warpW[i], p, tm, fps);
        damp += warps[i].second->damping(warpW[i], p, tm);
      }
      p.vel += a * static_cast<float>(dt);
      if (damp > 0.0f) p.vel *= std::pow(std::max(0.0f, 1.0f - std::min(damp, 1.0f)), static_cast<float>(dt));
      Vec3 p0 = p.pos;
      p.pos += p.vel * static_cast<float>(dt);
      for (size_t i = 0; i < warps.size(); ++i)
        if (warps[i].second->deflect(scene, warpW[i], p0, p, seed)) break;
      p.age += static_cast<float>(dt);
    }
    c.particles.erase(std::remove_if(c.particles.begin(), c.particles.end(), [](const Particle& p) { return p.age >= p.life; }),
                      c.particles.end());
    // Births in [t, t + dt).
    double e0 = std::max(t, em.start), e1 = std::min(t + dt, em.stop);
    if (e1 > e0) {
      c.birthAccum += em.rate * (e1 - e0);
      int count = static_cast<int>(std::floor(c.birthAccum));
      c.birthAccum -= count;
      if (count > 0) {
        Mat4 E = emitterMatrix(scene, *self, tm);
        for (int k = 0; k < count; ++k) {
          uint32_t id = c.nextId++;
          if (static_cast<int>(c.particles.size()) >= em.maxAlive) continue;
          Particle p;
          p.id = id;
          Vec3 lp, lv;
          float size = 1.0f;
          spawn(id, lp, lv, size, p.spinAxis);
          p.pos = E.transformPoint(lp);
          p.vel = E.transformDir(lv);
          p.size = size;
          p.life = static_cast<float>(std::max(1.0, em.life + hashS(seed, id, 17) * em.lifeVariation));
          p.spinPhase = hash01(seed, id, 19) * kTwoPi;
          // Spread births over the step so streams stay continuous.
          float frac = static_cast<float>((t + dt) - (e0 + (e1 - e0) * (k + 0.5) / count));
          p.pos += p.vel * frac;
          p.age = frac;
          c.particles.push_back(p);
        }
      }
    }
    c.time = t + dt;
  }
  return c.particles;
}

PolyMesh ParticleSystemObject::buildAt(const GeomState& st) const {
  PolyMesh m;
  if (!st.scene) return m;
  const Node* self = st.scene->findNode(st.nodeId);
  if (!self) return m;
  if (params.has("displayUntil") && st.time > params.getFloat("displayUntil")) return m;
  std::vector<Particle> ps = simulate(*st.scene, st.nodeId, st.time);
  if (ps.empty()) return m;
  Mat4 inv = (nodeWorldMatrixAt(*st.scene, st.nodeId, st.time) * self->objectOffset.matrix()).inverse();
  // Facing particles turn towards the render camera (or the front view).
  Vec3 eye(0.0f);
  bool hasEye = false;
  const std::string& camName = st.scene->renderSettings.has("camera") ? st.scene->renderSettings.getString("camera") : std::string();
  if (!camName.empty())
    if (const Node* cam = st.scene->findNodeByName(camName)) {
      eye = nodeWorldMatrixAt(*st.scene, cam->id, st.time).translation();
      hasEye = true;
    }
  if (!hasEye)
    for (const auto& up : st.scene->nodes)
      if (up->camera()) {
        eye = nodeWorldMatrixAt(*st.scene, up->id, st.time).translation();
        hasEye = true;
        break;
      }
  int sh = shape();
  float period = spinPeriod();
  m.positions.reserve(ps.size() * 8);
  for (const auto& p : ps) {
    Vec3 c = inv.transformPoint(p.pos);
    Vec3 dir = inv.transformDir(p.vel);
    Vec3 toEye = hasEye ? inv.transformDir(eye - p.pos) : inv.transformDir(Vec3(0, -1, 0));
    Mat4 spin = period > 0.0f ? Mat4::rotateAxis(p.spinAxis, p.spinPhase + kTwoPi * p.age / period)
                              : Mat4::rotateAxis(p.spinAxis, p.spinPhase);
    float size = p.size * sizeScale(p);
    if (size <= 1e-6f) continue;
    addParticleShape(m, sh, c, size, dir, toEye, spin);
  }
  return m;
}

// ---------------------------------------------------------------------------------------------
// Spray

SprayObject::SprayObject() {
  params.beginGroup("Particles");
  params.addInt("count", "Particle Count", 100, 1, 1000000);
  params.addFloat("dropSize", "Drop Size", 2.0, 0.0, 1e6, 0.1);
  params.addFloat("speed", "Speed", 10.0, 0.0, 1e6, 0.1);
  params.addFloat("variation", "Variation", 0.0, 0.0, 1e6, 0.1);
  params.addEnum("renderType", "Render", {"Tetrahedron", "Facing"}, 0);
  addSystemTiming(params, 30.0, 10.0);
  params.beginGroup("Emitter");
  params.addFloat("width", "Width", 50.0, 0.0, 1e6, 1.0);
  params.addFloat("length", "Length", 50.0, 0.0, 1e6, 1.0);
  params.addInt("seed", "Seed", 12345, 0, 2147483647);
}

std::unique_ptr<SceneObject> SprayObject::clone() const { return std::make_unique<SprayObject>(*this); }

ParticleSystemObject::Emission SprayObject::emission() const {
  Emission e;
  e.start = params.getFloat("start");
  e.stop = 1e9;
  e.life = params.getFloat("life");
  e.maxAlive = params.getInt("count");
  e.rate = params.getBool("constant") ? params.getInt("count") / std::max(1.0, e.life) : params.getFloat("birthRate");
  e.seed = params.getInt("seed");
  return e;
}

void SprayObject::spawn(uint32_t id, Vec3& pos, Vec3& vel, float& size, Vec3& spinAxis) const {
  uint32_t seed = static_cast<uint32_t>(params.getInt("seed"));
  pos = Vec3(hashS(seed, id, 1) * params.getFloat("width") * 0.5f, hashS(seed, id, 2) * params.getFloat("length") * 0.5f, 0.0f);
  float var = params.getFloat("variation");
  float speed = params.getFloat("speed");
  Vec3 dir = normalize(Vec3(0, 0, -1) + randomUnit(seed, id, 3) * (var * 0.05f));
  vel = dir * std::max(0.0f, speed * (1.0f + hashS(seed, id, 6) * var * 0.05f));
  size = params.getFloat("dropSize");
  spinAxis = randomUnit(seed, id, 8);
}

int SprayObject::shape() const { return params.getInt("renderType") == 1 ? 2 : 6; }

// ---------------------------------------------------------------------------------------------
// Snow

SnowObject::SnowObject() {
  params.beginGroup("Particles");
  params.addInt("count", "Particle Count", 100, 1, 1000000);
  params.addFloat("flakeSize", "Flake Size", 2.0, 0.0, 1e6, 0.1);
  params.addFloat("speed", "Speed", 10.0, 0.0, 1e6, 0.1);
  params.addFloat("variation", "Variation", 2.0, 0.0, 1e6, 0.1);
  params.addFloat("tumble", "Tumble", 0.0, 0.0, 1.0, 0.05);
  params.addFloat("tumbleRate", "Tumble Rate", 0.1, 0.0, 1e6, 0.01);
  params.addEnum("renderType", "Render", {"Six Point", "Triangle", "Facing"}, 0);
  addSystemTiming(params, 30.0, 10.0);
  params.beginGroup("Emitter");
  params.addFloat("width", "Width", 50.0, 0.0, 1e6, 1.0);
  params.addFloat("length", "Length", 50.0, 0.0, 1e6, 1.0);
  params.addInt("seed", "Seed", 12345, 0, 2147483647);
}

std::unique_ptr<SceneObject> SnowObject::clone() const { return std::make_unique<SnowObject>(*this); }

ParticleSystemObject::Emission SnowObject::emission() const {
  Emission e;
  e.start = params.getFloat("start");
  e.stop = 1e9;
  e.life = params.getFloat("life");
  e.maxAlive = params.getInt("count");
  e.rate = params.getBool("constant") ? params.getInt("count") / std::max(1.0, e.life) : params.getFloat("birthRate");
  e.seed = params.getInt("seed");
  return e;
}

void SnowObject::spawn(uint32_t id, Vec3& pos, Vec3& vel, float& size, Vec3& spinAxis) const {
  uint32_t seed = static_cast<uint32_t>(params.getInt("seed"));
  pos = Vec3(hashS(seed, id, 1) * params.getFloat("width") * 0.5f, hashS(seed, id, 2) * params.getFloat("length") * 0.5f, 0.0f);
  float var = params.getFloat("variation");
  // Snow drifts sideways much more than spray.
  Vec3 drift = Vec3(hashS(seed, id, 3), hashS(seed, id, 4), 0.0f) * (var * 0.1f);
  vel = Vec3(0, 0, -params.getFloat("speed") * (1.0f + hashS(seed, id, 5) * 0.1f)) + drift * params.getFloat("speed") * 0.1f;
  size = params.getFloat("flakeSize");
  spinAxis = normalize(lerp(Vec3(0, 0, 1), randomUnit(seed, id, 8), params.getFloat("tumble")));
}

int SnowObject::shape() const {
  switch (params.getInt("renderType")) {
    case 1: return 4;
    case 2: return 2;
    default: return 3;
  }
}

float SnowObject::spinPeriod() const {
  float rate = params.getFloat("tumbleRate");
  return params.getFloat("tumble") > 0.0f && rate > 0.0f ? 1.0f / rate : 0.0f;
}

// ---------------------------------------------------------------------------------------------
// Super Spray

SuperSprayObject::SuperSprayObject() {
  params.beginGroup("Basic Parameters");
  params.addFloat("offAxis", "Off Axis", 0.0, -180.0, 180.0, 1.0, 1);
  params.addFloat("offAxisSpread", "Off Axis Spread", 15.0, 0.0, 180.0, 1.0, 1);
  params.addFloat("offPlane", "Off Plane", 0.0, -180.0, 180.0, 1.0, 1);
  params.addFloat("offPlaneSpread", "Off Plane Spread", 180.0, 0.0, 180.0, 1.0, 1);
  params.addFloat("iconSize", "Icon Size", 10.0, 0.0, 1e6, 0.5);
  params.beginGroup("Particle Generation");
  params.addEnum("useRate", "Quantity", {"Use Rate", "Use Total"}, 0);
  params.addFloat("rate", "Rate", 10.0, 0.0, 1e6, 0.1, 1);
  params.addInt("total", "Total", 100, 0, 10000000);
  params.addFloat("speed", "Speed", 10.0, 0.0, 1e6, 0.1);
  params.addFloat("speedVariation", "Speed Variation %", 0.0, 0.0, 100.0, 1.0, 1);
  params.addFloat("emitStart", "Emit Start", 0.0, -1e6, 1e6, 1.0, 0);
  params.addFloat("emitStop", "Emit Stop", 30.0, -1e6, 1e6, 1.0, 0);
  params.addFloat("displayUntil", "Display Until", 100.0, -1e6, 1e6, 1.0, 0);
  params.addFloat("life", "Life", 30.0, 1.0, 1e6, 1.0, 0);
  params.addFloat("lifeVariation", "Life Variation", 0.0, 0.0, 1e6, 1.0, 0);
  params.addFloat("size", "Size", 1.0, 0.0, 1e6, 0.1);
  params.addFloat("sizeVariation", "Size Variation %", 0.0, 0.0, 100.0, 1.0, 1);
  params.addFloat("growFor", "Grow For", 10.0, 0.0, 1e6, 1.0, 0);
  params.addFloat("fadeFor", "Fade For", 10.0, 0.0, 1e6, 1.0, 0);
  params.addInt("seed", "Seed", 12345, 0, 2147483647);
  params.beginGroup("Particle Type");
  params.addEnum("particleType", "Standard Particles", {"Triangle", "Cube", "Facing", "Tetra", "Six Point", "Sphere"}, 1);
  params.beginGroup("Rotation and Collision");
  params.addFloat("spinTime", "Spin Time", 30.0, 0.0, 1e6, 1.0, 0);
  params.addFloat("spinVariation", "Spin Variation %", 0.0, 0.0, 100.0, 1.0, 1);
}

std::unique_ptr<SceneObject> SuperSprayObject::clone() const { return std::make_unique<SuperSprayObject>(*this); }

ParticleSystemObject::Emission SuperSprayObject::emission() const {
  Emission e;
  e.start = params.getFloat("emitStart");
  e.stop = std::max(e.start, static_cast<double>(params.getFloat("emitStop")));
  e.life = params.getFloat("life");
  e.lifeVariation = params.getFloat("lifeVariation");
  if (params.getInt("useRate") == 1)
    e.rate = params.getInt("total") / std::max(1.0, e.stop - e.start);
  else
    e.rate = params.getFloat("rate");
  e.seed = params.getInt("seed");
  return e;
}

void SuperSprayObject::spawn(uint32_t id, Vec3& pos, Vec3& vel, float& size, Vec3& spinAxis) const {
  uint32_t seed = static_cast<uint32_t>(params.getInt("seed"));
  pos = Vec3(0.0f);
  float a = radians(params.getFloat("offAxis") + hashS(seed, id, 1) * params.getFloat("offAxisSpread"));
  float b = radians(params.getFloat("offPlane") + hashS(seed, id, 2) * params.getFloat("offPlaneSpread"));
  Vec3 dir(std::sin(a) * std::cos(b), std::sin(a) * std::sin(b), std::cos(a));
  float speed = params.getFloat("speed") * (1.0f + hashS(seed, id, 3) * params.getFloat("speedVariation") / 100.0f);
  vel = dir * std::max(0.0f, speed);
  size = params.getFloat("size") * (1.0f + hashS(seed, id, 4) * params.getFloat("sizeVariation") / 100.0f);
  spinAxis = randomUnit(seed, id, 8);
}

float SuperSprayObject::sizeScale(const Particle& p) const {
  float s = 1.0f;
  float grow = params.getFloat("growFor"), fade = params.getFloat("fadeFor");
  if (grow > 0.0f && p.age < grow) s = std::min(s, 0.1f + 0.9f * p.age / grow);
  if (fade > 0.0f && p.life - p.age < fade) s = std::min(s, 0.1f + 0.9f * (p.life - p.age) / fade);
  return s;
}

int SuperSprayObject::shape() const {
  switch (params.getInt("particleType")) {
    case 0: return 4;
    case 1: return 1;
    case 2: return 2;
    case 3: return 0;
    case 4: return 3;
    default: return 5;
  }
}

float SuperSprayObject::spinPeriod() const { return params.getFloat("spinTime"); }

// ---------------------------------------------------------------------------------------------
// Binding

SpaceWarpBindingModifier::SpaceWarpBindingModifier() {
  params.beginGroup("Binding");
  params.addInt("warp", "Space Warp", 0, 0, 2147483647);
  params.addFloat("flexibility", "Flexibility", 1.0, 0.0, 1e6, 0.05);
}

std::vector<uint32_t> SpaceWarpBindingModifier::referencedNodes() const {
  int id = params.getInt("warp");
  return id > 0 ? std::vector<uint32_t>{static_cast<uint32_t>(id)} : std::vector<uint32_t>{};
}

void SpaceWarpBindingModifier::applyState(GeomState& st) const {
  if (st.isShape || !st.scene) return;
  int id = params.getInt("warp");
  const Node* w = id > 0 ? st.scene->findNode(static_cast<uint32_t>(id)) : nullptr;
  const Node* self = st.scene->findNode(st.nodeId);
  if (!w || !self || !w->object) return;
  auto* f = dynamic_cast<const ForceWarpObject*>(w->object.get());
  if (!f || !f->deforms()) return;
  if (dynamic_cast<const ParticleSystemObject*>(self->object.get())) return;
  Mat4 ww = w->worldMatrix(), wi = ww.inverse();
  Mat4 m = self->objectWorldMatrix(), mi = m.inverse();
  float flex = params.getFloat("flexibility");
  for (auto& p : st.mesh.positions) p = mi.transformPoint(f->deform(ww, wi, m.transformPoint(p), flex));
}

bool canBindToSpaceWarp(const Node& target, const Node& warp) {
  if (!target.object || !warp.object || &target == &warp) return false;
  auto* f = dynamic_cast<const ForceWarpObject*>(warp.object.get());
  if (!f) return false;
  bool particles = dynamic_cast<const ParticleSystemObject*>(target.object.get()) != nullptr;
  if (f->deforms()) return !particles && target.kind() == ObjectKind::Geometry;
  return particles;
}

bool bindToSpaceWarp(Node& target, const Node& warp) {
  if (!canBindToSpaceWarp(target, warp)) return false;
  for (const auto& m : target.modifiers)
    if (m && m->type() == "SpaceWarpBinding" && m->params.getInt("warp") == static_cast<int>(warp.id)) return false;
  auto mod = std::make_shared<SpaceWarpBindingModifier>();
  mod->params.setNumber("warp", warp.id);
  mod->displayName = objectDisplayName(warp.object->type()) + " Binding";
  target.modifiers.push_back(mod);
  target.invalidate();
  return true;
}

void registerParticleTypes() {
  const std::string geo = "Geometry", ps = "Particle Systems", sw = "Space Warps";
  registerObjectType({"Spray", "Spray", geo, ps, [] { return std::make_unique<SprayObject>(); }});
  registerObjectType({"Snow", "Snow", geo, ps, [] { return std::make_unique<SnowObject>(); }});
  registerObjectType({"SuperSpray", "Super Spray", geo, ps, [] { return std::make_unique<SuperSprayObject>(); }});
  const std::string forces = "Forces", defl = "Deflectors", geom = "Geometric/Deformable";
  registerObjectType({"Gravity", "Gravity", sw, forces, [] { return std::make_unique<GravityWarp>(); }});
  registerObjectType({"Wind", "Wind", sw, forces, [] { return std::make_unique<WindWarp>(); }});
  registerObjectType({"Drag", "Drag", sw, forces, [] { return std::make_unique<DragWarp>(); }});
  registerObjectType({"Vortex", "Vortex", sw, forces, [] { return std::make_unique<VortexWarp>(); }});
  registerObjectType({"Push", "Push", sw, forces, [] { return std::make_unique<PushWarp>(); }});
  registerObjectType({"Motor", "Motor", sw, forces, [] { return std::make_unique<MotorWarp>(); }});
  registerObjectType({"PBomb", "PBomb", sw, forces, [] { return std::make_unique<PBombWarp>(); }});
  registerObjectType({"Deflector", "Deflector", sw, defl, [] { return std::make_unique<DeflectorWarp>(); }});
  registerObjectType({"SDeflector", "SDeflector", sw, defl, [] { return std::make_unique<SDeflectorWarp>(); }});
  registerObjectType({"UDeflector", "UDeflector", sw, defl, [] { return std::make_unique<UDeflectorWarp>(); }});
  registerObjectType({"Ripple", "Ripple", sw, geom, [] { return std::make_unique<RippleWarp>(); }});
  registerObjectType({"Wave", "Wave", sw, geom, [] { return std::make_unique<WaveWarp>(); }});
}

void registerParticleModifiers() {
  registerModifierType({"SpaceWarpBinding", "Space Warp Binding", "", true, false,
                        [] { return std::make_unique<SpaceWarpBindingModifier>(); }});
}

}  // namespace veraa
