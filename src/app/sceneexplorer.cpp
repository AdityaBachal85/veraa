#include "sceneexplorer.h"

#include <QDropEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QMenu>
#include <QToolButton>
#include <QVBoxLayout>

#include <functional>

#include "icons.h"

namespace veraa {

namespace {
constexpr int kIdRole = Qt::UserRole + 1;
enum Column { kName, kType, kHidden, kFrozen, kRender, kLayer, kColumns };

QIcon kindIcon(ObjectKind k) {
  switch (k) {
    case ObjectKind::Light: return icons::light();
    case ObjectKind::Camera: return icons::camera();
    case ObjectKind::Shape: return icons::shapes();
    case ObjectKind::Helper: return icons::helpers();
    case ObjectKind::SpaceWarp: return icons::spaceWarps();
    default: return icons::object();
  }
}
}  // namespace

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

  tree_ = new DropTree(this);
  tree_->setColumnCount(kColumns);
  tree_->setHeaderLabels({tr("Name"), tr("Type"), tr("Hidden"), tr("Frozen"), tr("Render"), tr("Layer")});
  tree_->setRootIsDecorated(true);
  tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  tree_->setEditTriggers(QAbstractItemView::EditKeyPressed | QAbstractItemView::DoubleClicked);
  tree_->setUniformRowHeights(true);
  tree_->setSortingEnabled(true);
  tree_->sortByColumn(kName, Qt::AscendingOrder);
  tree_->header()->setStretchLastSection(false);
  tree_->header()->setSectionResizeMode(kName, QHeaderView::Stretch);
  for (int c = kType; c < kColumns; ++c) tree_->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
  tree_->setIconSize(QSize(16, 16));
  tree_->setContextMenuPolicy(Qt::CustomContextMenu);
  v->addWidget(tree_);

  connect(expand, &QToolButton::clicked, tree_, &QTreeWidget::expandAll);
  connect(collapse, &QToolButton::clicked, tree_, &QTreeWidget::collapseAll);
  connect(filter_, &QLineEdit::textChanged, this, &SceneExplorer::applyFilter);
  auto schedule = [this] {
    if (rebuildPending_) return;
    rebuildPending_ = true;
    QMetaObject::invokeMethod(this, [this] { rebuild(); }, Qt::QueuedConnection);
  };
  connect(doc_, &SceneDocument::structureChanged, this, schedule);
  connect(doc_, &SceneDocument::nodesChanged, this, [schedule](const QVector<uint32_t>&, int flags) {
    if (flags & (ChangeName | ChangeDisplay)) schedule();
  });
  connect(doc_, &SceneDocument::selectionChanged, this, &SceneExplorer::syncSelection);
  connect(tree_, &QTreeWidget::itemSelectionChanged, this, [this] {
    if (syncing_) return;
    QVector<uint32_t> ids;
    for (QTreeWidgetItem* it : tree_->selectedItems()) ids.push_back(it->data(kName, kIdRole).toUInt());
    doc_->setSelection(ids);
  });
  connect(tree_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* it, int col) {
    if (syncing_ || col != kName) return;
    Node* n = doc_->scene().findNode(it->data(kName, kIdRole).toUInt());
    if (!n) return;
    std::string name = it->text(kName).trimmed().toStdString();
    if (name.empty() || name == n->name) return;
    doc_->beginEdit(tr("Rename"));
    n->name = name;
    doc_->notifyNodeChanged(n, ChangeName);
    doc_->endEdit();
  });
  // Clicking the Hidden / Frozen / Render cells toggles them.
  connect(tree_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* it, int col) {
    if (col != kHidden && col != kFrozen && col != kRender) return;
    Node* n = doc_->scene().findNode(it->data(kName, kIdRole).toUInt());
    if (!n) return;
    doc_->beginEdit(col == kHidden ? tr("Hide") : col == kFrozen ? tr("Freeze") : tr("Renderable"));
    if (col == kHidden) n->hidden = !n->hidden;
    else if (col == kFrozen) n->frozen = !n->frozen;
    else n->renderable = !n->renderable;
    doc_->notifyNodeChanged(n, ChangeDisplay);
    doc_->endEdit();
  });
  tree_->onDrop = [this](const QList<QTreeWidgetItem*>& dragged, QTreeWidgetItem* target) {
    QVector<uint32_t> ids;
    for (QTreeWidgetItem* it : dragged) ids.push_back(it->data(kName, kIdRole).toUInt());
    linkNodes(ids, target ? target->data(kName, kIdRole).toUInt() : 0);
  };
  connect(tree_, &QWidget::customContextMenuRequested, this, &SceneExplorer::showMenu);
  rebuild();
}

