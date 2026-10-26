#include "object.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <unordered_map>

#include "registry.h"

namespace veraa {

json SceneObject::toJson() const {
  return json{{"type", type()}, {"params", params.toJson()}};
}

void SceneObject::fromJson(const json& j) {
  if (j.contains("params")) params.fromJson(j["params"]);
}

void SceneObject::touch() { revision_ = nextRevisionStamp(); }

void to_json(json& j, const IndexList& l) {
  j = json::array();
  for (int i : l) j.push_back(i);
}

void from_json(const json& j, IndexList& l) {
  l.clear();
  if (!j.is_array()) return;
  l.reserve(j.size());
  for (const auto& e : j) l.push_back(e.get<int>());
}

namespace {

// Meshes are stored as base64 little-endian binary arrays inside the JSON ("bin": 1). That is
// many times faster to write and read than JSON number arrays, and smaller.
const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64(const void* data, size_t n) {
  const auto* p = static_cast<const uint8_t*>(data);
  std::string out;
  out.resize((n + 2) / 3 * 4);
  if (out.empty()) return out;
  char* o = &out[0];
  size_t i = 0;
  for (; i + 2 < n; i += 3) {
    uint32_t v = (uint32_t(p[i]) << 16) | (uint32_t(p[i + 1]) << 8) | p[i + 2];
    *o++ = kB64[v >> 18];
    *o++ = kB64[(v >> 12) & 63];
    *o++ = kB64[(v >> 6) & 63];
    *o++ = kB64[v & 63];
  }
  if (i < n) {
    uint32_t v = uint32_t(p[i]) << 16;
    if (i + 1 < n) v |= uint32_t(p[i + 1]) << 8;
    *o++ = kB64[v >> 18];
    *o++ = kB64[(v >> 12) & 63];
    *o++ = i + 1 < n ? kB64[(v >> 6) & 63] : '=';
    *o++ = '=';
  }
  return out;
}

// Decodes into `out` (resized); returns false on malformed input.
bool unbase64(const std::string& s, std::vector<uint8_t>& out) {
  static const std::vector<int8_t> table = [] {
    std::vector<int8_t> t(256, -1);
    for (int i = 0; i < 64; ++i) t[static_cast<uint8_t>(kB64[i])] = static_cast<int8_t>(i);
    return t;
  }();
  size_t n = s.size();
  while (n > 0 && s[n - 1] == '=') --n;
  out.resize(n * 3 / 4);
  uint8_t* o = out.data();
  uint32_t acc = 0;
  int bits = 0;
  for (size_t i = 0; i < n; ++i) {
    int8_t v = table[static_cast<uint8_t>(s[i])];
    if (v < 0) return false;
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      *o++ = static_cast<uint8_t>(acc >> bits);
    }
  }
  out.resize(static_cast<size_t>(o - out.data()));
  return true;
}

template <typename T>
bool readArray(const json& m, const char* key, std::vector<T>& out, size_t count) {
  out.clear();
  auto it = m.find(key);
  if (it == m.end()) return count == 0;
  if (!it->is_string()) return false;
  std::vector<uint8_t> bytes;
  if (!unbase64(it->get_ref<const std::string&>(), bytes) || bytes.size() != count * sizeof(T)) return false;
  out.resize(count);
  if (count) std::memcpy(out.data(), bytes.data(), bytes.size());
  return true;
}

PolyMesh polyMeshFromBinary(const json& m) {
  PolyMesh mesh;
  size_t nv = m.value("nv", size_t(0)), nt = m.value("nt", size_t(0)), nf = m.value("nf", size_t(0));
  std::vector<float> p, t;
  std::vector<uint8_t> counts8, hasUv;
  std::vector<uint32_t> counts32, smooth;
  std::vector<int32_t> vi, ui, mat;
  if (!readArray(m, "p", p, nv * 3) || !readArray(m, "t", t, nt * 2)) return mesh;
  bool wide = m.contains("c32");
  if (wide ? !readArray(m, "c32", counts32, nf) : !readArray(m, "c", counts8, nf)) return mesh;
  size_t corners = 0;
  for (size_t f = 0; f < nf; ++f) corners += wide ? counts32[f] : counts8[f];
  if (!readArray(m, "v", vi, corners)) return mesh;
  if (m.contains("hu") && !readArray(m, "hu", hasUv, nf)) return mesh;
  bool anyUv = m.contains("u");
  size_t uvCorners = 0;
  if (anyUv)
    for (size_t f = 0; f < nf; ++f)
      if (hasUv.empty() || hasUv[f]) uvCorners += wide ? counts32[f] : counts8[f];
  if (anyUv && !readArray(m, "u", ui, uvCorners)) return mesh;
  if (m.contains("m") && !readArray(m, "m", mat, nf)) return mesh;
  if (m.contains("s") && !readArray(m, "s", smooth, nf)) return mesh;
  mesh.positions.resize(nv);
  for (size_t i = 0; i < nv; ++i) mesh.positions[i] = Vec3(p[i * 3], p[i * 3 + 1], p[i * 3 + 2]);
  mesh.uvs.resize(nt);
  for (size_t i = 0; i < nt; ++i) mesh.uvs[i] = Vec2(t[i * 2], t[i * 2 + 1]);
  mesh.faces.resize(nf);
  size_t c = 0, cu = 0;
  for (size_t f = 0; f < nf; ++f) {
    PolyMesh::Face& face = mesh.faces[f];
    size_t n = wide ? counts32[f] : counts8[f];
    face.v.assign(vi.data() + c, vi.data() + c + n);
    c += n;
    if (anyUv && (hasUv.empty() || hasUv[f])) {
      face.uv.assign(ui.data() + cu, ui.data() + cu + n);
      cu += n;
    }
    if (!mat.empty()) face.matId = mat[f];
    if (!smooth.empty()) face.smoothGroups = smooth[f];
  }
  return mesh;
}

thread_local MeshRefTable* t_meshRefs = nullptr;

}  // namespace

