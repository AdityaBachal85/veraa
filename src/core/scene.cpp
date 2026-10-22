#include "scene.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace veraa {

json Transform::toJson() const {
  return json{{"position", vec3ToJson(position)}, {"rotation", vec3ToJson(rotation)}, {"scale", vec3ToJson(scale)}};
}

void Transform::fromJson(const json& j) {
  position = vec3FromJson(j.value("position", json()), Vec3());
  rotation = vec3FromJson(j.value("rotation", json()), Vec3());
  scale = vec3FromJson(j.value("scale", json()), Vec3(1.0f));
}

std::unique_ptr<Node> Node::clone() const {
  auto n = std::make_unique<Node>();
  n->name = name;
  n->transform = transform;
  n->wireColor = wireColor;
  n->hidden = hidden;
  n->renderable = renderable;
  n->materialId = materialId;
  if (object) n->object = object->clone();
  for (const auto& m : modifiers) n->modifiers.push_back(m->clone());
  return n;
}

void Node::evaluate() const {
  if (!dirty_) return;
  mesh_.clear();
  if (auto* g = geometry()) {
    mesh_ = g->build();
    for (const auto& m : modifiers)
      if (m->enabled) m->apply(mesh_);
  }
  tri_ = triangulate(mesh_);
  dirty_ = false;
}

const PolyMesh& Node::mesh() const {
  evaluate();
  return mesh_;
}

const TriMesh& Node::triMesh() const {
  evaluate();
  return tri_;
}

Aabb Node::worldBounds() const {
  if (kind() != ObjectKind::Geometry) {
    Aabb b;
    b.expand(transform.position);
    return b;
  }
  return triMesh().bounds.transformed(worldMatrix());
}

json Node::toJson() const {
  json j{{"id", id},
         {"name", name},
         {"transform", transform.toJson()},
         {"wireColor", vec3ToJson(wireColor)},
         {"hidden", hidden},
         {"renderable", renderable},
         {"material", materialId}};
  if (object) j["object"] = object->toJson();
  json mods = json::array();
  for (const auto& m : modifiers) mods.push_back(m->toJson());
  j["modifiers"] = std::move(mods);
  return j;
}

void Node::fromJson(const json& j) {
  id = j.value("id", 0u);
  name = j.value("name", std::string("Object"));
  if (j.contains("transform")) transform.fromJson(j["transform"]);
  wireColor = vec3FromJson(j.value("wireColor", json()), wireColor);
  hidden = j.value("hidden", false);
  renderable = j.value("renderable", true);
  materialId = j.value("material", 0u);
  if (j.contains("object")) {
    std::string type = j["object"].value("type", std::string());
    object = createObject(type);
    if (!object) throw std::runtime_error("Unknown object type: " + type);
    object->fromJson(j["object"]);
  }
  modifiers.clear();
  if (j.contains("modifiers"))
    for (const auto& jm : j["modifiers"]) {
      std::string type = jm.value("type", std::string());
      auto m = createModifier(type);
      if (!m) throw std::runtime_error("Unknown modifier type: " + type);
      m->fromJson(jm);
      modifiers.push_back(std::move(m));
    }
  dirty_ = true;
}

void defineRenderSettings(ParamBlock& p) {
  p.beginGroup("Output size");
  p.addInt("width", "Width", 1280, 16, 16384);
  p.addInt("height", "Height", 720, 16, 16384);
  p.beginGroup("Image sampler");
  p.addInt("samples", "Max samples per pixel", 512, 1, 1000000);
  p.addFloat("timeLimit", "Time limit (s, 0 = none)", 0.0, 0.0, 1e6, 1.0, 0);
  p.beginGroup("Global illumination");
  p.addInt("maxDepth", "Max bounces", 8, 1, 64);
  p.addFloat("clampIndirect", "Clamp indirect (0 = off)", 10.0, 0.0, 1e6, 1.0, 1);
  p.beginGroup("Color mapping");
  p.addFloat("exposure", "Exposure (EV)", 0.0, -20.0, 20.0, 0.1, 2);
  p.addEnum("tonemap", "Tone mapping", {"Filmic (ACES)", "Reinhard", "Linear"}, 0);
  p.addFloat("contrast", "Contrast", 1.0, 0.0, 3.0, 0.05, 2);
  p.addFloat("saturation", "Saturation", 1.0, 0.0, 3.0, 0.05, 2);
  p.addBool("denoise", "Denoiser", true);
  p.beginGroup("Environment");
  p.addEnum("envMode", "Environment", {"Color", "Sky gradient", "HDRI", "Physical sky"}, 1);
  p.addColor("envColor", "Color", Vec3(0.5f, 0.6f, 0.75f));
  p.addFloat("envIntensity", "Intensity", 1.0, 0.0, 1e6, 0.1);
  p.addColor("skyZenith", "Sky zenith", Vec3(0.25f, 0.45f, 0.85f));
  p.addColor("skyHorizon", "Sky horizon", Vec3(0.8f, 0.85f, 0.9f));
  p.addColor("groundColor", "Ground", Vec3(0.3f, 0.28f, 0.25f));
  p.addFile("hdriPath", "HDRI file", "HDR images (*.hdr *.exr);;All files (*)");
  p.addFloat("hdriRotation", "HDRI rotation", 0.0, -360.0, 360.0, 1.0, 1);
  p.addFloat("hdriIntensity", "HDRI intensity", 1.0, 0.0, 1e6, 0.1);
  p.addFloat("skyTurbidity", "Sky turbidity", 3.0, 1.7, 10.0, 0.1);
  p.addBool("envVisible", "Visible to camera", true);
  p.addColor("backgroundColor", "Background", Vec3(0.0f));
  p.beginGroup("Camera");
  p.addString("camera", "Render camera", "");
}

