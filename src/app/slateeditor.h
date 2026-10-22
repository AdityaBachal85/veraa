// Slate Material Editor: the material tree as a node graph. Maps and sub-materials are nodes
// wired into the sockets of their parents; new nodes come from the browser on the left, the
// selected node's parameters are edited on the right.
#pragma once

#include <QGraphicsView>
#include <QMainWindow>

#include <memory>
#include <vector>

#include "document.h"
#include "materialwidgets.h"

class QGraphicsScene;
class QTreeWidget;
class QLabel;

namespace veraa {

class SlateNodeItem;

class SlateGraphView : public QGraphicsView {
  Q_OBJECT
 public:
  explicit SlateGraphView(QWidget* parent = nullptr);

 Q_SIGNALS:
  // Drag from an output port to an input socket finished.
  void connectRequested(SlateNodeItem* from, SlateNodeItem* to, int socket, bool copy);
  // A browser entry was dropped (type "map:<Type>" or "mtl:<Type>") at a scene position.
  void dropRequested(const QString& what, const QPointF& scenePos);
  void deleteRequested();

 protected:
  void mousePressEvent(QMouseEvent* e) override;
  void mouseMoveEvent(QMouseEvent* e) override;
  void mouseReleaseEvent(QMouseEvent* e) override;
  void wheelEvent(QWheelEvent* e) override;
  void keyPressEvent(QKeyEvent* e) override;
  void dragEnterEvent(QDragEnterEvent* e) override;
  void dragMoveEvent(QDragMoveEvent* e) override;
  void dropEvent(QDropEvent* e) override;

 private:
  SlateNodeItem* dragFrom_ = nullptr;
  QGraphicsPathItem* tempWire_ = nullptr;
  QPoint panLast_;
  bool panning_ = false;
};

class SlateEditor : public QMainWindow {
  Q_OBJECT
 public:
  explicit SlateEditor(SceneDocument* doc, QWidget* parent = nullptr);
  void showMaterial(uint32_t id);
  uint32_t materialId() const { return materialId_; }
  // Number of nodes in the view (tests).
  int nodeCount() const;
  // Connects a new map of the given type into a socket of the root material (tests and scripts).
  bool addMapToRoot(const std::string& key, const std::string& type);

 private:
  void rebuildGraph();
  void rebuildMaterialList();
  void selectNode(SlateNodeItem* item);
  void connectNodes(SlateNodeItem* from, SlateNodeItem* to, int socket, bool copy);
  void dropItem(const QString& what, const QPointF& pos);
  void deleteSelection();
  void layoutAll();

  SceneDocument* doc_;
  uint32_t materialId_ = 0;
  QGraphicsScene* scene_;
  SlateGraphView* view_;
  QTreeWidget* browser_;
  QTreeWidget* materials_;
  LevelEditor* level_;
  QLabel* title_;
  // Nodes not connected to the material (not saved with the scene).
  std::vector<std::unique_ptr<TexMap>> floatingMaps_;
  std::vector<std::unique_ptr<Material>> floatingMtls_;
  std::vector<QPointF> floatingMapPos_, floatingMtlPos_;
  std::map<std::string, QPointF> positions_;  // node positions by path key
};

}  // namespace veraa
