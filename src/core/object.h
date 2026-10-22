// Scene objects: parametric geometry (primitives), editable meshes, lights and cameras.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "mesh.h"
#include "params.h"

namespace veraa {

enum class ObjectKind { Geometry, Light, Camera };

class SceneObject {
 public:
  virtual ~SceneObject() = default;
  virtual std::string type() const = 0;  // e.g. "Box", "RectLight"
  virtual ObjectKind kind() const = 0;
  virtual std::unique_ptr<SceneObject> clone() const = 0;
  virtual json toJson() const;
  virtual void fromJson(const json& j);

  ParamBlock params;
};

class GeometryObject : public SceneObject {
 public:
  ObjectKind kind() const override { return ObjectKind::Geometry; }
  // Builds the base mesh in object space (before modifiers).
  virtual PolyMesh build() const = 0;
};

// Mesh stored explicitly (imported models; later the Editable Poly tools edit it).
class EditablePolyObject : public GeometryObject {
 public:
  std::string type() const override { return "EditablePoly"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<EditablePolyObject>(*this); }
  PolyMesh build() const override { return mesh; }
  json toJson() const override;
  void fromJson(const json& j) override;

  PolyMesh mesh;
};

// Lights emit along their local -Z axis (like 3ds Max), except Omni and Sphere lights.
class LightObject : public SceneObject {
 public:
  ObjectKind kind() const override { return ObjectKind::Light; }
};

class CameraObject : public SceneObject {
 public:
  CameraObject();
  std::string type() const override { return "Camera"; }
  ObjectKind kind() const override { return ObjectKind::Camera; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<CameraObject>(*this); }
  // Vertical field of view in degrees for a given image aspect (width / height).
  // The "fov" parameter is horizontal, as in 3ds Max.
  float verticalFovDeg(float aspect) const;
};

// Creates any object by type name ("Box", "Sphere", "OmniLight", "Camera", ...). Returns null if unknown.
std::unique_ptr<SceneObject> createObject(const std::string& type);

// Type names grouped like the Create panel.
const std::vector<std::string>& geometryTypes();
const std::vector<std::string>& lightTypes();
// Human-readable name for a type, e.g. "RectLight" -> "Plane Light".
std::string objectDisplayName(const std::string& type);

// Exact-position vertex weld followed by removal of degenerate faces.
void weldVertices(PolyMesh& mesh);

}  // namespace veraa
