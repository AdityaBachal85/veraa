// Command panel on the right: Create tab (object types) and Modify tab (modifier stack).
#pragma once

#include <vector>

#include <QHash>
#include <QTabWidget>

#include "createlayout.h"
#include "document.h"
#include "editorstate.h"

class QButtonGroup;
class QCheckBox;
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
  // Starts creating `type` from the Create panel; `label` picks the button when several buttons
  // create the same type (Cameras: Target / Free).
  void beginCreate(const QString& type, const QString& label = QString());
  // Create panel state (self-tests): category index, list name, button labels of the list.
  int createCategory() const { return category_; }
  QString createList() const { return subcategory_; }
  void setCreateCategory(int c) { setCategory(c); }
  void setCreateList(const QString& name);
  QStringList createButtonLabels() const;

 Q_SIGNALS:
  // A Create panel entry for something Veraa does not have yet was clicked.
  void featureNotAvailable(const QString& feature);

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

  // Create tab: categories and lists follow createlayout.h (3ds Max order, then registry extras).
  int category_ = 0;
  QString subcategory_;
  std::vector<CreateList> lists_;
  QString createLabel_, createLabelType_;  // the button that started the current creation
  QCheckBox* autoGrid_ = nullptr;
  QLabel* listNote_ = nullptr;
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
  // The object and modifiers the Modify tab was built for. Tools that replace them (Convert
  // To, Collapse, ...) leave the forms pointing at freed parameters, so a change makes the
  // tab rebuild instead of refresh.
  std::vector<const void*> modBuiltFor_;
  const void* createFormObject_ = nullptr;  // object whose parameters createForm_ shows
  QHash<uint32_t, int> stackChoice_;  // node -> chosen stack index
  bool rebuilding_ = false;
};

}  // namespace veraa
