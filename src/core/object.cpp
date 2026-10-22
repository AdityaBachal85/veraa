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

void SceneObject::touch() {
  static std::atomic<uint64_t> counter{1};
  revision_ = ++counter;
}

json polyMeshToJson(const PolyMesh& mesh) {
  json p = json::array(), t = json::array(), f = json::array();
  for (const auto& v : mesh.positions) { p.push_back(v.x); p.push_back(v.y); p.push_back(v.z); }
  for (const auto& v : mesh.uvs) { t.push_back(v.x); t.push_back(v.y); }
  for (const auto& face : mesh.faces) {
    json jf{{"v", face.v}};
    if (!face.uv.empty()) jf["t"] = face.uv;
    if (face.matId != 0) jf["m"] = face.matId;
    if (face.smoothGroups != 1) jf["s"] = face.smoothGroups;
    f.push_back(std::move(jf));
  }
  return json{{"p", std::move(p)}, {"t", std::move(t)}, {"f", std::move(f)}};
}

PolyMesh polyMeshFromJson(const json& m) {
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

json EditablePolyObject::toJson() const {
  json j = SceneObject::toJson();
  j["mesh"] = polyMeshToJson(mesh);
  j["sel"] = sel.toJson();
  return j;
}

void EditablePolyObject::fromJson(const json& j) {
  SceneObject::fromJson(j);
  mesh = j.contains("mesh") ? polyMeshFromJson(j["mesh"]) : PolyMesh();
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
  std::vector<int> remap(mesh.positions.size());
  std::vector<Vec3> newPos;
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
