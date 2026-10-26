#include "sceneexplorer.h"

#include <QDropEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelection>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QMenu>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

#include "icons.h"

namespace veraa {

namespace {

using Col = SceneTreeModel::Column;

const QIcon& kindIcon(ObjectKind k) {
  // Icons are painted on creation; make each once.
  static QIcon cache[8];
  int i = std::clamp(static_cast<int>(k), 0, 7);
  if (cache[i].isNull()) {
    switch (k) {
      case ObjectKind::Light: cache[i] = icons::light(); break;
      case ObjectKind::Camera: cache[i] = icons::camera(); break;
      case ObjectKind::Shape: cache[i] = icons::shapes(); break;
      case ObjectKind::Helper: cache[i] = icons::helpers(); break;
      case ObjectKind::SpaceWarp: cache[i] = icons::spaceWarps(); break;
      default: cache[i] = icons::object(); break;
    }
  }
  return cache[i];
}

const QIcon& eyeIcon(bool open) {
  static QIcon on = icons::eye(true), off = icons::eye(false);
  return open ? on : off;
}

}  // namespace

// ---------------------------------------------------------------------------------------------

SceneTreeModel::SceneTreeModel(SceneDocument* doc, QObject* parent) : QAbstractItemModel(parent), doc_(doc) {}

void SceneTreeModel::rebuild() {
  beginResetModel();
  roots_.clear();
  entries_.clear();
  const Scene& s = doc_->scene();
  entries_.reserve(s.nodes.size() * 2);
  for (const auto& n : s.nodes) entries_[n->id];
  for (const auto& n : s.nodes) {
    Entry& e = entries_[n->id];
    uint32_t parent = n->parentId && entries_.count(n->parentId) ? n->parentId : 0;
    e.parent = parent;
    std::vector<uint32_t>& list = parent ? entries_[parent].children : roots_;
    e.row = static_cast<int>(list.size());
    list.push_back(n->id);
  }
  endResetModel();
}

void SceneTreeModel::refresh(const QVector<uint32_t>& ids) {
  if (entries_.empty()) return;
  if (ids.isEmpty()) {
    // One signal per sibling list.
    std::function<void(const QModelIndex&, const std::vector<uint32_t>&)> walk = [&](const QModelIndex& parent,
                                                                                      const std::vector<uint32_t>& list) {
      if (list.empty()) return;
      Q_EMIT dataChanged(index(0, 0, parent), index(static_cast<int>(list.size()) - 1, kColumns - 1, parent));
      for (size_t r = 0; r < list.size(); ++r)
        if (!entries_.at(list[r]).children.empty()) walk(index(static_cast<int>(r), 0, parent), entries_.at(list[r]).children);
    };
    walk(QModelIndex(), roots_);
    return;
  }
  for (uint32_t id : ids) {
    QModelIndex a = indexOf(id, 0);
    if (a.isValid()) Q_EMIT dataChanged(a, a.siblingAtColumn(kColumns - 1));
  }
}

const std::vector<uint32_t>& SceneTreeModel::childrenOf(uint32_t id) const {
  if (id == 0) return roots_;
  static const std::vector<uint32_t> none;
  auto it = entries_.find(id);
  return it == entries_.end() ? none : it->second.children;
}

QModelIndex SceneTreeModel::indexOf(uint32_t id, int column) const {
  auto it = entries_.find(id);
  if (it == entries_.end()) return QModelIndex();
  return createIndex(it->second.row, column, static_cast<quintptr>(id));
}

QModelIndex SceneTreeModel::index(int row, int column, const QModelIndex& parent) const {
  if (row < 0 || column < 0 || column >= kColumns) return QModelIndex();
  const std::vector<uint32_t>& list = childrenOf(idOf(parent));
  if (static_cast<size_t>(row) >= list.size()) return QModelIndex();
  return createIndex(row, column, static_cast<quintptr>(list[static_cast<size_t>(row)]));
}

QModelIndex SceneTreeModel::parent(const QModelIndex& child) const {
  auto it = entries_.find(idOf(child));
  if (it == entries_.end() || it->second.parent == 0) return QModelIndex();
  return indexOf(it->second.parent, 0);
}

int SceneTreeModel::rowCount(const QModelIndex& parent) const {
  if (parent.isValid() && parent.column() != 0) return 0;
  return static_cast<int>(childrenOf(idOf(parent)).size());
}

int SceneTreeModel::columnCount(const QModelIndex&) const { return kColumns; }

QVariant SceneTreeModel::headerData(int section, Qt::Orientation orientation, int role) const {
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return QVariant();
  switch (section) {
    case kName: return tr("Name");
    case kType: return tr("Type");
    case kHidden: return tr("Hidden");
    case kFrozen: return tr("Frozen");
    case kRender: return tr("Render");
    case kLayer: return tr("Layer");
    default: return QVariant();
  }
}

QVariant SceneTreeModel::data(const QModelIndex& index, int role) const {
  const Scene& s = doc_->scene();
  const Node* n = s.findNode(idOf(index));
  if (!n) return QVariant();
  if (role == kIdRole) return n->id;
  if (role == kKindRole) return static_cast<int>(n->kind());
  switch (index.column()) {
    case kName:
      if (role == Qt::DisplayRole || role == Qt::EditRole) return QString::fromStdString(n->name);
      if (role == Qt::DecorationRole) return kindIcon(n->kind());
      if (role == Qt::ForegroundRole && !s.isNodeVisible(*n)) return QColor(130, 130, 130);
      break;
    case kType:
      if (role == Qt::DisplayRole) {
        if (n->groupHead) return tr("Group");
        return n->object ? QString::fromStdString(objectDisplayName(n->object->type())) : QString();
      }
      break;
    case kHidden:
      if (role == Qt::DecorationRole) return eyeIcon(!n->hidden);
      if (role == Qt::ToolTipRole) return n->hidden ? tr("Hidden (click to unhide)") : tr("Visible (click to hide)");
      if (role == Qt::DisplayRole) return QVariant();
      if (role == Qt::UserRole) return n->hidden;
      break;
    case kFrozen:
      if (role == Qt::DisplayRole) return n->frozen ? QString::fromUtf8("❄") : QString();
      if (role == Qt::ToolTipRole) return n->frozen ? tr("Frozen (click to unfreeze)") : tr("Click to freeze");
      break;
    case kRender:
      if (role == Qt::DisplayRole) return n->renderable ? QString::fromUtf8("✓") : QString();
      break;
    case kLayer:
      if (role == Qt::DisplayRole) {
        const Layer* l = s.findLayer(n->layerId);
        return l ? QString::fromStdString(l->name) : QString();
      }
      break;
    default: break;
  }
  return QVariant();
}

bool SceneTreeModel::setData(const QModelIndex& index, const QVariant& value, int role) {
  if (role != Qt::EditRole || index.column() != kName) return false;
  Q_EMIT renameRequested(idOf(index), value.toString());
  return true;
}

Qt::ItemFlags SceneTreeModel::flags(const QModelIndex& index) const {
  if (!index.isValid()) return Qt::ItemIsDropEnabled;
  Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled;
  if (index.column() == kName) f |= Qt::ItemIsEditable;
  return f;
}

// ---------------------------------------------------------------------------------------------

void SceneFilterModel::setFilter(int categories, const QString& text) {
  categories_ = categories;
  text_ = text;
  invalidateFilter();
}

bool SceneFilterModel::filterAcceptsRow(int row, const QModelIndex& parent) const {
  QModelIndex i = sourceModel()->index(row, SceneTreeModel::kName, parent);
  int kind = i.data(SceneTreeModel::kKindRole).toInt();
  if (!((categories_ >> kind) & 1)) return false;
  return text_.isEmpty() || i.data(Qt::DisplayRole).toString().contains(text_, Qt::CaseInsensitive);
}

bool SceneFilterModel::lessThan(const QModelIndex& a, const QModelIndex& b) const {
  QVariant va = a.data(Qt::DisplayRole), vb = b.data(Qt::DisplayRole);
  if (a.column() == SceneTreeModel::kHidden) return a.data(Qt::UserRole).toBool() < b.data(Qt::UserRole).toBool();
  return QString::compare(va.toString(), vb.toString(), Qt::CaseInsensitive) < 0;
}

// ---------------------------------------------------------------------------------------------

DropTree::DropTree(QWidget* parent) : QTreeWidget(parent) {
  setDragEnabled(true);
  setAcceptDrops(true);
  setDropIndicatorShown(true);
  setDragDropMode(QAbstractItemView::InternalMove);
}

void DropTree::dragMoveEvent(QDragMoveEvent* e) {
  QTreeWidget::dragMoveEvent(e);
  e->acceptProposedAction();
}

void DropTree::dropEvent(QDropEvent* e) {
  QTreeWidgetItem* target = itemAt(e->position().toPoint());
  QList<QTreeWidgetItem*> dragged = selectedItems();
  e->setDropAction(Qt::IgnoreAction);
  e->accept();
  if (onDrop) onDrop(dragged, target);
}

// ---------------------------------------------------------------------------------------------

SceneDropView::SceneDropView(QWidget* parent) : QTreeView(parent) {
  setDragEnabled(true);
  setAcceptDrops(true);
  setDropIndicatorShown(true);
  setDragDropMode(QAbstractItemView::InternalMove);
}

QVector<uint32_t> SceneDropView::selectedIds() const {
  QVector<uint32_t> ids;
  if (!selectionModel()) return ids;
  for (const QModelIndex& i : selectionModel()->selectedRows(0)) ids.push_back(i.data(SceneTreeModel::kIdRole).toUInt());
  return ids;
}

void SceneDropView::dragMoveEvent(QDragMoveEvent* e) {
  QTreeView::dragMoveEvent(e);
  e->acceptProposedAction();
}

void SceneDropView::dropEvent(QDropEvent* e) {
  QModelIndex target = indexAt(e->position().toPoint());
  QVector<uint32_t> dragged = selectedIds();
  e->setDropAction(Qt::IgnoreAction);
  e->accept();
  if (onDrop) onDrop(dragged, target.isValid() ? target.data(SceneTreeModel::kIdRole).toUInt() : 0);
}

// ---------------------------------------------------------------------------------------------

SceneExplorer::SceneExplorer(SceneDocument* doc, QWidget* parent) : QWidget(parent), doc_(doc) {
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(2, 2, 2, 2);
  v->setSpacing(2);
  // Category filters.
  auto* cats = new QHBoxLayout;
  cats->setSpacing(1);
  const struct {
    ObjectKind kind;
    const char* tip;
  } kinds[6] = {{ObjectKind::Geometry, "Display Geometry"}, {ObjectKind::Shape, "Display Shapes"},
                {ObjectKind::Light, "Display Lights"},       {ObjectKind::Camera, "Display Cameras"},
                {ObjectKind::Helper, "Display Helpers"},     {ObjectKind::SpaceWarp, "Display Space Warps"}};
  for (int i = 0; i < 6; ++i) {
    auto* b = new QToolButton(this);
    b->setIcon(kindIcon(kinds[i].kind));
    b->setCheckable(true);
    b->setChecked(true);
    b->setAutoRaise(true);
    b->setToolTip(tr(kinds[i].tip));
    int bit = 1 << static_cast<int>(kinds[i].kind);
    connect(b, &QToolButton::toggled, this, [this, bit](bool on) {
      categories_ = on ? (categories_ | bit) : (categories_ & ~bit);
      applyFilter();
    });
    catButtons_[i] = b;
    cats->addWidget(b);
  }
  cats->addStretch(1);
  auto* expand = new QToolButton(this);
  expand->setText("+");
  expand->setToolTip(tr("Expand All"));
  expand->setAutoRaise(true);
  auto* collapse = new QToolButton(this);
  collapse->setText("-");
  collapse->setToolTip(tr("Collapse All"));
  collapse->setAutoRaise(true);
  cats->addWidget(expand);
  cats->addWidget(collapse);
  v->addLayout(cats);
  filter_ = new QLineEdit(this);
  filter_->setPlaceholderText(tr("Find..."));
  filter_->setClearButtonEnabled(true);
  v->addWidget(filter_);

  model_ = new SceneTreeModel(doc_, this);
  proxy_ = new SceneFilterModel(this);
  proxy_->setSourceModel(model_);
  proxy_->setRecursiveFilteringEnabled(true);
  proxy_->setDynamicSortFilter(true);
  tree_ = new SceneDropView(this);
  tree_->setModel(proxy_);
  tree_->setRootIsDecorated(true);
  tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  tree_->setSelectionBehavior(QAbstractItemView::SelectRows);
  tree_->setEditTriggers(QAbstractItemView::EditKeyPressed | QAbstractItemView::DoubleClicked);
  tree_->setUniformRowHeights(true);
  tree_->setSortingEnabled(true);
  tree_->sortByColumn(Col::kName, Qt::AscendingOrder);
  tree_->header()->setStretchLastSection(false);
  tree_->header()->setSectionResizeMode(Col::kName, QHeaderView::Stretch);
  // Fixed widths: sizing to contents would measure every row.
  for (int c = Col::kType; c < Col::kColumns; ++c) tree_->header()->setSectionResizeMode(c, QHeaderView::Interactive);
  tree_->header()->resizeSection(Col::kType, 90);
  tree_->header()->resizeSection(Col::kHidden, 46);
  tree_->header()->resizeSection(Col::kFrozen, 46);
  tree_->header()->resizeSection(Col::kRender, 46);
  tree_->header()->resizeSection(Col::kLayer, 70);
  tree_->setIconSize(QSize(16, 16));
  tree_->setContextMenuPolicy(Qt::CustomContextMenu);
  v->addWidget(tree_);

  connect(expand, &QToolButton::clicked, this, [this] {
    collapsed_.clear();
    tree_->expandAll();
  });
  connect(collapse, &QToolButton::clicked, tree_, &QTreeView::collapseAll);
  connect(tree_, &QTreeView::collapsed, this, [this](const QModelIndex& i) {
    if (!syncing_) collapsed_.insert(i.data(SceneTreeModel::kIdRole).toUInt());
  });
  connect(tree_, &QTreeView::expanded, this, [this](const QModelIndex& i) {
    if (!syncing_) collapsed_.remove(i.data(SceneTreeModel::kIdRole).toUInt());
  });
  connect(filter_, &QLineEdit::textChanged, this, &SceneExplorer::applyFilter);
  auto schedule = [this] {
    if (rebuildPending_) return;
    rebuildPending_ = true;
    QMetaObject::invokeMethod(this, [this] { rebuild(); }, Qt::QueuedConnection);
  };
  connect(doc_, &SceneDocument::structureChanged, this, schedule);
  connect(doc_, &SceneDocument::nodesChanged, this, [this](const QVector<uint32_t>& ids, int flags) {
    if (rebuildPending_) return;
    // Hidden/frozen state also greys children and layer members: repaint every row.
    if (flags & ChangeDisplay) model_->refresh({});
    else if (flags & ChangeName) model_->refresh(ids);
    if (flags & (ChangeName | ChangeDisplay)) proxy_->invalidate();
  });
  connect(doc_, &SceneDocument::selectionChanged, this, &SceneExplorer::syncSelection);
  connect(tree_->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] {
    if (syncing_) return;
    doc_->setSelection(tree_->selectedIds());
  });
  connect(model_, &SceneTreeModel::renameRequested, this, [this](uint32_t id, const QString& text) {
    Node* n = doc_->scene().findNode(id);
    if (!n) return;
    std::string name = text.trimmed().toStdString();
    if (name.empty() || name == n->name) return;
    doc_->beginEdit(tr("Rename"));
    n->name = name;
    doc_->notifyNodeChanged(n, ChangeName);
    doc_->endEdit();
  });
  // Clicking the Hidden / Frozen / Render cells toggles them.
  connect(tree_, &QTreeView::clicked, this, [this](const QModelIndex& index) {
    int col = index.column();
    if (col != Col::kHidden && col != Col::kFrozen && col != Col::kRender) return;
    Node* n = doc_->scene().findNode(index.data(SceneTreeModel::kIdRole).toUInt());
    if (!n) return;
    doc_->beginEdit(col == Col::kHidden ? tr("Hide") : col == Col::kFrozen ? tr("Freeze") : tr("Renderable"));
    if (col == Col::kHidden) n->hidden = !n->hidden;
    else if (col == Col::kFrozen) n->frozen = !n->frozen;
    else n->renderable = !n->renderable;
    doc_->notifyNodeChanged(n, ChangeDisplay);
    doc_->endEdit();
  });
  tree_->onDrop = [this](const QVector<uint32_t>& ids, uint32_t target) { linkNodes(ids, target); };
  connect(tree_, &QWidget::customContextMenuRequested, this, &SceneExplorer::showMenu);
  rebuild();
}

