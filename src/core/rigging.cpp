#include "rigging.h"

#include <algorithm>
#include <cmath>

#include "registry.h"
#include "scene.h"

namespace veraa {
namespace {

json matJson(const Mat4& m) {
  json a = json::array();
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) a.push_back(m.m[r][c]);
  return a;
}

Mat4 matFrom(const json& j) {
  Mat4 m;
  if (!j.is_array() || j.size() != 16) return m;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) m.m[r][c] = j[static_cast<size_t>(r * 4 + c)].get<float>();
  return m;
}

// Closest distance from p to segment ab.
float segmentDistance(const Vec3& p, const Vec3& a, const Vec3& b) {
  Vec3 ab = b - a;
  float l2 = length2(ab);
  float t = l2 > 1e-12f ? clampf(dot(p - a, ab) / l2, 0.0f, 1.0f) : 0.0f;
  return length(p - (a + ab * t));
}

void addQuad(PolyMesh& m, int a, int b, int c, int d) { m.addFace({a, b, c, d}, {}, 0); }

}  // namespace

// ---------------------------------------------------------------------------------------------
// Bone

BoneObject::BoneObject() {
  params.beginGroup("Bone Object");
  params.addFloat("width", "Width", 4.0, 0.0, 1e6, 0.1);
  params.addFloat("height", "Height", 4.0, 0.0, 1e6, 0.1);
  params.addFloat("taper", "Taper", 90.0, -100.0, 100.0, 1.0, 1);
  params.addFloat("length", "Length", 20.0, 0.001, 1e6, 0.1);
  params.beginGroup("Bone Fins");
  params.addBool("sideFins", "Side Fins", false);
  params.addFloat("sideFinSize", "Side Fin Size", 2.0, 0.0, 1e6, 0.1);
  params.addBool("frontFin", "Front Fin", false);
  params.addFloat("frontFinSize", "Front Fin Size", 2.0, 0.0, 1e6, 0.1);
  params.addBool("backFin", "Back Fin", false);
  params.addFloat("backFinSize", "Back Fin Size", 2.0, 0.0, 1e6, 0.1);
}

PolyMesh BoneObject::build() const {
  PolyMesh m;
  float w = params.getFloat("width") * 0.5f, h = params.getFloat("height") * 0.5f;
  float len = params.getFloat("length");
  float s = 1.0f - params.getFloat("taper") / 100.0f;
  // The widest section sits a little way along the bone, like a classic bone display.
  float mid = std::min(len * 0.1f, std::max(w, h));
  int tip0 = m.addVertex(Vec3(0, 0, 0));
  int a = m.addVertex(Vec3(mid, -w, -h)), b = m.addVertex(Vec3(mid, w, -h)), c = m.addVertex(Vec3(mid, w, h)),
      d = m.addVertex(Vec3(mid, -w, h));
  int e = m.addVertex(Vec3(len, -w * s, -h * s)), f = m.addVertex(Vec3(len, w * s, -h * s)),
      g = m.addVertex(Vec3(len, w * s, h * s)), k = m.addVertex(Vec3(len, -w * s, h * s));
  m.addFace({tip0, b, a}, {}, 0);
  m.addFace({tip0, c, b}, {}, 0);
  m.addFace({tip0, d, c}, {}, 0);
  m.addFace({tip0, a, d}, {}, 0);
  addQuad(m, a, b, f, e);
  addQuad(m, b, c, g, f);
  addQuad(m, c, d, k, g);
  addQuad(m, d, a, e, k);
  if (s > 1e-4f) addQuad(m, e, f, g, k);
  auto fin = [&](const Vec3& dir, float size) {
    // Flat fin standing out of the bone along dir.
    Vec3 base0(mid, 0, 0), base1(len * 0.9f, 0, 0);
    Vec3 off0 = dir * (std::fabs(dir.y) > 0 ? w : h), off1 = off0 * std::max(s, 0.2f);
    int p0 = m.addVertex(base0 + off0), p1 = m.addVertex(base1 + off1);
    int p2 = m.addVertex(base1 + off1 + dir * size), p3 = m.addVertex(base0 + off0 + dir * size);
    m.addFace({p0, p1, p2, p3}, {}, 0);
    m.addFace({p3, p2, p1, p0}, {}, 0);
  };
  if (params.getBool("sideFins")) {
    fin(Vec3(0, 1, 0), params.getFloat("sideFinSize"));
    fin(Vec3(0, -1, 0), params.getFloat("sideFinSize"));
  }
  if (params.getBool("frontFin")) fin(Vec3(0, 0, 1), params.getFloat("frontFinSize"));
  if (params.getBool("backFin")) fin(Vec3(0, 0, -1), params.getFloat("backFinSize"));
  return m;
}

