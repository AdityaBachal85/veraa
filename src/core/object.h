// Scene objects: parametric geometry, shapes (splines), lights, cameras, helpers and space
// warps, plus the registry that lists every object type the Create panel can make.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "mesh.h"
#include "params.h"
#include "polysel.h"
#include "shape.h"

namespace veraa {

enum class ObjectKind { Geometry, Shape, Light, Camera, Helper, SpaceWarp };

class SceneObject {
 public:
  virtual ~SceneObject() = default;
  virtual std::string type() const = 0;  // e.g. "Box", "RectLight"
  virtual ObjectKind kind() const = 0;
  virtual std::unique_ptr<SceneObject> clone() const = 0;
  virtual json toJson() const;
  virtual void fromJson(const json& j);

  // Bumped by touch(); nodes sharing this object (instances) re-evaluate when it changes.
  uint64_t revision() const { return revision_; }
  void touch();

  ParamBlock params;

 private:
  uint64_t revision_ = 1;
};

class GeometryObject : public SceneObject {
 public:
  ObjectKind kind() const override { return ObjectKind::Geometry; }
  // Builds the base mesh in object space (before modifiers).
  virtual PolyMesh build() const = 0;
};

// Mesh stored explicitly (imported models, Convert to Editable Poly). The Editable Poly tools
// edit `mesh` and the sub-object selection directly.
class EditablePolyObject : public GeometryObject {
 public:
  std::string type() const override { return "EditablePoly"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<EditablePolyObject>(*this); }
  PolyMesh build() const override { return mesh; }
  json toJson() const override;
  void fromJson(const json& j) override;

  PolyMesh mesh;
  PolySelection sel;  // sub-object selection (level, selected/hidden parts)
};

// Splines. Common parameters (added by addShapeParams): "steps", "optimize",
// "renderInRenderer", "renderInViewport", "thickness", "sides", "angle".
class ShapeObject : public SceneObject {
 public:
  ObjectKind kind() const override { return ObjectKind::Shape; }
  // Builds the splines in object space (before modifiers).
  virtual BezierShape buildShape() const = 0;
  static void addShapeParams(ParamBlock& p);
};

// Spline stored explicitly (Line, Convert to Editable Spline). Edited by the Edit Spline tools.
class EditableSplineObject : public ShapeObject {
 public:
  EditableSplineObject() { addShapeParams(params); }
  std::string type() const override { return "EditableSpline"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<EditableSplineObject>(*this); }
  BezierShape buildShape() const override { return shape; }
  json toJson() const override;
  void fromJson(const json& j) override;

  BezierShape shape;
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

// Non-rendering helpers (Dummy, Point, Tape, Grid, groups' head nodes, ...).
class HelperObject : public SceneObject {
 public:
  ObjectKind kind() const override { return ObjectKind::Helper; }
};

// Space warps (Gravity, Wind, Ripple, ...). They affect particles or bound objects.
class SpaceWarpObject : public SceneObject {
 public:
  ObjectKind kind() const override { return ObjectKind::SpaceWarp; }
};

// ---- Object type registry ----

struct ObjectTypeInfo {
  std::string type;         // stable name stored in files, e.g. "ChamferBox"
  std::string displayName;  // shown in the UI, e.g. "Chamfer Box"
  std::string category;     // Create panel tab: Geometry, Shapes, Lights, Cameras, Helpers, Space Warps, Systems
  std::string subcategory;  // Create panel list, e.g. "Standard Primitives", "Extended Primitives", "Splines"
  std::function<std::unique_ptr<SceneObject>()> create;
};

// Registers (or replaces) an object type. Feature modules call this from their register
// function, which is listed in registerBuiltinObjectTypes() (registry.cpp).
void registerObjectType(ObjectTypeInfo info);
const std::vector<ObjectTypeInfo>& objectTypes();
const ObjectTypeInfo* findObjectType(const std::string& type);
// Types of one category (and optionally subcategory), in registration order.
std::vector<const ObjectTypeInfo*> objectTypesIn(const std::string& category, const std::string& subcategory = "");
// Subcategories of a category, in registration order.
std::vector<std::string> objectSubcategories(const std::string& category);

// Creates any object by type name ("Box", "Sphere", "OmniLight", "Camera", ...). Returns null if unknown.
std::unique_ptr<SceneObject> createObject(const std::string& type);

// Standard Primitives and Lights type names (compatibility helpers for the Create panel).
const std::vector<std::string>& geometryTypes();
const std::vector<std::string>& lightTypes();
// Human-readable name for a type, e.g. "RectLight" -> "Plane Light".
std::string objectDisplayName(const std::string& type);

// Exact-position vertex weld followed by removal of degenerate faces.
void weldVertices(PolyMesh& mesh);

// JSON helpers for meshes and shapes (used by editable objects and caches).
json polyMeshToJson(const PolyMesh& mesh);
PolyMesh polyMeshFromJson(const json& j);
json bezierShapeToJson(const BezierShape& shape);
BezierShape bezierShapeFromJson(const json& j);

}  // namespace veraa