void SceneExplorer::linkNodes(const QVector<uint32_t>& ids, uint32_t parentId) {
  Scene& s = doc_->scene();
  QVector<uint32_t> todo;
  for (uint32_t id : ids)
    if (id != parentId && s.findNode(id) && s.findNode(id)->parentId != parentId) todo.push_back(id);
  if (todo.isEmpty()) return;
  doc_->beginEdit(parentId ? tr("Link") : tr("Unlink"));
  for (uint32_t id : todo) s.setParent(id, parentId);
  doc_->notifyNodesChanged(todo, ChangeTransform);
  doc_->notifyStructureChanged();
  doc_->endEdit();
}

void SceneExplorer::setCategoryFilter(int mask) {
  for (int i = 0; i < 6; ++i) {
    QSignalBlocker b(catButtons_[i]);
    catButtons_[i]->setChecked((mask >> i) & 1);
  }
  categories_ = mask;
  applyFilter();
}

void SceneExplorer::setFindText(const QString& text) { filter_->setText(text); }

void SceneExplorer::rebuild() {
  rebuildPending_ = false;
  syncing_ = true;
  model_->rebuild();
  // Everything starts expanded except what the user collapsed.
  tree_->expandAll();
  for (uint32_t id : collapsed_) {
    QModelIndex i = proxy_->mapFromSource(model_->indexOf(id));
    if (i.isValid()) tree_->collapse(i);
  }
  syncing_ = false;
  syncSelection();
}

