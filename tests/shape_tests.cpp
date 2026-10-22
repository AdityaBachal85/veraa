// Shape tests: capping with holes, spline booleans and edits, Extrude/Lathe/Sweep/Bevel volumes,
// text outlines, compound and AEC objects.
#include <cmath>
#include <cstdio>
#include <map>

#include "aec.h"
#include "compound.h"
#include "modifier.h"
#include "patch_nurbs.h"
#include "scene.h"
#include "shape2d.h"
#include "shape_mesh.h"
#include "shape_modifiers.h"
#include "shapes.h"
#include "spline_ops.h"

using namespace veraa;

static int g_failures = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
      ++g_failures;                                                          \
    }                                                                        \
  } while (0)
#define CHECK_NEAR(a, b, tol)                                                                          \
  do {                                                                                                 \
    double va_ = (a), vb_ = (b);                                                                       \
    if (!(std::fabs(va_ - vb_) <= (tol))) {                                                            \
      std::printf("FAIL %s:%d: %s = %g, expected %g (tol %g)\n", __FILE__, __LINE__, #a, va_, vb_, (double)(tol)); \
      ++g_failures;                                                                                    \
    }                                                                                                  \
  } while (0)

static double signedVolume(const PolyMesh& m) {
  TriMesh t = triangulate(m);
  double v = 0;
  for (size_t i = 0; i + 2 < t.indices.size(); i += 3) {
    const Vec3& a = t.positions[t.indices[i]];
    const Vec3& b = t.positions[t.indices[i + 1]];
    const Vec3& c = t.positions[t.indices[i + 2]];
    v += dot(a, cross(b, c)) / 6.0;
  }
  return v;
}

// Every edge used by exactly two faces, in opposite directions.
static bool isClosedManifold(const PolyMesh& m) {
  std::map<std::pair<int, int>, int> directed;
  for (const auto& f : m.faces)
    for (size_t k = 0; k < f.v.size(); ++k) directed[{f.v[k], f.v[(k + 1) % f.v.size()]}]++;
  for (const auto& [e, n] : directed) {
    if (n != 1) return false;
    auto it = directed.find({e.second, e.first});
    if (it == directed.end() || it->second != 1) return false;
  }
  return !m.faces.empty();
}

static double shapeArea(const BezierShape& s, int steps = 16) {
  double a = 0;
  for (const auto& sp : s.splines) {
    if (!sp.closed) continue;
    shape2d::Polygon poly;
    for (const auto& p : sp.polyline(steps, true)) poly.emplace_back(p.x, p.y);
    a += shape2d::signedArea(poly);
  }
  return a;
}

static BezierShape single(const Spline& s) {
  BezierShape b;
  b.splines.push_back(s);
  return b;
}

static PolyMesh applyMod(const std::string& type, const BezierShape& shape, std::map<std::string, double> values,
                         int steps = 6) {
  auto mod = createModifier(type);
  for (const auto& [k, v] : values) mod->params.setNumber(k, v);
  GeomState st;
  st.shape = shape;
  st.isShape = true;
  st.shapeSteps = steps;
  mod->applyState(st);
  return st.isShape ? PolyMesh() : st.mesh;
}

static void testRegistration() {
  for (const char* t : {"Line", "Rectangle", "Circle", "Ellipse", "Arc", "Donut", "NGon", "Star", "Text", "Helix", "Egg",
                        "Section", "Freehand", "WalledRectangle", "Channel", "AngleShape", "Tee", "WideFlange"}) {
    auto o = createObject(t);
    CHECK(o != nullptr);
    if (!o) continue;
    CHECK(o->kind() == ObjectKind::Shape);
    auto* s = dynamic_cast<ShapeObject*>(o.get());
    if (std::string(t) != "Line" && std::string(t) != "Freehand" && std::string(t) != "Section") {
      BezierShape b = s->buildShape();
      if (b.empty()) std::printf("  %s builds an empty shape\n", t);
      CHECK(!b.empty());
    }
  }
  auto splines = objectTypesIn("Shapes", "Splines");
  CHECK(splines.size() == 13);
  if (splines.size() == 13) {
    CHECK(splines[0]->type == "Line");
    CHECK(splines[12]->type == "Freehand");
  }
  CHECK(objectTypesIn("Shapes", "Extended Splines").size() == 5);
  for (const char* m : {"Extrude", "Lathe", "Sweep", "Bevel", "BevelProfile", "EditSpline", "NormalizeSpline",
                        "FilletChamfer", "TrimExtend", "RenderableSpline"}) {
    const ModifierTypeInfo* info = findModifierType(m);
    CHECK(info != nullptr);
    if (info) CHECK(info->acceptsShapes);
  }
}

static void testCappingWithHoles() {
  // Rectangle 100 x 60 with two 10 x 10 holes, extruded 5.
  BezierShape s = single(makeRectangleSpline(100, 60, 0));
  s.splines.push_back(makeRectangleSpline(10, 10, 0, Vec2(-20, 0)));
  s.splines.push_back(makeRectangleSpline(10, 10, 0, Vec2(25, 5)));
  PolyMesh m = applyMod("Extrude", s, {{"amount", 5}});
  CHECK(isClosedManifold(m));
  CHECK_NEAR(signedVolume(m), (6000.0 - 200.0) * 5.0, 1e-2);
  // Holes inside holes (an island inside a hole) also cap correctly.
  s.splines.push_back(makeRectangleSpline(4, 4, 0, Vec2(-20, 0)));
  m = applyMod("Extrude", s, {{"amount", 5}});
  CHECK(isClosedManifold(m));
  CHECK_NEAR(signedVolume(m), (6000.0 - 200.0 + 16.0) * 5.0, 1e-2);
  // Donut: two circles.
  auto donut = createObject("Donut");
  donut->params.setNumber("radius1", 50);
  donut->params.setNumber("radius2", 30);
  BezierShape d = static_cast<ShapeObject*>(donut.get())->buildShape();
  CHECK(d.splines.size() == 2);
  m = applyMod("Extrude", d, {{"amount", 10}}, 16);
  CHECK(isClosedManifold(m));
  CHECK_NEAR(signedVolume(m), kPi * (2500.0 - 900.0) * 10.0, kPi * 1600.0 * 10.0 * 0.01);
  // Earcut directly: a square with a square hole -> 8 triangles.
  shape2d::Region r;
  r.outer = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};
  r.holes.push_back({{3, 3}, {3, 7}, {7, 7}, {7, 3}});
  CHECK(shape2d::triangulate(r).size() == 8 * 3);
}

