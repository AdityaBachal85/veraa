// Command panel tabs: Hierarchy (pivot, working pivot, adjust transform, link info), Display
// (display color, hide by category, hide, freeze, display properties) and Utilities.
#pragma once

#include <QHash>
#include <QScrollArea>

#include "vmath.h"

class QCheckBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QStackedWidget;
class QTableWidget;
class QVBoxLayout;

namespace veraa {

class MainWindow;
class Node;
class SceneDocument;

// Pivot modes of Hierarchy > Pivot > Adjust Pivot.
enum class PivotEditMode { None, AffectPivotOnly, AffectObjectOnly, AffectHierarchyOnly };

class HierarchyPanel : public QScrollArea {
  Q_OBJECT
 public:
  explicit HierarchyPanel(MainWindow* w);
  PivotEditMode pivotMode() const { return mode_; }
  void setPivotMode(PivotEditMode m);
  // Pivot operations (each one undo step) on the selection.
  void centerPivotToObject();
  void alignPivotToObject();
  void alignPivotToWorld();
  void resetPivot();
  void resetTransform();
  void resetScale();

 private:
  void cacheTransforms();
  void compensate(const QVector<uint32_t>& ids);
  void refreshLinkInfo();

  MainWindow* w_;
  SceneDocument* doc_;
  PivotEditMode mode_ = PivotEditMode::None;
  QPushButton* modeButtons_[3] = {};
  QHash<uint32_t, Mat4> nodeWorld_, geomWorld_;
  bool compensating_ = false;
  QStackedWidget* pages_ = nullptr;
  QLabel* linkInfo_ = nullptr;
  QCheckBox* locks_[9] = {};
  bool refreshing_ = false;
};

class DisplayPanel : public QScrollArea {
  Q_OBJECT
 public:
  explicit DisplayPanel(MainWindow* w);

 private:
  void refresh();
  MainWindow* w_;
  SceneDocument* doc_;
  QCheckBox* categories_[6] = {};
  QCheckBox* props_[3] = {};
  QCheckBox* hideFrozen_ = nullptr;
  QRadioButton *wireObject_ = nullptr, *wireMaterial_ = nullptr, *shadedObject_ = nullptr, *shadedMaterial_ = nullptr;
  bool refreshing_ = false;
};

class UtilitiesPanel : public QScrollArea {
  Q_OBJECT
 public:
  explicit UtilitiesPanel(MainWindow* w);
  // Opens a utility by name ("Measure", "Polygon Counter", ...).
  void openUtility(const QString& name);
  QStringList utilityNames() const;
  // Measure utility values for a node: surface area, volume, center of mass.
  static void measure(const Node& n, double* area, double* volume, Vec3* center);
  // Reset XForm: bakes rotation, scale and pivot offset into an Editable Poly mesh.
  static void resetXForm(SceneDocument* doc, const QVector<uint32_t>& ids);
  // Collapses the modifier stacks to Editable Poly.
  static void collapse(SceneDocument* doc, const QVector<uint32_t>& ids);
  // Scales the scene (or nodes) by a factor around the world origin.
  static void rescale(SceneDocument* doc, double factor, bool selectionOnly);

 private:
  QWidget* buildUtility(const QString& name);
  void rebuildButtons();

  MainWindow* w_;
  SceneDocument* doc_;
  QWidget* buttons_ = nullptr;
  QVBoxLayout* utilityHost_ = nullptr;
  QWidget* current_ = nullptr;
  QString currentName_;
};

}  // namespace veraa
