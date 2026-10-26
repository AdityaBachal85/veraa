// Scene Explorer: the object hierarchy with columns (name, type, hidden, frozen, renderable,
// layer), category filters, find, sorting, inline rename, drag-to-link and a context menu.
//
// It is a model/view: rows are read from the scene on demand, so scenes with many thousands of
// objects list instantly. Structure changes rebuild only the id hierarchy; name/display changes
// repaint the affected rows.
#pragma once

#include <QAbstractItemModel>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QTreeView>
#include <QTreeWidget>
#include <QWidget>

#include <functional>
#include <unordered_map>
#include <vector>

#include "document.h"

class QLineEdit;
class QToolButton;

namespace veraa {

// The scene's node hierarchy as an item model (internal id = node id).
class SceneTreeModel : public QAbstractItemModel {
  Q_OBJECT
 public:
  enum Column { kName, kType, kHidden, kFrozen, kRender, kLayer, kColumns };
  static constexpr int kIdRole = Qt::UserRole + 1;
  static constexpr int kKindRole = Qt::UserRole + 2;

  explicit SceneTreeModel(SceneDocument* doc, QObject* parent = nullptr);
  // Re-reads the hierarchy from the scene.
  void rebuild();
  // Repaints the rows of these nodes (all rows when ids is empty).
  void refresh(const QVector<uint32_t>& ids);
  QModelIndex indexOf(uint32_t id, int column = 0) const;
  static uint32_t idOf(const QModelIndex& index) { return index.isValid() ? static_cast<uint32_t>(index.internalId()) : 0; }

  QModelIndex index(int row, int column, const QModelIndex& parent = QModelIndex()) const override;
  QModelIndex parent(const QModelIndex& child) const override;
  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  int columnCount(const QModelIndex& parent = QModelIndex()) const override;
  QVariant data(const QModelIndex& index, int role) const override;
  bool setData(const QModelIndex& index, const QVariant& value, int role) override;
  QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
  Qt::ItemFlags flags(const QModelIndex& index) const override;
  Qt::DropActions supportedDropActions() const override { return Qt::MoveAction; }

 Q_SIGNALS:
  void renameRequested(uint32_t id, const QString& name);

 private:
  struct Entry {
    uint32_t parent = 0;
    int row = 0;
    std::vector<uint32_t> children;
  };
  const std::vector<uint32_t>& childrenOf(uint32_t id) const;
  SceneDocument* doc_;
  std::vector<uint32_t> roots_;
  std::unordered_map<uint32_t, Entry> entries_;
};

// Category (object kind) and find-text filter; a parent stays listed when a child matches.
class SceneFilterModel : public QSortFilterProxyModel {
  Q_OBJECT
 public:
  using QSortFilterProxyModel::QSortFilterProxyModel;
  void setFilter(int categories, const QString& text);

 protected:
  bool filterAcceptsRow(int row, const QModelIndex& parent) const override;
  bool lessThan(const QModelIndex& a, const QModelIndex& b) const override;

 private:
  int categories_ = 63;
  QString text_;
};

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

// Tree view whose drops are handled by a callback (dragged node ids, target node id or 0).
class SceneDropView : public QTreeView {
  Q_OBJECT
 public:
  explicit SceneDropView(QWidget* parent = nullptr);
  std::function<void(const QVector<uint32_t>& dragged, uint32_t target)> onDrop;
  QVector<uint32_t> selectedIds() const;

 protected:
  void dropEvent(QDropEvent* e) override;
  void dragMoveEvent(QDragMoveEvent* e) override;
};

class SceneExplorer : public QWidget {
  Q_OBJECT
 public:
  explicit SceneExplorer(SceneDocument* doc, QWidget* parent = nullptr);
  QTreeView* tree() const { return tree_; }
  SceneTreeModel* model() const { return model_; }
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

  SceneDocument* doc_;
  SceneTreeModel* model_;
  SceneFilterModel* proxy_;
  QLineEdit* filter_;
  SceneDropView* tree_;
  QToolButton* catButtons_[6] = {};
  int categories_ = 63;
  bool syncing_ = false;
  bool rebuildPending_ = false;
  QSet<uint32_t> collapsed_;  // nodes the user collapsed (kept across rebuilds)
};

}  // namespace veraa