static void testExtrude() {
  BezierShape rect = single(makeRectangleSpline(40, 20, 0));
  PolyMesh m = applyMod("Extrude", rect, {{"amount", 10}, {"segments", 3}});
  CHECK(isClosedManifold(m));
  CHECK_NEAR(signedVolume(m), 8000.0, 1e-2);
  // Material IDs: caps 0 and 1, sides 2.
  std::map<int, int> ids;
  for (const auto& f : m.faces) ids[f.matId]++;
  CHECK(ids[0] == 1 && ids[1] == 1 && ids[2] == 12);
  // Downward extrusion still faces outward.
  m = applyMod("Extrude", rect, {{"amount", -10}});
  CHECK(isClosedManifold(m));
  CHECK_NEAR(signedVolume(m), 8000.0, 1e-2);
  // Grid capping adds interior vertices and stays closed.
  PolyMesh g = applyMod("Extrude", rect, {{"amount", 10}, {"capping", 1}, {"gridSize", 5}});
  CHECK(isClosedManifold(g));
  CHECK_NEAR(signedVolume(g), 8000.0, 1e-1);
  CHECK(g.positions.size() > 16);
  // Rounded rectangle with grid capping (curved boundary).
  g = applyMod("Extrude", single(makeRectangleSpline(40, 20, 5)), {{"amount", 10}, {"capping", 1}, {"gridSize", 4}});
  CHECK(isClosedManifold(g));
  // Open splines extrude to walls only.
  m = applyMod("Extrude", single(makePolylineSpline({{0, 0}, {10, 0}, {10, 10}}, false)), {{"amount", 5}});
  CHECK(m.faces.size() == 2);
  // Through the scene: a Rectangle node with Extrude gives a mesh result.
  Scene scene;
  Node* n = scene.createNode("Rectangle");
  n->object->params.setNumber("length", 20);
  n->object->params.setNumber("width", 40);
  n->modifiers.push_back(createModifier("Extrude"));
  n->modifiers.back()->params.setNumber("amount", 10);
  n->invalidate();
  CHECK(!n->isShapeResult());
  CHECK_NEAR(signedVolume(n->mesh()), 8000.0, 1e-2);
}

static void testLathe() {
  // Profile: rectangle x in [0, 10], y in [0, 20] around Y at Min -> cylinder.
  BezierShape prof = single(makePolylineSpline({{0, 0}, {10, 0}, {10, 20}, {0, 20}}, true));
  PolyMesh m = applyMod("Lathe", prof, {{"segments", 16}});
  CHECK(isClosedManifold(m));
  double expected = 0.5 * 16 * 100.0 * std::sin(2 * kPi / 16) * 20.0;
  CHECK_NEAR(signedVolume(m), expected, expected * 1e-3);
  // Half revolution of an offset profile: closed with caps.
  BezierShape ring = single(makePolylineSpline({{5, 0}, {10, 0}, {10, 20}, {5, 20}}, true));
  auto lathe = createModifier("Lathe");
  lathe->params.setNumber("degrees", 180);
  lathe->params.setNumber("segments", 32);
  lathe->params.setNumber("align", 1);  // Center: the axis through x = 7.5 would cut the profile
  lathe->params.setNumber("align", 0);  // Min: axis at x = 5 (profile touches the axis)
  GeomState st;
  st.shape = ring;
  st.isShape = true;
  lathe->applyState(st);
  CHECK(isClosedManifold(st.mesh));
  CHECK(signedVolume(st.mesh) > 0);
  // Open profile (vase wall) produces a surface, no crash, outward normals.
  BezierShape vase = single(makePolylineSpline({{0, 0}, {8, 0}, {10, 10}, {6, 20}}, false));
  m = applyMod("Lathe", vase, {{"segments", 12}});
  CHECK(!m.faces.empty());
  CHECK(m.isValid());
}

