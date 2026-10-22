// Stairs: Straight, L-Type, U-Type and Spiral, each Open (treads only), Closed (treads and
// risers) or Box (solid), with optional stringers, carriage and handrails. Flights climb along
// their local +Y with the width along +X; the first step starts at the pivot.
#include <algorithm>
#include <cmath>

#include "aec.h"
#include "meshbuild.h"
#include "shape2d.h"
#include "shape_mesh.h"
#include "shapes.h"

namespace veraa {

namespace {

constexpr float kUv = 0.01f;
constexpr int kRiser = 0, kTread = 1, kStringer = 2, kPole = 3, kRail = 4, kCarriage = 5;

// Prism over a polygon with one material per polygon edge, between w0 and w1 along the prism's
// local Z, transformed by xf.
void prismEdgeMats(PolyMesh& m, std::vector<Vec2> pts, std::vector<int> mats, float w0, float w1, int capMat,
                   const Mat4& xf) {
  if (pts.size() < 3) return;
  if (shape2d::signedArea(pts) < 0) {
    std::reverse(pts.begin(), pts.end());
    size_t E = mats.size();
    std::vector<int> r(E);
    for (size_t i = 0; i < E; ++i) r[(2 * E - 2 - i) % E] = mats[i];
    mats = r;
  }
  ExtrudeOptions o;
  o.capStartMatId = o.capEndMatId = capMat;
  o.generateUVs = false;
  o.sharpAngleDeg = 10.0f;
  PolyMesh p = extrudeContours({Contour2D{pts, true}}, {{w0, 0.0f}, {w1, 0.0f}}, o);
  for (size_t f = 0; f < mats.size() && f < p.faces.size(); ++f) p.faces[f].matId = mats[f];
  p.transform(xf);
  boxMapUvs(p, kUv);
  m.append(p);
}

// Maps a side profile (u = run direction Y, v = height Z) extruded along w (= X).
Mat4 profileToStair() {
  Mat4 r;
  r.m[0][0] = 0; r.m[0][2] = 1;
  r.m[1][0] = 1; r.m[1][1] = 0;
  r.m[2][1] = 1; r.m[2][2] = 0;
  return r;
}

void tagBox(PolyMesh& m, const Mat4& xf, const Vec3& lo, const Vec3& hi, int mat) {
  if (hi.x - lo.x <= 1e-6f || hi.y - lo.y <= 1e-6f || hi.z - lo.z <= 1e-6f) return;
  MeshBuilder(m, kUv).box(xf, lo, hi, mat);
}

void addRailPath(PolyMesh& m, const std::vector<Vec3>& pts, float radius, int sides) {
  if (pts.size() < 2 || radius <= 0) return;
  BezierShape path;
  Spline s;
  for (const auto& p : pts) s.knots.push_back(cornerKnot(p));
  s.updateAutoHandles();
  path.splines.push_back(s);
  BezierShape sec;
  std::vector<Vec2> circle;
  for (int k = 0; k < sides; ++k) {
    float a = kTwoPi * k / sides;
    circle.emplace_back(std::cos(a) * radius, std::sin(a) * radius);
  }
  SweepOptions o;
  o.banking = false;
  o.generateUVs = true;
  PolyMesh r = sweepSection(path, 0, true, {Contour2D{circle, true}}, o);
  for (auto& f : r.faces) f.matId = kRail;
  m.append(r);
}

class StairObject : public GeometryObject {
 public:
  enum Kind { LType, Spiral, Straight, UType };
  explicit StairObject(Kind k) : kind_(k) {
    params.beginGroup("Type");
    params.addEnum("stairType", "Type", {"Open", "Closed", "Box"}, 1);
    params.beginGroup("Generate Geometry");
    params.addBool("stringers", "Stringers", false);
    params.addBool("carriage", "Carriage", false);
    params.addBool("railLeft", "Handrail Left", false);
    params.addBool("railRight", "Handrail Right", false);
    if (k == Spiral) params.addBool("centerPole", "Center Pole", true);
    params.beginGroup("Layout");
    switch (k) {
      case Straight:
        params.addFloat("length", "Length", 300, 1.0, 1e7, 1.0);
        params.addFloat("width", "Width", 100, 1.0, 1e7, 1.0);
        break;
      case LType:
      case UType:
        params.addFloat("length1", "Length 1", 200, 1.0, 1e7, 1.0);
        params.addFloat("length2", "Length 2", 200, 1.0, 1e7, 1.0);
        params.addFloat("width", "Width", 100, 1.0, 1e7, 1.0);
        if (k == LType) params.addEnum("turn", "Turn", {"Left", "Right"}, 0);
        else {
          params.addEnum("side", "Second Flight", {"Left", "Right"}, 1);
          params.addFloat("offset", "Offset", 20, 0.0, 1e7, 1.0);
        }
        break;
      case Spiral:
        params.addFloat("radius", "Radius", 120, 1.0, 1e7, 1.0);
        params.addFloat("revolutions", "Revolutions", 1, 0.05, 100.0, 0.05);
        params.addFloat("width", "Width", 90, 1.0, 1e7, 1.0);
        params.addEnum("direction", "Direction", {"CCW", "CW"}, 0);
        break;
    }
    params.beginGroup("Rise");
    params.addFloat("height", "Overall", 300, 1.0, 1e7, 1.0);
    params.addInt("risers", "Riser Count", 16, 2, 1000);
    params.beginGroup("Steps");
    params.addFloat("thickness", "Thickness", 5, 0.1, 1e7, 0.5);
    params.beginGroup("Stringers");
    params.addFloat("stringerDepth", "Depth", 30, 0.1, 1e7, 1.0);
    params.addFloat("stringerWidth", "Width", 5, 0.1, 1e7, 0.5);
    params.beginGroup("Carriage");
    params.addFloat("carriageDepth", "Depth", 20, 0.1, 1e7, 1.0);
    params.addFloat("carriageWidth", "Width", 15, 0.1, 1e7, 0.5);
    params.beginGroup("Railings");
    params.addFloat("railHeight", "Height", 90, 0.0, 1e7, 1.0);
    params.addFloat("railOffset", "Offset", 5, 0.0, 1e7, 0.5);
    params.addFloat("railRadius", "Radius", 2.5f, 0.1, 1e7, 0.1);
    params.addInt("railSegments", "Segments", 10, 3, 64);
    if (k == Spiral) {
      params.beginGroup("Center Pole");
      params.addFloat("poleRadius", "Radius", 10, 0.1, 1e7, 0.5);
      params.addFloat("poleExtra", "Height Above", 0, 0.0, 1e7, 1.0);
    }
  }
  std::string type() const override {
    static const char* names[] = {"LTypeStair", "SpiralStair", "StraightStair", "UTypeStair"};
    return names[kind_];
  }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<StairObject>(*this); }
  PolyMesh build() const override;

