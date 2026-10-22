#include "editorstate.h"

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

void EditorState::setShowGrid(bool on) {
  if (on == showGrid_) return;
  showGrid_ = on;
  Q_EMIT showGridChanged(on);
}

}  // namespace veraa
