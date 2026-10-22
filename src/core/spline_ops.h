// Editing operations on splines (Editable Spline / Edit Spline tools). Shapes are edited in their
// local XY plane; 2D operations (booleans, trim) ignore Z. Operations that change topology
// invalidate knot/segment indices, so callers clear their sub-object selection afterwards.
#pragma once

#include <utility>
#include <vector>

#include "shape.h"
#include "shape2d.h"

namespace veraa::splineops {

struct KnotRef {
  int spline = 0;
  int knot = 0;
  bool operator<(const KnotRef& o) const { return spline != o.spline ? spline < o.spline : knot < o.knot; }
  bool operator==(const KnotRef& o) const { return spline == o.spline && knot == o.knot; }
};
using SegmentRef = KnotRef;  // segment i goes from knot i to knot i + 1 (wrapping when closed)

// Changes knot types. Bezier/Bezier Corner knots keep (or get smooth) handles; Corner and Smooth
// handles are recomputed.
void setKnotType(BezierShape& s, const std::vector<KnotRef>& knots, KnotType type);
// Inserts a knot on a segment at parameter t without changing the curve. Returns the new knot.
KnotRef refine(BezierShape& s, SegmentRef seg, float t);
// Rounds (fillet) or cuts (chamfer) corners at the knots.
void fillet(BezierShape& s, const std::vector<KnotRef>& knots, float radius);
void chamfer(BezierShape& s, const std::vector<KnotRef>& knots, float distance);
// Splits splines at knots (each selected knot becomes two coincident end knots).
void breakKnots(BezierShape& s, const std::vector<KnotRef>& knots);
void deleteKnots(BezierShape& s, const std::vector<KnotRef>& knots);
// Merges selected knots closer than threshold: joins open splines end to end, closes splines
// whose ends meet, and merges coincident neighbours. Returns the number of welds.
int weld(BezierShape& s, const std::vector<KnotRef>& knots, float threshold);
// Moves the knots to their common center (without merging them).
void fuse(BezierShape& s, const std::vector<KnotRef>& knots);
// Joins two open-spline end knots with a straight segment (closing the spline if same spline).
bool connect(BezierShape& s, KnotRef a, KnotRef b);
void makeFirst(BezierShape& s, KnotRef knot);
// Inserts knots where two splines cross. Returns the number of crossings found.
int crossInsert(BezierShape& s, int splineA, int splineB);
void divideSegments(BezierShape& s, const std::vector<SegmentRef>& segs, int divisions);
void deleteSegments(BezierShape& s, const std::vector<SegmentRef>& segs);
// Copies segments into a new shape (each run of consecutive segments becomes one spline).
BezierShape extractSegments(const BezierShape& s, const std::vector<SegmentRef>& segs);
BezierShape extractSplines(const BezierShape& s, const std::vector<int>& splines);
void deleteSplines(BezierShape& s, const std::vector<int>& splines);
// Adds an offset copy (closed splines) or a closed outline (open splines). center places the
// original in the middle of the outline.
void outline(BezierShape& s, const std::vector<int>& splines, float width, bool center);
// 2D boolean of closed splines: the target is replaced by the result and the operands removed.
bool booleanSplines(BezierShape& s, int target, const std::vector<int>& operands, shape2d::BoolOp op);
// Mirrors splines about their own center: axis 0 horizontal (X), 1 vertical (Y), 2 both.
void mirror(BezierShape& s, const std::vector<int>& splines, int axis, bool copy);
void reverse(BezierShape& s, const std::vector<int>& splines);
void close(BezierShape& s, const std::vector<int>& splines);
// Removes the part of a spline between the crossings around the picked point.
bool trim(BezierShape& s, int spline, const Vec3& pick);
// Extends the open end nearest to pick until it meets another spline.
bool extend(BezierShape& s, int spline, const Vec3& pick);

// Resamples every spline into knots spaced segLength apart along the curve.
BezierShape normalized(const BezierShape& s, float segLength);
// Fillets (or chamfers) every corner of every spline.
BezierShape filletAll(const BezierShape& s, float radius, bool chamferInstead);

// Nearest knot / segment to a point in shape space (XY distance). Returns false if none.
bool nearestSegment(const BezierShape& s, const Vec3& p, SegmentRef& seg, float& t, float& dist);

}  // namespace veraa::splineops
