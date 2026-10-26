// UI-wide tool settings shared by the main toolbar, viewports and panels: selection region
// shape, reference coordinate system, pivot center, scale mode, selection lock, ...
#pragma once

#include <QObject>
#include <QString>

#include <cstdint>

namespace veraa {

enum class RegionShape { Rectangle, Circle, Fence, Lasso, Paint };
enum class RefCoordSys { View, Screen, World, Parent, Local, Gimbal, Grid, Working, LocalAligned, Pick };
enum class PivotCenter { PivotPoint, Selection, Coordinate };
enum class ScaleMode { Uniform, NonUniform, Squash };

QString refCoordSysName(RefCoordSys s);

class UiState : public QObject {
  Q_OBJECT
 public:
  static UiState& instance();

  RegionShape regionShape() const { return regionShape_; }
  void setRegionShape(RegionShape s);
  // Window: objects must be fully inside the region. Crossing (default): touching is enough.
  bool windowSelection() const { return window_; }
  void setWindowSelection(bool on);
  float paintBrushRadius() const { return paintRadius_; }  // pixels
  void setPaintBrushRadius(float r);

  RefCoordSys refCoordSys() const { return refCoord_; }
  void setRefCoordSys(RefCoordSys s);
  uint32_t pickCoordNode() const { return pickNode_; }  // object whose axes "Pick" uses
  void setPickCoordNode(uint32_t id);

  PivotCenter pivotCenter() const { return pivotCenter_; }
  void setPivotCenter(PivotCenter c);
  ScaleMode scaleMode() const { return scaleMode_; }
  void setScaleMode(ScaleMode m);

  bool selectionLocked() const { return selLock_; }
  void setSelectionLocked(bool on);
  bool shortcutOverride() const { return shortcutOverride_; }
  void setShortcutOverride(bool on);
  bool absoluteTypeIn() const { return absoluteTypeIn_; }
  void setAbsoluteTypeIn(bool on);
  bool adaptiveDegradation() const { return adaptive_; }
  void setAdaptiveDegradation(bool on);

  bool showViewCube() const { return viewCube_; }
  void setShowViewCube(bool on);
  bool showTransformGizmo() const { return showGizmo_; }
  void setShowTransformGizmo(bool on);
  bool shadeSelected() const { return shadeSelected_; }
  void setShadeSelected(bool on);
  bool showSelectionBrackets() const { return brackets_; }
  void setShowSelectionBrackets(bool on);
  float gizmoSize() const { return gizmoSize_; }  // pixels
  void setGizmoSize(float px);
  bool showFrozenInGray() const { return frozenGray_; }
  void setShowFrozenInGray(bool on);
  // Display Color panel: wireframe/shaded use object color (true) or material color (false).
  bool wireUsesObjectColor() const { return wireObjectColor_; }
  bool shadedUsesObjectColor() const { return shadedObjectColor_; }
  void setDisplayColors(bool wireObject, bool shadedObject);
  // Display panel > Hide by Category: bit (1 << int(ObjectKind)) set = that category is hidden.
  int hiddenCategories() const { return hiddenCategories_; }
  void setHiddenCategories(int mask);
  bool categoryHidden(int kind) const { return (hiddenCategories_ >> kind) & 1; }
  // Display panel > Hide Frozen Objects.
  bool hideFrozen() const { return hideFrozen_; }
  void setHideFrozen(bool on);
  // Axis Constraints toolbar > Snaps Use Axis Constraint Toggle: snapped moves stay on the
  // constrained axes.
  bool snapUseAxisConstraint() const { return snapAxisConstraint_; }
  void setSnapUseAxisConstraint(bool on);

 Q_SIGNALS:
  void changed();

 private:
  UiState() = default;
  RegionShape regionShape_ = RegionShape::Rectangle;
  bool window_ = false;
  float paintRadius_ = 18.0f;
  RefCoordSys refCoord_ = RefCoordSys::View;
  uint32_t pickNode_ = 0;
  PivotCenter pivotCenter_ = PivotCenter::PivotPoint;
  ScaleMode scaleMode_ = ScaleMode::Uniform;
  bool selLock_ = false;
  bool shortcutOverride_ = false;
  bool absoluteTypeIn_ = true;
  bool adaptive_ = true;
  bool viewCube_ = true;
  bool showGizmo_ = true;
  bool shadeSelected_ = false;
  bool brackets_ = true;
  float gizmoSize_ = 85.0f;
  bool frozenGray_ = true;
  bool wireObjectColor_ = true;
  bool shadedObjectColor_ = false;
  int hiddenCategories_ = 0;
  bool hideFrozen_ = false;
  bool snapAxisConstraint_ = false;
};

}  // namespace veraa
