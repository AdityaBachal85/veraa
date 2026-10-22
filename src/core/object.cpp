#include "object.h"

#include <cstring>
#include <unordered_map>

#include "primitives.h"

namespace veraa {

json SceneObject::toJson() const {
  return json{{"type", type()}, {"params", params.toJson()}};
}

void SceneObject::fromJson(const json& j) {
  if (j.contains("params")) params.fromJson(j["params"]);
}

json EditablePolyObject::toJson() const {
  json j = SceneObject::toJson();
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
  j["mesh"] = json{{"p", std::move(p)}, {"t", std::move(t)}, {"f", std::move(f)}};
  return j;
}

void EditablePolyObject::fromJson(const json& j) {
  SceneObject::fromJson(j);
  mesh.clear();
  if (!j.contains("mesh")) return;
  const json& m = j["mesh"];
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

std::unique_ptr<SceneObject> createObject(const std::string& type) {
  if (auto p = createPrimitive(type)) return p;
  if (type == "EditablePoly") return std::make_unique<EditablePolyObject>();
  if (type == "OmniLight") return std::make_unique<OmniLight>();
  if (type == "SpotLight") return std::make_unique<SpotLight>();
  if (type == "RectLight") return std::make_unique<RectLight>();
  if (type == "SphereLight") return std::make_unique<SphereLight>();
  if (type == "SunLight") return std::make_unique<SunLight>();
  if (type == "Camera") return std::make_unique<CameraObject>();
  return nullptr;
}

const std::vector<std::string>& geometryTypes() { return primitiveTypes(); }

const std::vector<std::string>& lightTypes() {
  static const std::vector<std::string> types = {"RectLight", "SphereLight", "OmniLight", "SpotLight", "SunLight"};
  return types;
}

std::string objectDisplayName(const std::string& type) {
  static const std::unordered_map<std::string, std::string> names = {
      {"ChamferBox", "Chamfer Box"}, {"GeoSphere", "GeoSphere"},   {"EditablePoly", "Editable Poly"},
      {"OmniLight", "Omni"},         {"SpotLight", "Spot"},         {"RectLight", "Plane Light"},
      {"SphereLight", "Sphere Light"}, {"SunLight", "Sun"},
  };
  auto it = names.find(type);
  return it == names.end() ? type : it->second;
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
