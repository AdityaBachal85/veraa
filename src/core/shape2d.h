// 2D polygon utilities for shapes: contour nesting, capping (triangulation with holes),
// booleans and offsets. Shapes are planar in their local XY plane.
#pragma once

#include <cstdint>
#include <vector>

#include "vmath.h"

namespace veraa::shape2d {

using Polygon = std::vector<Vec2>;

float signedArea(const Polygon& p);  // > 0 when counter-clockwise
bool pointInPolygon(const Vec2& pt, const Polygon& poly);
// Removes consecutive duplicate points and a closing point equal to the first.
Polygon cleaned(const Polygon& p, float eps = 1e-6f);

// A filled area: one counter-clockwise outer contour and clockwise holes.
struct Region {
  Polygon outer;
  std::vector<Polygon> holes;
};

// Groups closed contours by nesting (even-odd): contours inside an odd number of others are
// holes of their innermost container. Output contours are oriented (outer CCW, holes CW).
// contourIndex (optional) receives, per region, the input indices of outer then holes.
std::vector<Region> buildRegions(const std::vector<Polygon>& contours,
                                 std::vector<std::vector<int>>* contourIndex = nullptr);

// Triangulates a region. Indices refer to the region's points in order: outer, then each hole.
std::vector<uint32_t> triangulate(const Region& region);

enum class BoolOp { Union, Subtract, Intersect };
// Boolean of closed polygon sets (non-zero fill), result oriented outer CCW / holes CW.
std::vector<Polygon> boolean(const std::vector<Polygon>& a, const std::vector<Polygon>& b, BoolOp op);
// Offsets closed polygons outward by delta (negative shrinks). joinRound selects round joins.
std::vector<Polygon> offset(const std::vector<Polygon>& polys, float delta, bool joinRound);
// Clips polygon `subject` to the inside (keepInside) or outside of `clip` polygons.
std::vector<Polygon> clip(const Polygon& subject, const std::vector<Polygon>& clipPolys, bool keepInside);

// Intersection of segments a0-a1 and b0-b1; returns true with parameters ta, tb in [0, 1].
bool segmentIntersect(const Vec2& a0, const Vec2& a1, const Vec2& b0, const Vec2& b1, float& ta, float& tb);

// Delaunay triangulation of points (Bowyer-Watson). Returns triangle indices (CCW).
std::vector<uint32_t> delaunay(const std::vector<Vec2>& pts);

}  // namespace veraa::shape2d