Scene::Scene() { defineRenderSettings(renderSettings); }

namespace {

// Pleasant random-looking wire colors, cycled like 3ds Max's object colors.
const Vec3 kWireColors[] = {{0.53f, 0.81f, 0.98f}, {0.89f, 0.6f, 0.34f},  {0.6f, 0.89f, 0.6f},
                            {0.88f, 0.53f, 0.75f}, {0.96f, 0.87f, 0.45f}, {0.55f, 0.58f, 0.95f},
                            {0.42f, 0.8f, 0.76f},  {0.94f, 0.5f, 0.5f},   {0.74f, 0.62f, 0.9f}};

std::string baseNameFor(const std::string& type) {
  std::string n = objectDisplayName(type);
  n.erase(std::remove(n.begin(), n.end(), ' '), n.end());
  return n;
}

}  // namespace

Node* Scene::createNode(const std::string& objectType) {
  auto obj = createObject(objectType);
  if (!obj) return nullptr;
  auto node = std::make_unique<Node>();
  node->name = uniqueNodeName(baseNameFor(objectType));
  node->object = std::move(obj);
  if (node->kind() == ObjectKind::Geometry) {
    node->wireColor = kWireColors[colorCounter_++ % (sizeof(kWireColors) / sizeof(kWireColors[0]))];
  } else if (node->kind() == ObjectKind::Light) {
    node->wireColor = Vec3(0.95f, 0.85f, 0.2f);
  } else {
    node->wireColor = Vec3(0.3f, 0.6f, 1.0f);
  }
  return addNode(std::move(node));
}

Node* Scene::addNode(std::unique_ptr<Node> node) {
  node->id = nextNodeId_++;
  if (node->name.empty()) node->name = uniqueNodeName("Object");
  if (findNodeByName(node->name)) {
    // Strip a trailing 3-digit counter so a copy of "Box001" becomes "Box002".
    std::string base = node->name;
    auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
    if (base.size() > 3 && std::all_of(base.end() - 3, base.end(), isDigit)) base.resize(base.size() - 3);
    node->name = uniqueNodeName(base);
  }
  node->invalidate();
  nodes.push_back(std::move(node));
  return nodes.back().get();
}

bool Scene::removeNode(uint32_t id) {
  for (auto it = nodes.begin(); it != nodes.end(); ++it)
    if ((*it)->id == id) {
      nodes.erase(it);
      return true;
    }
  return false;
}

Node* Scene::findNode(uint32_t id) const {
  for (const auto& n : nodes)
    if (n->id == id) return n.get();
  return nullptr;
}

Node* Scene::findNodeByName(const std::string& name) const {
  for (const auto& n : nodes)
    if (n->name == name) return n.get();
  return nullptr;
}

Material* Scene::createMaterial(const std::string& name) {
  auto m = std::make_unique<Material>();
  m->name = name.empty() ? "Material" : name;
  return addMaterial(std::move(m));
}

Material* Scene::addMaterial(std::unique_ptr<Material> material) {
  material->id = nextMaterialId_++;
  if (material->name.empty()) material->name = "Material";
  for (const auto& m : materials)
    if (m->name == material->name) {
      material->name = uniqueMaterialName(material->name);
      break;
    }
  materials.push_back(std::move(material));
  return materials.back().get();
}

bool Scene::removeMaterial(uint32_t id) {
  for (auto it = materials.begin(); it != materials.end(); ++it)
    if ((*it)->id == id) {
      materials.erase(it);
      for (auto& n : nodes)
        if (n->materialId == id) n->materialId = 0;
      return true;
    }
  return false;
}

