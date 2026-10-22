// Mesh modifiers: free-form deformers (FFD 2x2x2 / 3x3x3 / 4x4x4 / Box / Cylinder), Symmetry,
// Slice, Cap Holes, Chamfer, Relax, Optimize, Lattice, Melt, Ripple, Wave, Spherify, Skew,
// Stretch, Squeeze, Displace, Subdivide, Tessellate, Face Extrude, Material, Material By
// Element, Smooth (auto smoothing groups), Vertex Weld, UVW XForm and Unwrap UVW.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "modifier.h"

namespace veraa {

// Free-form deformation lattice. Control points are stored as offsets from their rest
// positions, which are spread over the bounding box of the modifier's input (or a cylinder
// around it for FFD(Cyl)).
class FfdModifier : public Modifier {
 public:
  enum Kind { Fixed2, Fixed3, Fixed4, Box, Cylinder };
  explicit FfdModifier(Kind kind);
  std::string type() const override;
  std::unique_ptr<Modifier> clone() const override { return std::make_unique<FfdModifier>(*this); }
  void apply(PolyMesh& mesh) const override;
  json toJson() const override;
  void fromJson(const json& j) override;

  Kind kind() const { return kind_; }
  // Lattice resolution: box (x, y, z) or cylinder (sides, radial, height).
  void dims(int& a, int& b, int& c) const;
  int pointCount() const;
  // Bounds of the input at the last evaluation (the lattice frame).
  Aabb latticeBounds() const { return bounds_; }
  bool latticeValid() const { return boundsValid_; }
  Vec3 restPoint(int i) const;
  Vec3 point(int i) const { return restPoint(i) + offset(i); }
  Vec3 offset(int i) const;
  void setOffset(int i, const Vec3& d);
  void resetPoints() { offsets_.clear(); }
  // Lattice edges (pairs of point indices) for display.
  std::vector<std::pair<int, int>> latticeEdges() const;

 private:
  Kind kind_;
  std::vector<Vec3> offsets_;  // object-space offsets; resized to pointCount() on demand
  mutable Aabb bounds_;
  mutable bool boundsValid_ = false;
};

// Edited texture coordinates. Applied when the input has the same polygons as when the UVs
// were edited; otherwise the input passes through unchanged.
class UnwrapUvwModifier : public Modifier {
 public:
  UnwrapUvwModifier();
  std::string type() const override { return "UnwrapUVW"; }
  std::unique_ptr<Modifier> clone() const override { return std::make_unique<UnwrapUvwModifier>(*this); }
  void apply(PolyMesh& mesh) const override;
  json toJson() const override;
  void fromJson(const json& j) override;

  // True when edited UVs exist and match `mesh`'s polygons.
  bool matches(const PolyMesh& mesh) const;
  // Starts editing from the input's UVs (or a default projection when it has none).
  void initFrom(const PolyMesh& input);
  bool hasEdits() const { return !faceUvs.empty(); }

  std::vector<Vec2> uvs;
  std::vector<std::vector<int>> faceUvs;  // per polygon: uv index per corner
  // Input mesh at the last evaluation (for the editor).
  mutable PolyMesh lastInput;
  mutable bool lastInputValid = false;
};

// ---- UV editing algorithms (used by Unwrap UVW) ----

// Projects the given faces' UVs (new UV vertices, detached from other faces).
// mapping: 0 planar (best-fit), 1 box, 2 cylindrical, 3 spherical.
void projectFaceUvs(const PolyMesh& mesh, std::vector<Vec2>& uvs, std::vector<std::vector<int>>& faceUvs,
                    const std::vector<int>& faces, int mapping);
// Flatten mapping: groups faces into charts by normal angle, projects each chart and packs the
// charts into the 0..1 square without overlaps.
void flattenUvs(const PolyMesh& mesh, std::vector<Vec2>& uvs, std::vector<std::vector<int>>& faceUvs,
                float angleDeg, float spacing);
// Relaxes UV vertices toward the average of their UV neighbours (selected only if not empty).
void relaxUvs(std::vector<Vec2>& uvs, const std::vector<std::vector<int>>& faceUvs, const std::vector<int>& selected,
              float amount, int iterations);
// Stitch: UV vertices of the same mesh vertex among `selected` are merged to their average.
void stitchUvs(const PolyMesh& mesh, std::vector<Vec2>& uvs, std::vector<std::vector<int>>& faceUvs,
               const std::vector<int>& selected);
// Rebuilds the UV index lists so no index is shared by two different values (and drops unused).
void compactUvList(std::vector<Vec2>& uvs, std::vector<std::vector<int>>& faceUvs);
// Packs UV charts (connected through shared UV vertices) into the unit square.
void packUvCharts(std::vector<Vec2>& uvs, const std::vector<std::vector<int>>& faceUvs, float spacing);

// Quadric error decimation to at most targetFaces triangles.
PolyMesh decimateMesh(const PolyMesh& mesh, int targetFaces, bool keepBorders);

void registerMeshModifierTypes();

}  // namespace veraa