IKChainObject::IKChainObject() {
  params.beginGroup("IK Solver");
  params.addInt("startBone", "Start Joint", 0, 0, 2147483647);
  params.addInt("endBone", "End Joint", 0, 0, 2147483647);
  params.addEnum("solver", "Solver", {"HI Solver", "Limb Solver"}, 0);
  params.addBool("enabled", "Enabled", true);
  params.beginGroup("IK Solver Plane");
  params.addFloat("swivel", "Swivel Angle", 0.0, -36000.0, 36000.0, 1.0, 1);
  params.beginGroup("IK Solver Properties");
  params.addInt("iterations", "Iterations", 20, 1, 500);
  params.addFloat("threshold", "Position Threshold", 0.1, 0.0001, 1000.0, 0.01, 3);
  params.beginGroup("IK Display Options");
  params.addFloat("size", "Goal Size", 6.0, 0.01, 1e6, 0.1);
}

float boneLength(const Scene& scene, const Node& n) {
  if (n.object && n.object->type() == "Bone") return n.object->params.getFloat("length");
  for (const Node* c : scene.children(n.id)) {
    float d = length(c->transform.position);
    if (d > 1e-4f) return d;
  }
  Aabb b = n.mesh().bounds();
  if (b.valid()) return std::max(b.hi.x, maxComponent(b.extent()) * 0.5f);
  return 10.0f;
}

// ---------------------------------------------------------------------------------------------
// Skin

SkinModifier::SkinModifier() {
  params.beginGroup("Parameters");
  params.addBool("alwaysDeform", "Always Deform", true);
  params.addInt("boneLimit", "Bone Affect Limit", 20, 1, 20);
  params.beginGroup("Envelope Properties");
  params.addFloat("falloff", "Falloff", 1.0, 0.1, 4.0, 0.1);
  params.addBool("normalize", "Normalize", true);
}

std::vector<uint32_t> SkinModifier::referencedNodes() const {
  std::vector<uint32_t> ids;
  for (const auto& b : bones) ids.push_back(b.nodeId);
  return ids;
}

int SkinModifier::addBone(const Scene& scene, uint32_t boneId, const Node& meshNode) {
  for (size_t i = 0; i < bones.size(); ++i)
    if (bones[i].nodeId == boneId) return static_cast<int>(i);
  const Node* bn = scene.findNode(boneId);
  if (!bn || bn->id == meshNode.id) return -1;
  if (!bound) {
    meshBind = meshNode.objectWorldMatrix();
    bound = true;
  }
  SkinBone b;
  b.nodeId = boneId;
  b.bindWorld = bn->worldMatrix();
  float len = boneLength(scene, *bn);
  b.start = Vec3(0, 0, 0);
  b.end = Vec3(len, 0, 0);
  float thick = len * 0.25f;
  if (bn->object && bn->object->type() == "Bone")
    thick = std::max(bn->object->params.getFloat("width"), bn->object->params.getFloat("height"));
  // Envelope radii are stored in bone space; scale them by the bone's own scale.
  float sc = std::max(1e-6f, length(b.bindWorld.column(0)));
  thick = std::max(thick, len * 0.1f);
  b.innerRadius = thick / sc;
  b.outerRadius = thick * 2.0f / sc;
  bones.push_back(b);
  touch();
  return static_cast<int>(bones.size()) - 1;
}