static void testSweep() {
  // Bar 10 x 10 along a 100-long line.
  BezierShape path = single(makePolylineSpline({{0, 0}, {100, 0}}, false));
  PolyMesh m = applyMod("Sweep", path, {{"section", 0}, {"length", 10}, {"width", 10}});
  CHECK(isClosedManifold(m));
  CHECK_NEAR(signedVolume(m), 10000.0, 1.0);
  // L-shaped path with a mitered corner: volume = section area x centerline length.
  BezierShape lpath = single(makePolylineSpline({{0, 0}, {100, 0}, {100, 100}}, false));
  m = applyMod("Sweep", lpath, {{"section", 0}, {"length", 10}, {"width", 10}});
  CHECK(isClosedManifold(m));
  CHECK_NEAR(signedVolume(m), 20000.0, 2.0);
  // Pipe section (with a hole) along a closed rectangle path: closed torus-like manifold.
  BezierShape loop = single(makeRectangleSpline(200, 100, 0));
  m = applyMod("Sweep", loop, {{"section", 5}, {"width", 10}, {"thickness", 2}});
  CHECK(isClosedManifold(m));
  CHECK(signedVolume(m) > 0);
  // Mirrored section still faces outward.
  m = applyMod("Sweep", path, {{"section", 1}, {"mirrorXY", 1}});
  CHECK(isClosedManifold(m));
  CHECK(signedVolume(m) > 0);
  // Custom section survives a JSON round trip.
  SweepModifier sw;
  sw.customSection = single(makeCircleSpline(3));
  sw.params.setNumber("section", SweepModifier::customSectionIndex());
  auto copy = createModifier("Sweep");
  copy->fromJson(sw.toJson());
  GeomState st;
  st.shape = path;
  st.isShape = true;
  copy->applyState(st);
  CHECK(!st.isShape);
  CHECK(isClosedManifold(st.mesh));
}

static void testBevel() {
  BezierShape rect = single(makeRectangleSpline(40, 20, 0));
  // One level, no outline: same as Extrude.
  PolyMesh m = applyMod("Bevel", rect, {{"level1Height", 10}});
  CHECK(isClosedManifold(m));
  CHECK_NEAR(signedVolume(m), 8000.0, 1e-2);
  // Three levels with outlines (a truncated pyramid on a block).
  m = applyMod("Bevel", rect, {{"level1Height", 10},
                               {"level2", 1},
                               {"level2Height", 2},
                               {"level2Outline", -2},
                               {"level3", 1},
                               {"level3Height", 3},
                               {"level3Outline", -3},
                               {"segments", 2},
                               {"sides", 1}});
  CHECK(isClosedManifold(m));
  CHECK(signedVolume(m) > 8000.0);
  // Bevel Profile with a picked profile: a straight vertical profile = extrusion.
  BevelProfileModifier bp;
  bp.profile = single(makePolylineSpline({{0, 0}, {0, 10}}, false));
  GeomState st;
  st.shape = rect;
  st.isShape = true;
  bp.applyState(st);
  CHECK(isClosedManifold(st.mesh));
  CHECK_NEAR(signedVolume(st.mesh), 8000.0, 1e-2);
}

