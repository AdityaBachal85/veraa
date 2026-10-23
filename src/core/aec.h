// AEC objects: Doors (Pivot, Sliding, BiFold), Windows (Awning, Casement, Fixed, Pivoted,
// Projected, Sliding), AEC Extended (Foliage, Railing, Wall) and Stairs (L-Type, Spiral, Straight,
// U-Type). Each part gets its own material ID so a Multi/Sub-Object material can dress it.
// Registered by registerAecTypes().
//
// Material IDs (0-based; the UI shows them plus one):
//   Doors:   0 front, 1 back, 2 panels and glass, 3 frame, 4 leaf edges
//   Windows: 0 frame, 1 sash rails, 2 glass
//   Wall:    0 vertical ends, 1 outside face, 2 inside face, 3 top, 4 bottom
//   Railing: 0 top rail, 1 lower rails, 2 posts, 3 fencing
//   Stairs:  0 risers, 1 treads, 2 stringers, 3 center pole, 4 handrails, 5 carriage
//   Foliage: 0 trunk, 1 branches, 2 leaves, 3 fruit, 4 flowers
#pragma once

#include <vector>

#include "object.h"

namespace veraa {

// Wall: a polyline footprint extruded to the wall height with a width and justification.
class WallObject : public GeometryObject {
 public:
  WallObject();
  std::string type() const override { return "Wall"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<WallObject>(*this); }
  PolyMesh build() const override;
  json toJson() const override;
  void fromJson(const json& j) override;

  std::vector<Vec2> points;  // footprint in the object's XY plane
  bool closed = false;
};

// Railing: rails, posts and fencing along a picked path (or a straight run of "length").
class RailingObject : public GeometryObject {
 public:
  RailingObject();
  std::string type() const override { return "Railing"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<RailingObject>(*this); }
  PolyMesh build() const override;
  json toJson() const override;
  void fromJson(const json& j) override;

  BezierShape path;  // in the railing's object space; empty = straight along +X
  std::string pathName;
};

// Adds a prism over a closed polygon (counter-clockwise) between z0 and z1, transformed by xf.
void addPrism(PolyMesh& m, const std::vector<Vec2>& polygon, float z0, float z1, int sideMat, int topMat,
              int bottomMat, const Mat4& xf = Mat4());

void registerAecTypes();
void registerDoorWindowTypes();  // aec_doors.cpp
void registerStairTypes();       // aec_stairs.cpp

}  // namespace veraa
