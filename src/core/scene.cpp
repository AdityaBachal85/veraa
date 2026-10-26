#include "scene.h"

#include "polyops.h"
#include "rendersettings.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

#include "parallel.h"
#include "registry.h"

namespace veraa {

// ---------------------------------------------------------------------------------------------
// Transform, TimeConfig

json Transform::toJson() const {
  return json{{"position", vec3ToJson(position)}, {"rotation", vec3ToJson(rotation)}, {"scale", vec3ToJson(scale)}};
}

void Transform::fromJson(const json& j) {
  position = vec3FromJson(j.value("position", json()), Vec3());
  rotation = vec3FromJson(j.value("rotation", json()), Vec3());
  scale = vec3FromJson(j.value("scale", json()), Vec3(1.0f));
}

Transform decomposeTransform(const Mat4& m) {
  Transform t;
  t.position = m.translation();
  Vec3 c0 = m.column(0), c1 = m.column(1), c2 = m.column(2);
  float sx = length(c0), sy = length(c1), sz = length(c2);
  if (m.determinant3x3() < 0.0f) sx = -sx;  // mirrored: put the flip on X
  t.scale = Vec3(sx, sy, sz);
  Mat4 r;
  auto setCol = [&](int c, const Vec3& v) { r.m[0][c] = v.x; r.m[1][c] = v.y; r.m[2][c] = v.z; };
  setCol(0, std::fabs(sx) > 1e-12f ? c0 / sx : Vec3(1, 0, 0));
  setCol(1, sy > 1e-12f ? c1 / sy : Vec3(0, 1, 0));
  setCol(2, sz > 1e-12f ? c2 / sz : Vec3(0, 0, 1));
  t.rotation = eulerDegFromMatrix(r);
  return t;
}

json TimeConfig::toJson() const {
  return json{{"start", startFrame}, {"end", endFrame}, {"fps", fps}, {"current", currentFrame}};
}

void TimeConfig::fromJson(const json& j) {
  startFrame = j.value("start", 0);
  endFrame = j.value("end", 100);
  fps = j.value("fps", 30.0f);
  currentFrame = j.value("current", 0.0);
}

// ---------------------------------------------------------------------------------------------
// Node

namespace {
std::atomic<uint64_t> g_geometryStamp{1};

void copyNodeFields(const Node& from, Node& to) {
  to.name = from.name;
  to.transform = from.transform;
  to.objectOffset = from.objectOffset;
  to.parentId = from.parentId;
  to.layerId = from.layerId;
  to.wireColor = from.wireColor;
  to.materialId = from.materialId;
  to.hidden = from.hidden;
  to.frozen = from.frozen;
  to.seeThrough = from.seeThrough;
  to.displayAsBox = from.displayAsBox;
  to.backfaceCull = from.backfaceCull;
  to.renderable = from.renderable;
  to.castShadows = from.castShadows;
  to.receiveShadows = from.receiveShadows;
  to.visibleToCamera = from.visibleToCamera;
  to.visibleToReflections = from.visibleToReflections;
  to.gBufferId = from.gBufferId;
  to.motionBlur = from.motionBlur;
  to.groupHead = from.groupHead;
  to.groupOpen = from.groupOpen;
  to.groupMember = from.groupMember;
  to.userProps = from.userProps;
}
}  // namespace

std::unique_ptr<Node> Node::clone() const {
  auto n = std::make_unique<Node>();
  copyNodeFields(*this, *n);
  if (object) n->object = object->clone();
  for (const auto& m : modifiers)
    if (m) n->modifiers.push_back(m->clone());
  return n;
}

std::unique_ptr<Node> Node::instance() const {
  auto n = std::make_unique<Node>();
  copyNodeFields(*this, *n);
  n->object = object;
  n->modifiers = modifiers;
  return n;
}

Node* Node::parent() const { return (scene_ && parentId) ? scene_->findNode(parentId) : nullptr; }

Mat4 Node::parentWorldMatrix() const {
  const Node* p = parent();
  return p ? p->worldMatrix() : Mat4::identity();
}

Mat4 Node::worldMatrix() const {
  Mat4 m = localMatrix();
  const Node* p = parent();
  for (int guard = 0; p && guard < 1024; ++guard) {
    m = p->localMatrix() * m;
    p = p->parent();
  }
  return m;
}

void Node::setWorldMatrix(const Mat4& world) {
  transform = decomposeTransform(parentWorldMatrix().inverse() * world);
}

void Node::setWorldPosition(const Vec3& p) {
  transform.position = parentWorldMatrix().inverse().transformPoint(p);
}

uint64_t Node::stackRevision() const {
  uint64_t r = object ? object->revision() * 1000003ull : 0;
  for (const auto& m : modifiers) r = r * 31 + (m ? m->revision() * 2 + (m->enabled ? 1 : 0) : 0);
  return r + modifiers.size();
}

void Node::invalidate() {
  dirty_ = true;
  if (object) object->touch();
  for (auto& m : modifiers)
    if (m) m->touch();
}

void Node::evaluate() const {
  uint64_t rev = stackRevision();
  if (!dirty_ && rev == evaluatedRevision_) return;
  // An Editable Poly without modifiers (imported models): use its mesh as is, shared.
  if (auto* ep = dynamic_cast<const EditablePolyObject*>(geometry()); ep && ep->sel.hiddenFaces.empty()) {
    bool anyModifier = false;
    for (const auto& m : modifiers) anyModifier |= m && m->enabled;
    if (!anyModifier) {
      sharedMesh_ = ep->sharedMesh();
      mesh_.clear();
      shapeLines_.clear();
      isShape_ = false;
      tri_ = triangulate(*sharedMesh_);
      dirty_ = false;
      evaluatedRevision_ = rev;
      geometryStamp_ = ++g_geometryStamp;
      return;
    }
  }
  sharedMesh_.reset();
  GeomState st;
  st.time = scene_ ? scene_->time.currentFrame : 0.0;
  st.scene = scene_;
  st.nodeId = id;
  if (auto* g = geometry()) {
    st.mesh = g->buildAt(st);
    // An Editable Poly at a sub-object level passes its selection to the modifiers above.
    if (auto* ep = dynamic_cast<const EditablePolyObject*>(g)) st.selWeights = stackSelectionWeights(ep->mesh(), ep->sel);
  } else if (auto* s = shapeObject()) {
    st.shape = s->buildShape();
    st.isShape = true;
    if (s->params.has("steps")) st.shapeSteps = s->params.getInt("steps");
    if (s->params.has("optimize")) st.shapeOptimize = s->params.getBool("optimize");
  }
  for (const auto& m : modifiers)
    if (m && m->enabled) m->applyState(st);

  mesh_.clear();
  shapeLines_.clear();
  isShape_ = st.isShape;
  if (st.isShape) {
    const ParamBlock* sp = object ? &object->params : nullptr;
    int steps = sp && sp->has("steps") ? sp->getInt("steps") : 6;
    bool optimize = sp && sp->has("optimize") ? sp->getBool("optimize") : true;
    shapeLines_ = st.shape.polylines(steps, optimize);
    // Display lines of closed splines repeat their first point so they draw closed.
    for (size_t i = 0; i < shapeLines_.size() && i < st.shape.splines.size(); ++i)
      if (st.shape.splines[i].closed && shapeLines_[i].size() > 2) shapeLines_[i].push_back(shapeLines_[i].front());
    if (sp && (sp->getBool("renderInRenderer") || sp->getBool("renderInViewport")))
      mesh_ = shapeToTubeMesh(st.shape, sp->getFloat("thickness"), sp->getInt("sides"), sp->getFloat("angle"), steps,
                              optimize);
  } else {
    mesh_ = std::move(st.mesh);
  }
  tri_ = triangulate(mesh_);
  dirty_ = false;
  evaluatedRevision_ = rev;
  geometryStamp_ = ++g_geometryStamp;
}

const PolyMesh& Node::mesh() const {
  evaluate();
  return sharedMesh_ ? *sharedMesh_ : mesh_;
}

const TriMesh& Node::triMesh() const {
  evaluate();
  return tri_;
}

const std::vector<std::vector<Vec3>>& Node::shapeLines() const {
  evaluate();
  return shapeLines_;
}

bool Node::isShapeResult() const {
  evaluate();
  return isShape_;
}

uint64_t Node::geometryStamp() const {
  evaluate();
  return geometryStamp_;
}

Aabb Node::worldBounds() const {
  ObjectKind k = kind();
  if (k == ObjectKind::Geometry || k == ObjectKind::Shape) {
    Mat4 m = objectWorldMatrix();
    Aabb b = triMesh().bounds.transformed(m);
    for (const auto& line : shapeLines())
      for (const auto& p : line) b.expand(m.transformPoint(p));
    if (b.valid()) return b;
  }
  Aabb b;
  b.expand(worldPosition());
  return b;
}

json Node::fieldsJson() const {
  json j{{"id", id},
         {"name", name},
         {"transform", transform.toJson()},
         {"wireColor", vec3ToJson(wireColor)},
         {"hidden", hidden},
         {"renderable", renderable},
         {"material", materialId}};
  if (!objectOffset.isIdentity()) j["objectOffset"] = objectOffset.toJson();
  if (parentId) j["parent"] = parentId;
  if (layerId) j["layer"] = layerId;
  json flags = json::object();
  auto flag = [&](const char* key, bool v, bool def) {
    if (v != def) flags[key] = v;
  };
  flag("frozen", frozen, false);
  flag("seeThrough", seeThrough, false);
  flag("displayAsBox", displayAsBox, false);
  flag("backfaceCull", backfaceCull, false);
  flag("castShadows", castShadows, true);
  flag("receiveShadows", receiveShadows, true);
  flag("visibleToCamera", visibleToCamera, true);
  flag("visibleToReflections", visibleToReflections, true);
  flag("motionBlur", motionBlur, true);
  flag("groupHead", groupHead, false);
  flag("groupOpen", groupOpen, false);
  flag("groupMember", groupMember, false);
  if (!flags.empty()) j["flags"] = std::move(flags);
  if (gBufferId) j["gBufferId"] = gBufferId;
  if (!userProps.empty()) j["userProps"] = userProps;
  return j;
}

json Node::toJson() const {
  json j = fieldsJson();
  if (object) j["object"] = object->toJson();
  json mods = json::array();
  for (const auto& m : modifiers)
    if (m) mods.push_back(m->toJson());
  j["modifiers"] = std::move(mods);
  return j;
}

void Node::fromJson(const json& j) {
  fromJsonFields(j);
  object.reset();
  if (j.contains("object")) {
    std::string type = j["object"].value("type", std::string());
    auto obj = createObject(type);
    if (!obj) throw std::runtime_error("Unknown object type: " + type);
    obj->fromJson(j["object"]);
    object = std::move(obj);
  }
  modifiers.clear();
  if (j.contains("modifiers"))
    for (const auto& jm : j["modifiers"]) {
      if (jm.contains("ref")) {  // shared with another node; resolved by Scene::fromJson
        modifiers.push_back(nullptr);
        continue;
      }
      std::string type = jm.value("type", std::string());
      auto m = createModifier(type);
      if (!m) throw std::runtime_error("Unknown modifier type: " + type);
      m->fromJson(jm);
      modifiers.push_back(std::move(m));
    }
  dirty_ = true;
}

void Node::fromJsonFields(const json& j) {
  id = j.value("id", 0u);
  name = j.value("name", std::string("Object"));
  transform = Transform();
  if (j.contains("transform")) transform.fromJson(j["transform"]);
  objectOffset = Transform();
  if (j.contains("objectOffset")) objectOffset.fromJson(j["objectOffset"]);
  parentId = j.value("parent", 0u);
  layerId = j.value("layer", 0u);
  wireColor = vec3FromJson(j.value("wireColor", json()), wireColor);
  hidden = j.value("hidden", false);
  renderable = j.value("renderable", true);
  materialId = j.value("material", 0u);
  gBufferId = j.value("gBufferId", 0);
  userProps = j.value("userProps", std::string());
  if (j.contains("flags")) {
    const json& f = j["flags"];
    frozen = f.value("frozen", false);
    seeThrough = f.value("seeThrough", false);
    displayAsBox = f.value("displayAsBox", false);
    backfaceCull = f.value("backfaceCull", false);
    castShadows = f.value("castShadows", true);
    receiveShadows = f.value("receiveShadows", true);
    visibleToCamera = f.value("visibleToCamera", true);
    visibleToReflections = f.value("visibleToReflections", true);
    motionBlur = f.value("motionBlur", true);
    groupHead = f.value("groupHead", false);
    groupOpen = f.value("groupOpen", false);
    groupMember = f.value("groupMember", false);
  } else {
    frozen = seeThrough = displayAsBox = backfaceCull = false;
    castShadows = receiveShadows = visibleToCamera = visibleToReflections = motionBlur = true;
    groupHead = groupOpen = groupMember = false;
  }
}

// ---------------------------------------------------------------------------------------------
// Render settings

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
  // Values are stored in files: new types are appended (see render::ColorMappingType).
  p.addEnum("tonemap", "Color mapping type",
            {"Filmic (ACES fitted)", "Reinhard", "Linear multiply", "Exponential", "HSV exponential",
             "Intensity exponential", "Gamma correction", "Intensity gamma", "AgX (filmic view)"},
            0);
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
  defineAdvancedRenderSettings(p);
}

