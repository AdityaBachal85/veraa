// Modifiers that work on shapes (splines): Extrude, Lathe, Sweep, Bevel, Bevel Profile, Edit
// Spline, Normalize Spline, Fillet/Chamfer, Trim/Extend and Renderable Spline, plus the TextPlus
// geometry object. Registered by registerShapeModifierTypes(). The classes the editor needs to
// reach (stored shapes, picked operands) are declared here.
#pragma once

#include <string>
#include <vector>

#include "modifier.h"
#include "object.h"
#include "shape_mesh.h"

namespace veraa {

// Edit Spline: the stack's incoming shape is replaced by an edited copy once the user edits it
// (Editable Spline tools work on `shape`). Until then the modifier passes the shape through.
class EditSplineModifier : public Modifier {
 public:
  EditSplineModifier();
  std::string type() const override { return "EditSpline"; }
  std::unique_ptr<Modifier> clone() const override { return std::make_unique<EditSplineModifier>(*this); }
  void applyState(GeomState& state) const override;
  json toJson() const override;
  void fromJson(const json& j) override;

  bool hasEdits = false;
  BezierShape shape;
};

// Sweep with a picked custom section (section type "Custom").
class SweepModifier : public Modifier {
 public:
  SweepModifier();
  std::string type() const override { return "Sweep"; }
  std::unique_ptr<Modifier> clone() const override { return std::make_unique<SweepModifier>(*this); }
  void applyState(GeomState& state) const override;
  json toJson() const override;
  void fromJson(const json& j) override;
  // Index of "Custom" in the section type list.
  static int customSectionIndex();

  BezierShape customSection;  // in the section's local XY plane
  std::string customSectionName;
};

// Bevel Profile: extrudes the shape following a picked profile spline (x = outline, y = height,
// relative to the profile's first point).
class BevelProfileModifier : public Modifier {
 public:
  BevelProfileModifier();
  std::string type() const override { return "BevelProfile"; }
  std::unique_ptr<Modifier> clone() const override { return std::make_unique<BevelProfileModifier>(*this); }
  void applyState(GeomState& state) const override;
  json toJson() const override;
  void fromJson(const json& j) override;

  BezierShape profile;
  std::string profileName;
};

// Trim/Extend: a list of picks applied in order (trim the piece around the pick, or extend the
// open end nearest the pick). Picks are in the shape's local space.
class TrimExtendModifier : public Modifier {
 public:
  struct Op {
    bool extend = false;
    Vec3 pick;
  };
  TrimExtendModifier();
  std::string type() const override { return "TrimExtend"; }
  std::unique_ptr<Modifier> clone() const override { return std::make_unique<TrimExtendModifier>(*this); }
  void applyState(GeomState& state) const override;
  json toJson() const override;
  void fromJson(const json& j) override;

  std::vector<Op> ops;
};

// TextPlus: extruded (optionally bevelled) 3D text.
class TextPlusObject : public GeometryObject {
 public:
  TextPlusObject();
  std::string type() const override { return "TextPlus"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<TextPlusObject>(*this); }
  PolyMesh build() const override;
};

// Levels of a bevelled extrusion of total height `amount`: bevels of the given depth (height)
// and width (inset) at the top and optionally the bottom. profile: 0 flat, 1 round (convex),
// 2 concave.
std::vector<ExtrudeLevel> bevelledLevels(float amount, int segments, float depth, float width, int bevelSegments,
                                         int profile, bool bevelTop, bool bevelBottom);

}  // namespace veraa