void SceneExplorer::syncSelection() {
  if (rebuildPending_) return;  // the rebuild syncs afterwards
  syncing_ = true;
  // Selected rows as ranges of consecutive rows under the same parent (one range per run).
  std::vector<QModelIndex> rows;
  rows.reserve(static_cast<size_t>(doc_->selection().size()));
  for (uint32_t id : doc_->selection()) {
    QModelIndex i = proxy_->mapFromSource(model_->indexOf(id));
    if (i.isValid()) rows.push_back(i);
  }
  std::sort(rows.begin(), rows.end(), [](const QModelIndex& a, const QModelIndex& b) {
    if (a.parent() != b.parent()) return a.parent().internalId() < b.parent().internalId();
    return a.row() < b.row();
  });
  QItemSelection sel;
  for (size_t i = 0; i < rows.size();) {
    size_t j = i + 1;
    while (j < rows.size() && rows[j].parent() == rows[i].parent() && rows[j].row() == rows[j - 1].row() + 1) ++j;
    sel.select(rows[i], rows[j - 1].siblingAtColumn(Col::kColumns - 1));
    i = j;
  }
  tree_->selectionModel()->select(sel, QItemSelectionModel::ClearAndSelect);
  if (!rows.empty()) tree_->scrollTo(proxy_->mapFromSource(model_->indexOf(doc_->selection().back())));
  syncing_ = false;
}

