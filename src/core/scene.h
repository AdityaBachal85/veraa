// The scene: nodes (objects with transforms and modifier stacks), materials and render settings.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "material.h"
#include "modifier.h"
#include "object.h"

namespace veraa {

struct Transform {
  Vec3 position;
  Vec3 rotation;  // Euler XYZ, degrees
  Vec3 scale{1.0f, 1.0f, 1.0f};

  Mat4 matrix() const {
    return Mat4::translate(position) * Mat4::fromEulerDeg(rotation) * Mat4::scale(scale);
  }
  json toJson() const;
  void fromJson(const json& j);
};

class Node {
 public:
  Node() = default;
  Node(const Node&) = delete;
  Node& operator=(const Node&) = delete;

  // Deep copy (object, modifiers, transform, material). The copy has id 0 until added to a scene.
  std::unique_ptr<Node> clone() const;

  ObjectKind kind() const { return object ? object->kind() : ObjectKind::Geometry; }
  GeometryObject* geometry() const { return kind() == ObjectKind::Geometry ? static_cast<GeometryObject*>(object.get()) : nullptr; }
  LightObject* light() const { return kind() == ObjectKind::Light ? static_cast<LightObject*>(object.get()) : nullptr; }
  CameraObject* camera() const { return kind() == ObjectKind::Camera ? static_cast<CameraObject*>(object.get()) : nullptr; }

  Mat4 worldMatrix() const { return transform.matrix(); }

  // Object-space mesh after the modifier stack (empty for lights and cameras). Cached.
  const PolyMesh& mesh() const;
  // Object-space triangles of mesh(). Cached.
  const TriMesh& triMesh() const;
  // Call after changing object parameters or the modifier stack.
  void invalidate() { dirty_ = true; }
  Aabb worldBounds() const;

  json toJson() const;
  void fromJson(const json& j);  // throws std::runtime_error on unknown object types

  uint32_t id = 0;
  std::string name;
  Transform transform;
  Vec3 wireColor{0.6f, 0.6f, 0.6f};
  bool hidden = false;
  bool renderable = true;
  uint32_t materialId = 0;  // 0 = no material (renders as default grey)
  std::unique_ptr<SceneObject> object;
  std::vector<std::unique_ptr<Modifier>> modifiers;

 private:
  void evaluate() const;
  mutable bool dirty_ = true;
  mutable PolyMesh mesh_;
  mutable TriMesh tri_;
};

class Scene {
 public:
  Scene();
  Scene(const Scene&) = delete;
  Scene& operator=(const Scene&) = delete;

  // Creates a node holding a new object of the given type ("Box", "OmniLight", ...).
  Node* createNode(const std::string& objectType);
  // Takes ownership; assigns an id and makes the name unique.
  Node* addNode(std::unique_ptr<Node> node);
  bool removeNode(uint32_t id);
  Node* findNode(uint32_t id) const;
  Node* findNodeByName(const std::string& name) const;

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

  std::vector<std::unique_ptr<Node>> nodes;
  std::vector<std::unique_ptr<Material>> materials;
  ParamBlock renderSettings;
  std::string filePath;  // set by save/load

 private:
  uint32_t nextNodeId_ = 1;
  uint32_t nextMaterialId_ = 1;
  uint32_t colorCounter_ = 0;
};

// Render settings keys (Scene::renderSettings):
//  width, height, samples, timeLimit, maxDepth, clampIndirect, denoise,
//  exposure, tonemap (0 Filmic ACES, 1 Reinhard, 2 Linear), contrast, saturation,
//  envMode (0 Color, 1 Sky gradient, 2 HDRI, 3 Physical sky), envColor, envIntensity,
//  skyZenith, skyHorizon, groundColor, hdriPath, hdriRotation, hdriIntensity, skyTurbidity,
//  envVisible, backgroundColor, camera (render camera node name, "" = active viewport).
void defineRenderSettings(ParamBlock& p);

}  // namespace veraa