// ---------------------------------------------------------------------------------------------
// Scene

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

Layer defaultLayer() {
  Layer l;
  l.id = 0;
  l.name = "0";
  return l;
}

}  // namespace

namespace {

std::vector<std::pair<std::string, SceneModuleFactory>>& sceneModuleRegistry() {
  static std::vector<std::pair<std::string, SceneModuleFactory>> list;
  return list;
}

void ensureSceneModules() {
  static bool done = false;
  if (done) return;
  done = true;
  registerBuiltinSceneModules();
}

}  // namespace

void registerSceneModule(const std::string& name, SceneModuleFactory factory) {
  auto& reg = sceneModuleRegistry();
  for (auto& e : reg)
    if (e.first == name) {
      e.second = std::move(factory);
      return;
    }
  reg.emplace_back(name, std::move(factory));
}

SceneModule* Scene::module(const std::string& name) const {
  for (const auto& m : modules_)
    if (m.first == name) return m.second.get();
  return nullptr;
}

Scene::Scene() {
  defineRenderSettings(renderSettings);
  layers.push_back(defaultLayer());
  ensureSceneModules();
  for (const auto& e : sceneModuleRegistry())
    if (e.second) modules_.emplace_back(e.first, e.second());
}

Node* Scene::createNode(const std::string& objectType) {
  auto obj = createObject(objectType);
  if (!obj) return nullptr;
  auto node = std::make_unique<Node>();
  node->name = uniqueNodeName(baseNameFor(objectType));
  node->object = std::move(obj);
  node->layerId = currentLayerId;
  switch (node->kind()) {
    case ObjectKind::Geometry:
    case ObjectKind::Shape:
      node->wireColor = kWireColors[colorCounter_++ % (sizeof(kWireColors) / sizeof(kWireColors[0]))];
      break;
    case ObjectKind::Light: node->wireColor = Vec3(0.95f, 0.85f, 0.2f); break;
    case ObjectKind::Camera: node->wireColor = Vec3(0.3f, 0.6f, 1.0f); break;
    case ObjectKind::Helper: node->wireColor = Vec3(0.35f, 0.85f, 0.45f); break;
    case ObjectKind::SpaceWarp: node->wireColor = Vec3(0.6f, 0.6f, 0.95f); break;
  }
  return addNode(std::move(node));
}

