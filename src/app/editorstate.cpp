#include "editorstate.h"

#include <cmath>

namespace veraa {

void EditorState::setTool(TransformTool t) {
  if (!createType_.isEmpty()) setCreateType({});
  if (t == tool_) return;
  tool_ = t;
  Q_EMIT toolChanged(t);
}

void EditorState::setConstraint(GizmoHandle h) {
  if (h == GizmoHandle::None || h == GizmoHandle::Center || h == constraint_) return;
  constraint_ = h;
  Q_EMIT constraintChanged(h);
}

void EditorState::setCreateType(const QString& type) {
  if (type == createType_) return;
  createType_ = type;
  Q_EMIT createTypeChanged(type);
}

void EditorState::setViewportTool(std::shared_ptr<ViewportTool> tool) {
  if (tool == tool2_) return;
  if (tool2_) tool2_->cancel();
  tool2_ = std::move(tool);
  if (tool2_ && !tool2_->prompt().isEmpty()) setPrompt(tool2_->prompt());
  Q_EMIT viewportToolChanged();
  Q_EMIT redrawRequested();
}

void EditorState::setAngleSnap(bool on, float stepDeg) {
  angleSnap_ = on;
  angleSnapStep_ = stepDeg > 0 ? stepDeg : angleSnapStep_;
  Q_EMIT snapSettingsChanged();
}

void EditorState::setPercentSnap(bool on, float stepPercent) {
  percentSnap_ = on;
  percentSnapStep_ = stepPercent > 0 ? stepPercent : percentSnapStep_;
  Q_EMIT snapSettingsChanged();
}

float EditorState::snapAngle(float rad) const {
  if (!angleSnap_ || angleSnapStep_ <= 0) return rad;
  float step = radians(angleSnapStep_);
  return std::round(rad / step) * step;
}

Vec3 EditorState::snapPercent(const Vec3& f) const {
  if (!percentSnap_ || percentSnapStep_ <= 0) return f;
  float step = percentSnapStep_ / 100.0f;
  auto q = [&](float v) { return std::round(v / step) * step; };
  return Vec3(q(f.x), q(f.y), q(f.z));
}

void EditorState::setShowGrid(bool on) {
  if (on == showGrid_) return;
  showGrid_ = on;
  Q_EMIT showGridChanged(on);
}

}  // namespace veraa
