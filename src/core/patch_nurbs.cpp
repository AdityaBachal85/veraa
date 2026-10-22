#include "patch_nurbs.h"

#include <algorithm>
#include <cmath>

#include "registry.h"

namespace veraa {

// ---------------------------------------------------------------------------------------------
// NURBS curves

Spline interpolatingSpline(const std::vector<Vec3>& pts, bool closed) {
  Spline s;
  int n = static_cast<int>(pts.size());
  s.closed = closed && n >= 3;
  if (n == 0) return s;
  if (n == 1) {
    s.knots.push_back(cornerKnot(pts[0]));
    return s;
  }
  // Catmull-Rom tangents: handles one sixth of the neighbour chord (C1 through every point).
  auto at = [&](int i) {
    if (s.closed) return pts[(i % n + n) % n];
    return pts[std::clamp(i, 0, n - 1)];
  };
  for (int i = 0; i < n; ++i) {
    Vec3 t = (at(i + 1) - at(i - 1)) / 6.0f;
    if (!s.closed && (i == 0 || i == n - 1)) t = t * 2.0f;  // one-sided difference at open ends
    s.knots.push_back(bezierKnot(pts[i], pts[i] - t, pts[i] + t));
  }
  return s;
}

Spline bsplineToBezier(const std::vector<Vec3>& cvsIn, bool closed) {
  Spline s;
  int n = static_cast<int>(cvsIn.size());
  if (n < 2) {
    if (n == 1) s.knots.push_back(cornerKnot(cvsIn[0]));
    return s;
  }
  if (n == 2 && !closed) {
    s.knots = {cornerKnot(cvsIn[0]), cornerKnot(cvsIn[1])};
    s.updateAutoHandles();
    return s;
  }
  // Open curves repeat their end CVs so the curve starts and ends on them.
  std::vector<Vec3> P;
  bool periodic = closed && n >= 3;
  if (periodic) {
    P = cvsIn;
  } else {
    P.push_back(cvsIn.front());
    P.push_back(cvsIn.front());
    P.insert(P.end(), cvsIn.begin(), cvsIn.end());
    P.push_back(cvsIn.back());
    P.push_back(cvsIn.back());
  }
  int m = static_cast<int>(P.size());
  int segs = periodic ? m : m - 3;
  auto cv = [&](int i) { return P[((i % m) + m) % m]; };
  // Uniform cubic B-spline segment i (CVs i..i+3) as a Bezier: B0..B3.
  std::vector<Vec3> b0(segs), b1(segs), b2(segs), b3(segs);
  for (int i = 0; i < segs; ++i) {
    Vec3 p0 = cv(i), p1 = cv(i + 1), p2 = cv(i + 2), p3 = cv(i + 3);
    b0[i] = (p0 + p1 * 4.0f + p2) / 6.0f;
    b1[i] = (p1 * 2.0f + p2) / 3.0f;
    b2[i] = (p1 + p2 * 2.0f) / 3.0f;
    b3[i] = (p1 + p2 * 4.0f + p3) / 6.0f;
  }
  // Drop the degenerate pieces the repeated end CVs create (zero length).
  std::vector<int> keep;
  for (int i = 0; i < segs; ++i)
    if (length2(b3[i] - b0[i]) > 1e-12f || length2(b1[i] - b0[i]) > 1e-12f) keep.push_back(i);
  if (keep.empty()) {
    s.knots.push_back(cornerKnot(cvsIn.front()));
    return s;
  }
  int k = static_cast<int>(keep.size());
  for (int j = 0; j < k; ++j) {
    int i = keep[j];
    Vec3 in = j > 0 ? b2[keep[j - 1]] : (periodic ? b2[keep[k - 1]] : b0[i]);
    s.knots.push_back(bezierKnot(b0[i], in, b1[i]));
    s.knots.back().type = KnotType::BezierCorner;
  }
  if (periodic) {
    s.closed = true;
  } else {
    int last = keep[k - 1];
    s.knots.push_back(bezierKnot(b3[last], b2[last], b3[last]));
    s.knots.back().type = KnotType::BezierCorner;
  }
  return s;
}

NurbsCurveObject::NurbsCurveObject(bool cvCurve) : cv_(cvCurve) { addShapeParams(params); }

BezierShape NurbsCurveObject::buildShape() const {
  BezierShape b;
  if (points.size() < 2) return b;
  b.splines.push_back(cv_ ? bsplineToBezier(points, closed) : interpolatingSpline(points, closed));
  return b;
}

json NurbsCurveObject::toJson() const {
  json j = SceneObject::toJson();
  json pts = json::array();
  for (const auto& p : points) pts.push_back(vec3ToJson(p));
  j["points"] = pts;
  j["closed"] = closed;
  return j;
}

void NurbsCurveObject::fromJson(const json& j) {
  SceneObject::fromJson(j);
  points.clear();
  if (j.contains("points"))
    for (const auto& p : j["points"]) points.push_back(vec3FromJson(p));
  closed = j.value("closed", false);
}

// ---------------------------------------------------------------------------------------------
// Patch grids

namespace {

class PatchGridObject : public GeometryObject {
 public:
  explicit PatchGridObject(bool tri) : tri_(tri) {
    params.beginGroup("Parameters");
    params.addFloat("length", "Length", 100, 0.0, 1e7, 1.0);
    params.addFloat("width", "Width", 100, 0.0, 1e7, 1.0);
    params.addInt("lengthSegs", "Length Segs", 1, 1, 100);
    params.addInt("widthSegs", "Width Segs", 1, 1, 100);
    params.addInt("steps", "Interior Steps", 5, 0, 100);
    params.addBool("genMapping", "Generate Mapping Coords.", true);
  }
  std::string type() const override { return tri_ ? "TriPatch" : "QuadPatch"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<PatchGridObject>(*this); }
  PolyMesh build() const override {
    PolyMesh m;
    float L = params.getFloat("length"), W = params.getFloat("width");
    // Each patch is drawn with (steps + 1) subdivisions per side, like a patch's interior steps.
    int sub = params.getInt("steps") + 1;
    int nx = params.getInt("widthSegs") * sub, ny = params.getInt("lengthSegs") * sub;
    bool uv = params.getBool("genMapping");
    for (int j = 0; j <= ny; ++j)
      for (int i = 0; i <= nx; ++i) {
        float u = static_cast<float>(i) / nx, v = static_cast<float>(j) / ny;
        m.addVertex(Vec3((u - 0.5f) * W, (v - 0.5f) * L, 0));
        if (uv) m.addUv(Vec2(u, v));
      }
    auto id = [&](int i, int j) { return j * (nx + 1) + i; };
    for (int j = 0; j < ny; ++j)
      for (int i = 0; i < nx; ++i) {
        int a = id(i, j), b = id(i + 1, j), c = id(i + 1, j + 1), d = id(i, j + 1);
        if (tri_) {
          m.addFace({a, b, c}, uv ? std::vector<int>{a, b, c} : std::vector<int>{}, 1, 0);
          m.addFace({a, c, d}, uv ? std::vector<int>{a, c, d} : std::vector<int>{}, 1, 0);
        } else {
          m.addFace({a, b, c, d}, uv ? std::vector<int>{a, b, c, d} : std::vector<int>{}, 1, 0);
        }
      }
    return m;
  }

 private:
  bool tri_;
};

}  // namespace

void registerPatchGridTypes() {
  registerObjectType({"QuadPatch", "Quad Patch", "Geometry", "Patch Grids", [] { return std::make_unique<PatchGridObject>(false); }});
  registerObjectType({"TriPatch", "Tri Patch", "Geometry", "Patch Grids", [] { return std::make_unique<PatchGridObject>(true); }});
}

void registerNurbsCurveTypes() {
  registerObjectType({"PointCurve", "Point Curve", "Shapes", "NURBS Curves", [] { return std::make_unique<NurbsCurveObject>(false); }});
  registerObjectType({"CVCurve", "CV Curve", "Shapes", "NURBS Curves", [] { return std::make_unique<NurbsCurveObject>(true); }});
}

}  // namespace veraa