Node* Scene::addNode(std::unique_ptr<Node> node) {
  node->id = nextNodeId_++;
  node->scene_ = this;
  if (node->name.empty()) node->name = uniqueNodeName("Object");
  if (findNodeByName(node->name)) {
    // Strip a trailing 3-digit counter so a copy of "Box001" becomes "Box002".
    std::string base = node->name;
    auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
    if (base.size() > 3 && std::all_of(base.end() - 3, base.end(), isDigit)) base.resize(base.size() - 3);
    node->name = uniqueNodeName(base);
  }
  if (node->parentId && !findNode(node->parentId)) node->parentId = 0;
  if (!findLayer(node->layerId)) node->layerId = 0;
  node->dirty_ = true;
  if (bulkNames_) ++(*bulkNames_)[node->name];
  nodes.push_back(std::move(node));
  Node* n = nodes.back().get();
  index_[n->id] = n;
  return n;
}

bool Scene::removeNode(uint32_t id) {
  Node* victim = findNode(id);
  if (!victim) return false;
  for (Node* child : children(id)) {
    Mat4 w = child->worldMatrix();
    child->parentId = victim->parentId;
    if (victim->groupHead) child->groupMember = false;
    child->setWorldMatrix(w);
  }
  index_.erase(id);
  if (bulkNames_) {
    auto it = bulkNames_->find(victim->name);
    if (it != bulkNames_->end() && --it->second <= 0) bulkNames_->erase(it);
  }
  for (auto it = nodes.begin(); it != nodes.end(); ++it)
    if (it->get() == victim) {
      nodes.erase(it);
      break;
    }
  return true;
}

Node* Scene::findNode(uint32_t id) const {
  if (id == 0) return nullptr;
  auto it = index_.find(id);
  if (it != index_.end() && it->second->id == id) return it->second;
  // Fall back to a scan (nodes may have been added to the vector directly) and repair the index.
  for (const auto& n : nodes)
    if (n->id == id) {
      n->scene_ = const_cast<Scene*>(this);
      index_[id] = n.get();
      return n.get();
    }
  return nullptr;
}

Node* Scene::findNodeByName(const std::string& name) const {
  if (bulkNames_ && !bulkNames_->count(name)) return nullptr;
  for (const auto& n : nodes)
    if (n->name == name) return n.get();
  return nullptr;
}

void Scene::beginBulkNames() {
  bulkNames_ = std::make_unique<std::unordered_map<std::string, int>>();
  bulkNames_->reserve(nodes.size() * 2 + 64);
  for (const auto& n : nodes) ++(*bulkNames_)[n->name];
}

void Scene::endBulkNames() {
  bulkNames_.reset();
  bulkHints_.clear();
}

void Scene::renameNode(Node* n, const std::string& name) {
  if (!n || n->name == name) return;
  if (bulkNames_) {
    auto it = bulkNames_->find(n->name);
    if (it != bulkNames_->end() && --it->second <= 0) bulkNames_->erase(it);
    ++(*bulkNames_)[name];
  }
  n->name = name;
}

std::vector<Node*> Scene::children(uint32_t id) const {
  std::vector<Node*> out;
  if (id == 0) return out;
  for (const auto& n : nodes)
    if (n->parentId == id) out.push_back(n.get());
  return out;
}

bool Scene::isAncestor(uint32_t ancestorId, uint32_t nodeId) const {
  const Node* n = findNode(nodeId);
  for (int guard = 0; n && guard < 1024; ++guard) {
    if (n->parentId == ancestorId) return true;
    n = findNode(n->parentId);
  }
  return false;
}

bool Scene::setParent(uint32_t childId, uint32_t parentId) {
  Node* child = findNode(childId);
  if (!child) return false;
  if (parentId == childId) return false;
  if (parentId && (!findNode(parentId) || isAncestor(childId, parentId))) return false;
  Mat4 w = child->worldMatrix();
  child->parentId = parentId;
  child->setWorldMatrix(w);
  return true;
}

std::vector<Node*> Scene::instancesOf(const Node& n) const {
  std::vector<Node*> out;
  for (const auto& o : nodes)
    if (o.get() == &n || (n.object && o->object == n.object)) out.push_back(o.get());
  return out;
}

Layer* Scene::findLayer(uint32_t id) {
  for (auto& l : layers)
    if (l.id == id) return &l;
  return nullptr;
}

const Layer* Scene::findLayer(uint32_t id) const {
  for (const auto& l : layers)
    if (l.id == id) return &l;
  return nullptr;
}

Layer* Scene::createLayer(const std::string& name) {
  Layer l;
  l.id = nextLayerId_++;
  l.name = name.empty() ? "Layer" + std::to_string(l.id) : name;
  layers.push_back(l);
  return &layers.back();
}

bool Scene::removeLayer(uint32_t id) {
  if (id == 0) return false;
  for (auto it = layers.begin(); it != layers.end(); ++it)
    if (it->id == id) {
      layers.erase(it);
      for (auto& n : nodes)
        if (n->layerId == id) n->layerId = 0;
      if (currentLayerId == id) currentLayerId = 0;
      return true;
    }
  return false;
}

bool Scene::isNodeVisible(const Node& n) const {
  if (n.hidden) return false;
  const Layer* l = findLayer(n.layerId);
  if (l && l->hidden) return false;
  // Hiding a group head hides its members.
  const Node* cur = &n;
  for (int guard = 0; cur && cur->groupMember && guard < 1024; ++guard) {
    const Node* p = cur->parent();
    if (!p) break;
    if (p->hidden) return false;
    cur = p;
  }
  return true;
}

