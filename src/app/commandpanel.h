// Command panel on the right: Create tab (object types) and Modify tab (modifier stack).
#pragma once

#include <QHash>
#include <QTabWidget>

#include "document.h"
#include "editorstate.h"

class QButtonGroup;
class QComboBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QScrollArea;
class QToolButton;

namespace veraa {

class ColorSwatch;
class ParamForm;
class Rollout;

class CommandPanel : public QTabWidget {
  Q_OBJECT
 public:
  CommandPanel(SceneDocument* doc, EditorState* state, QWidget* parent = nullptr);
  void showCreate();
  void showModify();
  void addModifier(const QString& type);  // to every selected geometry node

 private:
  QWidget* buildCreateTab();
  QWidget* buildModifyTab();
  void setCategory(int c);
  void rebuildTypeButtons();
  void syncCreate();
  void rebuildModify();
  void refreshModifyValues();
  void selectStackRow(int row);
  // Stack index: -1 = base object, otherwise modifier index (bottom = 0).
  int currentStackIndex() const;
  void wireForm(ParamForm* form, uint32_t nodeId, int stackIndex);

  SceneDocument* doc_;
  EditorState* state_;

  // Create tab: categories are the object registry's categories (Geometry, Shapes, Lights, ...).
  int category_ = 0;
  QString subcategory_;
  QToolButton* catButtons_[7] = {};
  QComboBox* subCombo_ = nullptr;
  QGridLayout* typeGrid_ = nullptr;
  QButtonGroup* typeGroup_ = nullptr;
  Rollout* nameRollout_ = nullptr;
  QLineEdit* createName_ = nullptr;
  ColorSwatch* createColor_ = nullptr;
  ParamForm* createForm_ = nullptr;
  uint32_t createFormNode_ = 0;

  // Modify tab.
  QLineEdit* name_ = nullptr;
  ColorSwatch* color_ = nullptr;
  QComboBox* modList_ = nullptr;
  QListWidget* stack_ = nullptr;
  QToolButton *delMod_ = nullptr, *upMod_ = nullptr, *downMod_ = nullptr;
  QLabel* modInfo_ = nullptr;
  QWidget* modExtraHost_ = nullptr;  // holds type-specific rollouts (see registerModifyRollouts)
  QWidget* modExtra_ = nullptr;
  ParamForm* modForm_ = nullptr;
  uint32_t modNode_ = 0;
  QHash<uint32_t, int> stackChoice_;  // node -> chosen stack index
  bool rebuilding_ = false;
};

}  // namespace veraa
