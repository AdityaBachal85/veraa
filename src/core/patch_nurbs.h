// Patch Grids (Quad Patch, Tri Patch) and NURBS curves (Point Curve, CV Curve).
// Point curves interpolate their points; CV curves are uniform cubic B-splines whose control
// vertices pull the curve (it passes through the end CVs of open curves). Both convert exactly
// to Bezier splines, so every shape modifier works on them. Registered by registerPatchGridTypes()
// (registry.cpp) and registerNurbsCurveTypes() (called from registerShapeTypes for the Create
// panel order: Splines, NURBS Curves, Extended Splines).
#pragma once

#include <vector>

#include "object.h"

namespace veraa {

class NurbsCurveObject : public ShapeObject {
 public:
  explicit NurbsCurveObject(bool cvCurve);
  std::string type() const override { return cv_ ? "CVCurve" : "PointCurve"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<NurbsCurveObject>(*this); }
  BezierShape buildShape() const override;
  json toJson() const override;
  void fromJson(const json& j) override;
  bool isCvCurve() const { return cv_; }

  std::vector<Vec3> points;  // curve points (Point Curve) or control vertices (CV Curve)
  bool closed = false;

 private:
  bool cv_;
};

// Bezier conversion helpers (exposed for tests).
Spline interpolatingSpline(const std::vector<Vec3>& pts, bool closed);
Spline bsplineToBezier(const std::vector<Vec3>& cvs, bool closed);

void registerPatchGridTypes();
void registerNurbsCurveTypes();

}  // namespace veraa
