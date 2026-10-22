// Tool state shared by the viewports, toolbar and command panel.
#pragma once

#include <QObject>
#include <QString>

#include "gizmo.h"

namespace veraa {

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

 Q_SIGNALS:
  void toolChanged(TransformTool tool);
  void constraintChanged(GizmoHandle h);
  void createTypeChanged(const QString& type);
  void showGridChanged(bool on);
  void promptChanged(const QString& text);

 private:
  TransformTool tool_ = TransformTool::Select;
  GizmoHandle constraint_ = GizmoHandle::XY;
  QString createType_;
  bool showGrid_ = true;
};

}  // namespace veraa