void SkinModifier::removeBone(int index) {
  if (index < 0 || index >= static_cast<int>(bones.size())) return;
  bones.erase(bones.begin() + index);
  for (auto it = overrides.begin(); it != overrides.end();) {
    VertexWeights out;
    for (auto [bi, w] : it->second) {
      if (bi == index) continue;
      out.emplace_back(bi > index ? bi - 1 : bi, w);
    }
    if (out.empty()) {
      it = overrides.erase(it);
    } else {
      it->second = out;
      ++it;
    }
  }
  touch();
}

void SkinModifier::resetSkinPose(const Scene& scene, const Node& meshNode) {
  meshBind = meshNode.objectWorldMatrix();
  bound = true;
  for (auto& b : bones)
    if (const Node* n = scene.findNode(b.nodeId)) b.bindWorld = n->worldMatrix();
  touch();
}

std::vector<VertexWeights> SkinModifier::computeWeights(const PolyMesh& mesh) const {
  std::vector<VertexWeights> out(mesh.positions.size());
  if (bones.empty()) return out;
  float falloff = params.getFloat("falloff");
  int limit = params.getInt("boneLimit");
  bool norm = params.getBool("normalize");
  std::vector<Mat4> invBind(bones.size());
  for (size_t b = 0; b < bones.size(); ++b) invBind[b] = bones[b].bindWorld.inverse();
  for (size_t v = 0; v < mesh.positions.size(); ++v) {
    auto ov = overrides.find(static_cast<int>(v));
    if (ov != overrides.end()) {
      out[v] = ov->second;
      continue;
    }
    Vec3 world = meshBind.transformPoint(mesh.positions[v]);
    VertexWeights w;
    int nearest = 0;
    float nearestD = kInfinity;
    for (size_t b = 0; b < bones.size(); ++b) {
      Vec3 local = invBind[b].transformPoint(world);
      float d = segmentDistance(local, bones[b].start, bones[b].end);
      float rel = d - bones[b].innerRadius;
      if (rel < nearestD) {
        nearestD = rel;
        nearest = static_cast<int>(b);
      }
      float span = std::max(1e-6f, bones[b].outerRadius - bones[b].innerRadius);
      float t = d <= bones[b].innerRadius ? 1.0f : 1.0f - (d - bones[b].innerRadius) / span;
      if (t <= 0.0f) continue;
      // Smooth falloff curve; the exponent shapes it (Falloff).
      float s = t * t * (3.0f - 2.0f * t);
      w.emplace_back(static_cast<int>(b), std::pow(s, falloff));
    }
    if (w.empty()) w.emplace_back(nearest, 1.0f);
    std::sort(w.begin(), w.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    if (static_cast<int>(w.size()) > limit) w.resize(static_cast<size_t>(limit));
    float sum = 0.0f;
    for (const auto& p : w) sum += p.second;
    if (norm || sum > 1.0f)
      for (auto& p : w) p.second /= std::max(sum, 1e-9f);
    out[v] = std::move(w);
  }
  return out;
}

void SkinModifier::setVertexWeight(const PolyMesh& bindMesh, int vertex, int bone, float weight) {
  if (vertex < 0 || vertex >= static_cast<int>(bindMesh.positions.size())) return;
  if (bone < 0 || bone >= static_cast<int>(bones.size())) return;
  weight = clampf(weight, 0.0f, 1.0f);
  VertexWeights cur;
  auto ov = overrides.find(vertex);
  if (ov != overrides.end()) {
    cur = ov->second;
  } else {
    // Start from the envelope weights of this vertex.
    PolyMesh one;
    one.positions.push_back(bindMesh.positions[static_cast<size_t>(vertex)]);
    std::map<int, VertexWeights> saved;
    saved.swap(overrides);
    cur = computeWeights(one)[0];
    saved.swap(overrides);
  }
  float others = 0.0f;
  for (const auto& p : cur)
    if (p.first != bone) others += p.second;
  VertexWeights next;
  for (const auto& p : cur) {
    if (p.first == bone) continue;
    float w = others > 1e-9f ? p.second / others * (1.0f - weight) : 0.0f;
    if (w > 1e-5f) next.emplace_back(p.first, w);
  }
  if (others <= 1e-9f && weight < 1.0f && next.empty()) weight = 1.0f;  // nothing else to give weight to
  if (weight > 1e-5f) next.emplace_back(bone, weight);
  overrides[vertex] = next;
  touch();
}

void SkinModifier::applyState(GeomState& st) const {
  if (st.isShape || bones.empty() || !bound || !st.scene) return;
  const Node* self = st.scene->findNode(st.nodeId);
  if (!self) return;
  std::vector<VertexWeights> weights = computeWeights(st.mesh);
  std::vector<Mat4> deform(bones.size());
  for (size_t b = 0; b < bones.size(); ++b) {
    const Node* bn = st.scene->findNode(bones[b].nodeId);
    Mat4 cur = bn ? bn->worldMatrix() : bones[b].bindWorld;
    deform[b] = cur * bones[b].bindWorld.inverse();
  }
  Mat4 toObject = self->objectWorldMatrix().inverse();
  for (size_t v = 0; v < st.mesh.positions.size(); ++v) {
    Vec3 world = meshBind.transformPoint(st.mesh.positions[v]);
    Vec3 acc(0.0f);
    float sum = 0.0f;
    for (const auto& [b, w] : weights[v]) {
      if (b < 0 || b >= static_cast<int>(deform.size())) continue;
      acc += deform[static_cast<size_t>(b)].transformPoint(world) * w;
      sum += w;
    }
    if (sum < 1.0f) acc += world * (1.0f - sum);  // unweighted share stays at the skin pose
    st.mesh.positions[v] = toObject.transformPoint(acc);
  }
}

json SkinModifier::toJson() const {
  json j = Modifier::toJson();
  json jb = json::array();
  for (const auto& b : bones)
    jb.push_back(json{{"node", b.nodeId}, {"bind", matJson(b.bindWorld)}, {"start", vec3ToJson(b.start)},
                      {"end", vec3ToJson(b.end)}, {"inner", b.innerRadius}, {"outer", b.outerRadius}});
  j["bones"] = jb;
  j["bound"] = bound;
  j["meshBind"] = matJson(meshBind);
  json jo = json::array();
  for (const auto& [v, w] : overrides) {
    json e = json::array({v});
    for (const auto& p : w) {
      e.push_back(p.first);
      e.push_back(p.second);
    }
    jo.push_back(e);
  }
  j["weights"] = jo;
  return j;
}

void SkinModifier::fromJson(const json& j) {
  Modifier::fromJson(j);
  bones.clear();
  overrides.clear();
  if (j.contains("bones"))
    for (const auto& e : j["bones"]) {
      SkinBone b;
      b.nodeId = e.value("node", 0u);
      b.bindWorld = matFrom(e.value("bind", json()));
      b.start = vec3FromJson(e.value("start", json()));
      b.end = vec3FromJson(e.value("end", json()), Vec3(10, 0, 0));
      b.innerRadius = e.value("inner", 5.0f);
      b.outerRadius = e.value("outer", 10.0f);
      bones.push_back(b);
    }
  bound = j.value("bound", false);
  meshBind = matFrom(j.value("meshBind", json()));
  if (j.contains("weights"))
    for (const auto& e : j["weights"]) {
      if (!e.is_array() || e.empty()) continue;
      VertexWeights w;
      for (size_t i = 1; i + 1 < e.size(); i += 2) w.emplace_back(e[i].get<int>(), e[i + 1].get<float>());
      overrides[e[0].get<int>()] = w;
    }
  touch();
}

// ---------------------------------------------------------------------------------------------
// Morpher

MorpherModifier::MorpherModifier() {
  params.beginGroup("Channel List");
  for (int i = 1; i <= kMorphChannels; ++i) params.addFloat("ch" + std::to_string(i), "Channel " + std::to_string(i), 0.0, -100.0, 200.0, 1.0, 1);
  params.beginGroup("Global Parameters");
  params.addBool("useLimits", "Use Limits", true);
  params.addFloat("minLimit", "Minimum", 0.0, -1000.0, 1000.0, 1.0, 1);
  params.addFloat("maxLimit", "Maximum", 100.0, -1000.0, 1000.0, 1.0, 1);
}

void MorpherModifier::apply(PolyMesh& mesh) const {
  std::vector<Vec3> base = mesh.positions;
  bool limits = params.getBool("useLimits");
  float lo = params.getFloat("minLimit"), hi = params.getFloat("maxLimit");
  for (int i = 0; i < kMorphChannels; ++i) {
    const MorphChannel& c = channels[static_cast<size_t>(i)];
    if (c.positions.size() != base.size()) continue;
    float w = params.getFloat("ch" + std::to_string(i + 1));
    if (limits) w = clampf(w, lo, hi);
    w /= 100.0f;
    if (std::fabs(w) < 1e-6f) continue;
    for (size_t v = 0; v < base.size(); ++v) mesh.positions[v] += (c.positions[v] - base[v]) * w;
  }
}

bool MorpherModifier::captureTarget(int index, const Node& target) {
  if (index < 0 || index >= kMorphChannels) return false;
  MorphChannel& c = channels[static_cast<size_t>(index)];
  c.positions = target.mesh().positions;
  c.sourceNode = target.id;
  c.name = target.name;
  touch();
  return !c.positions.empty();
}

json MorpherModifier::toJson() const {
  json j = Modifier::toJson();
  json jc = json::array();
  for (const auto& c : channels) {
    json p = json::array();
    for (const auto& v : c.positions) {
      p.push_back(v.x);
      p.push_back(v.y);
      p.push_back(v.z);
    }
    jc.push_back(json{{"name", c.name}, {"source", c.sourceNode}, {"p", p}});
  }
  j["channels"] = jc;
  return j;
}

void MorpherModifier::fromJson(const json& j) {
  Modifier::fromJson(j);
  channels.assign(kMorphChannels, MorphChannel());
  if (j.contains("channels"))
    for (size_t i = 0; i < j["channels"].size() && i < channels.size(); ++i) {
      const json& e = j["channels"][i];
      MorphChannel& c = channels[i];
      c.name = e.value("name", std::string());
      c.sourceNode = e.value("source", 0u);
      if (e.contains("p"))
        for (size_t k = 0; k + 2 < e["p"].size(); k += 3)
          c.positions.emplace_back(e["p"][k].get<float>(), e["p"][k + 1].get<float>(), e["p"][k + 2].get<float>());
    }
  touch();
}

// ---------------------------------------------------------------------------------------------
// Path Deform

PathDeformModifier::PathDeformModifier() {
  params.beginGroup("Parameters");
  params.addInt("path", "Path", 0, 0, 2147483647);
  params.addFloat("percent", "Percent", 0.0, -1e6, 1e6, 1.0, 1);
  params.addFloat("stretch", "Stretch", 1.0, -1e6, 1e6, 0.01, 3);
  params.addFloat("rotation", "Rotation", 0.0, -36000.0, 36000.0, 1.0, 1);
  params.addFloat("twist", "Twist", 0.0, -36000.0, 36000.0, 1.0, 1);
  params.beginGroup("Path Deform Axis");
  params.addEnum("axis", "Axis", {"X", "Y", "Z"}, 2);
  params.addBool("flip", "Flip", false);
  params.addBool("worldSpace", "Move Object to Path", true);
}

std::vector<uint32_t> PathDeformModifier::referencedNodes() const {
  int id = params.getInt("path");
  return id > 0 ? std::vector<uint32_t>{static_cast<uint32_t>(id)} : std::vector<uint32_t>{};
}

void deformAlongPolyline(std::vector<Vec3>& points, const std::vector<Vec3>& path, int axis, float percent,
                         float stretch, float rotationDeg, float twistDeg, bool flip) {
  if (path.size() < 2 || points.empty()) return;
  axis = std::clamp(axis, 0, 2);
  int b = (axis + 1) % 3, c = (axis + 2) % 3;
  if (axis == 2) {
    b = 0;
    c = 1;
  }
  // Arc lengths and parallel-transported frames along the path.
  size_t n = path.size();
  std::vector<float> cum(n, 0.0f);
  for (size_t i = 1; i < n; ++i) cum[i] = cum[i - 1] + length(path[i] - path[i - 1]);
  float total = cum.back();
  if (total < 1e-9f) return;
  std::vector<Vec3> tang(n), nrm(n);
  for (size_t i = 0; i < n; ++i) {
    Vec3 d = i + 1 < n ? path[i + 1] - path[i] : path[i] - path[i - 1];
    if (i > 0 && i + 1 < n) d = path[i + 1] - path[i - 1];
    tang[i] = length2(d) > 1e-20f ? normalize(d) : (i ? tang[i - 1] : Vec3(1, 0, 0));
  }
  Vec3 up = std::fabs(tang[0].z) < 0.95f ? Vec3(0, 0, 1) : Vec3(0, 1, 0);
  nrm[0] = normalize(cross(up, tang[0]));
  for (size_t i = 1; i < n; ++i) {
    Vec3 nn = nrm[i - 1] - tang[i] * dot(nrm[i - 1], tang[i]);
    nrm[i] = length2(nn) > 1e-12f ? normalize(nn) : nrm[i - 1];
  }
  float start = percent / 100.0f * total;
  float rot = radians(rotationDeg), twist = radians(twistDeg);
  for (auto& p : points) {
    float along = (flip ? -p[axis] : p[axis]) * stretch + start;
    // Locate the segment.
    Vec3 pos, t, nv;
    if (along <= 0.0f) {
      t = tang[0];
      nv = nrm[0];
      pos = path[0] + t * along;
    } else if (along >= total) {
      t = tang[n - 1];
      nv = nrm[n - 1];
      pos = path[n - 1] + t * (along - total);
    } else {
      size_t i = static_cast<size_t>(std::upper_bound(cum.begin(), cum.end(), along) - cum.begin());
      i = std::clamp<size_t>(i, 1, n - 1);
      float seg = cum[i] - cum[i - 1];
      float f = seg > 1e-12f ? (along - cum[i - 1]) / seg : 0.0f;
      pos = lerp(path[i - 1], path[i], f);
      t = normalize(lerp(tang[i - 1], tang[i], f));
      nv = lerp(nrm[i - 1], nrm[i], f);
      nv = normalize(nv - t * dot(nv, t));
    }
    Vec3 bv = cross(t, nv);
    float ang = rot + twist * (along - start) / total;
    float ca = std::cos(ang), sa = std::sin(ang);
    float x = p[b], y = flip ? -p[c] : p[c];
    float rx = ca * x - sa * y, ry = sa * x + ca * y;
    p = pos + nv * rx + bv * ry;
  }
}

void PathDeformModifier::applyState(GeomState& st) const {
  if (st.isShape || !st.scene) return;
  int pid = params.getInt("path");
  const Node* pathNode = pid > 0 ? st.scene->findNode(static_cast<uint32_t>(pid)) : nullptr;
  const Node* self = st.scene->findNode(st.nodeId);
  if (!pathNode || !self || pathNode == self) return;
  const auto& lines = pathNode->shapeLines();
  if (lines.empty() || lines[0].size() < 2) return;
  std::vector<Vec3> path = lines[0];
  Mat4 rel = params.getBool("worldSpace") ? self->objectWorldMatrix().inverse() * pathNode->objectWorldMatrix()
                                          : Mat4::identity();
  for (auto& p : path) p = rel.transformPoint(p);
  deformAlongPolyline(st.mesh.positions, path, params.getInt("axis"), params.getFloat("percent"),
                      params.getFloat("stretch"), params.getFloat("rotation"), params.getFloat("twist"),
                      params.getBool("flip"));
}

void registerRiggingTypes() {
  registerObjectType({"Bone", "Bones", "Systems", "Standard", [] { return std::make_unique<BoneObject>(); }});
  // IK chains are made with Animation > IK Solvers, not from the Create panel.
  registerObjectType({"IKChain", "IK Chain", "", "", [] { return std::make_unique<IKChainObject>(); }});
}

void registerRiggingModifiers() {
  registerModifierType({"Skin", "Skin", "Animation", true, false, [] { return std::make_unique<SkinModifier>(); }});
  registerModifierType({"Morpher", "Morpher", "Animation", true, false, [] { return std::make_unique<MorpherModifier>(); }});
  registerModifierType({"PathDeform", "Path Deform", "Animation", true, false,
                        [] { return std::make_unique<PathDeformModifier>(); }});
}

}  // namespace veraa