Material* Scene::findMaterial(uint32_t id) const {
  if (id == 0) return nullptr;
  for (const auto& m : materials)
    if (m->id == id) return m.get();
  return nullptr;
}

std::string Scene::uniqueNodeName(const std::string& base) const {
  for (int i = 1;; ++i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%03d", i);
    std::string candidate = base + buf;
    if (!findNodeByName(candidate)) return candidate;
  }
}

std::string Scene::uniqueMaterialName(const std::string& base) const {
  auto taken = [&](const std::string& n) {
    for (const auto& m : materials)
      if (m->name == n) return true;
    return false;
  };
  if (!taken(base)) return base;
  for (int i = 2;; ++i) {
    std::string candidate = base + " #" + std::to_string(i);
    if (!taken(candidate)) return candidate;
  }
}

Aabb Scene::bounds(bool geometryOnly) const {
  Aabb b;
  for (const auto& n : nodes) {
    if (n->hidden) continue;
    if (geometryOnly && n->kind() != ObjectKind::Geometry) continue;
    b.expand(n->worldBounds());
  }
  return b;
}

void Scene::clear() {
  nodes.clear();
  materials.clear();
  renderSettings = ParamBlock();
  defineRenderSettings(renderSettings);
  filePath.clear();
  nextNodeId_ = 1;
  nextMaterialId_ = 1;
  colorCounter_ = 0;
}

json Scene::toJson() const {
  json j{{"format", "veraa-scene"}, {"version", 1}, {"renderSettings", renderSettings.toJson()}};
  json mats = json::array();
  for (const auto& m : materials) mats.push_back(m->toJson());
  json ns = json::array();
  for (const auto& n : nodes) ns.push_back(n->toJson());
  j["materials"] = std::move(mats);
  j["nodes"] = std::move(ns);
  j["nextNodeId"] = nextNodeId_;
  j["nextMaterialId"] = nextMaterialId_;
  return j;
}

bool Scene::fromJson(const json& j, std::string* error) {
  try {
    if (j.value("format", std::string()) != "veraa-scene") throw std::runtime_error("Not a Veraa scene file");
    std::string keepPath = filePath;
    clear();
    filePath = keepPath;
    if (j.contains("renderSettings")) renderSettings.fromJson(j["renderSettings"]);
    uint32_t maxMat = 0, maxNode = 0;
    if (j.contains("materials"))
      for (const auto& jm : j["materials"]) {
        auto m = std::make_unique<Material>();
        m->fromJson(jm);
        maxMat = std::max(maxMat, m->id);
        materials.push_back(std::move(m));
      }
    if (j.contains("nodes"))
      for (const auto& jn : j["nodes"]) {
        auto n = std::make_unique<Node>();
        n->fromJson(jn);
        maxNode = std::max(maxNode, n->id);
        nodes.push_back(std::move(n));
      }
    nextNodeId_ = std::max(j.value("nextNodeId", 1u), maxNode + 1);
    nextMaterialId_ = std::max(j.value("nextMaterialId", 1u), maxMat + 1);
    // Repair missing or duplicate ids from hand-edited files.
    for (size_t a = 0; a < nodes.size(); ++a)
      for (size_t b = 0; b < a; ++b)
        if (nodes[a]->id == 0 || nodes[a]->id == nodes[b]->id) { nodes[a]->id = nextNodeId_++; break; }
    colorCounter_ = static_cast<uint32_t>(nodes.size());
    return true;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    return false;
  }
}

bool Scene::save(const std::string& path, std::string* error) {
  std::ofstream f(std::filesystem::u8path(path), std::ios::binary);
  if (!f) {
    if (error) *error = "Cannot write " + path;
    return false;
  }
  f << toJson().dump(1);
  if (!f) {
    if (error) *error = "Write failed: " + path;
    return false;
  }
  filePath = path;
  return true;
}

bool Scene::load(const std::string& path, std::string* error) {
  std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
  if (!f) {
    if (error) *error = "Cannot open " + path;
    return false;
  }
  json j;
  try {
    j = json::parse(f);
  } catch (const std::exception& e) {
    if (error) *error = std::string("Invalid scene file: ") + e.what();
    return false;
  }
  filePath = path;
  return fromJson(j, error);
}

std::string Scene::resolvePath(const std::string& path) const {
  if (path.empty()) return path;
  std::filesystem::path p = std::filesystem::u8path(path);
  if (p.is_absolute() || filePath.empty()) return path;
  return (std::filesystem::u8path(filePath).parent_path() / p).u8string();
}

}  // namespace veraa