bool Scene::isNodeFrozen(const Node& n) const {
  if (n.frozen) return true;
  const Layer* l = findLayer(n.layerId);
  return l && l->frozen;
}

bool Scene::isNodeRenderable(const Node& n) const {
  if (!n.renderable || !isNodeVisible(n)) return false;
  const Layer* l = findLayer(n.layerId);
  return !l || l->renderable;
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
  bool indexed = !matIndexDirty_ && matIndex_.size() == materials.size();
  materials.push_back(std::move(material));
  if (indexed) matIndex_.emplace(materials.back()->id, materials.size() - 1);
  else matIndexDirty_ = true;
  return materials.back().get();
}

bool Scene::removeMaterial(uint32_t id) {
  for (auto it = materials.begin(); it != materials.end(); ++it)
    if ((*it)->id == id) {
      materials.erase(it);
      matIndexDirty_ = true;
      for (auto& n : nodes)
        if (n->materialId == id) n->materialId = 0;
      return true;
    }
  return false;
}

Material* Scene::findMaterial(uint32_t id) const {
  if (id == 0) return nullptr;
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (matIndexDirty_ || matIndex_.size() != materials.size()) {
      matIndex_.clear();
      matIndex_.reserve(materials.size());
      for (size_t i = 0; i < materials.size(); ++i) matIndex_.emplace(materials[i]->id, i);
      matIndexDirty_ = false;
    }
    auto it = matIndex_.find(id);
    if (it == matIndex_.end()) return nullptr;
    if (it->second < materials.size() && materials[it->second]->id == id) return materials[it->second].get();
    matIndexDirty_ = true;  // the list was changed directly; rebuild and look again
  }
  return nullptr;
}

std::string Scene::uniqueNodeName(const std::string& base) const {
  auto candidate = [&](int i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%03d", i);
    return base + buf;
  };
  if (bulkNames_) {
    // Names are only added while loading in bulk, so numbers below the last one handed out for
    // this base are still taken.
    int& hint = bulkHints_[base];
    for (int i = std::max(hint, 1);; ++i)
      if (!bulkNames_->count(candidate(i))) {
        hint = i;
        return candidate(i);
      }
  }
  // One pass collecting the numbers already used after `base`, instead of a scan per number.
  std::unordered_set<std::string> taken;
  for (const auto& n : nodes)
    if (n->name.size() > base.size() && n->name.compare(0, base.size(), base) == 0) taken.insert(n->name);
  for (int i = 1;; ++i) {
    std::string c = candidate(i);
    if (!taken.count(c)) return c;
  }
}

std::string Scene::uniqueMaterialName(const std::string& base) const {
  std::unordered_set<std::string> names;
  names.reserve(materials.size() * 2);
  for (const auto& m : materials) names.insert(m->name);
  auto taken = [&](const std::string& n) { return names.count(n) != 0; };
  if (!taken(base)) return base;
  for (int i = 2;; ++i) {
    std::string candidate = base + " #" + std::to_string(i);
    if (!taken(candidate)) return candidate;
  }
}

bool Scene::prepareMeshes(ImportProgress* progress, double from, double to) {
  std::vector<const Node*> todo;
  for (const auto& n : nodes) {
    const auto* ep = dynamic_cast<const EditablePolyObject*>(n->geometry());
    if (!ep || !ep->sel.hiddenFaces.empty()) continue;
    bool anyModifier = false;
    for (const auto& m : n->modifiers) anyModifier |= m && m->enabled;
    if (!anyModifier) todo.push_back(n.get());
  }
  // Biggest first, so one huge mesh does not start last.
  std::sort(todo.begin(), todo.end(), [](const Node* a, const Node* b) {
    return static_cast<const EditablePolyObject*>(a->geometry())->mesh().faces.size() >
           static_cast<const EditablePolyObject*>(b->geometry())->mesh().faces.size();
  });
  std::atomic<size_t> done{0};
  parallelFor(todo.size(), [&](size_t i) {
    if (progress && progress->cancelled()) return;
    todo[i]->evaluate();
    reportProgress(progress, from, to, double(++done) / double(todo.size()));
  });
  return !(progress && progress->cancelled());
}

std::vector<uint32_t> Scene::adopt(Scene& src, std::unordered_map<uint32_t, uint32_t>* idMap) {
  std::unordered_map<uint32_t, uint32_t> matMap, layerMap, nodeMap;
  for (auto& m : src.materials) {
    uint32_t old = m->id;
    matMap[old] = addMaterial(std::move(m))->id;
  }
  src.materials.clear();
  src.matIndexDirty_ = true;
  for (const Layer& l : src.layers) {
    if (l.id == 0) continue;
    uint32_t id = 0;
    for (const Layer& dl : layers)
      if (dl.name == l.name) id = dl.id;
    if (!id) {
      Layer* nl = createLayer(l.name);
      nl->hidden = l.hidden;
      nl->frozen = l.frozen;
      nl->renderable = l.renderable;
      nl->color = l.color;
      id = nl->id;
    }
    layerMap[l.id] = id;
  }
  bool ownBulk = !bulkNames_;
  if (ownBulk) beginBulkNames();
  std::vector<uint32_t> ids;
  ids.reserve(src.nodes.size());
  for (size_t i = 0; i < src.nodes.size(); ++i) nodeMap[src.nodes[i]->id] = nextNodeId_ + static_cast<uint32_t>(i);
  nodes.reserve(nodes.size() + src.nodes.size());
  for (auto& up : src.nodes) {
    Node* n = up.get();
    n->id = nodeMap[n->id];
    n->scene_ = this;
    auto pm = nodeMap.find(n->parentId);
    n->parentId = n->parentId && pm != nodeMap.end() ? pm->second : 0;
    auto mm = matMap.find(n->materialId);
    n->materialId = n->materialId && mm != matMap.end() ? mm->second : 0;
    auto lm = layerMap.find(n->layerId);
    n->layerId = n->layerId && lm != layerMap.end() ? lm->second : currentLayerId;
    if (n->name.empty() || findNodeByName(n->name)) {
      std::string baseName = n->name.empty() ? std::string("Object") : n->name;
      auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
      if (baseName.size() > 3 && std::all_of(baseName.end() - 3, baseName.end(), isDigit)) baseName.resize(baseName.size() - 3);
      n->name = uniqueNodeName(baseName);
    }
    ++(*bulkNames_)[n->name];
    index_[n->id] = n;
    ids.push_back(n->id);
    nodes.push_back(std::move(up));
  }
  nextNodeId_ += static_cast<uint32_t>(src.nodes.size());
  if (ownBulk) endBulkNames();
  src.nodes.clear();
  src.index_.clear();
  // Scene-wide settings an importer changed from the defaults (render settings, time range,
  // extension sections) come along too.
  {
    ParamBlock defaults;
    defineRenderSettings(defaults);
    json rs = src.renderSettings.toJson(), def = defaults.toJson(), changed = json::object();
    for (auto it = rs.begin(); it != rs.end(); ++it)
      if (!def.contains(it.key()) || def[it.key()] != it.value()) changed[it.key()] = it.value();
    if (!changed.empty()) renderSettings.fromJson(changed);
    if (src.time.toJson() != TimeConfig().toJson()) time = src.time;
    for (auto it = src.extensions.begin(); it != src.extensions.end(); ++it)
      if (!extensions.contains(it.key())) extensions[it.key()] = it.value();
  }
  if (idMap) *idMap = std::move(nodeMap);
  return ids;
}