static void testSplineOps() {
  // Booleans on two overlapping squares.
  auto twoSquares = [] {
    BezierShape s = single(makeRectangleSpline(10, 10, 0));
    s.splines.push_back(makeRectangleSpline(10, 10, 0, Vec2(5, 0)));
    return s;
  };
  BezierShape u = twoSquares();
  CHECK(splineops::booleanSplines(u, 0, {1}, shape2d::BoolOp::Union));
  CHECK(u.splines.size() == 1);
  CHECK_NEAR(shapeArea(u), 150.0, 1e-2);
  BezierShape d = twoSquares();
  CHECK(splineops::booleanSplines(d, 0, {1}, shape2d::BoolOp::Subtract));
  CHECK_NEAR(shapeArea(d), 50.0, 1e-2);
  BezierShape i = twoSquares();
  CHECK(splineops::booleanSplines(i, 0, {1}, shape2d::BoolOp::Intersect));
  CHECK_NEAR(shapeArea(i), 50.0, 1e-2);
  // Subtracting a square from the middle leaves a hole (two splines).
  BezierShape h = single(makeRectangleSpline(30, 30, 0));
  h.splines.push_back(makeRectangleSpline(10, 10, 0));
  CHECK(splineops::booleanSplines(h, 0, {1}, shape2d::BoolOp::Subtract));
  CHECK(h.splines.size() == 2);
  CHECK_NEAR(std::fabs(shapeArea(h)), 800.0, 1e-2);

  // Refine keeps the curve.
  BezierShape c = single(makeCircleSpline(50));
  Vec3 before = c.splines[0].evaluate(1, 0.3f);
  splineops::KnotRef k = splineops::refine(c, {0, 1}, 0.5f);
  CHECK(c.splines[0].knots.size() == 5);
  CHECK(k.knot == 2);
  CHECK(length(c.splines[0].evaluate(1, 0.6f) - before) < 1e-2f);

  // Fillet all corners of a square: area 10000 - (4 - pi) r^2.
  BezierShape f = splineops::filletAll(single(makeRectangleSpline(100, 100, 0)), 10, false);
  CHECK_NEAR(shapeArea(f), 10000.0 - (4.0 - kPi) * 100.0, 5.0);
  BezierShape ch = splineops::filletAll(single(makeRectangleSpline(100, 100, 0)), 10, true);
  CHECK_NEAR(shapeArea(ch), 10000.0 - 4 * 50.0, 1e-1);

  // Outline of a closed square adds an offset copy (outward for positive widths, inward for
  // negative ones).
  BezierShape o = single(makeRectangleSpline(100, 100, 0));
  splineops::outline(o, {0}, 10, false);
  CHECK(o.splines.size() == 2);
  CHECK_NEAR(std::fabs(shapeArea(single(o.splines[1]))), 120.0 * 120.0, 1.0);
  BezierShape oi = single(makeRectangleSpline(100, 100, 0));
  splineops::outline(oi, {0}, -10, false);
  CHECK_NEAR(std::fabs(shapeArea(single(oi.splines[1]))), 80.0 * 80.0, 1.0);
  // Outline of an open line gives a closed outline.
  BezierShape ol = single(makePolylineSpline({{0, 0}, {100, 0}}, false));
  splineops::outline(ol, {0}, 10, true);
  CHECK(ol.splines.size() == 1 && ol.splines[0].closed);
  CHECK_NEAR(std::fabs(shapeArea(ol)), 1000.0, 1.0);

  // Weld two open lines sharing an end into one.
  BezierShape w = single(makePolylineSpline({{0, 0}, {10, 0}}, false));
  w.splines.push_back(makePolylineSpline({{10.05f, 0}, {10, 10}}, false));
  int welds = splineops::weld(w, {{0, 1}, {1, 0}}, 0.1f);
  CHECK(welds == 1);
  CHECK(w.splines.size() == 1 && w.splines[0].knots.size() == 3);
  // Weld the two ends of a U into a closed triangle.
  BezierShape tri = single(makePolylineSpline({{0, 0}, {10, 0}, {5, 8}, {0.01f, 0}}, false));
  splineops::weld(tri, {{0, 0}, {0, 3}}, 0.1f);
  CHECK(tri.splines[0].closed && tri.splines[0].knots.size() == 3);

  // Break then delete.
  BezierShape b = single(makeRectangleSpline(10, 10, 0));
  splineops::breakKnots(b, {{0, 0}});
  CHECK(!b.splines[0].closed);
  CHECK(b.splines[0].knots.size() == 5);
  splineops::deleteKnots(b, {{0, 4}});
  CHECK(b.splines[0].knots.size() == 4);

  // Divide segments and delete a segment.
  BezierShape dv = single(makeRectangleSpline(10, 10, 0));
  splineops::divideSegments(dv, {{0, 0}}, 3);
  CHECK(dv.splines[0].knots.size() == 7);
  splineops::deleteSegments(dv, {{0, 0}});
  CHECK(!dv.splines[0].closed);

  // Cross insert at the crossing of two lines.
  BezierShape x = single(makePolylineSpline({{-10, 0}, {10, 0}}, false));
  x.splines.push_back(makePolylineSpline({{0, -10}, {0, 10}}, false));
  CHECK(splineops::crossInsert(x, 0, 1) == 1);
  CHECK(x.splines[0].knots.size() == 3 && x.splines[1].knots.size() == 3);

  // Trim the middle piece of a line crossed by two others.
  BezierShape t = single(makePolylineSpline({{-30, 0}, {30, 0}}, false));
  t.splines.push_back(makePolylineSpline({{-10, -10}, {-10, 10}}, false));
  t.splines.push_back(makePolylineSpline({{10, -10}, {10, 10}}, false));
  CHECK(splineops::trim(t, 0, Vec3(0, 0, 0)));
  CHECK(t.splines.size() == 4);
  // Extend a line to meet a wall.
  BezierShape e = single(makePolylineSpline({{0, 0}, {5, 0}}, false));
  e.splines.push_back(makePolylineSpline({{20, -10}, {20, 10}}, false));
  CHECK(splineops::extend(e, 0, Vec3(5, 0, 0)));
  CHECK(std::fabs(e.splines[0].knots.back().p.x - 20.0f) < 1e-2f);

  // Normalize: knots spaced along the curve.
  BezierShape n = splineops::normalized(single(makeCircleSpline(50)), 10);
  int kc = static_cast<int>(n.splines[0].knots.size());
  CHECK(kc >= 30 && kc <= 33);

  // Mirror, reverse, make first, close.
  BezierShape mr = single(makePolylineSpline({{0, 0}, {10, 0}, {10, 5}}, false));
  splineops::reverse(mr, {0});
  CHECK(mr.splines[0].knots[0].p.y == 5.0f);
  splineops::close(mr, {0});
  CHECK(mr.splines[0].closed);
  splineops::mirror(mr, {0}, 0, true);
  CHECK(mr.splines.size() == 2);
}

static void testText() {
  std::string font = defaultFontName();
  CHECK(!availableFonts().empty());
  BezierShape o = textToShape("O", font, 100, 0, 0, 0);
  CHECK(o.splines.size() == 2);
  BezierShape bb = textToShape("B", font, 100, 0, 0, 0);
  CHECK(bb.splines.size() == 3);
  BezierShape hello = textToShape("Hello", font, 100, 0, 0, 0);
  CHECK(hello.splines.size() == 7);
  // Two lines are taller than one.
  BezierShape two = textToShape("A\nA", font, 100, 0, 0, 0);
  CHECK(two.splines.size() == textToShape("AA", font, 100, 0, 0, 0).splines.size());
  CHECK(two.bounds().extent().y > textToShape("A", font, 100, 0, 0, 0).bounds().extent().y * 1.5f);
  // Text node exists and builds.
  auto text = createObject("Text");
  text->params.setString("text", "Veraa");
  CHECK(!static_cast<ShapeObject*>(text.get())->buildShape().empty());
  // TextPlus: closed solid letters.
  auto tp = createObject("TextPlus");
  CHECK(tp != nullptr);
  if (tp) {
    tp->params.setString("text", "O");
    PolyMesh m = static_cast<GeometryObject*>(tp.get())->build();
    CHECK(isClosedManifold(m));
    CHECK(signedVolume(m) > 0);
    tp->params.setNumber("applyBevel", 1);
    tp->params.setNumber("bevelProfile", 1);
    m = static_cast<GeometryObject*>(tp.get())->build();
    CHECK(isClosedManifold(m));
    CHECK(signedVolume(m) > 0);
  }
}

