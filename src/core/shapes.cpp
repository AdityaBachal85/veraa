#include "shapes.h"

#include <cmath>
#include <map>
#include <unordered_map>

#include "patch_nurbs.h"
#include "registry.h"

namespace veraa {

namespace {

Vec3 v3(const Vec2& p) { return Vec3(p.x, p.y, 0.0f); }

bool samePoint(const Vec3& a, const Vec3& b) { return length2(a - b) < 1e-10f; }

// Closes s if its last knot coincides with the first (merging their handles).
void closeSpline(Spline& s) {
  if (s.knots.size() > 2 && samePoint(s.knots.front().p, s.knots.back().p)) {
    s.knots.front().in = s.knots.back().in;
    s.knots.pop_back();
  }
  s.closed = true;
}

Spline rotated(Spline s, float deg) {
  if (deg == 0.0f) return s;
  Mat4 r = Mat4::rotateZ(radians(deg));
  for (auto& k : s.knots) {
    k.p = r.transformPoint(k.p);
    k.in = r.transformPoint(k.in);
    k.out = r.transformPoint(k.out);
  }
  return s;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Builders

void appendArc(Spline& s, const Vec2& center, float radius, float a0, float a1) {
  float span = a1 - a0;
  int n = std::max(1, static_cast<int>(std::ceil(std::fabs(span) / (kPi * 0.5f) - 1e-4f)));
  float step = span / n;
  float k = 4.0f / 3.0f * std::tan(step / 4.0f) * radius;
  auto pt = [&](float a) { return Vec3(center.x + radius * std::cos(a), center.y + radius * std::sin(a), 0.0f); };
  auto tan = [&](float a) { return Vec3(-std::sin(a), std::cos(a), 0.0f); };
  for (int i = 0; i < n; ++i) {
    float a = a0 + step * i, b = a0 + step * (i + 1);
    Vec3 p0 = pt(a), p1 = pt(b);
    if (!s.knots.empty() && samePoint(s.knots.back().p, p0)) {
      s.knots.back().out = p0 + tan(a) * k;
      s.knots.back().type = KnotType::BezierCorner;
    } else {
      s.knots.push_back(Knot{p0, p0, p0 + tan(a) * k, KnotType::BezierCorner});
    }
    s.knots.push_back(Knot{p1, p1 - tan(b) * k, p1, KnotType::BezierCorner});
  }
}

Spline makeCircleSpline(float radius, const Vec2& center) {
  Spline s;
  appendArc(s, center, radius, 0.0f, kTwoPi);
  closeSpline(s);
  for (auto& k : s.knots) k.type = KnotType::Bezier;
  return s;
}

Spline makeEllipseSpline(float hx, float hy, const Vec2& center) {
  Spline s = makeCircleSpline(1.0f);
  for (auto& k : s.knots) {
    auto sc = [&](const Vec3& p) { return Vec3(center.x + p.x * hx, center.y + p.y * hy, 0.0f); };
    k.p = sc(k.p);
    k.in = sc(k.in);
    k.out = sc(k.out);
  }
  return s;
}

Spline makeArcSpline(float radius, float fromDeg, float toDeg, bool pie, const Vec2& center) {
  Spline s;
  float a0 = radians(fromDeg), a1 = radians(toDeg);
  if (a1 <= a0) a1 += kTwoPi;
  if (pie) s.knots.push_back(cornerKnot(v3(center)));
  appendArc(s, center, radius, a0, a1);
  if (pie) {
    s.closed = true;
  }
  return s;
}

Spline makeRectangleSpline(float width, float length, float r, const Vec2& c) {
  float hw = width * 0.5f, hl = length * 0.5f;
  r = std::min(std::max(r, 0.0f), std::min(hw, hl));
  if (r <= 1e-6f)
    return makePolylineSpline({{c.x - hw, c.y - hl}, {c.x + hw, c.y - hl}, {c.x + hw, c.y + hl}, {c.x - hw, c.y + hl}},
                              true);
  Spline s;
  appendArc(s, Vec2(c.x + hw - r, c.y - hl + r), r, -kPi * 0.5f, 0.0f);
  appendArc(s, Vec2(c.x + hw - r, c.y + hl - r), r, 0.0f, kPi * 0.5f);
  appendArc(s, Vec2(c.x - hw + r, c.y + hl - r), r, kPi * 0.5f, kPi);
  appendArc(s, Vec2(c.x - hw + r, c.y - hl + r), r, kPi, kPi * 1.5f);
  closeSpline(s);
  return s;
}

Spline makePolylineSpline(const std::vector<Vec2>& pts, bool closed) {
  Spline s;
  for (const auto& p : pts) s.knots.push_back(cornerKnot(v3(p)));
  s.closed = closed;
  s.updateAutoHandles();
  return s;
}

Spline makeFilletedPolygon(const std::vector<Vec2>& pts, const std::vector<float>& radii) {
  size_t n = pts.size();
  Spline s;
  if (n < 3) return makePolylineSpline(pts, true);
  for (size_t i = 0; i < n; ++i) {
    Vec2 p = pts[i];
    float r = i < radii.size() ? radii[i] : 0.0f;
    Vec2 prev = pts[(i + n - 1) % n], next = pts[(i + 1) % n];
    Vec2 dp = prev - p, dn = next - p;
    float lp = std::sqrt(dp.x * dp.x + dp.y * dp.y), ln = std::sqrt(dn.x * dn.x + dn.y * dn.y);
    if (r <= 1e-6f || lp < 1e-6f || ln < 1e-6f) {
      s.knots.push_back(cornerKnot(v3(p)));
      continue;
    }
    dp = dp / lp;
    dn = dn / ln;
    float cosT = clampf(dp.x * dn.x + dp.y * dn.y, -1.0f, 1.0f);
    float theta = std::acos(cosT);  // interior angle between the two edges
    if (theta < 1e-3f || theta > kPi - 1e-3f) {
      s.knots.push_back(cornerKnot(v3(p)));
      continue;
    }
    float d = r / std::tan(theta * 0.5f);
    float dmax = 0.5f * std::min(lp, ln);
    if (d > dmax) {
      d = dmax;
      r = d * std::tan(theta * 0.5f);
    }
    Vec2 bis = dp + dn;
    float bl = std::sqrt(bis.x * bis.x + bis.y * bis.y);
    bis = bis / bl;
    Vec2 center = p + bis * (r / std::sin(theta * 0.5f));
    Vec2 a = p + dp * d, b = p + dn * d;
    float a0 = std::atan2(a.y - center.y, a.x - center.x);
    float a1 = std::atan2(b.y - center.y, b.x - center.x);
    float delta = a1 - a0;
    while (delta > kPi) delta -= kTwoPi;
    while (delta < -kPi) delta += kTwoPi;
    appendArc(s, center, r, a0, a0 + delta);
  }
  closeSpline(s);
  return s;
}

namespace {

BezierShape single(Spline s) {
  BezierShape b;
  b.splines.push_back(std::move(s));
  return b;
}

// Profiles as polygons with fillet radii; center bounding box on the origin.
std::vector<Vec2> channelPts(float w, float l, float t) {
  float hw = w * 0.5f, hl = l * 0.5f;
  return {{-hw, -hl}, {hw, -hl}, {hw, -hl + t}, {-hw + t, -hl + t}, {-hw + t, hl - t}, {hw, hl - t}, {hw, hl}, {-hw, hl}};
}
std::vector<Vec2> anglePts(float w, float l, float t) {
  float hw = w * 0.5f, hl = l * 0.5f;
  return {{-hw, -hl}, {hw, -hl}, {hw, -hl + t}, {-hw + t, -hl + t}, {-hw + t, hl}, {-hw, hl}};
}
std::vector<Vec2> teePts(float w, float l, float t) {
  float hw = w * 0.5f, hl = l * 0.5f, ht = t * 0.5f;
  return {{-ht, -hl}, {ht, -hl}, {ht, hl - t}, {hw, hl - t}, {hw, hl}, {-hw, hl}, {-hw, hl - t}, {-ht, hl - t}};
}
std::vector<Vec2> flangePts(float w, float l, float t) {
  float hw = w * 0.5f, hl = l * 0.5f, ht = t * 0.5f;
  return {{-hw, -hl},     {hw, -hl},     {hw, -hl + t}, {ht, -hl + t}, {ht, hl - t},   {hw, hl - t},
          {hw, hl},       {-hw, hl},     {-hw, hl - t}, {-ht, hl - t}, {-ht, -hl + t}, {-hw, -hl + t}};
}

}  // namespace

const std::vector<std::string>& sectionProfileNames() {
  static const std::vector<std::string> names = {"Bar",  "Angle",        "Channel", "Cylinder", "Half Round",
                                                 "Pipe", "Quarter Round", "Tee",     "Tube",     "Wide Flange"};
  return names;
}

BezierShape makeSectionProfile(int type, float l, float w, float t, float r) {
  t = std::max(0.01f, std::min(t, std::min(l, w) * 0.49f));
  switch (type) {
    case 0: return single(makeRectangleSpline(w, l, r));
    case 1: return single(makeFilletedPolygon(anglePts(w, l, t), {r, 0, 0, r * 0.5f, 0, 0}));
    case 2: return single(makeFilletedPolygon(channelPts(w, l, t), {r, 0, 0, r * 0.5f, r * 0.5f, 0, 0, r}));
    case 3: return single(makeCircleSpline(w * 0.5f));
    case 4: {
      Spline s;
      s.knots.push_back(cornerKnot(Vec3(-w * 0.5f, 0, 0)));
      appendArc(s, Vec2(), w * 0.5f, 0.0f, kPi);
      closeSpline(s);
      return single(s);
    }
    case 5: {
      BezierShape b = single(makeCircleSpline(w * 0.5f));
      b.splines.push_back(makeCircleSpline(std::max(0.01f, w * 0.5f - t)));
      return b;
    }
    case 6: return single(makeArcSpline(w, 0.0f, 90.0f, true, Vec2(-w * 0.5f, -w * 0.5f)));
    case 7: return single(makeFilletedPolygon(teePts(w, l, t), {0, 0, r, 0, 0, 0, 0, r}));
    case 8: {
      BezierShape b = single(makeRectangleSpline(w, l, r));
      b.splines.push_back(makeRectangleSpline(std::max(0.01f, w - 2 * t), std::max(0.01f, l - 2 * t), std::max(0.0f, r - t)));
      return b;
    }
    default:
      return single(makeFilletedPolygon(flangePts(w, l, t), {0, 0, 0, r, r, 0, 0, 0, 0, r, r, 0}));
  }
}

// ---------------------------------------------------------------------------------------------
// Parametric shape objects

namespace {

using DefineFn = void (*)(ParamBlock&);
using BuildFn = BezierShape (*)(const ParamBlock&);

class FnShape : public ShapeObject {
 public:
  FnShape(std::string type, DefineFn define, BuildFn build) : type_(std::move(type)), build_(build) {
    addShapeParams(params);
    params.beginGroup("Parameters");
    define(params);
  }
  std::string type() const override { return type_; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<FnShape>(*this); }
  BezierShape buildShape() const override { return build_(params); }

 private:
  std::string type_;
  BuildFn build_;
};

void defRectangle(ParamBlock& p) {
  p.addFloat("length", "Length", 100, 0.0, 1e7, 1.0);
  p.addFloat("width", "Width", 100, 0.0, 1e7, 1.0);
  p.addFloat("cornerRadius", "Corner Radius", 0, 0.0, 1e7, 0.5);
}
BezierShape buildRectangle(const ParamBlock& p) {
  return single(makeRectangleSpline(p.getFloat("width"), p.getFloat("length"), p.getFloat("cornerRadius")));
}

void defCircle(ParamBlock& p) { p.addFloat("radius", "Radius", 50, 0.0, 1e7, 1.0); }
BezierShape buildCircle(const ParamBlock& p) { return single(makeCircleSpline(p.getFloat("radius"))); }

void defEllipse(ParamBlock& p) {
  p.addFloat("length", "Length", 100, 0.0, 1e7, 1.0);
  p.addFloat("width", "Width", 150, 0.0, 1e7, 1.0);
  p.addBool("outline", "Outline", false);
  p.addFloat("thickness", "Thickness", 5, 0.0, 1e7, 0.5);
}
BezierShape buildEllipse(const ParamBlock& p) {
  float hx = p.getFloat("width") * 0.5f, hy = p.getFloat("length") * 0.5f;
  BezierShape b = single(makeEllipseSpline(hx, hy));
  if (p.getBool("outline")) {
    float t = p.getFloat("thickness");
    b.splines.push_back(makeEllipseSpline(std::max(0.01f, hx - t), std::max(0.01f, hy - t)));
  }
  return b;
}

void defArc(ParamBlock& p) {
  p.addFloat("radius", "Radius", 50, 0.0, 1e7, 1.0);
  p.addFloat("from", "From", 0, -36000, 36000, 1.0, 1);
  p.addFloat("to", "To", 180, -36000, 36000, 1.0, 1);
  p.addBool("pie", "Pie Slice", false);
  p.addBool("reverse", "Reverse", false);
}
BezierShape buildArc(const ParamBlock& p) {
  Spline s = makeArcSpline(p.getFloat("radius"), p.getFloat("from"), p.getFloat("to"), p.getBool("pie"));
  if (p.getBool("reverse")) {
    std::reverse(s.knots.begin(), s.knots.end());
    for (auto& k : s.knots) std::swap(k.in, k.out);
  }
  return single(s);
}

void defDonut(ParamBlock& p) {
  p.addFloat("radius1", "Radius 1", 50, 0.0, 1e7, 1.0);
  p.addFloat("radius2", "Radius 2", 40, 0.0, 1e7, 1.0);
}
BezierShape buildDonut(const ParamBlock& p) {
  BezierShape b = single(makeCircleSpline(p.getFloat("radius1")));
  b.splines.push_back(makeCircleSpline(p.getFloat("radius2")));
  return b;
}

void defNGon(ParamBlock& p) {
  p.addFloat("radius", "Radius", 50, 0.0, 1e7, 1.0);
  p.addEnum("fit", "Type", {"Inscribed", "Circumscribed"}, 0);
  p.addInt("sides", "Sides", 6, 3, 100);
  p.addFloat("cornerRadius", "Corner Radius", 0, 0.0, 1e7, 0.5);
  p.addBool("circular", "Circular", false);
}
BezierShape buildNGon(const ParamBlock& p) {
  int n = p.getInt("sides");
  float r = p.getFloat("radius");
  if (p.getInt("fit") == 1) r /= std::cos(kPi / n);
  float start = -kPi * 0.5f + kPi / n;
  std::vector<Vec2> pts;
  for (int i = 0; i < n; ++i) {
    float a = start + kTwoPi * i / n;
    pts.emplace_back(r * std::cos(a), r * std::sin(a));
  }
  if (p.getBool("circular")) {
    Spline s = makePolylineSpline(pts, true);
    for (auto& k : s.knots) k.type = KnotType::Smooth;
    s.updateAutoHandles();
    return single(s);
  }
  return single(makeFilletedPolygon(pts, std::vector<float>(n, p.getFloat("cornerRadius"))));
}

void defStar(ParamBlock& p) {
  p.addFloat("radius1", "Radius 1", 50, 0.0, 1e7, 1.0);
  p.addFloat("radius2", "Radius 2", 25, 0.0, 1e7, 1.0);
  p.addInt("points", "Points", 6, 3, 100);
  p.addFloat("distortion", "Distortion", 0, -180, 180, 1.0, 1);
  p.addFloat("fillet1", "Fillet Radius 1", 0, 0.0, 1e7, 0.5);
  p.addFloat("fillet2", "Fillet Radius 2", 0, 0.0, 1e7, 0.5);
}
BezierShape buildStar(const ParamBlock& p) {
  int n = p.getInt("points");
  float r1 = p.getFloat("radius1"), r2 = p.getFloat("radius2"), dist = radians(p.getFloat("distortion"));
  std::vector<Vec2> pts;
  std::vector<float> radii;
  for (int i = 0; i < n; ++i) {
    float a = kPi * 0.5f + kTwoPi * i / n;
    pts.emplace_back(r1 * std::cos(a), r1 * std::sin(a));
    radii.push_back(p.getFloat("fillet1"));
    float b = a + kPi / n + dist;
    pts.emplace_back(r2 * std::cos(b), r2 * std::sin(b));
    radii.push_back(p.getFloat("fillet2"));
  }
  return single(makeFilletedPolygon(pts, radii));
}

void defHelix(ParamBlock& p) {
  p.addFloat("radius1", "Radius 1", 50, 0.0, 1e7, 1.0);
  p.addFloat("radius2", "Radius 2", 50, 0.0, 1e7, 1.0);
  p.addFloat("height", "Height", 100, -1e7, 1e7, 1.0);
  p.addFloat("turns", "Turns", 3, 0.0, 1000, 0.1);
  p.addFloat("bias", "Bias", 0, -1, 1, 0.05);
  p.addEnum("direction", "Direction", {"CW", "CCW"}, 1);
}
BezierShape buildHelix(const ParamBlock& p) {
  float r1 = p.getFloat("radius1"), r2 = p.getFloat("radius2"), h = p.getFloat("height");
  float turns = p.getFloat("turns"), bias = p.getFloat("bias");
  float dir = p.getInt("direction") == 1 ? 1.0f : -1.0f;
  int n = std::max(2, static_cast<int>(std::ceil(turns * 8)) + 1);
  float expo = std::pow(2.0f, -bias * 2.0f);
  Spline s;
  for (int i = 0; i < n; ++i) {
    float t = static_cast<float>(i) / (n - 1);
    float a = dir * kTwoPi * turns * t;
    float r = r1 + (r2 - r1) * t;
    s.knots.push_back(Knot{Vec3(r * std::cos(a), r * std::sin(a), h * std::pow(t, expo)), {}, {}, KnotType::Smooth});
  }
  s.updateAutoHandles();
  return single(s);
}

std::vector<Vec2> eggPoints(float hx, float hy) {
  std::vector<Vec2> pts;
  const int n = 16;
  for (int i = 0; i < n; ++i) {
    float t = kTwoPi * i / n;
    float x = hx * std::sin(t) * (1.0f + 0.18f * std::cos(t));
    float y = -hy * std::cos(t);
    pts.emplace_back(x, y);
  }
  return pts;
}

void defEgg(ParamBlock& p) {
  p.addFloat("length", "Length", 100, 0.0, 1e7, 1.0);
  p.addFloat("width", "Width", 75, 0.0, 1e7, 1.0);
  p.addFloat("angle", "Angle", 0, -360, 360, 1.0, 1);
  p.addBool("outline", "Outline", true);
  p.addFloat("thickness", "Thickness", 5, 0.0, 1e7, 0.5);
}
BezierShape buildEgg(const ParamBlock& p) {
  auto egg = [](float hx, float hy) {
    Spline s = makePolylineSpline(eggPoints(hx, hy), true);
    for (auto& k : s.knots) k.type = KnotType::Smooth;
    s.updateAutoHandles();
    return s;
  };
  float hx = p.getFloat("width") * 0.5f, hy = p.getFloat("length") * 0.5f;
  BezierShape b = single(rotated(egg(hx, hy), p.getFloat("angle")));
  if (p.getBool("outline")) {
    float t = p.getFloat("thickness");
    b.splines.push_back(rotated(egg(std::max(0.01f, hx - t), std::max(0.01f, hy - t)), p.getFloat("angle")));
  }
  return b;
}

// Extended splines.
void defWalledRect(ParamBlock& p) {
  p.addFloat("length", "Length", 100, 0.0, 1e7, 1.0);
  p.addFloat("width", "Width", 100, 0.0, 1e7, 1.0);
  p.addFloat("thickness", "Thickness", 10, 0.0, 1e7, 0.5);
  p.addBool("syncFillets", "Sync Corner Fillets", true);
  p.addFloat("cornerRadius1", "Corner Radius 1", 0, 0.0, 1e7, 0.5);
  p.addFloat("cornerRadius2", "Corner Radius 2", 0, 0.0, 1e7, 0.5);
}
BezierShape buildWalledRect(const ParamBlock& p) {
  float w = p.getFloat("width"), l = p.getFloat("length"), t = p.getFloat("thickness");
  t = std::min(t, std::min(w, l) * 0.49f);
  float r1 = p.getFloat("cornerRadius1");
  float r2 = p.getBool("syncFillets") ? std::max(0.0f, r1 - t) : p.getFloat("cornerRadius2");
  BezierShape b = single(makeRectangleSpline(w, l, r1));
  b.splines.push_back(makeRectangleSpline(std::max(0.01f, w - 2 * t), std::max(0.01f, l - 2 * t), r2));
  return b;
}

void defProfile(ParamBlock& p) {
  p.addFloat("length", "Length", 100, 0.0, 1e7, 1.0);
  p.addFloat("width", "Width", 60, 0.0, 1e7, 1.0);
  p.addFloat("thickness", "Thickness", 10, 0.0, 1e7, 0.5);
  p.addBool("syncFillets", "Sync Corner Fillets", true);
  p.addFloat("cornerRadius1", "Corner Radius 1", 0, 0.0, 1e7, 0.5);
  p.addFloat("cornerRadius2", "Corner Radius 2", 0, 0.0, 1e7, 0.5);
}
void defTeeFlange(ParamBlock& p) {
  p.addFloat("length", "Length", 100, 0.0, 1e7, 1.0);
  p.addFloat("width", "Width", 80, 0.0, 1e7, 1.0);
  p.addFloat("thickness", "Thickness", 10, 0.0, 1e7, 0.5);
  p.addFloat("cornerRadius", "Corner Radius", 0, 0.0, 1e7, 0.5);
}
float clampThickness(const ParamBlock& p) {
  return std::max(0.01f, std::min(p.getFloat("thickness"), std::min(p.getFloat("width"), p.getFloat("length")) * 0.49f));
}
BezierShape buildChannel(const ParamBlock& p) {
  float t = clampThickness(p), r1 = p.getFloat("cornerRadius1");
  float r2 = p.getBool("syncFillets") ? std::max(0.0f, r1 - t) : p.getFloat("cornerRadius2");
  return single(makeFilletedPolygon(channelPts(p.getFloat("width"), p.getFloat("length"), t), {r1, 0, 0, r2, r2, 0, 0, r1}));
}
BezierShape buildAngle(const ParamBlock& p) {
  float t = clampThickness(p), r1 = p.getFloat("cornerRadius1");
  float r2 = p.getBool("syncFillets") ? std::max(0.0f, r1 - t) : p.getFloat("cornerRadius2");
  return single(makeFilletedPolygon(anglePts(p.getFloat("width"), p.getFloat("length"), t), {r1, 0, 0, r2, 0, 0}));
}
BezierShape buildTee(const ParamBlock& p) {
  float t = clampThickness(p), r = p.getFloat("cornerRadius");
  return single(makeFilletedPolygon(teePts(p.getFloat("width"), p.getFloat("length"), t), {0, 0, r, 0, 0, 0, 0, r}));
}
BezierShape buildFlange(const ParamBlock& p) {
  float t = clampThickness(p), r = p.getFloat("cornerRadius");
  return single(makeFilletedPolygon(flangePts(p.getFloat("width"), p.getFloat("length"), t),
                                    {0, 0, 0, r, r, 0, 0, 0, 0, r, r, 0}));
}

class TextShape : public ShapeObject {
 public:
  TextShape() {
    addShapeParams(params);
    params.beginGroup("Parameters");
    params.addString("font", "Font", defaultFontName());
    params.addBool("italic", "Italic", false);
    params.addBool("underline", "Underline", false);
    params.addEnum("align", "Alignment", {"Left", "Center", "Right", "Justify"}, 0);
    params.addFloat("size", "Size", 100, 0.0, 1e7, 1.0);
    params.addFloat("kerning", "Kerning", 0, -1e5, 1e5, 0.5);
    params.addFloat("leading", "Leading", 0, -1e5, 1e5, 0.5);
    params.addString("text", "Text", "Veraa Text");
  }
  std::string type() const override { return "Text"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<TextShape>(*this); }
  BezierShape buildShape() const override {
    return textToShape(params.getString("text"), params.getString("font"), params.getFloat("size"),
                       params.getFloat("kerning"), params.getFloat("leading"), params.getInt("align"),
                       params.getBool("italic"), params.getBool("underline"));
  }
};

}  // namespace

// ---------------------------------------------------------------------------------------------
// Section

SectionShape::SectionShape() {
  addShapeParams(params);
  params.beginGroup("Section Size");
  params.addFloat("length", "Length", 200, 0.0, 1e7, 1.0);
  params.addFloat("width", "Width", 200, 0.0, 1e7, 1.0);
  params.beginGroup("Section Extents");
  params.addEnum("extents", "Extents", {"Infinite", "Section Boundary"}, 0);
}

json SectionShape::toJson() const {
  json j = SceneObject::toJson();
  j["shape"] = bezierShapeToJson(shape);
  return j;
}

void SectionShape::fromJson(const json& j) {
  SceneObject::fromJson(j);
  shape = j.contains("shape") ? bezierShapeFromJson(j["shape"]) : BezierShape();
}

BezierShape sliceMeshes(const std::vector<std::pair<const TriMesh*, Mat4>>& meshes, const Mat4& planeWorld,
                        float limitHalfLength, float limitHalfWidth) {
  Mat4 toPlane = planeWorld.inverse();
  struct Seg {
    Vec3 a, b;
  };
  std::vector<Seg> segs;
  for (const auto& [mesh, world] : meshes) {
    if (!mesh) continue;
    Mat4 m = toPlane * world;
    std::vector<Vec3> P(mesh->positions.size());
    for (size_t i = 0; i < P.size(); ++i) P[i] = m.transformPoint(mesh->positions[i]);
    for (size_t t = 0; t + 2 < mesh->indices.size(); t += 3) {
      Vec3 v[3] = {P[mesh->indices[t]], P[mesh->indices[t + 1]], P[mesh->indices[t + 2]]};
      Vec3 hits[3];
      int nh = 0;
      for (int e = 0; e < 3; ++e) {
        const Vec3 &p = v[e], &q = v[(e + 1) % 3];
        bool sp = p.z >= 0.0f, sq = q.z >= 0.0f;
        if (sp == sq) continue;
        float f = p.z / (p.z - q.z);
        if (nh < 3) hits[nh++] = lerp(p, q, f);
      }
      if (nh == 2) {
        hits[0].z = hits[1].z = 0.0f;
        if (length2(hits[0] - hits[1]) > 1e-12f) segs.push_back({hits[0], hits[1]});
      }
    }
  }
  // Chain segments by shared endpoints (quantized).
  auto key = [](const Vec3& p) {
    long long x = std::llround(p.x * 1000.0), y = std::llround(p.y * 1000.0);
    return std::make_pair(x, y);
  };
  std::map<std::pair<long long, long long>, std::vector<int>> at;
  for (size_t i = 0; i < segs.size(); ++i) {
    at[key(segs[i].a)].push_back(static_cast<int>(i));
    at[key(segs[i].b)].push_back(static_cast<int>(i));
  }
  std::vector<bool> used(segs.size(), false);
  BezierShape out;
  for (size_t s0 = 0; s0 < segs.size(); ++s0) {
    if (used[s0]) continue;
    used[s0] = true;
    std::vector<Vec3> chain = {segs[s0].a, segs[s0].b};
    // Extend forward then backward.
    for (int dir = 0; dir < 2; ++dir) {
      for (;;) {
        Vec3 end = dir == 0 ? chain.back() : chain.front();
        int next = -1;
        Vec3 nextPt;
        for (int cand : at[key(end)]) {
          if (used[cand]) continue;
          next = cand;
          nextPt = key(segs[cand].a) == key(end) ? segs[cand].b : segs[cand].a;
          break;
        }
        if (next < 0) break;
        used[next] = true;
        if (dir == 0) chain.push_back(nextPt);
        else chain.insert(chain.begin(), nextPt);
      }
    }
    Spline sp;
    bool closed = chain.size() > 3 && key(chain.front()) == key(chain.back());
    if (closed) chain.pop_back();
    for (const auto& p : chain) {
      if (limitHalfLength > 0 && (std::fabs(p.x) > limitHalfWidth || std::fabs(p.y) > limitHalfLength)) continue;
      sp.knots.push_back(cornerKnot(p));
    }
    if (sp.knots.size() < 2) continue;
    sp.closed = closed && sp.knots.size() == chain.size();
    sp.updateAutoHandles();
    out.splines.push_back(std::move(sp));
  }
  return out;
}

// ---------------------------------------------------------------------------------------------
// Registration

void registerShapeTypes() {
  auto fn = [](const char* type, const char* name, const char* sub, DefineFn d, BuildFn b) {
    std::string t = type;
    registerObjectType({type, name, "Shapes", sub, [t, d, b] { return std::make_unique<FnShape>(t, d, b); }});
  };
  const char* splines = "Splines";
  registerObjectType({"Line", "Line", "Shapes", splines, [] {
                        auto o = std::make_unique<LineShape>();
                        return o;
                      }});
  fn("Rectangle", "Rectangle", splines, defRectangle, buildRectangle);
  fn("Circle", "Circle", splines, defCircle, buildCircle);
  fn("Ellipse", "Ellipse", splines, defEllipse, buildEllipse);
  fn("Arc", "Arc", splines, defArc, buildArc);
  fn("Donut", "Donut", splines, defDonut, buildDonut);
  fn("NGon", "NGon", splines, defNGon, buildNGon);
  fn("Star", "Star", splines, defStar, buildStar);
  registerObjectType({"Text", "Text", "Shapes", splines, [] { return std::make_unique<TextShape>(); }});
  fn("Helix", "Helix", splines, defHelix, buildHelix);
  fn("Egg", "Egg", splines, defEgg, buildEgg);
  registerObjectType({"Section", "Section", "Shapes", splines, [] { return std::make_unique<SectionShape>(); }});
  registerObjectType({"Freehand", "Freehand", "Shapes", splines, [] { return std::make_unique<FreehandShape>(); }});
  registerNurbsCurveTypes();
  const char* ext = "Extended Splines";
  fn("WalledRectangle", "Walled Rectangle", ext, defWalledRect, buildWalledRect);
  fn("Channel", "Channel", ext, defProfile, buildChannel);
  fn("AngleShape", "Angle", ext, defProfile, buildAngle);
  fn("Tee", "Tee", ext, defTeeFlange, buildTee);
  fn("WideFlange", "Wide Flange", ext, defTeeFlange, buildFlange);
}

}  // namespace veraa