Aabb Scene::bounds(bool geometryOnly) const {
  Aabb b;
  for (const auto& n : nodes) {
    if (!isNodeVisible(*n)) continue;
    ObjectKind k = n->kind();
    if (geometryOnly && k != ObjectKind::Geometry && k != ObjectKind::Shape) continue;
    b.expand(n->worldBounds());
  }
  return b;
}

void Scene::clear() {
  nodes.clear();
  index_.clear();
  if (bulkNames_) bulkNames_->clear();
  bulkHints_.clear();
  materials.clear();
  matIndexDirty_ = true;
  layers.clear();
  layers.push_back(defaultLayer());
  currentLayerId = 0;
  renderSettings = ParamBlock();
  defineRenderSettings(renderSettings);
  time = TimeConfig();
  extensions = json::object();
  for (auto& m : modules_) m.second->fromJson(json());
  filePath.clear();
  nextNodeId_ = 1;
  nextMaterialId_ = 1;
  nextLayerId_ = 1;
  colorCounter_ = 0;
}

json Scene::globalsJson() const {
  json j{{"format", "veraa-scene"}, {"version", 2}, {"renderSettings", renderSettings.toJson()}};
  json ls = json::array();
  for (const auto& l : layers)
    ls.push_back(json{{"id", l.id},
                      {"name", l.name},
                      {"hidden", l.hidden},
                      {"frozen", l.frozen},
                      {"renderable", l.renderable},
                      {"color", vec3ToJson(l.color)}});
  j["layers"] = std::move(ls);
  j["currentLayer"] = currentLayerId;
  j["time"] = time.toJson();
  json ext = extensions;
  for (const auto& m : modules_) {
    json mj = m.second->toJson();
    if (mj.is_null()) ext.erase(m.first);
    else ext[m.first] = std::move(mj);
  }
  if (!ext.empty()) j["extensions"] = std::move(ext);
  j["nextNodeId"] = nextNodeId_;
  j["nextMaterialId"] = nextMaterialId_;
  j["nextLayerId"] = nextLayerId_;
  return j;
}

std::vector<json> Scene::nodesJson() const {
  // Instances: the first node owning an object/modifier writes it; later ones reference it.
  // Decided in order first, then the nodes are written in parallel.
  std::unordered_map<const SceneObject*, uint32_t> objectOwner;
  std::unordered_map<const Modifier*, std::pair<uint32_t, size_t>> modifierOwner;
  std::vector<uint32_t> instanceOf(nodes.size(), 0);
  std::vector<std::vector<std::pair<uint32_t, size_t>>> modifierRefs(nodes.size());
  for (size_t i = 0; i < nodes.size(); ++i) {
    const Node& n = *nodes[i];
    // Only shared objects need the owner map (a use count of 1 cannot be an instance).
    if (n.object && n.object.use_count() > 1) {
      auto it = objectOwner.find(n.object.get());
      if (it != objectOwner.end()) instanceOf[i] = it->second;
      else objectOwner[n.object.get()] = n.id;
    }
    size_t written = 0;
    for (const auto& m : n.modifiers) {
      if (!m) continue;
      std::pair<uint32_t, size_t> ref{0, 0};
      if (m.use_count() > 1) {
        auto mit = modifierOwner.find(m.get());
        if (mit != modifierOwner.end()) ref = mit->second;
        else modifierOwner[m.get()] = {n.id, written};
      }
      if (!modifierRefs[i].empty() || ref.first) {
        modifierRefs[i].resize(written, {0, 0});
        modifierRefs[i].push_back(ref);
      }
      ++written;
    }
  }
  std::vector<json> ns(nodes.size());
  // Mesh references are recorded per thread (the table is not thread-safe) and merged after.
  MeshRefTable* refs = MeshRefScope::current();
  const size_t ranges = std::max<size_t>(1, std::min<size_t>(nodes.size() / 64, size_t(parallelThreads()) * 4));
  std::vector<MeshRefTable> localRefs(refs ? ranges : 0);
  parallelFor(ranges, [&](size_t r) {
    MeshRefScope scope(refs ? &localRefs[r] : nullptr);
    for (size_t i = nodes.size() * r / ranges; i < nodes.size() * (r + 1) / ranges; ++i) {
      const Node& n = *nodes[i];
      json jn = n.fieldsJson();
      if (n.object) {
        if (instanceOf[i]) jn["instanceOf"] = instanceOf[i];
        else jn["object"] = n.object->toJson();
      }
      json mods = json::array();
      size_t written = 0;
      for (const auto& m : n.modifiers) {
        if (!m) continue;
        if (written < modifierRefs[i].size() && modifierRefs[i][written].first)
          mods.push_back(json{{"ref", json::array({modifierRefs[i][written].first, modifierRefs[i][written].second})}});
        else
          mods.push_back(m->toJson());
        ++written;
      }
      jn["modifiers"] = std::move(mods);
      ns[i] = std::move(jn);
    }
  });
  if (refs)
    for (auto& t : localRefs) refs->insert(t.begin(), t.end());
  return ns;
}

json Scene::toJson() const {
  json j = globalsJson();
  json mats = json::array();
  for (const auto& m : materials) mats.push_back(m->toJson());
  json ns = json::array();
  for (auto& jn : nodesJson()) ns.push_back(std::move(jn));
  j["materials"] = std::move(mats);
  j["nodes"] = std::move(ns);
  return j;
}

void Scene::applyGlobals(const json& j) {
  renderSettings = ParamBlock();
  defineRenderSettings(renderSettings);
  if (j.contains("renderSettings")) renderSettings.fromJson(j["renderSettings"]);
  time = TimeConfig();
  if (j.contains("time")) time.fromJson(j["time"]);
  extensions = j.contains("extensions") && j["extensions"].is_object() ? j["extensions"] : json::object();
  uint32_t maxLayer = 0;
  layers.clear();
  if (j.contains("layers"))
    for (const auto& jl : j["layers"]) {
      Layer l;
      l.id = jl.value("id", 0u);
      l.name = jl.value("name", std::string("Layer"));
      l.hidden = jl.value("hidden", false);
      l.frozen = jl.value("frozen", false);
      l.renderable = jl.value("renderable", true);
      l.color = vec3FromJson(jl.value("color", json()), l.color);
      maxLayer = std::max(maxLayer, l.id);
      layers.push_back(l);
    }
  if (!findLayer(0)) layers.insert(layers.begin(), defaultLayer());
  currentLayerId = j.value("currentLayer", 0u);
  if (!findLayer(currentLayerId)) currentLayerId = 0;
  nextLayerId_ = std::max(j.value("nextLayerId", 1u), maxLayer + 1);
  nextNodeId_ = std::max(nextNodeId_, j.value("nextNodeId", 1u));
  nextMaterialId_ = std::max(nextMaterialId_, j.value("nextMaterialId", 1u));
  // Module data (live) is loaded from its extensions section, which it then owns.
  for (auto& m : modules_) {
    auto it = extensions.find(m.first);
    m.second->fromJson(it != extensions.end() ? *it : json());
    if (it != extensions.end()) extensions.erase(it);
  }
}