static void testShapesAndSections() {
  // Every section profile is a valid closed shape that sweeps into a closed solid.
  BezierShape path = single(makePolylineSpline({{0, 0}, {50, 0}, {50, 50}}, false));
  for (int i = 0; i < static_cast<int>(sectionProfileNames().size()); ++i) {
    PolyMesh m = applyMod("Sweep", path, {{"section", i}, {"length", 12}, {"width", 10}, {"thickness", 1}});
    bool ok = isClosedManifold(m) && signedVolume(m) > 0;
    if (!ok) std::printf("  section %s did not sweep into a closed solid\n", sectionProfileNames()[i].c_str());
    CHECK(ok);
  }
  // Extended splines extrude into closed solids.
  for (const char* t : {"WalledRectangle", "Channel", "AngleShape", "Tee", "WideFlange", "Star", "NGon", "Egg", "Ellipse"}) {
    auto o = createObject(t);
    BezierShape s = static_cast<ShapeObject*>(o.get())->buildShape();
    PolyMesh m = applyMod("Extrude", s, {{"amount", 10}});
    bool ok = isClosedManifold(m) && signedVolume(m) > 0;
    if (!ok) std::printf("  %s did not extrude into a closed solid\n", t);
    CHECK(ok);
  }
  // Section slicing a box gives one closed loop.
  auto box = createObject("Box");
  TriMesh t = triangulate(static_cast<GeometryObject*>(box.get())->build());
  BezierShape sec = sliceMeshes({{&t, Mat4::identity()}}, Mat4::translate(Vec3(0, 0, 5)));
  CHECK(sec.splines.size() == 1);
  if (!sec.splines.empty()) CHECK(sec.splines[0].closed);
  // Edit Spline round trip.
  EditSplineModifier es;
  es.hasEdits = true;
  es.shape = single(makeCircleSpline(5));
  auto es2 = createModifier("EditSpline");
  es2->fromJson(es.toJson());
  GeomState st;
  st.shape = single(makeRectangleSpline(10, 10, 0));
  st.isShape = true;
  es2->applyState(st);
  CHECK(st.shape.splines.size() == 1 && st.shape.splines[0].knots.size() == 4 &&
        std::fabs(st.shape.bounds().extent().x - 10.0f) < 1e-3f);
  // Renderable Spline turns a shape into a mesh.
  PolyMesh r = applyMod("RenderableSpline", single(makeCircleSpline(20)), {{"thickness", 2}});
  CHECK(!r.faces.empty());
  PolyMesh rr = applyMod("RenderableSpline", path, {{"mode", 1}});
  CHECK(isClosedManifold(rr));
}

static CompoundOperand meshOperand(const std::string& role, PolyMesh mesh) {
  CompoundOperand op;
  op.role = role;
  op.mesh = std::move(mesh);
  return op;
}

static CompoundOperand shapeOperand(const std::string& role, BezierShape shape, float percent = 0) {
  CompoundOperand op;
  op.role = role;
  op.isShape = true;
  op.shape = std::move(shape);
  op.percent = percent;
  return op;
}

static PolyMesh boxMesh(float size) {
  auto box = createObject("Box");
  box->params.setNumber("length", size);
  box->params.setNumber("width", size);
  box->params.setNumber("height", size);
  return static_cast<GeometryObject*>(box.get())->build();
}