QTreeWidgetItem* SceneExplorer::itemFor(uint32_t id) const {
  for (QTreeWidgetItemIterator it(tree_); *it; ++it)
    if ((*it)->data(kName, kIdRole).toUInt() == id) return *it;
  return nullptr;
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
  // Remember expanded nodes.
  QSet<uint32_t> collapsed;
  for (QTreeWidgetItemIterator it(tree_); *it; ++it)
    if ((*it)->childCount() && !(*it)->isExpanded()) collapsed.insert((*it)->data(kName, kIdRole).toUInt());
  tree_->setSortingEnabled(false);
  tree_->clear();
  const Scene& s = doc_->scene();
  QHash<uint32_t, QTreeWidgetItem*> items;
  std::function<QTreeWidgetItem*(Node*)> make = [&](Node* n) -> QTreeWidgetItem* {
    if (QTreeWidgetItem* done = items.value(n->id)) return done;
    QTreeWidgetItem* parentItem = n->parentId ? (s.findNode(n->parentId) ? make(s.findNode(n->parentId)) : nullptr) : nullptr;
    auto* item = parentItem ? new QTreeWidgetItem(parentItem) : new QTreeWidgetItem(tree_);
    item->setText(kName, QString::fromStdString(n->name));
    item->setData(kName, kIdRole, n->id);
    item->setFlags(item->flags() | Qt::ItemIsEditable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
    item->setIcon(kName, kindIcon(n->kind()));
    item->setText(kType, n->object ? QString::fromStdString(objectDisplayName(n->object->type())) : QString());
    item->setIcon(kHidden, icons::eye(!n->hidden));
    item->setToolTip(kHidden, n->hidden ? tr("Hidden (click to unhide)") : tr("Visible (click to hide)"));
    item->setText(kFrozen, n->frozen ? QString::fromUtf8("❄") : QString());
    item->setToolTip(kFrozen, n->frozen ? tr("Frozen (click to unfreeze)") : tr("Click to freeze"));
    item->setText(kRender, n->renderable ? QString::fromUtf8("✓") : QString());
    const Layer* l = s.findLayer(n->layerId);
    item->setText(kLayer, l ? QString::fromStdString(l->name) : QString());
    if (!s.isNodeVisible(*n)) item->setForeground(kName, QColor(130, 130, 130));
    if (n->groupHead) item->setText(kType, tr("Group"));
    items.insert(n->id, item);
    return item;
  };
  for (const auto& n : s.nodes) make(n.get());
  for (QTreeWidgetItemIterator it(tree_); *it; ++it)
    (*it)->setExpanded(!collapsed.contains((*it)->data(kName, kIdRole).toUInt()));
  tree_->setSortingEnabled(true);
  syncing_ = false;
  applyFilter();
  syncSelection();
}

void SceneExplorer::syncSelection() {
  syncing_ = true;
  tree_->clearSelection();
  QTreeWidgetItem* last = nullptr;
  for (QTreeWidgetItemIterator it(tree_); *it; ++it)
    if (doc_->isSelected((*it)->data(kName, kIdRole).toUInt())) {
      (*it)->setSelected(true);
      last = *it;
    }
  if (last) tree_->scrollToItem(last);
  syncing_ = false;
}

void SceneExplorer::applyFilter() {
  QString f = filter_->text().trimmed();
  const Scene& s = doc_->scene();
  // Bottom-up: an item is shown if it matches or a descendant is shown.
  std::function<bool(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem* item) -> bool {
    bool anyChild = false;
    for (int i = 0; i < item->childCount(); ++i) anyChild |= walk(item->child(i));
    Node* n = s.findNode(item->data(kName, kIdRole).toUInt());
    bool match = n && ((categories_ >> static_cast<int>(n->kind())) & 1) &&
                 (f.isEmpty() || item->text(kName).contains(f, Qt::CaseInsensitive));
    item->setHidden(!(match || anyChild));
    return match || anyChild;
  };
  for (int i = 0; i < tree_->topLevelItemCount(); ++i) walk(tree_->topLevelItem(i));
}

void SceneExplorer::showMenu(const QPoint& pos) {
  QMenu menu(this);
  bool has = !doc_->selection().isEmpty();
  menu.addAction(tr("Rename"), this, [this] {
    if (QTreeWidgetItem* it = tree_->currentItem()) tree_->editItem(it, kName);
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
  menu.addAction(tr("Expand All"), tree_, &QTreeWidget::expandAll);
  menu.addAction(tr("Collapse All"), tree_, &QTreeWidget::collapseAll);
  menu.exec(tree_->viewport()->mapToGlobal(pos));
}

}  // namespace veraa