void SceneExplorer::applyFilter() { proxy_->setFilter(categories_, filter_->text().trimmed()); }

void SceneExplorer::showMenu(const QPoint& pos) {
  QMenu menu(this);
  bool has = !doc_->selection().isEmpty();
  menu.addAction(tr("Rename"), this, [this] {
    QModelIndex i = tree_->currentIndex();
    if (i.isValid()) tree_->edit(i.siblingAtColumn(Col::kName));
  })->setEnabled(has);
  menu.addAction(tr("Select Children"), this, [this] {
    QVector<uint32_t> ids;
    std::function<void(uint32_t)> add = [&](uint32_t id) {
      ids.push_back(id);
      for (Node* c : doc_->scene().children(id)) add(c->id);
    };
    for (uint32_t id : doc_->selection()) add(id);
    doc_->setSelection(ids);
  })->setEnabled(has);
  menu.addAction(tr("Unlink"), this, [this] { linkNodes(doc_->selection(), 0); })->setEnabled(has);
  menu.addSeparator();
  auto setFlag = [this](bool Node::*flag, bool value, const QString& label) {
    doc_->beginEdit(label);
    QVector<uint32_t> ids = doc_->selection();
    for (Node* n : doc_->selectedNodes()) n->*flag = value;
    doc_->notifyNodesChanged(ids, ChangeDisplay);
    doc_->endEdit();
  };
  menu.addAction(tr("Hide"), this, [setFlag] { setFlag(&Node::hidden, true, tr("Hide")); })->setEnabled(has);
  menu.addAction(tr("Unhide"), this, [setFlag] { setFlag(&Node::hidden, false, tr("Unhide")); })->setEnabled(has);
  menu.addAction(tr("Freeze"), this, [setFlag] { setFlag(&Node::frozen, true, tr("Freeze")); })->setEnabled(has);
  menu.addAction(tr("Unfreeze"), this, [setFlag] { setFlag(&Node::frozen, false, tr("Unfreeze")); })->setEnabled(has);
  menu.addSeparator();
  menu.addAction(tr("Delete"), this, [this] { doc_->deleteSelected(); })->setEnabled(has);
  menu.addSeparator();
  menu.addAction(tr("Expand All"), this, [this] {
    collapsed_.clear();
    tree_->expandAll();
  });
  menu.addAction(tr("Collapse All"), tree_, &QTreeView::collapseAll);
  menu.exec(tree_->viewport()->mapToGlobal(pos));
}

}  // namespace veraa
