// Small dialogs: Select by Name, OBJ import options, object properties, shortcuts list.
#pragma once

#include <QDialog>

#include "document.h"
#include "obj_import.h"

class QCheckBox;
class QDoubleSpinBox;
class QLineEdit;
class QListWidget;
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
  CloneOptionsDialog(const QString& defaultName, bool showCopies, QWidget* parent = nullptr);
  int copies() const;
  QString name() const;

 private:
  QSpinBox* copies_ = nullptr;
  QLineEdit* name_;
};

class ObjectPropertiesDialog : public QDialog {
  Q_OBJECT
 public:
  ObjectPropertiesDialog(SceneDocument* doc, Node* node, QWidget* parent = nullptr);
};

void showShortcutsDialog(QWidget* parent);

}  // namespace veraa
