// Turning shapes into meshes: extrusion with bevel levels, lathe (surface of revolution) and
// sweep (a cross-section along a path). Used by the shape modifiers, TextPlus and AEC objects.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "mesh.h"
#include "shape.h"

namespace veraa {

struct Contour2D {
  std::vector<Vec2> pts;
  bool closed = false;
};

// Interpolates the shape's splines (projected to XY) into contours. Closed contours are oriented
// by nesting (outer counter-clockwise, holes clockwise); open contours keep their direction.
std::vector<Contour2D> shapeContours(const BezierShape& shape, int steps, bool optimize);

// One ring of an extrusion: height above the shape plane and outline offset (positive grows the
// filled area outward).
struct ExtrudeLevel {
  float z = 0.0f;
  float outline = 0.0f;
};

struct ExtrudeOptions {
  bool capStart = true;
  bool capEnd = true;
  bool generateUVs = true;
  bool smoothAcrossLevels = false;  // false: each level span gets its own smoothing groups
  int capStartMatId = 0;            // 3ds Max IDs minus one: caps 1 and 2, sides 3
  int capEndMatId = 1;
  int sideMatId = 2;
  float sharpAngleDeg = 30.0f;      // contour corners sharper than this are not smoothed
  float gridCapSize = 0.0f;         // > 0: Grid capping (caps get interior grid vertices)
};

// Extrudes contours through the levels (at least two). Closed contours give closed manifolds.
PolyMesh extrudeContours(const std::vector<Contour2D>& contours, const std::vector<ExtrudeLevel>& levels,
                         const ExtrudeOptions& opt);

struct LatheOptions {
  float degrees = 360.0f;
  int segments = 16;
  int axis = 1;   // rotation axis direction: 0 X, 1 Y, 2 Z
  int align = 1;  // axis position on the shape bounds: 0 Min, 1 Center, 2 Max
  bool weldCore = false;
  bool flipNormals = false;
  bool capStart = true;
  bool capEnd = true;
  bool generateUVs = true;
};

PolyMesh latheContours(const std::vector<Contour2D>& contours, const LatheOptions& opt);

struct SweepOptions {
  float angleDeg = 0.0f;
  Vec2 offset;
  int pivotAlign = 4;  // 3x3 grid, 0 = bottom-left ... 4 = center ... 8 = top-right
  bool mirrorX = false;
  bool mirrorY = false;
  bool banking = true;
  bool generateUVs = true;
  bool capEnds = true;
  bool smoothSection = true;  // smooth around the section (sharp section corners stay sharp)
  bool smoothPath = true;     // smooth along the path
};

// Sweeps a closed or open cross-section (contours in its XY plane) along every spline of path.
PolyMesh sweepSection(const BezierShape& path, int pathSteps, bool pathOptimize,
                      const std::vector<Contour2D>& section, const SweepOptions& opt);

// ---- Helpers shared with compound and AEC objects ----

// Frames along a path polyline: a section point (x, y) maps to P + R x + U y, stretched at
// corners so straight runs keep their thickness (miter joints).
struct PathFrames {
  std::vector<Vec3> P, T, R, U, bend;
  std::vector<float> miter;  // corner stretch along `bend`
  std::vector<float> dist;   // distance along the path at each point
  float length = 0.0f;
  bool closed = false;
  int segments() const;
  Vec3 place(int i, const Vec2& q) const;
};
// banking: frames follow the path's turns (parallel transport); otherwise they stay level.
PathFrames pathFrames(const std::vector<Vec3>& polyline, bool closed, bool banking);

// Smoothing-group bit index (0..2) per contour edge; runs break at corners sharper than sharpDeg.
std::vector<int> contourSmoothBits(const std::vector<Vec2>& pts, bool closed, float sharpDeg);

// Caps the closed contours (holes allowed) whose vertices already exist in m.
void capContours(PolyMesh& m, const std::vector<Contour2D>& contours, const std::function<int(int, int)>& vertexOf,
                 bool reverse, int matId, uint32_t smooth, bool uvs);

}  // namespace veraa