MeshRefScope::MeshRefScope(MeshRefTable* table) : previous_(t_meshRefs) { t_meshRefs = table; }
MeshRefScope::~MeshRefScope() { t_meshRefs = previous_; }
MeshRefTable* MeshRefScope::current() { return t_meshRefs; }

json polyMeshToJson(const PolyMesh& mesh) {
  size_t nf = mesh.faces.size();
  std::vector<float> p(mesh.positions.size() * 3), t(mesh.uvs.size() * 2);
  for (size_t i = 0; i < mesh.positions.size(); ++i) {
    p[i * 3] = mesh.positions[i].x;
    p[i * 3 + 1] = mesh.positions[i].y;
    p[i * 3 + 2] = mesh.positions[i].z;
  }
  for (size_t i = 0; i < mesh.uvs.size(); ++i) {
    t[i * 2] = mesh.uvs[i].x;
    t[i * 2 + 1] = mesh.uvs[i].y;
  }
  bool wide = false, anyUv = false, allUv = true, anyMat = false, anySmooth = false;
  size_t corners = 0;
  for (const auto& f : mesh.faces) {
    wide |= f.v.size() > 255;
    bool faceUv = !f.uv.empty() && f.uv.size() == f.v.size();
    anyUv |= faceUv;
    allUv &= faceUv;
    anyMat |= f.matId != 0;
    anySmooth |= f.smoothGroups != 1;
    corners += f.v.size();
  }
  std::vector<uint8_t> c8, hu;
  std::vector<uint32_t> c32, s;
  std::vector<int32_t> v, u, m;
  v.reserve(corners);
  if (anyUv) u.reserve(corners);
  if (wide) c32.reserve(nf);
  else c8.reserve(nf);
  for (const auto& f : mesh.faces) {
    if (wide) c32.push_back(static_cast<uint32_t>(f.v.size()));
    else c8.push_back(static_cast<uint8_t>(f.v.size()));
    v.insert(v.end(), f.v.begin(), f.v.end());
    bool faceUv = !f.uv.empty() && f.uv.size() == f.v.size();
    if (anyUv && faceUv) u.insert(u.end(), f.uv.begin(), f.uv.end());
    if (anyUv && !allUv) hu.push_back(faceUv ? 1 : 0);
    if (anyMat) m.push_back(f.matId);
    if (anySmooth) s.push_back(f.smoothGroups);
  }
  json j{{"bin", 1},
         {"nv", mesh.positions.size()},
         {"nt", mesh.uvs.size()},
         {"nf", nf},
         {"p", base64(p.data(), p.size() * sizeof(float))},
         {"v", base64(v.data(), v.size() * sizeof(int32_t))}};
  if (!t.empty()) j["t"] = base64(t.data(), t.size() * sizeof(float));
  if (wide) j["c32"] = base64(c32.data(), c32.size() * sizeof(uint32_t));
  else j["c"] = base64(c8.data(), c8.size());
  if (anyUv) j["u"] = base64(u.data(), u.size() * sizeof(int32_t));
  if (!hu.empty()) j["hu"] = base64(hu.data(), hu.size());
  if (anyMat) j["m"] = base64(m.data(), m.size() * sizeof(int32_t));
  if (anySmooth) j["s"] = base64(s.data(), s.size() * sizeof(uint32_t));
  return j;
}