static void testCompound() {
  auto types = objectTypesIn("Geometry", "Compound Objects");
  CHECK(types.size() >= 7);
  // Loft: a 10 x 10 square along a 100-long line = a 10000 box.
  auto loftObj = createObject("Loft");
  auto* loft = dynamic_cast<CompoundObject*>(loftObj.get());
  CHECK(loft != nullptr);
  if (loft) {
    loft->setOperand(shapeOperand("path", single(makePolylineSpline({{0, 0}, {100, 0}}, false))));
    loft->setOperand(shapeOperand("shape", single(makeRectangleSpline(10, 10, 0)), 0));
    PolyMesh m = loft->build();
    CHECK(isClosedManifold(m));
    CHECK_NEAR(signedVolume(m), 10000.0, 1.0);
    // Second shape at 100%: a frustum from a 10 square to a 20 square (same point counts).
    loft->setOperand(shapeOperand("shape", single(makeRectangleSpline(20, 20, 0)), 100));
    loft->params.setNumber("pathSteps", 20);
    m = loft->build();
    CHECK(isClosedManifold(m));
    CHECK_NEAR(signedVolume(m), 100.0 / 3.0 * (100 + 400 + 200), 300.0);
    // Shapes with different point counts are resampled to match (circle to square).
    loft->operands.pop_back();
    loft->setOperand(shapeOperand("shape", single(makeCircleSpline(10)), 100));
    m = loft->build();
    CHECK(isClosedManifold(m));
    // JSON round trip keeps the operands.
    auto copy = createObject("Loft");
    copy->fromJson(loft->toJson());
    CHECK(dynamic_cast<CompoundObject*>(copy.get())->operands.size() == 3);
  }
  // Scatter: 25 copies of a small box over a plane, distribution hidden.
  auto sc = createObject("Scatter");
  auto* scatter = dynamic_cast<CompoundObject*>(sc.get());
  scatter->setOperand(meshOperand("source", boxMesh(2)));
  auto plane = createObject("Plane");
  scatter->setOperand(meshOperand("distribution", static_cast<GeometryObject*>(plane.get())->build()));
  sc->params.setNumber("duplicates", 25);
  sc->params.setNumber("hideDistribution", 1);
  PolyMesh sm = scatter->build();
  CHECK(sm.faces.size() == 25 * boxMesh(2).faces.size());
  Aabb pb = static_cast<GeometryObject*>(plane.get())->build().bounds();
  CHECK(sm.bounds().lo.x >= pb.lo.x - 3 && sm.bounds().hi.x <= pb.hi.x + 3);
  for (int mode = 0; mode < 8; ++mode) {
    sc->params.setNumber("distribution", mode);
    CHECK(!scatter->build().faces.empty());
  }
  // Volume needs a closed distribution object: copies land inside the box.
  sc->params.setNumber("distribution", 8);
  scatter->setOperand(meshOperand("distribution", boxMesh(50)));
  PolyMesh vol = scatter->build();
  CHECK(vol.faces.size() == 25 * boxMesh(2).faces.size());
  CHECK(vol.bounds().hi.z <= 50 + 2.0f && vol.bounds().lo.z >= -2.0f);
  // Connect: two open-ended tubes facing each other are bridged into one closed surface.
  auto tube = [](float z0) {
    PolyMesh t;
    int n = 12;
    for (int r = 0; r < 2; ++r)
      for (int k = 0; k < n; ++k)
        t.addVertex(Vec3(10 * std::cos(kTwoPi * k / n), 10 * std::sin(kTwoPi * k / n), z0 + r * 20));
    for (int k = 0; k < n; ++k) t.addFace({k, (k + 1) % n, n + (k + 1) % n, n + k});
    std::vector<int> cap;
    if (z0 < 0) {  // closed bottom, open top
      for (int k = n - 1; k >= 0; --k) cap.push_back(k);
    } else {  // open bottom, closed top
      for (int k = 0; k < n; ++k) cap.push_back(n + k);
    }
    t.addFace(cap);
    return t;
  };
  auto cn = createObject("Connect");
  auto* connect = dynamic_cast<CompoundObject*>(cn.get());
  connect->setOperand(meshOperand("operand", tube(-60)));
  connect->setOperand(meshOperand("operand", tube(20)));
  cn->params.setNumber("segments", 4);
  PolyMesh cm = connect->build();
  CHECK(isClosedManifold(cm));
  CHECK(signedVolume(cm) > 0);
  // ShapeMerge: cookie-cut a 20 x 20 square out of a 100 x 100 plane.
  auto smo = createObject("ShapeMerge");
  auto* merge = dynamic_cast<CompoundObject*>(smo.get());
  PolyMesh planeMesh = static_cast<GeometryObject*>(plane.get())->build();
  merge->setOperand(meshOperand("mesh", planeMesh));
  BezierShape cut = single(makeRectangleSpline(20, 20, 0));
  cut.transform(Mat4::translate(Vec3(0, 0, 10)));
  merge->setOperand(shapeOperand("shape", cut));
  auto area = [](const PolyMesh& m) {
    double a = 0;
    for (const auto& f : m.faces) a += length(m.faceNormal(f)) * 0.5;  // Newell normal length = 2 x area
    return a;
  };
  smo->params.setNumber("operation", 0);  // Cookie Cutter
  PolyMesh cutMesh = merge->build();
  CHECK_NEAR(area(cutMesh), area(planeMesh) - 400.0, 1.0);
  smo->params.setNumber("invert", 1);
  CHECK_NEAR(area(merge->build()), 400.0, 1.0);
  smo->params.setNumber("operation", 1);  // Merge: keeps everything, inside gets the material ID
  smo->params.setNumber("invert", 0);
  smo->params.setNumber("setMatId", 1);
  smo->params.setNumber("matId", 5);
  PolyMesh merged = merge->build();
  CHECK_NEAR(area(merged), area(planeMesh), 1.0);
  double insideArea = 0;
  for (const auto& f : merged.faces)
    if (f.matId == 4) insideArea += length(merged.faceNormal(f)) * 0.5;
  CHECK_NEAR(insideArea, 400.0, 1.0);
  // Terrain from two contours: surface and solid.
  auto te = createObject("Terrain");
  auto* terrain = dynamic_cast<CompoundObject*>(te.get());
  BezierShape c1 = single(makeCircleSpline(100));
  BezierShape c2 = single(makeCircleSpline(50));
  c2.transform(Mat4::translate(Vec3(0, 0, 30)));
  terrain->setOperand(shapeOperand("contour", c1));
  terrain->setOperand(shapeOperand("contour", c2));
  PolyMesh tm = terrain->build();
  CHECK(!tm.faces.empty());
  CHECK(std::fabs(tm.bounds().hi.z - 30.0f) < 1e-3f);
  te->params.setNumber("form", 1);
  PolyMesh solid = terrain->build();
  CHECK(isClosedManifold(solid));
  CHECK(signedVolume(solid) > 0);
  te->params.setNumber("form", 2);
  CHECK(!terrain->build().faces.empty());
  // Conform: a plane above a box drops onto it.
  auto co = createObject("Conform");
  auto* conform = dynamic_cast<CompoundObject*>(co.get());
  PolyMesh sheet = planeMesh;
  sheet.transform(Mat4::translate(Vec3(0, 0, 100)) * Mat4::scale(Vec3(0.1f, 0.1f, 1)));
  conform->setOperand(meshOperand("wrapper", sheet));
  conform->setOperand(meshOperand("target", boxMesh(40)));
  co->params.setNumber("standoff", 1);
  co->params.setNumber("hideTarget", 1);
  PolyMesh wrapped = conform->build();
  CHECK(std::fabs(wrapped.bounds().hi.z - 41.0f) < 0.05f);
  // BlobMesh: a single blob is a closed sphere of about the given size.
  auto bl = createObject("BlobMesh");
  auto* blob = dynamic_cast<CompoundObject*>(bl.get());
  PolyMesh pt;
  pt.addVertex(Vec3(0, 0, 0));
  blob->setOperand(meshOperand("source", pt));
  bl->params.setNumber("size", 20);
  bl->params.setNumber("coarseness", 2);
  PolyMesh bm = blob->build();
  CHECK(isClosedManifold(bm));
  double sphere = 4.0 / 3.0 * kPi * 8000.0;
  CHECK_NEAR(signedVolume(bm), sphere, sphere * 0.05);
  // Two nearby blobs merge into one surface.
  PolyMesh two;
  two.addVertex(Vec3(-12, 0, 0));
  two.addVertex(Vec3(12, 0, 0));
  blob->operands.clear();
  blob->setOperand(meshOperand("source", two));
  PolyMesh bm2 = blob->build();
  CHECK(isClosedManifold(bm2));
  CHECK(boundaryLoops(bm2).empty());
  CHECK(metaballMesh({Vec3()}, 10, 1, 1).faces.size() > 100);
}

