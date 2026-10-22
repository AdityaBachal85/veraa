// Shape (spline) object types: Line, Rectangle, Circle, Ellipse, Arc, Donut, NGon, Star, Text,
// Helix, Egg, Section, Freehand and the Extended Splines (Walled Rectangle, Channel, Angle, Tee,
// Wide Flange). Registered by registerShapeTypes(). All shapes lie in their local XY plane
// (except Helix, which rises along Z).
#pragma once

#include <string>
#include <vector>

#include "object.h"
#include "shape.h"

namespace veraa {

// ---- Spline builders (XY plane) ----

// Circle of 4 Bezier knots.
Spline makeCircleSpline(float radius, const Vec2& center = Vec2());
Spline makeEllipseSpline(float halfX, float halfY, const Vec2& center = Vec2());
// Arc between two angles (degrees, counter-clockwise from +X). pie adds the two radii.
Spline makeArcSpline(float radius, float fromDeg, float toDeg, bool pie, const Vec2& center = Vec2());
// Rectangle of width (X) by length (Y), optional rounded corners.
Spline makeRectangleSpline(float width, float length, float cornerRadius, const Vec2& center = Vec2());
// Straight segments between points (corner knots).
Spline makePolylineSpline(const std::vector<Vec2>& pts, bool closed);
// Closed polygon whose corners are rounded with the given radii (0 = sharp corner).
Spline makeFilletedPolygon(const std::vector<Vec2>& pts, const std::vector<float>& radii);
// Appends Bezier knots of an arc (center, radius, angles in radians) to s. The first knot is
// merged with s's last knot when they coincide.
void appendArc(Spline& s, const Vec2& center, float radius, float a0, float a1);

// Cross-section profiles used by Sweep and AEC objects (XY plane, centered on the origin).
// type: 0 Bar, 1 Angle, 2 Channel, 3 Cylinder, 4 Half Round, 5 Pipe, 6 Quarter Round, 7 Tee,
// 8 Tube, 9 Wide Flange.
BezierShape makeSectionProfile(int type, float length, float width, float thickness, float cornerRadius);
const std::vector<std::string>& sectionProfileNames();

// ---- Shape objects with an explicit spline (editable) ----

// Line and Freehand are editable splines created by clicking/drawing.
class LineShape : public EditableSplineObject {
 public:
  std::string type() const override { return "Line"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<LineShape>(*this); }
};

class FreehandShape : public EditableSplineObject {
 public:
  std::string type() const override { return "Freehand"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<FreehandShape>(*this); }
};

// Section keeps the last computed slice through the scene (Update Section recomputes it).
class SectionShape : public ShapeObject {
 public:
  SectionShape();
  std::string type() const override { return "Section"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<SectionShape>(*this); }
  BezierShape buildShape() const override { return shape; }
  json toJson() const override;
  void fromJson(const json& j) override;

  BezierShape shape;
};

// Slices triangle meshes (each with its world matrix) with the XY plane of planeWorld.
// Returns the cut lines in plane space, chained into splines (closed where they loop).
BezierShape sliceMeshes(const std::vector<std::pair<const TriMesh*, Mat4>>& meshes, const Mat4& planeWorld,
                        float limitHalfLength = 0.0f, float limitHalfWidth = 0.0f);

// ---- Text ----

// Font search folders (assets/fonts next to the app, system fonts). The app adds its own folder.
void addFontSearchPath(const std::string& dir);
// Font names (file names without extension) available for Text, sorted, bundled fonts first.
std::vector<std::string> availableFonts();
std::string defaultFontName();
// Outlines of the text in the XY plane. size = cap-to-descender height; kerning adds spacing,
// leading adds line spacing; align: 0 left, 1 center, 2 right, 3 justify. Returns an empty shape
// if the font can't be loaded.
BezierShape textToShape(const std::string& utf8, const std::string& fontName, float size, float kerning,
                        float leading, int align, bool italic = false, bool underline = false);

void registerShapeTypes();

}  // namespace veraa