PolyMesh polyMeshFromJson(const json& m) {
  if (m.is_object() && m.value("bin", 0) == 1) {
    PolyMesh mesh = polyMeshFromBinary(m);
    if (!mesh.isValid()) mesh.clear();
    return mesh;
  }
  PolyMesh mesh;
  if (!m.is_object()) return mesh;
  if (m.contains("p")) {
    const json& p = m["p"];
    for (size_t i = 0; i + 2 < p.size(); i += 3)
      mesh.positions.emplace_back(p[i].get<float>(), p[i + 1].get<float>(), p[i + 2].get<float>());
  }
  if (m.contains("t")) {
    const json& t = m["t"];
    for (size_t i = 0; i + 1 < t.size(); i += 2) mesh.uvs.emplace_back(t[i].get<float>(), t[i + 1].get<float>());
  }
  if (m.contains("f")) {
    for (const auto& jf : m["f"]) {
      PolyMesh::Face face;
      face.v = jf.value("v", std::vector<int>{});
      face.uv = jf.value("t", std::vector<int>{});
      face.matId = jf.value("m", 0);
      face.smoothGroups = jf.value("s", 1u);
      mesh.faces.push_back(std::move(face));
    }
  }
  if (!mesh.isValid()) mesh.clear();
  return mesh;
}

json bezierShapeToJson(const BezierShape& shape) {
  json splines = json::array();
  for (const auto& s : shape.splines) {
    json knots = json::array();
    for (const auto& k : s.knots)
      knots.push_back(json{{"p", vec3ToJson(k.p)}, {"in", vec3ToJson(k.in)}, {"out", vec3ToJson(k.out)},
                           {"type", static_cast<int>(k.type)}});
    splines.push_back(json{{"closed", s.closed}, {"knots", std::move(knots)}});
  }
  return splines;
}

BezierShape bezierShapeFromJson(const json& j) {
  BezierShape shape;
  if (!j.is_array()) return shape;
  for (const auto& js : j) {
    Spline s;
    s.closed = js.value("closed", false);
    if (js.contains("knots"))
      for (const auto& jk : js["knots"]) {
        Knot k;
        k.p = vec3FromJson(jk.value("p", json()));
        k.in = vec3FromJson(jk.value("in", json()), k.p);
        k.out = vec3FromJson(jk.value("out", json()), k.p);
        k.type = static_cast<KnotType>(std::clamp(jk.value("type", 0), 0, 3));
        s.knots.push_back(k);
      }
    shape.splines.push_back(std::move(s));
  }
  return shape;
}

PolyMesh EditablePolyObject::build() const {
  const PolyMesh& mesh = *mesh_;
  if (sel.hiddenFaces.empty()) return mesh;
  PolyMesh out;
  out.positions = mesh.positions;
  out.uvs = mesh.uvs;
  for (size_t f = 0; f < mesh.faces.size(); ++f)
    if (!sel.isFaceHidden(static_cast<int>(f))) out.faces.push_back(mesh.faces[f]);
  return out;
}

