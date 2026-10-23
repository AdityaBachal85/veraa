// Dialogs and floaters of the Edit, Tools and Views menus.
#pragma once

#include <QDialog>

#include "document.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QTableWidget;
class QTreeWidget;
class QLineEdit;

namespace veraa {

class ColorSwatch;
class MainWindow;
class UnitSpinBox;
class ViewportPanel;

// Edit > Transform Type-In (F12): absolute and offset values for the current transform tool.
class TransformTypeInDialog : public QDialog {
  Q_OBJECT
 public:
  explicit TransformTypeInDialog(MainWindow* w);
  void refresh();

 private:
  void apply(bool offset, int axis, double v);
  MainWindow* w_;
  UnitSpinBox* abs_[3];
  UnitSpinBox* off_[3];
  QLabel* absTitle_;
  QLabel* offTitle_;
  bool updating_ = false;
};

// Edit > Transform Toolbox: quick rotate, scale, mirror, align pivot and object size.
class TransformToolbox : public QDialog {
  Q_OBJECT
 public:
  explicit TransformToolbox(MainWindow* w);

 private:
  void rotateSelection(int axis, float degrees);
  void scaleSelection(float factor);
  void alignPivot(int mode);  // 0 center, 1 min, 2 max (of the bounding box, per axis set)
  void setSize(int axis, double size);
  MainWindow* w_;
  QDoubleSpinBox* angle_;
  QDoubleSpinBox* percent_;
  QComboBox* sizeAxis_;
  UnitSpinBox* size_;
};

// Edit > Manage Selection Sets.
class SelectionSetsDialog : public QDialog {
  Q_OBJECT
 public:
  SelectionSetsDialog(SceneDocument* doc, QWidget* parent = nullptr);

 private:
  void rebuild();
  QString currentSet() const;
  SceneDocument* doc_;
  QTreeWidget* tree_;
};

// Tools > Display Floater: hide/freeze operations and per-category display.
class DisplayFloater : public QDialog {
  Q_OBJECT
 public:
  explicit DisplayFloater(MainWindow* w);

 private:
  MainWindow* w_;
};

// Tools > Manage Scene States.
class SceneStatesDialog : public QDialog {
  Q_OBJECT
 public:
  SceneStatesDialog(SceneDocument* doc, QWidget* parent = nullptr);

 private:
  void rebuild();
  SceneDocument* doc_;
  QListWidget* list_;
  QCheckBox* parts_[6];
};

// Tools > Light Lister: every light with its main settings, editable in place.
class LightListerDialog : public QDialog {
  Q_OBJECT
 public:
  LightListerDialog(SceneDocument* doc, QWidget* parent = nullptr);
  void rebuild();

 private:
  SceneDocument* doc_;
  QTableWidget* table_;
  bool rebuilding_ = false;
};

// Tools > Rename Objects.
class RenameObjectsDialog : public QDialog {
  Q_OBJECT
 public:
  RenameObjectsDialog(SceneDocument* doc, QWidget* parent = nullptr);
  // Applies the current settings to these nodes (one undo step). Returns the count renamed.
  int apply(const QVector<uint32_t>& ids);

 private:
  SceneDocument* doc_;
  QCheckBox *useBase_, *usePrefix_, *useSuffix_, *removeFirst_, *removeLast_, *numbered_;
  QLineEdit *base_, *prefix_, *suffix_;
  QSpinBox *firstN_, *lastN_, *baseNumber_, *step_;
};

// Tools > Color Clipboard: twelve swatches to copy and paste object colors.
class ColorClipboard : public QDialog {
  Q_OBJECT
 public:
  ColorClipboard(SceneDocument* doc, QWidget* parent = nullptr);

 private:
  SceneDocument* doc_;
  ColorSwatch* swatches_[12];
};

// Tools > Mesh Inspector: statistics and problems of the selected meshes.
class MeshInspectorDialog : public QDialog {
  Q_OBJECT
 public:
  MeshInspectorDialog(SceneDocument* doc, QWidget* parent = nullptr);
  struct Stats {
    size_t vertices = 0, faces = 0, triangles = 0, openEdges = 0, degenerateFaces = 0, isolatedVertices = 0,
           nonManifoldEdges = 0;
  };
  static Stats inspect(const PolyMesh& m);
};

// Views > Viewport Configuration: Layout, Display and Background tabs.
class ViewportConfigDialog : public QDialog {
  Q_OBJECT
 public:
  ViewportConfigDialog(ViewportPanel* panel, QWidget* parent = nullptr, int tab = 0);
};

// Workspaces > Manage Workspaces.
class ManageWorkspacesDialog : public QDialog {
  Q_OBJECT
 public:
  explicit ManageWorkspacesDialog(MainWindow* w);

 private:
  void rebuild();
  MainWindow* w_;
  QListWidget* list_;
};

// Tools > Mirror (used when the modeling module does not provide its own).
class MirrorDialog : public QDialog {
  Q_OBJECT
 public:
  MirrorDialog(SceneDocument* doc, QWidget* parent = nullptr);
  // Mirrors the selection about its center: axis 0..5 = X, Y, Z, XY, YZ, ZX; clone 0 none,
  // 1 copy, 2 instance, 3 reference.
  static void mirror(SceneDocument* doc, int axis, float offset, int clone);

 private:
  SceneDocument* doc_;
};

void showUserGuide(QWidget* parent);
void showWhatsNew(QWidget* parent);

}  // namespace veraa
