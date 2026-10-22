// Small dialogs: Select by Name, OBJ import options, object properties, shortcuts list.
#pragma once

#include <QDialog>

#include "document.h"
#include "obj_import.h"

class QCheckBox;
class QDoubleSpinBox;
class QLineEdit;
class QListWidget;

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

class ObjectPropertiesDialog : public QDialog {
  Q_OBJECT
 public:
  ObjectPropertiesDialog(SceneDocument* doc, Node* node, QWidget* parent = nullptr);
};

void showShortcutsDialog(QWidget* parent);

}  // namespace veraa