bool Scene::fromJson(const json& j, std::string* error) {
  try {
    if (j.value("format", std::string()) != "veraa-scene") throw std::runtime_error("Not a Veraa scene file");
    std::string keepPath = filePath;
    clear();
    filePath = keepPath;
    if (j.contains("renderSettings")) {
      const json& rs = j["renderSettings"];
      renderSettings.fromJson(rs);
      // Scenes saved before the physical sky models existed keep their Preetham sky.
      if (rs.is_object() && !rs.contains("skyModel")) renderSettings.setNumber("skyModel", 0);
    }
    if (j.contains("time")) time.fromJson(j["time"]);
    if (j.contains("extensions") && j["extensions"].is_object()) extensions = j["extensions"];
    uint32_t maxLayer = 0, maxMat = 0, maxNode = 0;
    if (j.contains("layers")) {
      layers.clear();
      for (const auto& jl : j["layers"]) {
        Layer l;
        l.id = jl.value("id", 0u);
        l.name = jl.value("name", std::string("Layer"));
        l.hidden = jl.value("hidden", false);
        l.frozen = jl.value("frozen", false);
        l.renderable = jl.value("renderable", true);
        l.color = vec3FromJson(jl.value("color", json()), l.color);
        maxLayer = std::max(maxLayer, l.id);
        layers.push_back(l);
      }
      if (!findLayer(0)) layers.insert(layers.begin(), defaultLayer());
    }
    currentLayerId = j.value("currentLayer", 0u);
    if (!findLayer(currentLayerId)) currentLayerId = 0;
    if (j.contains("materials"))
      for (const auto& jm : j["materials"]) {
        auto m = std::make_unique<Material>();
        m->fromJson(jm);
        maxMat = std::max(maxMat, m->id);
        materials.push_back(std::move(m));
      }
    matIndexDirty_ = true;
    std::vector<const json*> nodeJson;
    if (j.contains("nodes"))
      for (const auto& jn : j["nodes"]) {
        auto n = std::make_unique<Node>();
        n->fromJson(jn);
        n->scene_ = this;
        maxNode = std::max(maxNode, n->id);
        nodes.push_back(std::move(n));
        nodeJson.push_back(&jn);
      }
    // Repair missing or duplicate ids from hand-edited files.
    nextNodeId_ = std::max(j.value("nextNodeId", 1u), maxNode + 1);
    {
      std::unordered_set<uint32_t> seen;
      seen.reserve(nodes.size() * 2);
      for (auto& n : nodes)
        if (n->id == 0 || !seen.insert(n->id).second) {
          n->id = nextNodeId_++;
          seen.insert(n->id);
        }
    }
    index_.clear();
    for (auto& n : nodes) index_[n->id] = n.get();

    // Resolve instances.
    for (size_t i = 0; i < nodes.size(); ++i) {
      const json& jn = *nodeJson[i];
      Node& n = *nodes[i];
      if (!n.object && jn.contains("instanceOf"))
        if (Node* owner = findNode(jn["instanceOf"].get<uint32_t>())) n.object = owner->object;
      if (jn.contains("modifiers")) {
        const json& mods = jn["modifiers"];
        for (size_t k = 0; k < mods.size() && k < n.modifiers.size(); ++k) {
          if (n.modifiers[k] || !mods[k].contains("ref")) continue;
          const json& ref = mods[k]["ref"];
          Node* owner = findNode(ref[0].get<uint32_t>());
          size_t idx = ref[1].get<size_t>();
          if (owner && idx < owner->modifiers.size()) n.modifiers[k] = owner->modifiers[idx];
        }
      }
      n.modifiers.erase(std::remove(n.modifiers.begin(), n.modifiers.end(), nullptr), n.modifiers.end());
    }
    // Nodes whose instance source was missing get an empty mesh rather than crashing.
    for (auto& n : nodes)
      if (!n->object) n->object = std::make_shared<EditablePolyObject>();
    for (auto& n : nodes) {
      if (n->parentId && (!findNode(n->parentId) || isAncestor(n->id, n->parentId))) n->parentId = 0;
      if (!findLayer(n->layerId)) n->layerId = 0;
    }
    nextMaterialId_ = std::max(j.value("nextMaterialId", 1u), maxMat + 1);
    nextLayerId_ = std::max(j.value("nextLayerId", 1u), maxLayer + 1);
    colorCounter_ = static_cast<uint32_t>(nodes.size());
    // Module data (live) is loaded from its extensions section, which it then owns.
    for (auto& m : modules_) {
      auto it = extensions.find(m.first);
      m.second->fromJson(it != extensions.end() ? *it : json());
      if (it != extensions.end()) extensions.erase(it);
    }
    return true;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    return false;
  }
}

// ---------------------------------------------------------------------------------------------
// Undo snapshots

namespace {

bool sameEntries(const std::vector<std::shared_ptr<const json>>& a, const std::vector<std::shared_ptr<const json>>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (a[i] != b[i] && *a[i] != *b[i]) return false;
  return true;
}

// Shares `j` with the entry of `previous` holding the same id when equal (or makes a new entry).
std::shared_ptr<const json> shareEntry(json&& j, const std::unordered_map<uint32_t, const std::shared_ptr<const json>*>& previous) {
  auto it = previous.find(j.value("id", 0u));
  if (it != previous.end() && **it->second == j) return *it->second;
  return std::make_shared<const json>(std::move(j));
}

}  // namespace

bool SceneSnapshot::sameAs(const SceneSnapshot& o) const {
  return globals == o.globals && sameEntries(materials, o.materials) && sameEntries(nodes, o.nodes);
}

std::shared_ptr<const SceneSnapshot> Scene::snapshot(const SceneSnapshot* previous) const {
  auto s = std::make_shared<SceneSnapshot>();
  MeshRefScope refs(&s->meshes);
  s->globals = globalsJson();
  std::unordered_map<uint32_t, const std::shared_ptr<const json>*> prevNodes, prevMats;
  if (previous) {
    for (const auto& e : previous->nodes) prevNodes[e->value("id", 0u)] = &e;
    for (const auto& e : previous->materials) prevMats[e->value("id", 0u)] = &e;
  }
  s->materials.resize(materials.size());
  parallelRanges(materials.size(), 64, [&](size_t a, size_t b) {
    for (size_t i = a; i < b; ++i) s->materials[i] = shareEntry(materials[i]->toJson(), prevMats);
  });
  std::vector<json> ns = nodesJson();
  s->nodes.resize(ns.size());
  parallelRanges(ns.size(), 64, [&](size_t a, size_t b) {
    for (size_t i = a; i < b; ++i) s->nodes[i] = shareEntry(std::move(ns[i]), prevNodes);
  });
  return s;
}