PolyMesh& EditablePolyObject::editMesh() {
  // Unshare before writing (copy-on-write). mesh_ always points at a non-const PolyMesh made by
  // make_shared<PolyMesh>, so the const_cast is safe once this object is the only owner.
  if (mesh_.use_count() != 1) mesh_ = std::make_shared<PolyMesh>(*mesh_);
  return const_cast<PolyMesh&>(*mesh_);
}

void EditablePolyObject::setMesh(PolyMesh m) { mesh_ = std::make_shared<PolyMesh>(std::move(m)); }

void EditablePolyObject::setSharedMesh(std::shared_ptr<const PolyMesh> m) {
  mesh_ = m ? std::move(m) : std::make_shared<PolyMesh>();
}

json EditablePolyObject::toJson() const {
  json j = SceneObject::toJson();
  if (MeshRefTable* refs = MeshRefScope::current()) {
    uint64_t id = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(mesh_.get()));
    (*refs)[id] = mesh_;
    j["meshRef"] = id;
  } else {
    j["mesh"] = polyMeshToJson(*mesh_);
  }
  j["sel"] = sel.toJson();
  return j;
}

void EditablePolyObject::fromJson(const json& j) {
  SceneObject::fromJson(j);
  std::shared_ptr<const PolyMesh> shared;
  if (MeshRefTable* refs = MeshRefScope::current(); refs && j.contains("meshRef")) {
    auto it = refs->find(j["meshRef"].get<uint64_t>());
    if (it != refs->end()) shared = it->second;
  }
  if (shared) mesh_ = std::move(shared);
  else setMesh(j.contains("mesh") ? polyMeshFromJson(j["mesh"]) : PolyMesh());
  sel = PolySelection();
  if (j.contains("sel")) sel.fromJson(j["sel"]);
}

void ShapeObject::addShapeParams(ParamBlock& p) {
  p.beginGroup("Rendering");
  p.addBool("renderInRenderer", "Enable In Renderer", false);
  p.addBool("renderInViewport", "Enable In Viewport", false);
  p.addFloat("thickness", "Thickness", 1.0, 0.0, 1e6, 0.1);
  p.addInt("sides", "Sides", 12, 3, 100);
  p.addFloat("angle", "Angle", 0.0, -360.0, 360.0, 1.0, 1);
  p.beginGroup("Interpolation");
  p.addInt("steps", "Steps", 6, 0, 100);
  p.addBool("optimize", "Optimize", true);
}

json EditableSplineObject::toJson() const {
  json j = SceneObject::toJson();
  j["shape"] = bezierShapeToJson(shape);
  return j;
}

void EditableSplineObject::fromJson(const json& j) {
  SceneObject::fromJson(j);
  shape = j.contains("shape") ? bezierShapeFromJson(j["shape"]) : BezierShape();
}

