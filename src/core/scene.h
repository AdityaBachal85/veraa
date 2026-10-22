// The scene: nodes (objects with transforms, links and modifier stacks), layers, materials,
// render settings and time settings.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "material.h"
#include "modifier.h"
#include "object.h"

namespace veraa {

class Scene;

struct Transform {
  Vec3 position;
  Vec3 rotation;  // Euler XYZ, degrees
  Vec3 scale{1.0f, 1.0f, 1.0f};

  Mat4 matrix() const {
    return Mat4::translate(position) * Mat4::fromEulerDeg(rotation) * Mat4::scale(scale);
  }
  bool isIdentity() const { return position == Vec3() && rotation == Vec3() && scale == Vec3(1.0f); }
  json toJson() const;
  void fromJson(const json& j);
};

// Splits a matrix (rotation and non-negative or mirrored scale, no shear) into a Transform.
Transform decomposeTransform(const Mat4& m);

struct Layer {
  uint32_t id = 0;
  std::string name;
  bool hidden = false;
  bool frozen = false;
  bool renderable = true;
  Vec3 color{0.6f, 0.6f, 0.6f};
};

struct TimeConfig {
  int startFrame = 0;
  int endFrame = 100;
  float fps = 30.0f;
  double currentFrame = 0.0;
  json toJson() const;
  void fromJson(const json& j);
};

class Node {
 public:
  Node() = default;
  Node(const Node&) = delete;
  Node& operator=(const Node&) = delete;

  // Deep copy (object, modifiers, transform, material). The copy has id 0 and no parent until
  // added to a scene.
  std::unique_ptr<Node> clone() const;
  // Instance: shares the object and modifiers with this node (3ds Max "Instance" clone).
  std::unique_ptr<Node> instance() const;

  ObjectKind kind() const { return object ? object->kind() : ObjectKind::Geometry; }
  GeometryObject* geometry() const { return kind() == ObjectKind::Geometry ? static_cast<GeometryObject*>(object.get()) : nullptr; }
  ShapeObject* shapeObject() const { return kind() == ObjectKind::Shape ? static_cast<ShapeObject*>(object.get()) : nullptr; }
  LightObject* light() const { return kind() == ObjectKind::Light ? static_cast<LightObject*>(object.get()) : nullptr; }
  CameraObject* camera() const { return kind() == ObjectKind::Camera ? static_cast<CameraObject*>(object.get()) : nullptr; }

  // ---- Transforms ----
  // Local transform relative to the parent (or the world when unlinked).
  Mat4 localMatrix() const { return transform.matrix(); }
  // Node (pivot) transform in world space, following parent links.
  Mat4 worldMatrix() const;
  Mat4 parentWorldMatrix() const;
  // Geometry transform: world matrix times the object offset (pivot adjustment).
  // Use this to place meshes and shapes; use worldMatrix() for the pivot.
  Mat4 objectWorldMatrix() const { return worldMatrix() * objectOffset.matrix(); }
  // Sets the local transform so the node ends up at this world matrix.
  void setWorldMatrix(const Mat4& world);
  Vec3 worldPosition() const { return worldMatrix().translation(); }
  void setWorldPosition(const Vec3& p);
  Node* parent() const;
  Scene* scene() const { return scene_; }

  // ---- Geometry (object space, i.e. before objectWorldMatrix) ----
  // Mesh after the modifier stack (empty for lights, cameras and non-renderable shapes). Cached.
  const PolyMesh& mesh() const;
  const TriMesh& triMesh() const;
  // Spline display lines after the modifier stack (shapes only; empty otherwise). Closed
  // splines repeat their first point at the end.
  const std::vector<std::vector<Vec3>>& shapeLines() const;
  // The evaluated state is a shape (splines), e.g. a Line without an Extrude modifier.
  bool isShapeResult() const;
  // Call after changing object parameters or the modifier stack. Instances update too.
  void invalidate();
  // Increases every time the geometry is re-evaluated (for GPU caches).
  uint64_t geometryStamp() const;
  Aabb worldBounds() const;

  json toJson() const;
  void fromJson(const json& j);  // throws std::runtime_error on unknown object types

  uint32_t id = 0;
  std::string name;
  Transform transform;
  Transform objectOffset;   // pivot offset applied to the geometry only (Hierarchy > Pivot)
  uint32_t parentId = 0;    // 0 = world
  uint32_t layerId = 0;
  Vec3 wireColor{0.6f, 0.6f, 0.6f};
  uint32_t materialId = 0;  // 0 = no material (renders with the wire color)

  // Display.
  bool hidden = false;
  bool frozen = false;
  bool seeThrough = false;
  bool displayAsBox = false;
  bool backfaceCull = false;
  // Rendering.
  bool renderable = true;
  bool castShadows = true;
  bool receiveShadows = true;
  bool visibleToCamera = true;
  bool visibleToReflections = true;
  int gBufferId = 0;         // object ID render element
  bool motionBlur = true;
  // Groups: a group is a helper head node; members are linked to it and flagged.
  bool groupHead = false;
  bool groupOpen = false;
  bool groupMember = false;
  // Free-form user properties (Object Properties > User Defined).
  std::string userProps;

  std::shared_ptr<SceneObject> object;
  std::vector<std::shared_ptr<Modifier>> modifiers;

