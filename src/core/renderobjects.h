// Objects and modifiers of the rendering module: VeraaLight (plane, disc, sphere, mesh, dome),
// VeraaIES, VeraaSun / Sun Positioner, VeraaPhysicalCamera, VeraaProxy, VeraaFur,
// VeraaInfinitePlane and the VeraaDisplacementMod modifier.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "mesh.h"
#include "modifier.h"
#include "object.h"
#include "texmap.h"

namespace veraa {

// Light shapes of VeraaLight (parameter "lightType").
enum class VeraaLightShape { Plane = 0, Disc = 1, Sphere = 2, Mesh = 3, Dome = 4 };

class VeraaLightObject : public LightObject {
 public:
  VeraaLightObject();
  std::string type() const override { return "VeraaLight"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<VeraaLightObject>(*this); }
  VeraaLightShape shape() const { return static_cast<VeraaLightShape>(params.getInt("lightType")); }
};

class VeraaIesObject : public LightObject {
 public:
  VeraaIesObject();
  std::string type() const override { return "VeraaIES"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<VeraaIesObject>(*this); }
};

// Physical sun. With "positionMode" = 1 (date, time and location) its direction comes from the
// solar position instead of the node's orientation, like the Sun Positioner.
class VeraaSunObject : public LightObject {
 public:
  explicit VeraaSunObject(bool positioner = false);
  std::string type() const override { return positioner_ ? "SunPositioner" : "VeraaSun"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<VeraaSunObject>(*this); }
  // Unit vector toward the sun in world space. nodeDir is the node's -Z axis reversed (manual mode).
  Vec3 sunDirection(const Vec3& nodeDir) const;
  // Solar elevation/azimuth in degrees for the date/time mode (azimuth clockwise from north).
  void solarAngles(float& azimuthDeg, float& elevationDeg) const;

 private:
  bool positioner_ = false;
};

class VeraaPhysicalCameraObject : public CameraObject {
 public:
  VeraaPhysicalCameraObject();
  std::string type() const override { return "VeraaPhysicalCamera"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<VeraaPhysicalCameraObject>(*this); }
  float verticalFovDeg(float aspect) const override;
  float horizontalFovDeg() const;
  // Lens aperture radius in scene units (cm) from focal length and f-number.
  float apertureRadius() const;
  // Shutter open time in seconds (still: 1/shutterSpeed; cinematic: angle / 360 / fps).
  float shutterSeconds(float fps) const;
  // Image multiplier from f-number, shutter and ISO (1 when exposure is off).
  float exposureMultiplier(float fps) const;
  // White balance multiplier (divides by the white point color).
  Vec3 whiteBalance() const;
};

// Render-time mesh loaded from a .vxp file (object space). Viewports show a preview.
class VeraaProxyObject : public GeometryObject {
 public:
  VeraaProxyObject();
  std::string type() const override { return "VeraaProxy"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<VeraaProxyObject>(*this); }
  PolyMesh build() const override;
};

// Hair/fur grown from another node's surface (parameter "sourceNode"). Rendered as curves.
class VeraaFurObject : public GeometryObject {
 public:
  VeraaFurObject();
  std::string type() const override { return "VeraaFur"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<VeraaFurObject>(*this); }
  PolyMesh build() const override { return PolyMesh(); }
};

// Infinite ground plane (rendered analytically; viewports show a finite grid).
class VeraaInfinitePlaneObject : public GeometryObject {
 public:
  VeraaInfinitePlaneObject();
  std::string type() const override { return "VeraaInfinitePlane"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<VeraaInfinitePlaneObject>(*this); }
  PolyMesh build() const override;
};

// Render-time displacement: subdivides the mesh and moves vertices along their normals by a
// texture map. Does nothing in viewports.
class VeraaDisplacementModifier : public Modifier {
 public:
  VeraaDisplacementModifier();
  std::string type() const override { return "VeraaDisplacementMod"; }
  std::unique_ptr<Modifier> clone() const override { return std::make_unique<VeraaDisplacementModifier>(*this); }
  json toJson() const override;
  void fromJson(const json& j) override;
  MapSlot texmap;  // the displacement map (falls back to the "bitmap" file parameter)
};

// ---- Fur strand generation (shared by the renderer and the viewport preview) ----
struct FurStrand {
  std::vector<Vec3> points;  // root first
  float rootRadius = 0.05f;
  float tipRadius = 0.0f;
  Vec2 rootUv;
};
// Grows strands on a world-space triangle mesh. maxStrands caps the count (0 = no cap).
std::vector<FurStrand> generateFur(const ParamBlock& furParams, const TriMesh& worldMesh, size_t maxStrands);

// ---- .vxp proxy files ----
// Binary: "VXP1", uint32 vertex count, uint32 triangle count, positions (3 floats),
// normals (3 floats), uvs (2 floats), indices (3 uint32 per triangle), material ids (int32).
bool writeProxyFile(const std::string& path, const TriMesh& mesh, std::string* error = nullptr);
// Cached by path; returns null when the file cannot be read.
std::shared_ptr<const TriMesh> loadProxyFile(const std::string& path, std::string* error = nullptr);

}  // namespace veraa