namespace {

void addCommonLightParams(ParamBlock& p, float intensity, const Vec3& color = Vec3(1.0f)) {
  p.beginGroup("General");
  p.addBool("enabled", "On", true);
  p.addColor("color", "Color", color);
  p.addFloat("intensity", "Multiplier", intensity, 0.0, 1e6, 0.1, 3);
  p.addBool("castShadows", "Shadows", true);
}

class OmniLight : public LightObject {
 public:
  OmniLight() {
    addCommonLightParams(params, 1.0f);
    params.beginGroup("Shadows");
    params.addFloat("radius", "Shadow radius", 0.0, 0.0, 1e5, 0.5);
    params.beginGroup("Intensity");
    params.addEnum("decay", "Decay", {"None", "Inverse square"}, 1);
  }
  std::string type() const override { return "OmniLight"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<OmniLight>(*this); }
};

class SpotLight : public LightObject {
 public:
  SpotLight() {
    addCommonLightParams(params, 1.0f);
    params.beginGroup("Spotlight");
    params.addFloat("hotspot", "Hotspot/Beam", 40.0, 0.5, 179.0, 1.0, 1);
    params.addFloat("falloff", "Falloff/Field", 45.0, 1.0, 179.5, 1.0, 1);
    params.beginGroup("Shadows");
    params.addFloat("radius", "Shadow radius", 0.0, 0.0, 1e5, 0.5);
    params.beginGroup("Intensity");
    params.addEnum("decay", "Decay", {"None", "Inverse square"}, 1);
  }
  std::string type() const override { return "SpotLight"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<SpotLight>(*this); }
};

// Rectangular area light, like a V-Ray Plane light.
class RectLight : public LightObject {
 public:
  RectLight() {
    addCommonLightParams(params, 5.0f);
    params.beginGroup("Size");
    params.addFloat("width", "Width (X)", 100.0, 0.01, 1e6, 1.0);
    params.addFloat("length", "Length (Y)", 100.0, 0.01, 1e6, 1.0);
    params.beginGroup("Options");
    params.addBool("doubleSided", "Double-sided", false);
    params.addBool("invisible", "Invisible", false);
  }
  std::string type() const override { return "RectLight"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<RectLight>(*this); }
};

class SphereLight : public LightObject {
 public:
  SphereLight() {
    addCommonLightParams(params, 10.0f);
    params.beginGroup("Size");
    params.addFloat("radius", "Radius", 10.0, 0.01, 1e6, 0.5);
    params.beginGroup("Options");
    params.addBool("invisible", "Invisible", false);
  }
  std::string type() const override { return "SphereLight"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<SphereLight>(*this); }
};

// Directional sun. Its local -Z axis points from the sun toward the scene.
class SunLight : public LightObject {
 public:
  SunLight() {
    addCommonLightParams(params, 3.0f, Vec3(1.0f, 0.95f, 0.88f));
    params.beginGroup("Sun");
    params.addFloat("sizeMultiplier", "Size multiplier", 1.0, 0.0, 100.0, 0.1);
  }
  std::string type() const override { return "SunLight"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<SunLight>(*this); }
};

}  // namespace

CameraObject::CameraObject() {
  params.beginGroup("Lens");
  params.addFloat("fov", "FOV (horizontal)", 45.0, 1.0, 175.0, 1.0, 1);
  params.beginGroup("Depth of field");
  params.addBool("dof", "Enable", false);
  params.addFloat("aperture", "Aperture radius", 1.0, 0.0, 1e4, 0.1);
  params.addFloat("focusDistance", "Focus distance", 300.0, 0.01, 1e7, 1.0);
}

float CameraObject::verticalFovDeg(float aspect) const {
  float h = radians(params.getFloat("fov"));
  return degrees(2.0f * std::atan(std::tan(h * 0.5f) / std::max(aspect, 1e-6f)));
}

namespace {

std::vector<ObjectTypeInfo>& registry() {
  static std::vector<ObjectTypeInfo> types;
  return types;
}

void ensureBuiltins() {
  static bool done = false;
  if (done) return;
  done = true;
  registerBuiltinObjectTypes();
}

}  // namespace

void registerCoreObjectTypes() {
  registerObjectType({"EditablePoly", "Editable Poly", "", "", [] { return std::make_unique<EditablePolyObject>(); }});
  registerObjectType(
      {"EditableSpline", "Editable Spline", "", "", [] { return std::make_unique<EditableSplineObject>(); }});
  const char* lights = "Lights";
  const char* std = "Standard";
  registerObjectType({"RectLight", "Plane Light", lights, std, [] { return std::make_unique<RectLight>(); }});
  registerObjectType({"SphereLight", "Sphere Light", lights, std, [] { return std::make_unique<SphereLight>(); }});
  registerObjectType({"OmniLight", "Omni", lights, std, [] { return std::make_unique<OmniLight>(); }});
  registerObjectType({"SpotLight", "Spot", lights, std, [] { return std::make_unique<SpotLight>(); }});
  registerObjectType({"SunLight", "Sun", lights, std, [] { return std::make_unique<SunLight>(); }});
  registerObjectType({"Camera", "Camera", "Cameras", std, [] { return std::make_unique<CameraObject>(); }});
}

void registerObjectType(ObjectTypeInfo info) {
  auto& reg = registry();
  for (auto& t : reg)
    if (t.type == info.type) {
      t = std::move(info);
      return;
    }
  reg.push_back(std::move(info));
}

