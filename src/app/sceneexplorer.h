// Scene Explorer: the object hierarchy with columns (name, type, hidden, frozen, renderable,
// layer), category filters, find, sorting, inline rename, drag-to-link and a context menu.
#pragma once

#include <QTreeWidget>
#include <QWidget>

#include <functional>

#include "document.h"

class QLineEdit;
class QToolButton;

namespace veraa {

// Tree widget whose drops are handled by a callback (dragged items, target item or null).
class DropTree : public QTreeWidget {
  Q_OBJECT
 public:
  explicit DropTree(QWidget* parent = nullptr);
  std::function<void(const QList<QTreeWidgetItem*>& dragged, QTreeWidgetItem* target)> onDrop;

 protected:
  void dropEvent(QDropEvent* e) override;
  void dragMoveEvent(QDragMoveEvent* e) override;
};

class SceneExplorer : public QWidget {
  Q_OBJECT
 public:
  explicit SceneExplorer(SceneDocument* doc, QWidget* parent = nullptr);
  QTreeWidget* tree() const { return tree_; }
  // Links the dragged nodes under `parentId` (0 = unlink), one undo step.
  void linkNodes(const QVector<uint32_t>& ids, uint32_t parentId);
  // Category filter bits (1 << int(ObjectKind)); hidden categories are not listed.
  void setCategoryFilter(int mask);
  int categoryFilter() const { return categories_; }
  void setFindText(const QString& text);

 private:
  void rebuild();
  void syncSelection();
  void applyFilter();
  void showMenu(const QPoint& pos);
  QTreeWidgetItem* itemFor(uint32_t id) const;

  SceneDocument* doc_;
  QLineEdit* filter_;
  DropTree* tree_;
  QToolButton* catButtons_[6] = {};
  int categories_ = 63;
  bool syncing_ = false;
  bool rebuildPending_ = false;
};

}  // namespace veraa