SceneRestoreResult Scene::restore(const SceneSnapshot& s) {
  SceneRestoreResult r;
  // ---- Globals (render settings, layers, time, module data) ----
  json cur = globalsJson();
  if (cur != s.globals) {
    r.globals = true;
    r.renderSettings = cur.value("renderSettings", json()) != s.globals.value("renderSettings", json());
    applyGlobals(s.globals);
  }

  // ---- Materials: rebuilt when any differs (they are small) ----
  {
    std::atomic<bool> same{materials.size() == s.materials.size()};
    if (same)
      parallelRanges(materials.size(), 64, [&](size_t a, size_t b) {
        for (size_t i = a; i < b && same; ++i)
          if (!(materials[i]->toJson() == *s.materials[i])) same = false;
      });
    if (!same) {
      r.materials = true;
      materials.clear();
      for (const auto& jm : s.materials) {
        auto m = std::make_unique<Material>();
        m->fromJson(*jm);
        materials.push_back(std::move(m));
      }
      matIndexDirty_ = true;
    }
  }

  // ---- Nodes: keep the ones that match, update fields in place, rebuild the rest ----
  MeshRefTable currentRefs;
  std::vector<json> current;
  {
    MeshRefScope refs(&currentRefs);
    current = nodesJson();
  }
  std::unordered_map<uint32_t, const json*> currentById;
  for (const auto& jn : current) currentById[jn.value("id", 0u)] = &jn;

  std::unordered_map<uint32_t, std::unique_ptr<Node>> pool;  // existing nodes by id
  std::vector<uint32_t> oldOrder;
  oldOrder.reserve(nodes.size());
  for (auto& n : nodes) {
    oldOrder.push_back(n->id);
    pool[n->id] = std::move(n);
  }
  nodes.clear();
  index_.clear();

  auto field = [](const json& j, const char* key) -> const json* {
    auto it = j.find(key);
    return it == j.end() ? nullptr : &*it;
  };
  auto sameField = [&](const json& a, const json& b, const char* key) {
    const json* x = field(a, key);
    const json* y = field(b, key);
    return (!x && !y) || (x && y && *x == *y);
  };

  std::unordered_set<uint32_t> rebuilt;  // nodes whose object or modifiers were recreated
  std::vector<std::pair<Node*, const json*>> needsResolve;
  {
    MeshRefScope refs(const_cast<MeshRefTable*>(&s.meshes));
    for (const auto& entry : s.nodes) {
      const json& target = *entry;
      uint32_t id = target.value("id", 0u);
      auto pit = pool.find(id);
      std::unique_ptr<Node> node;
      if (pit != pool.end() && pit->second) {
        node = std::move(pit->second);
        const json& was = *currentById[id];
        if (was != target) {
          r.nodes.push_back(id);
          int what = 0;
          auto differs = [&](const char* key) { return !sameField(was, target, key); };
          if (differs("transform") || differs("objectOffset") || differs("parent")) what |= 1;
          if (differs("object") || differs("instanceOf") || differs("modifiers")) what |= 2;
          if (differs("material")) what |= 4;
          if (differs("name")) what |= 8;
          for (const char* k : {"hidden", "renderable", "wireColor", "layer", "flags", "gBufferId", "userProps"})
            if (differs(k)) what |= 16;
          r.nodeChanges.emplace_back(id, what ? what : 31);
          bool sameGeometry = sameField(was, target, "object") && sameField(was, target, "instanceOf") &&
                              sameField(was, target, "modifiers");
          if (sameGeometry) {
            node->fromJsonFields(target);
          } else {
            node->fromJson(target);
            rebuilt.insert(id);
            r.geometryNodes.push_back(id);
            needsResolve.emplace_back(node.get(), &target);
          }
        }
      } else {
        node = std::make_unique<Node>();
        node->fromJson(target);
        rebuilt.insert(id);
        r.nodes.push_back(id);
        r.geometryNodes.push_back(id);
        r.structure = true;
        needsResolve.emplace_back(node.get(), &target);
      }
      node->scene_ = this;
      index_[node->id] = node.get();
      nodes.push_back(std::move(node));
    }
  }
  for (auto& [id, n] : pool)
    if (n) r.structure = true;  // removed
  if (!r.structure) {
    // Same set of nodes; did the order change?
    for (size_t i = 0; i < nodes.size() && !r.structure; ++i) r.structure = i >= oldOrder.size() || nodes[i]->id != oldOrder[i];
  }
  pool.clear();

  // Kept nodes that share an object or modifier with a rebuilt node must point at the new one.
  for (size_t i = 0; i < nodes.size(); ++i) {
    Node& n = *nodes[i];
    if (rebuilt.count(n.id)) continue;
    const json& target = *s.nodes[i];
    bool refers = false;
    if (auto it = target.find("instanceOf"); it != target.end()) refers |= rebuilt.count(it->get<uint32_t>()) != 0;
    if (auto it = target.find("modifiers"); it != target.end())
      for (const auto& jm : *it)
        if (auto ref = jm.find("ref"); ref != jm.end()) refers |= rebuilt.count((*ref)[0].get<uint32_t>()) != 0;
    if (refers) needsResolve.emplace_back(&n, &target);
  }
  for (auto& [np, jp] : needsResolve) {
    Node& n = *np;
    const json& jn = *jp;
    if (auto it = jn.find("instanceOf"); it != jn.end())
      if (Node* owner = findNode(it->get<uint32_t>()); owner && owner->object) n.object = owner->object;
    if (auto it = jn.find("modifiers"); it != jn.end()) {
      const json& mods = *it;
      if (n.modifiers.size() != mods.size()) n.modifiers.resize(mods.size());
      for (size_t k = 0; k < mods.size(); ++k) {
        auto ref = mods[k].find("ref");
        if (ref == mods[k].end()) continue;
        Node* owner = findNode((*ref)[0].get<uint32_t>());
        size_t idx = (*ref)[1].get<size_t>();
        n.modifiers[k] = owner && idx < owner->modifiers.size() ? owner->modifiers[idx] : nullptr;
      }
      n.modifiers.erase(std::remove(n.modifiers.begin(), n.modifiers.end(), nullptr), n.modifiers.end());
    }
    if (!n.object) n.object = std::make_shared<EditablePolyObject>();
    n.dirty_ = true;
    if (!rebuilt.count(n.id)) {
      r.nodes.push_back(n.id);
      r.geometryNodes.push_back(n.id);
    }
  }
  for (auto& n : nodes) {
    if (n->parentId && (!findNode(n->parentId) || isAncestor(n->id, n->parentId))) n->parentId = 0;
    if (!findLayer(n->layerId)) n->layerId = 0;
  }
  uint32_t maxNode = 0, maxMat = 0;
  for (auto& n : nodes) maxNode = std::max(maxNode, n->id);
  for (auto& m : materials) maxMat = std::max(maxMat, m->id);
  nextNodeId_ = std::max(s.globals.value("nextNodeId", 1u), maxNode + 1);
  nextMaterialId_ = std::max(s.globals.value("nextMaterialId", 1u), maxMat + 1);
  auto dedupe = [](std::vector<uint32_t>& v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
  };
  dedupe(r.nodes);
  dedupe(r.geometryNodes);
  colorCounter_ = static_cast<uint32_t>(nodes.size());
  if (bulkNames_) {
    bulkNames_->clear();
    for (auto& n : nodes) ++(*bulkNames_)[n->name];
  }
  return r;
}