static void testAec() {
  for (const char* sub : {"Doors", "Windows", "AEC Extended", "Stairs"}) {
    auto types = objectTypesIn("Geometry", sub);
    CHECK(!types.empty());
    for (const auto* t : types) {
      auto o = createObject(t->type);
      PolyMesh m = static_cast<GeometryObject*>(o.get())->build();
      bool ok = !m.faces.empty() && m.isValid() && !triangulate(m).empty();
      if (!ok) std::printf("  %s builds no valid mesh\n", t->type.c_str());
      CHECK(ok);
    }
  }
  CHECK(objectTypesIn("Geometry", "Doors").size() == 3);
  CHECK(objectTypesIn("Geometry", "Windows").size() == 6);
  CHECK(objectTypesIn("Geometry", "Stairs").size() == 4);
  // Wall: straight 300 x 20 x 250 is a closed box with its material IDs.
  WallObject wall;
  wall.points = {Vec2(0, 0), Vec2(300, 0)};
  PolyMesh wm = wall.build();
  CHECK(isClosedManifold(wm));
  CHECK_NEAR(signedVolume(wm), 300.0 * 20.0 * 250.0, 1.0);
  std::map<int, int> ids;
  for (const auto& f : wm.faces) ids[f.matId]++;
  CHECK(ids[0] == 2 && ids[1] == 1 && ids[2] == 1 && ids[3] == 1 && ids[4] == 1);
  // An L-shaped wall keeps its thickness at the corner (mitered).
  wall.points = {Vec2(0, 0), Vec2(300, 0), Vec2(300, 200)};
  wm = wall.build();
  CHECK(isClosedManifold(wm));
  CHECK_NEAR(signedVolume(wm), (300.0 + 200.0) * 20.0 * 250.0, 10.0);
  // A closed room is a ring with outside and inside faces.
  wall.points = {Vec2(0, 0), Vec2(400, 0), Vec2(400, 300), Vec2(0, 300)};
  wall.closed = true;
  wm = wall.build();
  CHECK(isClosedManifold(wm));
  CHECK_NEAR(signedVolume(wm), (420.0 * 320.0 - 380.0 * 280.0) * 250.0, 10.0);
  auto wall2 = createObject("Wall");
  wall2->fromJson(wall.toJson());
  CHECK(static_cast<WallObject*>(wall2.get())->points.size() == 4);
  // Door: frame, leaf and panel material IDs all present; opening changes the bounds.
  auto door = createObject("PivotDoor");
  PolyMesh dm = static_cast<GeometryObject*>(door.get())->build();
  std::map<int, int> did;
  for (const auto& f : dm.faces) did[f.matId]++;
  CHECK(did.count(0) && did.count(1) && did.count(2) && did.count(3) && did.count(4));
  Aabb closedBox = dm.bounds();
  door->params.setNumber("open", 90);
  Aabb openBox = static_cast<GeometryObject*>(door.get())->build().bounds();
  CHECK(openBox.lo.y < closedBox.lo.y - 40);
  // Window glass gets its own ID.
  auto win = createObject("CasementWindow");
  bool glass = false;
  for (const auto& f : static_cast<GeometryObject*>(win.get())->build().faces) glass = glass || f.matId == 2;
  CHECK(glass);
  // Straight box stair: a closed solid whose top reaches the overall height.
  auto stair = createObject("StraightStair");
  stair->params.setNumber("stairType", 2);
  PolyMesh st = static_cast<GeometryObject*>(stair.get())->build();
  CHECK(isClosedManifold(st));
  CHECK(std::fabs(st.bounds().hi.z - 300.0f) < 1e-2f);
  // The solid's volume is the sum of the steps: step j is g deep, (j + 1) risers tall, 100 wide.
  double g = 300.0 / 16, r = 300.0 / 16, vol = 0;
  for (int j = 0; j < 16; ++j) vol += g * r * (j + 1) * 100.0;
  CHECK_NEAR(signedVolume(st), vol, 1.0);
  // Railing along a picked path follows it.
  RailingObject rail;
  rail.path = single(makePolylineSpline({{0, 0}, {200, 0}, {200, 150}}, false));
  PolyMesh rm = rail.build();
  CHECK(!rm.faces.empty());
  CHECK(rm.bounds().hi.y > 140);
  // Spiral stair with rails, stringers and pole.
  auto sp = createObject("SpiralStair");
  for (const char* k : {"railLeft", "railRight", "stringers", "carriage"}) sp->params.setNumber(k, 1);
  sp->params.setNumber("stairType", 0);
  PolyMesh spm = static_cast<GeometryObject*>(sp.get())->build();
  std::map<int, int> sid;
  for (const auto& f : spm.faces) sid[f.matId]++;
  CHECK(sid.count(1) && sid.count(2) && sid.count(3) && sid.count(4) && sid.count(5));
}

