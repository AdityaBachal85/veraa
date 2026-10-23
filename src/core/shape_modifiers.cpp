#include "shape_modifiers.h"

#include <algorithm>
#include <cmath>

#include "registry.h"
#include "shape_mesh.h"
#include "shapes.h"
#include "spline_ops.h"

namespace veraa {

namespace {

template <class Derived>
class ShapeModifier : public Modifier {
 public:
  std::unique_ptr<Modifier> clone() const override {
    return std::make_unique<Derived>(static_cast<const Derived&>(*this));
  }
};

const std::vector<std::string> kCapping = {"Morph", "Grid"};

// Turns the state into a mesh result.
void setMesh(GeomState& st, PolyMesh m, bool genMatIds, bool smooth) {
  for (auto& f : m.faces) {
    if (!genMatIds) f.matId = 0;
    if (!smooth) f.smoothGroups = 0;
  }
  st.mesh = std::move(m);
  st.isShape = false;
  st.shape = BezierShape();
}

void addCappingParams(ParamBlock& p) {
  p.beginGroup("Capping");
  p.addBool("capStart", "Cap Start", true);
  p.addBool("capEnd", "Cap End", true);
  p.addEnum("capping", "Capping", kCapping, 0);
  p.addFloat("gridSize", "Grid Size", 10, 0.01, 1e7, 1.0);
}

ExtrudeOptions cappingOptions(const ParamBlock& p) {
  ExtrudeOptions o;
  o.capStart = p.getBool("capStart");
  o.capEnd = p.getBool("capEnd");
  o.gridCapSize = p.getInt("capping") == 1 ? p.getFloat("gridSize") : 0.0f;
  o.generateUVs = p.getBool("genMapping");
  return o;
}

// Catmull-Rom interpolation between profile points (z, outline) for curved bevel sides.
Vec2 catmullRom(const Vec2& p0, const Vec2& p1, const Vec2& p2, const Vec2& p3, float t) {
  float t2 = t * t, t3 = t2 * t;
  return (p1 * 2.0f + (p2 - p0) * t + (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * t2 +
          (p1 * 3.0f - p0 - p2 * 3.0f + p3) * t3) *
         0.5f;
}

// Subdivides a (z, outline) profile into extrusion levels, segments per span.
std::vector<ExtrudeLevel> profileLevels(const std::vector<Vec2>& pts, int segments, bool curved) {
  std::vector<ExtrudeLevel> levels;
  if (pts.empty()) return levels;
  levels.push_back({pts[0].x, pts[0].y});
  int n = static_cast<int>(pts.size());
  for (int i = 0; i + 1 < n; ++i) {
    for (int s = 1; s <= segments; ++s) {
      float t = static_cast<float>(s) / segments;
      Vec2 p;
      if (curved && n > 2) {
        const Vec2& a = pts[std::max(0, i - 1)];
        const Vec2& d = pts[std::min(n - 1, i + 2)];
        p = catmullRom(a, pts[i], pts[i + 1], d, t);
      } else {
        p = pts[i] + (pts[i + 1] - pts[i]) * t;
      }
      levels.push_back({p.x, p.y});
    }
  }
  return levels;
}

// ---------------------------------------------------------------------------------------------

class ExtrudeModifier : public ShapeModifier<ExtrudeModifier> {
 public:
  ExtrudeModifier() {
    params.beginGroup("Parameters");
    params.addFloat("amount", "Amount", 10, -1e7, 1e7, 1.0);
    params.addInt("segments", "Segments", 1, 1, 10000);
    addCappingParams(params);
    params.beginGroup("Output");
    params.addBool("genMapping", "Generate Mapping Coords.", true);
    params.addBool("genMatIds", "Generate Material IDs", true);
    params.addBool("smooth", "Smooth", true);
  }
  std::string type() const override { return "Extrude"; }
  void applyState(GeomState& st) const override {
    if (!st.isShape) return;
    std::vector<Contour2D> contours = shapeContours(st.shape, st.shapeSteps, st.shapeOptimize);
    int segs = params.getInt("segments");
    float amount = params.getFloat("amount");
    std::vector<ExtrudeLevel> levels;
    for (int i = 0; i <= segs; ++i) levels.push_back({amount * static_cast<float>(i) / segs, 0.0f});
    ExtrudeOptions o = cappingOptions(params);
    o.smoothAcrossLevels = true;
    setMesh(st, extrudeContours(contours, levels, o), params.getBool("genMatIds"), params.getBool("smooth"));
  }
};

class LatheModifier : public ShapeModifier<LatheModifier> {
 public:
  LatheModifier() {
    params.beginGroup("Parameters");
    params.addFloat("degrees", "Degrees", 360, 0.0, 360.0, 1.0, 1);
    params.addBool("weldCore", "Weld Core", false);
    params.addBool("flipNormals", "Flip Normals", false);
    params.addInt("segments", "Segments", 16, 3, 10000);
    params.beginGroup("Capping");
    params.addBool("capStart", "Cap Start", true);
    params.addBool("capEnd", "Cap End", true);
    params.beginGroup("Direction");
    params.addEnum("direction", "Direction", {"X", "Y", "Z"}, 1);
    params.beginGroup("Align");
    params.addEnum("align", "Align", {"Min", "Center", "Max"}, 0);
    params.beginGroup("Output");
    params.addBool("genMapping", "Generate Mapping Coords.", true);
    params.addBool("genMatIds", "Generate Material IDs", true);
    params.addBool("smooth", "Smooth", true);
  }
  std::string type() const override { return "Lathe"; }
  void applyState(GeomState& st) const override {
    if (!st.isShape) return;
    std::vector<Contour2D> contours = shapeContours(st.shape, st.shapeSteps, st.shapeOptimize);
    LatheOptions o;
    o.degrees = params.getFloat("degrees");
    o.segments = params.getInt("segments");
    o.axis = params.getInt("direction");
    o.align = params.getInt("align");
    o.weldCore = params.getBool("weldCore");
    o.flipNormals = params.getBool("flipNormals");
    o.capStart = params.getBool("capStart");
    o.capEnd = params.getBool("capEnd");
    o.generateUVs = params.getBool("genMapping");
    setMesh(st, latheContours(contours, o), params.getBool("genMatIds"), params.getBool("smooth"));
  }
};

class BevelModifier : public ShapeModifier<BevelModifier> {
 public:
  BevelModifier() {
    addCappingParams(params);
    params.beginGroup("Surface");
    params.addEnum("sides", "Sides", {"Linear Sides", "Curved Sides"}, 0);
    params.addInt("segments", "Segments", 1, 1, 1000);
    params.addBool("smoothAcrossLevels", "Smooth Across Levels", false);
    params.addBool("genMapping", "Generate Mapping Coords.", true);
    params.beginGroup("Bevel Values");
    params.addFloat("startOutline", "Start Outline", 0, -1e7, 1e7, 0.5);
    params.addFloat("level1Height", "Level 1 Height", 10, -1e7, 1e7, 0.5);
    params.addFloat("level1Outline", "Level 1 Outline", 0, -1e7, 1e7, 0.5);
    params.addBool("level2", "Level 2", false);
    params.addFloat("level2Height", "Level 2 Height", 0, -1e7, 1e7, 0.5);
    params.addFloat("level2Outline", "Level 2 Outline", 0, -1e7, 1e7, 0.5);
    params.addBool("level3", "Level 3", false);
    params.addFloat("level3Height", "Level 3 Height", 0, -1e7, 1e7, 0.5);
    params.addFloat("level3Outline", "Level 3 Outline", 0, -1e7, 1e7, 0.5);
  }
  std::string type() const override { return "Bevel"; }
  void applyState(GeomState& st) const override {
    if (!st.isShape) return;
    std::vector<Vec2> pts;  // (z, outline)
    float z = 0, o = params.getFloat("startOutline");
    pts.emplace_back(z, o);
    z += params.getFloat("level1Height");
    o += params.getFloat("level1Outline");
    pts.emplace_back(z, o);
    for (int lv = 2; lv <= 3; ++lv) {
      std::string k = "level" + std::to_string(lv);
      if (!params.getBool(k)) continue;
      z += params.getFloat(k + "Height");
      o += params.getFloat(k + "Outline");
      pts.emplace_back(z, o);
    }
    std::vector<ExtrudeLevel> levels = profileLevels(pts, params.getInt("segments"), params.getInt("sides") == 1);
    ExtrudeOptions opt = cappingOptions(params);
    opt.smoothAcrossLevels = params.getBool("smoothAcrossLevels");
    std::vector<Contour2D> contours = shapeContours(st.shape, st.shapeSteps, st.shapeOptimize);
    setMesh(st, extrudeContours(contours, levels, opt), true, true);
  }
};

class NormalizeSplineModifier : public ShapeModifier<NormalizeSplineModifier> {
 public:
  NormalizeSplineModifier() {
    params.beginGroup("Parameters");
    params.addFloat("segLength", "Seg Length", 20, 0.01, 1e7, 1.0);
  }
  std::string type() const override { return "NormalizeSpline"; }
  void applyState(GeomState& st) const override {
    if (!st.isShape) return;
    st.shape = splineops::normalized(st.shape, params.getFloat("segLength"));
  }
};

class FilletChamferModifier : public ShapeModifier<FilletChamferModifier> {
 public:
  FilletChamferModifier() {
    params.beginGroup("Fillet/Chamfer");
    params.addEnum("mode", "Mode", {"Fillet", "Chamfer"}, 0);
    params.addFloat("amount", "Radius / Distance", 10, 0.0, 1e7, 0.5);
  }
  std::string type() const override { return "FilletChamfer"; }
  void applyState(GeomState& st) const override {
    if (!st.isShape || params.getFloat("amount") <= 0) return;
    st.shape = splineops::filletAll(st.shape, params.getFloat("amount"), params.getInt("mode") == 1);
  }
};

class RenderableSplineModifier : public ShapeModifier<RenderableSplineModifier> {
 public:
  RenderableSplineModifier() {
    params.beginGroup("Rendering");
    params.addEnum("mode", "Section", {"Radial", "Rectangular"}, 0);
    params.addFloat("thickness", "Thickness", 1, 0.0, 1e7, 0.1);
    params.addInt("sides", "Sides", 12, 3, 100);
    params.addFloat("angle", "Angle", 0, -36000, 36000, 1.0, 1);
    params.addFloat("length", "Length", 6, 0.0, 1e7, 0.1);
    params.addFloat("width", "Width", 2, 0.0, 1e7, 0.1);
    params.addBool("genMapping", "Generate Mapping Coords.", true);
  }
  std::string type() const override { return "RenderableSpline"; }
  void applyState(GeomState& st) const override {
    if (!st.isShape) return;
    PolyMesh m;
    if (params.getInt("mode") == 0) {
      m = shapeToTubeMesh(st.shape, params.getFloat("thickness"), params.getInt("sides"), params.getFloat("angle"),
                          st.shapeSteps, st.shapeOptimize);
      if (!params.getBool("genMapping")) {
        m.uvs.clear();
        for (auto& f : m.faces) f.uv.clear();
      }
    } else {
      BezierShape rect;
      rect.splines.push_back(
          makeRectangleSpline(std::max(1e-3f, params.getFloat("width")), std::max(1e-3f, params.getFloat("length")), 0));
      SweepOptions o;
      o.angleDeg = params.getFloat("angle");
      o.generateUVs = params.getBool("genMapping");
      m = sweepSection(st.shape, st.shapeSteps, st.shapeOptimize, shapeContours(rect, 0, true), o);
    }
    setMesh(st, std::move(m), true, true);
  }
};

}  // namespace

// ---------------------------------------------------------------------------------------------
// Bevelled levels (TextPlus)

std::vector<ExtrudeLevel> bevelledLevels(float amount, int segments, float depth, float width, int bevelSegments,
                                         int profile, bool bevelTop, bool bevelBottom) {
  segments = std::max(1, segments);
  bevelSegments = std::max(1, bevelSegments);
  float sign = amount < 0 ? -1.0f : 1.0f;
  float a = std::fabs(amount);
  int bevels = (bevelTop ? 1 : 0) + (bevelBottom ? 1 : 0);
  if (bevels > 0) depth = clampf(depth, 0.0f, a / bevels);
  else depth = 0;
  // Bevel curve from (z 0, inset -width) to (z depth, inset 0).
  auto curve = [&](float t) {
    float ang = t * kPi * 0.5f;
    switch (profile) {
      case 1: return Vec2(depth * (1.0f - std::cos(ang)), -width + width * std::sin(ang));
      case 2: return Vec2(depth * std::sin(ang), -width * std::cos(ang));
      default: return Vec2(depth * t, -width * (1.0f - t));
    }
  };
  std::vector<ExtrudeLevel> lv;
  if (bevelBottom && depth > 0)
    for (int i = 0; i <= bevelSegments; ++i) {
      Vec2 c = curve(static_cast<float>(i) / bevelSegments);
      lv.push_back({c.x, c.y});
    }
  else
    lv.push_back({0.0f, 0.0f});
  float z0 = lv.back().z, z1 = (bevelTop && depth > 0) ? a - depth : a;
  for (int s = 1; s <= segments; ++s) lv.push_back({z0 + (z1 - z0) * static_cast<float>(s) / segments, 0.0f});
  if (bevelTop && depth > 0)
    for (int i = bevelSegments - 1; i >= 0; --i) {
      Vec2 c = curve(static_cast<float>(i) / bevelSegments);
      lv.push_back({a - c.x, c.y});
    }
  for (auto& l : lv) l.z *= sign;
  return lv;
}

// ---------------------------------------------------------------------------------------------
// Edit Spline

EditSplineModifier::EditSplineModifier() = default;

void EditSplineModifier::applyState(GeomState& st) const {
  if (st.isShape && hasEdits) st.shape = shape;
}

json EditSplineModifier::toJson() const {
  json j = Modifier::toJson();
  j["hasEdits"] = hasEdits;
  if (hasEdits) j["shape"] = bezierShapeToJson(shape);
  return j;
}

void EditSplineModifier::fromJson(const json& j) {
  Modifier::fromJson(j);
  hasEdits = j.value("hasEdits", false);
  shape = hasEdits && j.contains("shape") ? bezierShapeFromJson(j["shape"]) : BezierShape();
}

// ---------------------------------------------------------------------------------------------
// Sweep

SweepModifier::SweepModifier() {
  std::vector<std::string> sections = sectionProfileNames();
  sections.push_back("Custom");
  params.beginGroup("Section Type");
  params.addEnum("section", "Built-In Section", sections, 1);
  params.beginGroup("Parameters");
  params.addFloat("length", "Length", 10, 0.0, 1e7, 0.5);
  params.addFloat("width", "Width", 10, 0.0, 1e7, 0.5);
  params.addFloat("thickness", "Thickness", 1, 0.0, 1e7, 0.1);
  params.addFloat("cornerRadius", "Corner Radius", 0, 0.0, 1e7, 0.1);
  params.beginGroup("Sweep Parameters");
  params.addBool("mirrorXZ", "Mirror on XZ Plane", false);
  params.addBool("mirrorXY", "Mirror on XY Plane", false);
  params.addFloat("xOffset", "X Offset", 0, -1e7, 1e7, 0.5);
  params.addFloat("yOffset", "Y Offset", 0, -1e7, 1e7, 0.5);
  params.addFloat("angle", "Angle", 0, -36000, 36000, 1.0, 1);
  params.addBool("smoothSection", "Smooth Section", true);
  params.addBool("smoothPath", "Smooth Path", false);
  params.addEnum("pivotAlign", "Pivot Alignment",
                 {"Bottom Left", "Bottom Center", "Bottom Right", "Middle Left", "Center", "Middle Right", "Top Left",
                  "Top Center", "Top Right"},
                 4);
  params.addBool("banking", "Banking", true);
  params.addBool("capEnds", "Cap Ends", true);
  params.addBool("genMapping", "Generate Mapping Coords.", true);
  params.addBool("genMatIds", "Generate Material IDs", true);
}

int SweepModifier::customSectionIndex() { return static_cast<int>(sectionProfileNames().size()); }

void SweepModifier::applyState(GeomState& st) const {
  if (!st.isShape) return;
  int sec = params.getInt("section");
  BezierShape section;
  if (sec == customSectionIndex()) {
    if (customSection.empty()) return;  // nothing picked yet: the path passes through
    section = customSection;
    // A custom section sweeps around its own center.
    Aabb b = section.bounds();
    if (b.valid()) section.transform(Mat4::translate(Vec3(-b.center().x, -b.center().y, 0)));
  } else {
    section = makeSectionProfile(sec, params.getFloat("length"), params.getFloat("width"), params.getFloat("thickness"),
                                 params.getFloat("cornerRadius"));
  }
  SweepOptions o;
  o.angleDeg = params.getFloat("angle");
  o.offset = Vec2(params.getFloat("xOffset"), params.getFloat("yOffset"));
  o.pivotAlign = params.getInt("pivotAlign");
  o.mirrorX = params.getBool("mirrorXY");
  o.mirrorY = params.getBool("mirrorXZ");
  o.banking = params.getBool("banking");
  o.generateUVs = params.getBool("genMapping");
  o.capEnds = params.getBool("capEnds");
  o.smoothSection = params.getBool("smoothSection");
  o.smoothPath = params.getBool("smoothPath");
  PolyMesh m = sweepSection(st.shape, st.shapeSteps, st.shapeOptimize, shapeContours(section, 6, true), o);
  setMesh(st, std::move(m), params.getBool("genMatIds"), true);
}

json SweepModifier::toJson() const {
  json j = Modifier::toJson();
  if (!customSection.empty()) {
    j["customSection"] = bezierShapeToJson(customSection);
    j["customSectionName"] = customSectionName;
  }
  return j;
}

void SweepModifier::fromJson(const json& j) {
  Modifier::fromJson(j);
  customSection = j.contains("customSection") ? bezierShapeFromJson(j["customSection"]) : BezierShape();
  customSectionName = j.value("customSectionName", std::string());
}

// ---------------------------------------------------------------------------------------------
// Bevel Profile

BevelProfileModifier::BevelProfileModifier() {
  addCappingParams(params);
  params.beginGroup("Output");
  params.addBool("genMapping", "Generate Mapping Coords.", true);
  params.addBool("smoothAcrossLevels", "Smooth Across Levels", true);
}

void BevelProfileModifier::applyState(GeomState& st) const {
  if (!st.isShape || profile.empty()) return;
  const Spline& sp = profile.splines.front();
  std::vector<Vec3> pts = sp.polyline(6, true);
  if (sp.closed && !pts.empty()) pts.push_back(pts.front());
  if (pts.size() < 2) return;
  std::vector<ExtrudeLevel> levels;
  for (const auto& p : pts) {
    ExtrudeLevel l{p.y - pts[0].y, p.x - pts[0].x};
    if (!levels.empty() && std::fabs(l.z - levels.back().z) < 1e-7f && std::fabs(l.outline - levels.back().outline) < 1e-7f)
      continue;
    levels.push_back(l);
  }
  if (levels.size() < 2) return;
  ExtrudeOptions o = cappingOptions(params);
  o.smoothAcrossLevels = params.getBool("smoothAcrossLevels");
  std::vector<Contour2D> contours = shapeContours(st.shape, st.shapeSteps, st.shapeOptimize);
  setMesh(st, extrudeContours(contours, levels, o), true, true);
}

json BevelProfileModifier::toJson() const {
  json j = Modifier::toJson();
  if (!profile.empty()) {
    j["profile"] = bezierShapeToJson(profile);
    j["profileName"] = profileName;
  }
  return j;
}

void BevelProfileModifier::fromJson(const json& j) {
  Modifier::fromJson(j);
  profile = j.contains("profile") ? bezierShapeFromJson(j["profile"]) : BezierShape();
  profileName = j.value("profileName", std::string());
}

// ---------------------------------------------------------------------------------------------
// Trim/Extend

TrimExtendModifier::TrimExtendModifier() = default;

void TrimExtendModifier::applyState(GeomState& st) const {
  if (!st.isShape) return;
  for (const Op& op : ops) {
    splineops::SegmentRef seg;
    float t = 0, dist = 0;
    if (!splineops::nearestSegment(st.shape, op.pick, seg, t, dist)) continue;
    if (op.extend) splineops::extend(st.shape, seg.spline, op.pick);
    else splineops::trim(st.shape, seg.spline, op.pick);
  }
}

json TrimExtendModifier::toJson() const {
  json j = Modifier::toJson();
  json a = json::array();
  for (const Op& op : ops) a.push_back({{"extend", op.extend}, {"pick", vec3ToJson(op.pick)}});
  j["ops"] = a;
  return j;
}

void TrimExtendModifier::fromJson(const json& j) {
  Modifier::fromJson(j);
  ops.clear();
  if (j.contains("ops"))
    for (const auto& o : j["ops"]) ops.push_back({o.value("extend", false), vec3FromJson(o.value("pick", json()))});
}

// ---------------------------------------------------------------------------------------------
// TextPlus

TextPlusObject::TextPlusObject() {
  params.beginGroup("Text");
  params.addString("text", "Text", "TextPlus");
  params.beginGroup("Parameters");
  params.addString("font", "Font", defaultFontName());
  params.addBool("italic", "Italic", false);
  params.addBool("underline", "Underline", false);
  params.addEnum("align", "Alignment", {"Left", "Center", "Right", "Justify"}, 0);
  params.addFloat("size", "Size", 100, 0.0, 1e7, 1.0);
  params.addFloat("tracking", "Tracking", 0, -1e5, 1e5, 0.5);
  params.addFloat("leading", "Leading", 0, -1e5, 1e5, 0.5);
  params.addInt("steps", "Interpolation Steps", 4, 0, 100);
  params.beginGroup("Geometry");
  params.addFloat("extrude", "Extrude", 10, -1e7, 1e7, 0.5);
  params.addInt("segments", "Extrude Segments", 1, 1, 1000);
  params.addBool("applyBevel", "Apply Bevel", false);
  params.addEnum("bevelProfile", "Bevel Profile", {"Flat", "Round", "Concave"}, 0);
  params.addFloat("bevelDepth", "Bevel Depth", 2, 0.0, 1e7, 0.1);
  params.addFloat("bevelWidth", "Bevel Width", 1, 0.0, 1e7, 0.1);
  params.addInt("bevelSegments", "Bevel Segments", 3, 1, 100);
  params.addBool("bevelBack", "Bevel Back", true);
  params.addBool("genMapping", "Generate Mapping Coords.", true);
}

PolyMesh TextPlusObject::build() const {
  BezierShape shape = textToShape(params.getString("text"), params.getString("font"), params.getFloat("size"),
                                  params.getFloat("tracking"), params.getFloat("leading"), params.getInt("align"),
                                  params.getBool("italic"), params.getBool("underline"));
  std::vector<Contour2D> contours = shapeContours(shape, params.getInt("steps"), true);
  float amount = params.getFloat("extrude");
  ExtrudeOptions o;
  o.generateUVs = params.getBool("genMapping");
  if (std::fabs(amount) < 1e-6f) {
    // Flat text: a single cap facing up.
    o.capStart = false;
    return extrudeContours(contours, {{0, 0}, {0, 0}}, o);
  }
  bool bevel = params.getBool("applyBevel");
  std::vector<ExtrudeLevel> levels =
      bevelledLevels(amount, params.getInt("segments"), bevel ? params.getFloat("bevelDepth") : 0.0f,
                     params.getFloat("bevelWidth"), params.getInt("bevelSegments"), params.getInt("bevelProfile"), bevel,
                     bevel && params.getBool("bevelBack"));
  o.smoothAcrossLevels = params.getInt("bevelProfile") != 0;
  return extrudeContours(contours, levels, o);
}

// ---------------------------------------------------------------------------------------------

void registerShapeGeometryTypes() {
  registerObjectType({"TextPlus", "TextPlus", "Geometry", "Standard Primitives",
                      [] { return std::make_unique<TextPlusObject>(); }});
}

void registerShapeModifierTypes() {
  auto reg = [](const char* type, const char* name, const char* category, std::function<std::unique_ptr<Modifier>()> f) {
    registerModifierType({type, name, category, false, true, std::move(f)});
  };
  const char* mesh = "Mesh Editing";
  const char* spline = "Patch/Spline Editing";
  reg("Extrude", "Extrude", mesh, [] { return std::make_unique<ExtrudeModifier>(); });
  reg("Lathe", "Lathe", mesh, [] { return std::make_unique<LatheModifier>(); });
  reg("Bevel", "Bevel", mesh, [] { return std::make_unique<BevelModifier>(); });
  reg("BevelProfile", "Bevel Profile", mesh, [] { return std::make_unique<BevelProfileModifier>(); });
  reg("Sweep", "Sweep", spline, [] { return std::make_unique<SweepModifier>(); });
  reg("EditSpline", "Edit Spline", spline, [] { return std::make_unique<EditSplineModifier>(); });
  reg("NormalizeSpline", "Normalize Spl.", spline, [] { return std::make_unique<NormalizeSplineModifier>(); });
  reg("FilletChamfer", "Fillet/Chamfer", spline, [] { return std::make_unique<FilletChamferModifier>(); });
  reg("TrimExtend", "Trim/Extend", spline, [] { return std::make_unique<TrimExtendModifier>(); });
  reg("RenderableSpline", "Renderable Spline", spline, [] { return std::make_unique<RenderableSplineModifier>(); });
}

}  // namespace veraa
