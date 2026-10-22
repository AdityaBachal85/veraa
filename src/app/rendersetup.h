// Render Setup (F10): tabs for output (Common), image sampler, global illumination, color
// mapping and exposure, environment and atmosphere, frame buffer effects and render elements.
#pragma once

#include <QDialog>
#include <QStringList>

#include "document.h"

class QComboBox;
class QLabel;
class QTabWidget;

namespace veraa {

class ParamForm;

// A ParamForm on the scene's render settings that shows only some rollouts (by group name).
// Edits are undoable and notify the document. With `others` set it also shows every group
// that is not in `claimed` (so settings added by other modules always appear somewhere).
class RenderSettingsPanel : public QWidget {
  Q_OBJECT
 public:
  RenderSettingsPanel(SceneDocument* doc, const QStringList& groups, QWidget* parent = nullptr,
                      const QStringList& excludeKeys = {}, const QStringList& claimed = {});
  ParamForm* form() const { return form_; }

 private:
  void reload();
  SceneDocument* doc_;
  QStringList groups_, exclude_, claimed_;
  ParamForm* form_;
};

class RenderSetupDialog : public QDialog {
  Q_OBJECT
 public:
  explicit RenderSetupDialog(SceneDocument* doc, QWidget* parent = nullptr);
  // Shows a tab by its title ("Common", "Sampler", "GI", ...). Returns false if unknown.
  bool showTab(const QString& title);
  QStringList tabTitles() const;

 Q_SIGNALS:
  void renderRequested();
  void printSizeRequested();
  void batchRenderRequested();
  void environmentRequested();

 private:
  void rebuildCameraList();
  void updateAspect();
  void savePreset();
  void loadPreset();
  SceneDocument* doc_;
  QTabWidget* tabs_;
  QComboBox* camera_;
  QComboBox* sizePreset_;
  QLabel* aspect_;
};

}  // namespace veraa
