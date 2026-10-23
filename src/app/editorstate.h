// Tool state shared by the viewports, toolbar and command panel.
#pragma once

#include <QObject>
#include <QString>

#include <functional>
#include <memory>
#include <vector>

#include "extension.h"
#include "gizmo.h"

namespace veraa {

class Node;
class Viewport;

class EditorState : public QObject {
  Q_OBJECT
 public:
  explicit EditorState(QObject* parent = nullptr) : QObject(parent) {}

  TransformTool tool() const { return tool_; }
  GizmoHandle constraint() const { return constraint_; }
  // Object type being created ("Box", "OmniLight", ...); empty when not in creation mode.
  QString createType() const { return createType_; }
  bool creating() const { return !createType_.isEmpty(); }
  bool showGrid() const { return showGrid_; }

  void setTool(TransformTool t);
  void setConstraint(GizmoHandle h);
  void setCreateType(const QString& type);
  void setShowGrid(bool on);
  void setPrompt(const QString& text) { Q_EMIT promptChanged(text); }
  // Create panel > AutoGrid: new objects are built on the surface under the cursor.
  bool autoGrid() const { return autoGrid_; }
  void setAutoGrid(bool on) { autoGrid_ = on; }

  // ---- Viewport tools and decorators (see extension.h) ----
  ViewportTool* viewportTool() const { return tool2_.get(); }
  std::shared_ptr<ViewportTool> viewportToolShared() const { return tool2_; }
  // Replaces the active tool (cancelling the previous one). Pass nullptr to return to normal.
  void setViewportTool(std::shared_ptr<ViewportTool> tool);
  void addDecorator(std::shared_ptr<ViewportDecorator> d) { decorators_.push_back(std::move(d)); }
  const std::vector<std::shared_ptr<ViewportDecorator>>& decorators() const { return decorators_; }
  // Asks every viewport to repaint (e.g. after a tool changed what it draws).
  void requestRedraw() { Q_EMIT redrawRequested(); }

  // ---- Snapping (installed by the snapping module) ----
  // Returns true and sets `out` when a snap target is found near screen point (px, py);
  // `candidate` is the unsnapped world point.
  using PointSnapper = std::function<bool(const Viewport& vp, float px, float py, const Vec3& candidate, Vec3& out)>;
  void setPointSnapper(PointSnapper s) { pointSnapper_ = std::move(s); }
  bool snapPoint(const Viewport& vp, float px, float py, const Vec3& candidate, Vec3& out) const {
    return pointSnapper_ && pointSnapper_(vp, px, py, candidate, out);
  }
  bool angleSnap() const { return angleSnap_; }
  float angleSnapStep() const { return angleSnapStep_; }  // degrees
  bool percentSnap() const { return percentSnap_; }
  float percentSnapStep() const { return percentSnapStep_; }  // percent
  void setAngleSnap(bool on, float stepDeg);
  void setPercentSnap(bool on, float stepPercent);
  // Rounds a rotation (radians) / scale factors to the snap steps when those snaps are on.
  float snapAngle(float radians) const;
  Vec3 snapPercent(const Vec3& factors) const;

  // ---- Selection filter (toolbar "All / Geometry / Shapes / Lights / ...") ----
  using SelectionFilter = std::function<bool(const Node&)>;
  void setSelectionFilter(SelectionFilter f) { selectionFilter_ = std::move(f); }
  bool selectionFilterAccepts(const Node& n) const { return !selectionFilter_ || selectionFilter_(n); }

 Q_SIGNALS:
  void toolChanged(TransformTool tool);
  void constraintChanged(GizmoHandle h);
  void createTypeChanged(const QString& type);
  void showGridChanged(bool on);
  void promptChanged(const QString& text);
  void viewportToolChanged();
  void snapSettingsChanged();
  void redrawRequested();

 private:
  TransformTool tool_ = TransformTool::Select;
  GizmoHandle constraint_ = GizmoHandle::XY;
  QString createType_;
  bool showGrid_ = true;
  bool autoGrid_ = false;
  std::shared_ptr<ViewportTool> tool2_;
  std::vector<std::shared_ptr<ViewportDecorator>> decorators_;
  PointSnapper pointSnapper_;
  bool angleSnap_ = false;
  float angleSnapStep_ = 5.0f;
  bool percentSnap_ = false;
  float percentSnapStep_ = 10.0f;
  SelectionFilter selectionFilter_;
};

}  // namespace veraa