static void testPatchNurbs() {
  std::vector<std::string> subs = objectSubcategories("Shapes");
  CHECK(subs.size() >= 3 && subs[0] == "Splines" && subs[1] == "NURBS Curves" && subs[2] == "Extended Splines");
  // Point curve passes through its points.
  std::vector<Vec3> pts = {{0, 0, 0}, {50, 40, 0}, {100, 0, 0}, {150, 30, 0}};
  Spline pc = interpolatingSpline(pts, false);
  CHECK(pc.knots.size() == 4);
  for (int i = 0; i < 3; ++i) CHECK(length(pc.evaluate(i, 0) - pts[i]) < 1e-4f);
  CHECK(length(pc.evaluate(2, 1) - pts[3]) < 1e-4f);
  // CV curve passes through its end CVs and stays inside the control polygon's bounds.
  Spline cv = bsplineToBezier(pts, false);
  CHECK(length(cv.evaluate(0, 0) - pts.front()) < 1e-4f);
  CHECK(length(cv.evaluate(cv.segmentCount() - 1, 1) - pts.back()) < 1e-4f);
  for (int g = 0; g < cv.segmentCount(); ++g)
    for (float t : {0.25f, 0.5f, 0.75f}) {
      Vec3 p = cv.evaluate(g, t);
      CHECK(p.y >= -1e-3f && p.y <= 40.0f && p.x >= -1e-3f && p.x <= 150.0f);
    }
  // A middle CV does not lie on the curve (it pulls it): the curve stays below y = 40.
  float maxY = 0;
  for (int g = 0; g < cv.segmentCount(); ++g)
    for (int k = 0; k <= 20; ++k) maxY = std::max(maxY, cv.evaluate(g, k / 20.0f).y);
  CHECK(maxY < 39.0f);
  // Closed CV curve around a square is a smooth closed loop inside the square.
  std::vector<Vec3> sq = {{-50, -50, 0}, {50, -50, 0}, {50, 50, 0}, {-50, 50, 0}};
  Spline loop = bsplineToBezier(sq, true);
  CHECK(loop.closed && loop.knots.size() == 4);
  CHECK(std::fabs(loop.evaluate(0, 0).x) < 50.0f);
  auto curve = createObject("CVCurve");
  auto* nc = dynamic_cast<NurbsCurveObject*>(curve.get());
  CHECK(nc && nc->isCvCurve());
  if (nc) {
    nc->points = sq;
    nc->closed = true;
    PolyMesh m = applyMod("Extrude", nc->buildShape(), {{"amount", 10}});
    CHECK(isClosedManifold(m) && signedVolume(m) > 0);
    auto copy = createObject("CVCurve");
    copy->fromJson(nc->toJson());
    CHECK(static_cast<NurbsCurveObject*>(copy.get())->points.size() == 4);
  }
  // Patch grids.
  auto quad = createObject("QuadPatch");
  quad->params.setNumber("steps", 3);
  quad->params.setNumber("lengthSegs", 2);
  PolyMesh qm = static_cast<GeometryObject*>(quad.get())->build();
  CHECK(qm.faces.size() == 4u * 8u && qm.faces[0].v.size() == 4);
  auto tri = createObject("TriPatch");
  PolyMesh tm = static_cast<GeometryObject*>(tri.get())->build();
  CHECK(!tm.faces.empty() && tm.faces[0].v.size() == 3);
  CHECK(std::fabs(tm.bounds().extent().x - 100.0f) < 1e-3f);
}

int main() {
  testRegistration();
  testPatchNurbs();
  testCompound();
  testAec();
  testCappingWithHoles();
  testExtrude();
  testLathe();
  testSweep();
  testBevel();
  testSplineOps();
  testText();
  testShapesAndSections();
  if (g_failures) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("All shape tests passed\n");
  return 0;
}
