// Small dialogs: Select by Name, OBJ import options, object properties, shortcuts list.
#pragma once

#include <QDialog>

#include "document.h"
#include "obj_import.h"

class QCheckBox;
class QDoubleSpinBox;
class QLineEdit;
class QListWidget;
class QRadioButton;
class QSpinBox;

namespace veraa {

class SelectByNameDialog : public QDialog {
  Q_OBJECT
 public:
  SelectByNameDialog(SceneDocument* doc, QWidget* parent = nullptr);
  QVector<uint32_t> chosen() const;

 private:
  QLineEdit* filter_;
  QListWidget* list_;
};

class ObjImportDialog : public QDialog {
  Q_OBJECT
 public:
  explicit ObjImportDialog(QWidget* parent = nullptr);
  ObjImportOptions options() const;

 private:
  QCheckBox* yUp_;
  QCheckBox* materials_;
  QDoubleSpinBox* scale_;
};

// Clone Options (Ctrl+V, and after Shift+drag with the Move tool).
class CloneOptionsDialog : public QDialog {
  Q_OBJECT
 public:
  // Copy: independent; Instance: shares object and modifiers; Reference: shares the base
  // object, modifiers are separate copies.
  enum Mode { Copy = 0, Instance = 1, Reference = 2 };
  CloneOptionsDialog(const QString& defaultName, bool showCopies, QWidget* parent = nullptr);
  int copies() const;
  QString name() const;
  Mode mode() const;

 private:
  QSpinBox* copies_ = nullptr;
  QLineEdit* name_;
  QRadioButton* instance_ = nullptr;
  QRadioButton* reference_ = nullptr;
};

// Makes `cloneId` (a copy of `sourceId`) an instance or reference of the source.
void applyCloneMode(Scene& scene, uint32_t sourceId, uint32_t cloneId, CloneOptionsDialog::Mode mode);

class ObjectPropertiesDialog : public QDialog {
  Q_OBJECT
 public:
  ObjectPropertiesDialog(SceneDocument* doc, Node* node, QWidget* parent = nullptr);
};

void showShortcutsDialog(QWidget* parent);

}  // namespace veraa