 private:
  struct Run {
    Mat4 xf;
    float length = 0;
    int steps = 0;
    float base = 0;  // height under the first step
  };
  void buildRun(PolyMesh& m, const Run& run, float W, float rise) const;
  void buildLanding(PolyMesh& m, const Mat4& xf, float w, float l, float top) const;
  PolyMesh buildSpiral() const;

  Kind kind_;
};

void StairObject::buildRun(PolyMesh& m, const Run& run, float W, float rise) const {
  if (run.steps <= 0 || run.length <= 0) return;
  int type = params.getInt("stairType");
  float t = params.getFloat("thickness");
  int k = run.steps;
  float g = run.length / k, L = run.length, b = run.base;
  float topZ = b + k * rise;
  Mat4 P = run.xf * profileToStair();
  if (type == 2) {
    // Box: one solid sawtooth profile across the width, down to the floor.
    std::vector<Vec2> pts;
    std::vector<int> mats;
    pts.emplace_back(0, 0);
    for (int j = 0; j < k; ++j) {
      pts.emplace_back(j * g, b + (j + 1) * rise);  // riser up
      mats.push_back(j == 0 ? kRiser : kRiser);
      pts.emplace_back((j + 1) * g, b + (j + 1) * rise);  // tread
      mats.push_back(kTread);
    }
    pts.emplace_back(L, 0);
    mats.push_back(kStringer);  // back
    mats.push_back(kStringer);  // bottom
    // The first edge (0,0)->(0, b + rise) is the first riser; the loop above pushed one material
    // per edge starting with it, so the counts match: 2k edges + back + bottom.
    prismEdgeMats(m, pts, mats, 0.0f, W, kStringer, P);
  } else {
    for (int j = 0; j < k; ++j) {
      float top = b + (j + 1) * rise;
      tagBox(m, run.xf, Vec3(0, j * g, top - t), Vec3(W, (j + 1) * g, top), kTread);
      if (type == 1) {
        float rb = j == 0 ? b : top - rise;
        tagBox(m, run.xf, Vec3(0, j * g, rb), Vec3(W, j * g + std::min(t, g * 0.5f), top - t), kRiser);
      }
    }
  }
  // Sloped beams under the step line: from (0, rise) to (L, topZ), depth d below it.
  auto beam = [&](float x0, float x1, float d, int mat) {
    float slope = (topZ - (b + rise)) / std::max(1e-3f, L - g);
    float zTop0 = b + rise - slope * 0.0f;
    std::vector<Vec2> pts;
    float zb0 = zTop0 - d;
    if (zb0 >= b) {
      pts = {Vec2(0, zb0), Vec2(0, zTop0), Vec2(L, topZ), Vec2(L, topZ - d)};
    } else {
      float yHit = (b - zb0) / std::max(1e-6f, slope);
      pts = {Vec2(0, b), Vec2(0, zTop0), Vec2(L, topZ), Vec2(L, topZ - d)};
      if (yHit < L) pts.insert(pts.begin() + 0, Vec2(std::min(yHit, L), b));
    }
    prismEdgeMats(m, pts, std::vector<int>(pts.size(), mat), x0, x1, mat, P);
  };
  if (type != 2 && params.getBool("stringers")) {
    float sw = params.getFloat("stringerWidth"), sd = params.getFloat("stringerDepth");
    beam(-sw, 0.0f, sd, kStringer);
    beam(W, W + sw, sd, kStringer);
  }
  if (type != 2 && params.getBool("carriage")) {
    float cw = params.getFloat("carriageWidth"), cd = params.getFloat("carriageDepth");
    beam(W * 0.5f - cw * 0.5f, W * 0.5f + cw * 0.5f, cd + t, kCarriage);
  }
  float rh = params.getFloat("railHeight"), ro = params.getFloat("railOffset");
  float rr = params.getFloat("railRadius");
  int rs = params.getInt("railSegments");
  auto rail = [&](float x) {
    std::vector<Vec3> pts = {run.xf.transformPoint(Vec3(x, 0, b + rise + rh)),
                             run.xf.transformPoint(Vec3(x, L, topZ + rh))};
    addRailPath(m, pts, rr, rs);
    // Posts at both ends.
    MeshBuilder bld(m, kUv);
    for (const auto& q : {Vec3(x, g * 0.5f, b + rise), Vec3(x, L - g * 0.5f, topZ)}) {
      Vec3 a = run.xf.transformPoint(q);
      bld.cylinder(a, a + Vec3(0, 0, rh), rr * 0.8f, std::max(6, rs / 2), true, kRail);
    }
  };
  if (params.getBool("railLeft")) rail(ro);
  if (params.getBool("railRight")) rail(W - ro);
}

void StairObject::buildLanding(PolyMesh& m, const Mat4& xf, float w, float l, float top) const {
  int type = params.getInt("stairType");
  float t = params.getFloat("thickness");
  if (type == 2) {
    MeshBuilder b(m, kUv);
    size_t first = m.faces.size();
    b.box(xf, Vec3(0, 0, 0), Vec3(w, l, top), kStringer);
    if (m.faces.size() == first + 6) m.faces[first + 1].matId = kTread;  // +Z face
  } else {
    tagBox(m, xf, Vec3(0, 0, top - t), Vec3(w, l, top), kTread);
  }
}

PolyMesh StairObject::buildSpiral() const {
  PolyMesh m;
  float R = params.getFloat("radius"), w = std::min(params.getFloat("width"), R);
  float H = params.getFloat("height");
  int n = params.getInt("risers");
  float rise = H / n;
  float rev = params.getFloat("revolutions");
  float dir = params.getInt("direction") == 1 ? -1.0f : 1.0f;
  float total = kTwoPi * rev * dir;
  bool pole = params.getBool("centerPole");
  float pr = pole ? std::min(params.getFloat("poleRadius"), R * 0.9f) : 0.0f;
  float rin = std::max(pr, R - w);
  int type = params.getInt("stairType");
  float t = params.getFloat("thickness");
  int arcSegs = std::max(2, static_cast<int>(std::ceil(std::fabs(total) / n / radians(10.0f))));
  auto wedge = [&](float a0, float a1) {
    std::vector<Vec2> pts;
    for (int s = 0; s <= arcSegs; ++s) {
      float a = a0 + (a1 - a0) * s / arcSegs;
      pts.emplace_back(std::cos(a) * R, std::sin(a) * R);
    }
    for (int s = arcSegs; s >= 0; --s) {
      float a = a0 + (a1 - a0) * s / arcSegs;
      pts.emplace_back(std::cos(a) * std::max(rin, 0.01f), std::sin(a) * std::max(rin, 0.01f));
    }
    return pts;
  };
  float stepAngle = total / n;
  int perRev = static_cast<int>(std::floor(n / std::max(rev, 1e-3f)));
  for (int j = 0; j < n; ++j) {
    float a0 = stepAngle * j, a1 = stepAngle * (j + 1);
    float top = (j + 1) * rise;
    std::vector<Vec2> pts = wedge(a0, a1);
    if (type == 2) {
      // Solid down to the step one revolution below (or the floor).
      float bottom = j >= perRev && perRev > 0 ? (j - perRev + 1) * rise : 0.0f;
      addPrism(m, pts, bottom, top, kStringer, kTread, kStringer);
    } else {
      addPrism(m, pts, top - t, top, kTread, kTread, kTread);
      if (type == 1) {
        // Riser plate along the step's leading edge.
        float c = std::cos(a0), s = std::sin(a0);
        Vec2 side(-s * dir, c * dir);  // into the step
        float th = std::min(t, 2.0f * kPi * rin * rev / n * 0.4f + t * 0.1f);
        std::vector<Vec2> plate = {Vec2(c * rin, s * rin), Vec2(c * R, s * R), Vec2(c * R, s * R) + side * th,
                                   Vec2(c * rin, s * rin) + side * th};
        addPrism(m, plate, top - rise, top - t, kRiser, kRiser, kRiser);
      }
    }
  }
  MeshBuilder b(m, kUv);
  if (pole) b.cylinder(Vec3(), Vec3(0, 0, H + params.getFloat("poleExtra")), pr, 16, true, kPole);
  // Helical path at radius r and height offset h.
  auto helix = [&](float r, float h) {
    std::vector<Vec3> pts;
    int segs = std::max(8, static_cast<int>(std::ceil(std::fabs(total) / radians(8.0f))));
    for (int s = 0; s <= segs; ++s) {
      float f = static_cast<float>(s) / segs;
      float a = total * f;
      pts.emplace_back(std::cos(a) * r, std::sin(a) * r, rise + (H - rise) * f + h);
    }
    return pts;
  };
  if (type != 2 && params.getBool("stringers")) {
    float sw = params.getFloat("stringerWidth"), sd = params.getFloat("stringerDepth");
    BezierShape path;
    Spline s;
    for (const auto& p : helix(R + sw * 0.5f, -sd * 0.5f)) s.knots.push_back(cornerKnot(p));
    path.splines.push_back(s);
    BezierShape sec;
    sec.splines.push_back(makeRectangleSpline(sw, sd, 0));
    SweepOptions o;
    o.banking = false;
    PolyMesh st = sweepSection(path, 0, true, shapeContours(sec, 0, true), o);
    for (auto& f : st.faces) f.matId = kStringer;
    m.append(st);
  }
  if (type != 2 && params.getBool("carriage")) {
    float cw = params.getFloat("carriageWidth"), cd = params.getFloat("carriageDepth");
    BezierShape path;
    Spline s;
    for (const auto& p : helix((R + rin) * 0.5f, -t - cd * 0.5f)) s.knots.push_back(cornerKnot(p));
    path.splines.push_back(s);
    BezierShape sec;
    sec.splines.push_back(makeRectangleSpline(cw, cd, 0));
    SweepOptions o;
    o.banking = false;
    PolyMesh st = sweepSection(path, 0, true, shapeContours(sec, 0, true), o);
    for (auto& f : st.faces) f.matId = kCarriage;
    m.append(st);
  }
  float rh = params.getFloat("railHeight"), ro = params.getFloat("railOffset"), rr = params.getFloat("railRadius");
  int rs = params.getInt("railSegments");
  if (params.getBool("railRight")) addRailPath(m, helix(R - ro, rh), rr, rs);
  if (params.getBool("railLeft")) addRailPath(m, helix(rin + ro, rh), rr, rs);
  return m;
}

PolyMesh StairObject::build() const {
  if (kind_ == Spiral) return buildSpiral();
  PolyMesh m;
  float H = params.getFloat("height"), W = params.getFloat("width");
  int n = params.getInt("risers");
  float rise = H / n;
  if (kind_ == Straight) {
    buildRun(m, {Mat4(), params.getFloat("length"), n, 0.0f}, W, rise);
    return m;
  }
  float L1 = params.getFloat("length1"), L2 = params.getFloat("length2");
  int n1 = std::max(1, static_cast<int>(std::lround((n - 1) * L1 / (L1 + L2))));
  n1 = std::min(n1, n - 2);
  int n2 = n - 1 - n1;
  float landingTop = (n1 + 1) * rise;
  buildRun(m, {Mat4(), L1, n1, 0.0f}, W, rise);
  if (kind_ == LType) {
    bool left = params.getInt("turn") == 0;
    buildLanding(m, Mat4::translate(Vec3(0, L1, 0)), W, W, landingTop);
    Mat4 xf = left ? Mat4::translate(Vec3(0, L1, 0)) * Mat4::rotateZ(kPi * 0.5f)
                   : Mat4::translate(Vec3(W, L1 + W, 0)) * Mat4::rotateZ(-kPi * 0.5f);
    buildRun(m, {xf, L2, n2, landingTop}, W, rise);
  } else {
    bool right = params.getInt("side") == 1;
    float off = params.getFloat("offset");
    float lx = right ? 0.0f : -(W + off);
    buildLanding(m, Mat4::translate(Vec3(lx, L1, 0)), 2 * W + off, W, landingTop);
    Mat4 xf = Mat4::translate(right ? Vec3(2 * W + off, L1, 0) : Vec3(-off, L1, 0)) * Mat4::rotateZ(kPi);
    buildRun(m, {xf, L2, n2, landingTop}, W, rise);
  }
  return m;
}

}  // namespace

void registerStairTypes() {
  const char* geo = "Geometry";
  const char* sub = "Stairs";
  registerObjectType({"LTypeStair", "L-Type Stair", geo, sub, [] { return std::make_unique<StairObject>(StairObject::LType); }});
  registerObjectType({"SpiralStair", "Spiral Stair", geo, sub, [] { return std::make_unique<StairObject>(StairObject::Spiral); }});
  registerObjectType(
      {"StraightStair", "Straight Stair", geo, sub, [] { return std::make_unique<StairObject>(StairObject::Straight); }});
  registerObjectType({"UTypeStair", "U-Type Stair", geo, sub, [] { return std::make_unique<StairObject>(StairObject::UType); }});
}

}  // namespace veraa
