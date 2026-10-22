// Bezier splines, the data behind shapes (Line, Rectangle, Circle, Text, ...). Shapes are
// drawn as lines, can be made renderable as tubes, and feed modifiers such as Extrude,
// Lathe and Sweep.
#pragma once

#include <vector>

#include "mesh.h"
#include "vmath.h"

namespace veraa {

enum class KnotType { Corner, Smooth, Bezier, BezierCorner };

struct Knot {
  Vec3 p;    // knot position
  Vec3 in;   // incoming handle (absolute position)
  Vec3 out;  // outgoing handle (absolute position)
  KnotType type = KnotType::Corner;
};

struct Spline {
  std::vector<Knot> knots;
  bool closed = false;

  int segmentCount() const;
  // Point on segment seg (0-based) at parameter t in [0, 1].
  Vec3 evaluate(int seg, float t) const;
  Vec3 tangent(int seg, float t) const;
  // Interpolated polyline with `steps` extra points per segment (3ds Max "Steps").
  // With optimize, straight segments get no extra points. Closed splines repeat no point.
  std::vector<Vec3> polyline(int steps, bool optimize) const;
  float length(int steps = 16) const;
  // Recomputes handles of Corner (straight) and Smooth (auto) knots from their neighbours.
  void updateAutoHandles();
};

struct BezierShape {
  std::vector<Spline> splines;

  bool empty() const { return splines.empty(); }
  Aabb bounds() const;
  void transform(const Mat4& m);
  void updateAutoHandles();
  std::vector<std::vector<Vec3>> polylines(int steps, bool optimize) const;
};

// Knot helpers: corner knot (handles on the knot) and bezier knot with explicit handles.
Knot cornerKnot(const Vec3& p);
Knot bezierKnot(const Vec3& p, const Vec3& in, const Vec3& out);

// Round tube along every spline (renderable splines: "Enable In Renderer"/"Viewport").
PolyMesh shapeToTubeMesh(const BezierShape& shape, float thickness, int sides, float angleDeg, int steps,
                         bool optimize);

}  // namespace veraa