 private:
  friend class Scene;
  void evaluate() const;
  uint64_t stackRevision() const;

  Scene* scene_ = nullptr;
  mutable bool dirty_ = true;
  mutable uint64_t evaluatedRevision_ = 0;
  mutable uint64_t geometryStamp_ = 0;
  mutable bool isShape_ = false;
  mutable PolyMesh mesh_;
  mutable TriMesh tri_;
  mutable std::vector<std::vector<Vec3>> shapeLines_;
};

// Live data a feature module keeps with the scene (animation tracks, ...). It is written to
// Scene::extensions[name] when the scene is serialized and read back when it is loaded, so it
// is saved in files and is part of undo snapshots.
class SceneModule {
 public:
  virtual ~SceneModule() = default;
  virtual json toJson() const = 0;
  // Called with the saved section, or with null for a new or cleared scene.
  virtual void fromJson(const json& j) = 0;
};
using SceneModuleFactory = std::function<std::unique_ptr<SceneModule>()>;
// Every Scene creates one instance of each registered module. Register from a registerXxx()
// function listed in registry.cpp (registerBuiltinSceneModules).
void registerSceneModule(const std::string& name, SceneModuleFactory factory);

class Scene {
 public:
  Scene();
  Scene(const Scene&) = delete;
  Scene& operator=(const Scene&) = delete;

  // Creates a node holding a new object of the given type ("Box", "OmniLight", ...).
  Node* createNode(const std::string& objectType);
  // Takes ownership; assigns an id and makes the name unique.
  Node* addNode(std::unique_ptr<Node> node);
  // Removes a node. Its children are unlinked and keep their world placement.
  bool removeNode(uint32_t id);
  Node* findNode(uint32_t id) const;
  Node* findNodeByName(const std::string& name) const;

  // ---- Hierarchy ----
  std::vector<Node*> children(uint32_t id) const;
  // Links child to parent (0 = unlink) keeping the child's world placement. Refuses cycles.
  bool setParent(uint32_t childId, uint32_t parentId);
  bool isAncestor(uint32_t ancestorId, uint32_t nodeId) const;
  // Nodes sharing the same object (instances), including n itself.
  std::vector<Node*> instancesOf(const Node& n) const;

  // ---- Layers ----
  Layer* findLayer(uint32_t id);
  const Layer* findLayer(uint32_t id) const;
  Layer* createLayer(const std::string& name);
  bool removeLayer(uint32_t id);  // moves its nodes to layer 0; layer 0 cannot be removed
  // Effective state combining the node, its layer and (for hidden/frozen) closed groups.
  bool isNodeVisible(const Node& n) const;
  bool isNodeFrozen(const Node& n) const;
  bool isNodeRenderable(const Node& n) const;

  Material* createMaterial(const std::string& name = "");
  Material* addMaterial(std::unique_ptr<Material> material);  // assigns a fresh id
  bool removeMaterial(uint32_t id);                           // also unassigns it from nodes
  Material* findMaterial(uint32_t id) const;

  // "Box" -> "Box001", "Box002", ... (3ds Max naming).
  std::string uniqueNodeName(const std::string& base) const;
  std::string uniqueMaterialName(const std::string& base) const;
  Aabb bounds(bool geometryOnly = true) const;
  void clear();

  json toJson() const;
  bool fromJson(const json& j, std::string* error = nullptr);
  bool save(const std::string& path, std::string* error = nullptr);
  bool load(const std::string& path, std::string* error = nullptr);

  // Resolves a texture/HDRI path relative to the scene file's folder.
  std::string resolvePath(const std::string& path) const;

  // Live data of a registered scene module (see SceneModule), or null.
  SceneModule* module(const std::string& name) const;

  std::vector<std::unique_ptr<Node>> nodes;
  std::vector<Layer> layers;  // layers[0] is layer "0"
  uint32_t currentLayerId = 0;
  std::vector<std::unique_ptr<Material>> materials;
  ParamBlock renderSettings;
  TimeConfig time;
  // Extra top-level sections owned by feature modules (animation, environment effects, ...).
  // Keys are module names; modules read/write their own entry. Saved with the scene.
  json extensions = json::object();
  std::string filePath;  // set by save/load

 private:
  uint32_t nextNodeId_ = 1;
  uint32_t nextMaterialId_ = 1;
  uint32_t nextLayerId_ = 1;
  uint32_t colorCounter_ = 0;
  mutable std::unordered_map<uint32_t, Node*> index_;  // id -> node, for fast parent lookups
  std::vector<std::pair<std::string, std::unique_ptr<SceneModule>>> modules_;
};

// Render settings keys (Scene::renderSettings):
//  width, height, samples, timeLimit, maxDepth, clampIndirect, denoise,
//  exposure, tonemap (0 Filmic ACES, 1 Reinhard, 2 Linear), contrast, saturation,
//  envMode (0 Color, 1 Sky gradient, 2 HDRI, 3 Physical sky), envColor, envIntensity,
//  skyZenith, skyHorizon, groundColor, hdriPath, hdriRotation, hdriIntensity, skyTurbidity,
//  envVisible, backgroundColor, camera (render camera node name, "" = active viewport).
void defineRenderSettings(ParamBlock& p);

}  // namespace veraa