const std::vector<ObjectTypeInfo>& objectTypes() {
  ensureBuiltins();
  return registry();
}

const ObjectTypeInfo* findObjectType(const std::string& type) {
  for (const auto& t : objectTypes())
    if (t.type == type) return &t;
  return nullptr;
}

std::vector<const ObjectTypeInfo*> objectTypesIn(const std::string& category, const std::string& subcategory) {
  std::vector<const ObjectTypeInfo*> out;
  for (const auto& t : objectTypes())
    if (t.category == category && (subcategory.empty() || t.subcategory == subcategory)) out.push_back(&t);
  return out;
}

std::vector<std::string> objectSubcategories(const std::string& category) {
  std::vector<std::string> out;
  for (const auto& t : objectTypes())
    if (t.category == category && std::find(out.begin(), out.end(), t.subcategory) == out.end())
      out.push_back(t.subcategory);
  return out;
}

std::unique_ptr<SceneObject> createObject(const std::string& type) {
  const ObjectTypeInfo* info = findObjectType(type);
  return info && info->create ? info->create() : nullptr;
}

const std::vector<std::string>& geometryTypes() {
  static std::vector<std::string> types;
  types.clear();
  for (const auto* t : objectTypesIn("Geometry"))
    if (t->subcategory == "Standard Primitives" || t->subcategory == "Extended Primitives") types.push_back(t->type);
  return types;
}

const std::vector<std::string>& lightTypes() {
  static std::vector<std::string> types;
  types.clear();
  for (const auto* t : objectTypesIn("Lights")) types.push_back(t->type);
  return types;
}

std::string objectDisplayName(const std::string& type) {
  const ObjectTypeInfo* info = findObjectType(type);
  return info && !info->displayName.empty() ? info->displayName : type;
}

void weldVertices(PolyMesh& mesh) {
  struct Key {
    uint32_t x, y, z;
    bool operator==(const Key& o) const { return x == o.x && y == o.y && z == o.z; }
  };
  struct KeyHash {
    size_t operator()(const Key& k) const { return (k.x * 73856093u) ^ (k.y * 19349663u) ^ (k.z * 83492791u); }
  };
  auto bits = [](float f) {
    if (f == 0.0f) f = 0.0f;  // merge -0 and +0
    uint32_t u;
    std::memcpy(&u, &f, sizeof(u));
    return u;
  };

  std::unordered_map<Key, int, KeyHash> map;
  map.reserve(mesh.positions.size());
  std::vector<int> remap(mesh.positions.size());
  std::vector<Vec3> newPos;
  newPos.reserve(mesh.positions.size());
  for (size_t i = 0; i < mesh.positions.size(); ++i) {
    const Vec3& p = mesh.positions[i];
    Key k{bits(p.x), bits(p.y), bits(p.z)};
    auto it = map.find(k);
    if (it == map.end()) {
      int idx = static_cast<int>(newPos.size());
      newPos.push_back(p);
      map.emplace(k, idx);
      remap[i] = idx;
    } else {
      remap[i] = it->second;
    }
  }
  mesh.positions = std::move(newPos);

  std::vector<PolyMesh::Face> faces;
  faces.reserve(mesh.faces.size());
  for (auto& f : mesh.faces) {
    PolyMesh::Face nf;
    nf.matId = f.matId;
    nf.smoothGroups = f.smoothGroups;
    bool hasUv = !f.uv.empty();
    for (size_t k = 0; k < f.v.size(); ++k) {
      int v = remap[f.v[k]];
      if (!nf.v.empty() && nf.v.back() == v) continue;
      nf.v.push_back(v);
      if (hasUv) nf.uv.push_back(f.uv[k]);
    }
    while (nf.v.size() > 1 && nf.v.front() == nf.v.back()) {
      nf.v.pop_back();
      if (hasUv) nf.uv.pop_back();
    }
    if (nf.v.size() >= 3) faces.push_back(std::move(nf));
  }
  mesh.faces = std::move(faces);
}

}  // namespace veraa
