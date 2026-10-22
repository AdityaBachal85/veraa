// Layer Explorer: layers with their objects; create, delete, rename, set current, hide, freeze,
// renderable and color per layer; drag objects onto a layer to move them.
#pragma once

#include <QWidget>

#include "document.h"

class QTreeWidgetItem;

namespace veraa {

class DropTree;

class LayerExplorer : public QWidget {
  Q_OBJECT
 public:
  explicit LayerExplorer(SceneDocument* doc, QWidget* parent = nullptr);

  // One undo step each.
  uint32_t createLayer(const QString& name, bool addSelection);
  void deleteLayer(uint32_t id);
  void moveToLayer(const QVector<uint32_t>& nodeIds, uint32_t layerId);
  void setCurrentLayer(uint32_t id);
  void setLayerFlag(uint32_t id, int flag, bool value);  // 0 hidden, 1 frozen, 2 renderable

 private:
  void rebuild();
  uint32_t currentLayerOfItem(QTreeWidgetItem* it) const;
  void showMenu(const QPoint& pos);

  SceneDocument* doc_;
  DropTree* tree_;
  bool syncing_ = false;
  bool rebuildPending_ = false;
};

}  // namespace veraa