// ---------------------------------------------------------------------------------------------
// Files

// .veraa files: the scene JSON, optionally followed by a binary mesh section. Scenes holding
// Editable Poly meshes are written as
//   "VERAABIN" u32 version u32 0  u64 jsonBytes  json  u64 meshCount  { u64 ref  u64 bytes  mesh }*
// where the JSON refers to meshes by "meshRef" (see MeshRefScope) and each mesh is in the
// encodePolyMesh form. Plain JSON files (older versions, hand-made scenes) still load.
namespace {
constexpr char kBinMagic[8] = {'V', 'E', 'R', 'A', 'A', 'B', 'I', 'N'};
constexpr uint32_t kBinVersion = 1;
}  // namespace

bool Scene::save(const std::string& path, std::string* error) {
  MeshRefTable refs;
  json j;
  {
    MeshRefScope scope(&refs);
    j = toJson();
  }
  std::ofstream f(std::filesystem::u8path(path), std::ios::binary);
  if (!f) {
    if (error) *error = "Cannot write " + path;
    return false;
  }
  if (refs.empty()) {
    f << j.dump(1);
  } else {
    std::string text = j.dump(1);
    auto putU32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto putU64 = [&](uint64_t v) { f.write(reinterpret_cast<const char*>(&v), 8); };
    f.write(kBinMagic, 8);
    putU32(kBinVersion);
    putU32(0);
    putU64(text.size());
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    std::vector<std::pair<uint64_t, const PolyMesh*>> list;
    list.reserve(refs.size());
    for (const auto& [id, m] : refs) list.emplace_back(id, m.get());
    std::sort(list.begin(), list.end());
    putU64(list.size());
    // Encode in parallel batches, then write in order (bounded extra memory).
    const size_t batch = std::max<size_t>(8, parallelThreads() * 4);
    std::vector<std::string> blobs;
    for (size_t start = 0; start < list.size() && f; start += batch) {
      size_t count = std::min(batch, list.size() - start);
      blobs.assign(count, std::string());
      parallelFor(count, [&](size_t i) { encodePolyMesh(*list[start + i].second, blobs[i]); });
      for (size_t i = 0; i < count; ++i) {
        putU64(list[start + i].first);
        putU64(blobs[i].size());
        f.write(blobs[i].data(), static_cast<std::streamsize>(blobs[i].size()));
      }
    }
  }
  if (!f) {
    if (error) *error = "Write failed: " + path;
    return false;
  }
  filePath = path;
  return true;
}

bool Scene::readFile(const std::string& path, SceneFile& out, std::string* error, ImportProgress* progress) {
  std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
  if (!f) {
    if (error) *error = "Cannot open " + path;
    return false;
  }
  char magic[8] = {};
  f.read(magic, 8);
  bool binary = f.gcount() == 8 && std::memcmp(magic, kBinMagic, 8) == 0;
  json& j = out.doc;
  MeshRefTable& refs = out.meshes;
  try {
    if (!binary) {
      f.clear();
      f.seekg(0);
      j = json::parse(f);
    } else {
      auto fail = [&](const std::string& why) {
        if (error) *error = "Invalid scene file: " + why;
        return false;
      };
      uint32_t version = 0, reserved = 0;
      uint64_t jsonBytes = 0;
      f.read(reinterpret_cast<char*>(&version), 4);
      f.read(reinterpret_cast<char*>(&reserved), 4);
      f.read(reinterpret_cast<char*>(&jsonBytes), 8);
      if (!f || version != kBinVersion) return fail("unsupported version");
      f.seekg(0, std::ios::end);
      uint64_t fileSize = static_cast<uint64_t>(f.tellg());
      if (jsonBytes > fileSize) return fail("truncated");
      f.seekg(24);
      std::string text(jsonBytes, '\0');
      f.read(text.data(), static_cast<std::streamsize>(jsonBytes));
      if (!f) return fail("truncated");
      j = json::parse(text);
      text = std::string();
      uint64_t count = 0;
      f.read(reinterpret_cast<char*>(&count), 8);
      if (!f || count > fileSize / 16) return fail("bad mesh count");
      // Read the mesh blobs, then decode them in parallel.
      std::vector<uint64_t> ids(count);
      std::vector<std::string> blobs(count);
      for (uint64_t i = 0; i < count; ++i) {
        if (reportProgress(progress, 0.05, 0.6, double(i) / double(count))) return fail("cancelled");
        uint64_t bytes = 0;
        f.read(reinterpret_cast<char*>(&ids[i]), 8);
        f.read(reinterpret_cast<char*>(&bytes), 8);
        if (!f || bytes > fileSize) return fail("truncated mesh data");
        blobs[i].resize(bytes);
        f.read(blobs[i].data(), static_cast<std::streamsize>(bytes));
        if (!f) return fail("truncated mesh data");
      }
      std::vector<std::shared_ptr<PolyMesh>> meshes(count);
      std::atomic<bool> bad{false};
      std::atomic<size_t> decoded{0};
      parallelFor(count, [&](size_t i) {
        auto m = std::make_shared<PolyMesh>();
        if (!decodePolyMesh(blobs[i].data(), blobs[i].size(), *m)) bad = true;
        blobs[i] = std::string();
        meshes[i] = std::move(m);
        reportProgress(progress, 0.6, 1.0, double(++decoded) / double(count));
      });
      if (bad) return fail("damaged mesh data");
      for (uint64_t i = 0; i < count; ++i) refs[ids[i]] = std::move(meshes[i]);
    }
  } catch (const std::exception& e) {
    if (error) *error = std::string("Invalid scene file: ") + e.what();
    return false;
  }
  if (progress && progress->cancelled()) {
    if (error) *error = "Cancelled";
    return false;
  }
  return true;
}

bool Scene::fromFile(const SceneFile& file, const std::string& path, std::string* error) {
  filePath = path;
  MeshRefScope scope(const_cast<MeshRefTable*>(&file.meshes));
  return fromJson(file.doc, error);
}

bool Scene::load(const std::string& path, std::string* error) {
  SceneFile file;
  return readFile(path, file, error) && fromFile(file, path, error);
}

std::string Scene::resolvePath(const std::string& path) const {
  if (path.empty()) return path;
  std::filesystem::path p = std::filesystem::u8path(path);
  if (p.is_absolute() || filePath.empty()) return path;
  return (std::filesystem::u8path(filePath).parent_path() / p).u8string();
}

}  // namespace veraa
