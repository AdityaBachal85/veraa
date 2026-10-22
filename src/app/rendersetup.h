// Render Setup (F10): output size, sampling, GI, color mapping, environment and camera.
#pragma once

#include <QDialog>

#include "document.h"

class QComboBox;

namespace veraa {

class ParamForm;

class RenderSetupDialog : public QDialog {
  Q_OBJECT
 public:
  explicit RenderSetupDialog(SceneDocument* doc, QWidget* parent = nullptr);

 Q_SIGNALS:
  void renderRequested();

 private:
  void rebuildCameraList();
  SceneDocument* doc_;
  ParamForm* form_;
  QComboBox* camera_;
  QComboBox* sizePreset_;
};

}  // namespace veraa
