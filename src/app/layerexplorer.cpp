#include "layerexplorer.h"

#include <QColorDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QMenu>
#include <QToolButton>
#include <QVBoxLayout>

#include "icons.h"
#include "sceneexplorer.h"

namespace veraa {

namespace {
constexpr int kLayerRole = Qt::UserRole + 1;
constexpr int kNodeRole = Qt::UserRole + 2;
enum Column { kName, kHidden, kFrozen, kRender, kColor, kColumns };

QIcon swatch(const Vec3& c) {
  QPixmap pm(14, 14);
  pm.fill(QColor::fromRgbF(c.x, c.y, c.z));
  return QIcon(pm);
}
}  // namespace

LayerExplorer::LayerExplorer(SceneDocument* doc, QWidget* parent) : QWidget(parent), doc_(doc) {
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(2, 2, 2, 2);
  v->setSpacing(2);
  auto* bar = new QHBoxLayout;
  bar->setSpacing(1);
  auto tool = [&](const QString& text, const QString& tip, const std::function<void()>& fn) {
    auto* b = new QToolButton(this);
    b->setText(text);
    b->setToolTip(tip);
    b->setAutoRaise(true);
    connect(b, &QToolButton::clicked, this, fn);
    bar->addWidget(b);
    return b;
  };
  tool(tr("New"), tr("Create New Layer (adds the selection)"), [this] {
    bool ok = false;
    QString name = QInputDialog::getText(this, tr("Create New Layer"), tr("Name:"), QLineEdit::Normal,
                                         tr("Layer%1").arg(doc_->scene().layers.size(), 3, 10, QChar('0')), &ok);
    if (ok && !name.trimmed().isEmpty()) createLayer(name.trimmed(), true);
  });
  tool(tr("Delete"), tr("Delete the highlighted layer (its objects move to layer 0)"), [this] {
    if (QTreeWidgetItem* it = tree_->currentItem()) deleteLayer(currentLayerOfItem(it));
  });
  tool(tr("Add Sel"), tr("Add the selected objects to the highlighted layer"), [this] {
    if (QTreeWidgetItem* it = tree_->currentItem()) moveToLayer(doc_->selection(), currentLayerOfItem(it));
  });
  tool(tr("Select"), tr("Select the objects of the highlighted layer"), [this] {
    QTreeWidgetItem* it = tree_->currentItem();
    if (!it) return;
    uint32_t id = currentLayerOfItem(it);
    QVector<uint32_t> ids;
    for (const auto& n : doc_->scene().nodes)
      if (n->layerId == id && doc_->scene().isNodeVisible(*n)) ids.push_back(n->id);
    doc_->setSelection(ids);
  });
  tool(tr("Current"), tr("Make the highlighted layer current (new objects go on it)"), [this] {
    if (QTreeWidgetItem* it = tree_->currentItem()) setCurrentLayer(currentLayerOfItem(it));
  });
  bar->addStretch(1);
  v->addLayout(bar);

  tree_ = new DropTree(this);
  tree_->setColumnCount(kColumns);
  tree_->setHeaderLabels({tr("Name"), tr("Hidden"), tr("Frozen"), tr("Render"), tr("Color")});
  tree_->header()->setStretchLastSection(false);
  tree_->header()->setSectionResizeMode(kName, QHeaderView::Stretch);
  for (int c = kHidden; c < kColumns; ++c) tree_->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
  tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  tree_->setEditTriggers(QAbstractItemView::EditKeyPressed | QAbstractItemView::DoubleClicked);
  tree_->setContextMenuPolicy(Qt::CustomContextMenu);
  v->addWidget(tree_);

  auto schedule = [this] {
    if (rebuildPending_) return;
    rebuildPending_ = true;
    QMetaObject::invokeMethod(this, [this] { rebuild(); }, Qt::QueuedConnection);
  };
  connect(doc_, &SceneDocument::structureChanged, this, schedule);
  connect(doc_, &SceneDocument::nodesChanged, this, [schedule](const QVector<uint32_t>&, int f) {
    if (f & (ChangeName | ChangeDisplay)) schedule();
  });
  connect(tree_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* it, int col) {
    if (it->data(kName, kNodeRole).toUInt()) {
      if (col == kName) doc_->setSelection({it->data(kName, kNodeRole).toUInt()});
      return;
    }
    uint32_t id = it->data(kName, kLayerRole).toUInt();
    Layer* l = doc_->scene().findLayer(id);
    if (!l) return;
    if (col == kHidden) setLayerFlag(id, 0, !l->hidden);
    else if (col == kFrozen) setLayerFlag(id, 1, !l->frozen);
    else if (col == kRender) setLayerFlag(id, 2, !l->renderable);
    else if (col == kColor) {
      QColor c = QColorDialog::getColor(QColor::fromRgbF(l->color.x, l->color.y, l->color.z), this, tr("Layer Color"));
      if (!c.isValid()) return;
      doc_->beginEdit(tr("Layer Color"));
      l->color = Vec3(static_cast<float>(c.redF()), static_cast<float>(c.greenF()), static_cast<float>(c.blueF()));
      doc_->notifyStructureChanged();
      doc_->endEdit();
    }
  });
  connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* it, int col) {
    if (col == kName && !it->data(kName, kNodeRole).toUInt()) tree_->editItem(it, kName);
  });
  connect(tree_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* it, int col) {
    if (syncing_ || col != kName || it->data(kName, kNodeRole).toUInt()) return;
    Layer* l = doc_->scene().findLayer(it->data(kName, kLayerRole).toUInt());
    QString name = it->text(kName).trimmed();
    if (name.startsWith(QString::fromUtf8("▶ "))) name = name.mid(2);
    if (!l || name.isEmpty() || name.toStdString() == l->name || l->id == 0) {
      QMetaObject::invokeMethod(this, [this] { rebuild(); }, Qt::QueuedConnection);
      return;
    }
    doc_->beginEdit(tr("Rename Layer"));
    l->name = name.toStdString();
    doc_->notifyStructureChanged();
    doc_->endEdit();
  });
  tree_->onDrop = [this](const QList<QTreeWidgetItem*>& dragged, QTreeWidgetItem* target) {
    if (!target) return;
    QVector<uint32_t> ids;
    for (QTreeWidgetItem* it : dragged)
      if (uint32_t n = it->data(kName, kNodeRole).toUInt()) ids.push_back(n);
    moveToLayer(ids, currentLayerOfItem(target));
  };
  connect(tree_, &QWidget::customContextMenuRequested, this, &LayerExplorer::showMenu);
  rebuild();
}

uint32_t LayerExplorer::currentLayerOfItem(QTreeWidgetItem* it) const {
  while (it && it->parent()) it = it->parent();
  return it ? it->data(kName, kLayerRole).toUInt() : 0;
}

void LayerExplorer::rebuild() {
  rebuildPending_ = false;
  syncing_ = true;
  QSet<uint32_t> collapsed;
  for (int i = 0; i < tree_->topLevelItemCount(); ++i)
    if (!tree_->topLevelItem(i)->isExpanded()) collapsed.insert(tree_->topLevelItem(i)->data(kName, kLayerRole).toUInt());
  tree_->clear();
  const Scene& s = doc_->scene();
  for (const Layer& l : s.layers) {
    auto* item = new QTreeWidgetItem(tree_);
    bool current = l.id == s.currentLayerId;
    item->setText(kName, (current ? QString::fromUtf8("▶ ") : QString()) + QString::fromStdString(l.name));
    item->setToolTip(kName, current ? tr("Current layer") : QString());
    item->setData(kName, kLayerRole, l.id);
    item->setFlags((item->flags() | Qt::ItemIsDropEnabled | Qt::ItemIsEditable) & ~Qt::ItemIsDragEnabled);
    QFont f = item->font(kName);
    f.setBold(current);
    item->setFont(kName, f);
    item->setIcon(kHidden, icons::eye(!l.hidden));
    item->setText(kFrozen, l.frozen ? QString::fromUtf8("❄") : QString());
    item->setText(kRender, l.renderable ? QString::fromUtf8("✓") : QString());
    item->setIcon(kColor, swatch(l.color));
    for (const auto& n : s.nodes) {
      if (n->layerId != l.id) continue;
      auto* child = new QTreeWidgetItem(item);
      child->setText(kName, QString::fromStdString(n->name));
      child->setData(kName, kNodeRole, n->id);
      child->setFlags((child->flags() | Qt::ItemIsDragEnabled) & ~Qt::ItemIsDropEnabled & ~Qt::ItemIsEditable);
      child->setIcon(kHidden, icons::eye(!n->hidden));
      if (n->frozen) child->setText(kFrozen, QString::fromUtf8("❄"));
      if (doc_->isSelected(n->id)) child->setForeground(kName, QColor(245, 197, 24));
    }
    item->setExpanded(!collapsed.contains(l.id));
  }
  syncing_ = false;
}

uint32_t LayerExplorer::createLayer(const QString& name, bool addSelection) {
  doc_->beginEdit(tr("Create Layer"));
  Layer* l = doc_->scene().createLayer(name.toStdString());
  uint32_t id = l ? l->id : 0;
  if (l && addSelection)
    for (Node* n : doc_->selectedNodes()) n->layerId = id;
  if (l) doc_->scene().currentLayerId = id;
  doc_->notifyNodesChanged(doc_->selection(), ChangeDisplay);
  doc_->notifyStructureChanged();
  doc_->endEdit();
  return id;
}

void LayerExplorer::deleteLayer(uint32_t id) {
  if (id == 0) return;
  doc_->beginEdit(tr("Delete Layer"));
  doc_->scene().removeLayer(id);
  if (doc_->scene().currentLayerId == id) doc_->scene().currentLayerId = 0;
  doc_->notifyStructureChanged();
  doc_->endEdit();
}

void LayerExplorer::moveToLayer(const QVector<uint32_t>& nodeIds, uint32_t layerId) {
  if (nodeIds.isEmpty() || !doc_->scene().findLayer(layerId)) return;
  doc_->beginEdit(tr("Move to Layer"));
  for (uint32_t id : nodeIds)
    if (Node* n = doc_->scene().findNode(id)) n->layerId = layerId;
  doc_->notifyNodesChanged(nodeIds, ChangeDisplay);
  doc_->notifyStructureChanged();
  doc_->endEdit();
}

void LayerExplorer::setCurrentLayer(uint32_t id) {
  if (!doc_->scene().findLayer(id)) return;
  doc_->beginEdit(tr("Set Current Layer"));
  doc_->scene().currentLayerId = id;
  doc_->notifyStructureChanged();
  doc_->endEdit();
}

void LayerExplorer::setLayerFlag(uint32_t id, int flag, bool value) {
  Layer* l = doc_->scene().findLayer(id);
  if (!l) return;
  doc_->beginEdit(flag == 0 ? tr("Hide Layer") : flag == 1 ? tr("Freeze Layer") : tr("Layer Renderable"));
  if (flag == 0) l->hidden = value;
  else if (flag == 1) l->frozen = value;
  else l->renderable = value;
  QVector<uint32_t> ids;
  for (const auto& n : doc_->scene().nodes)
    if (n->layerId == id) ids.push_back(n->id);
  // Hidden or frozen objects cannot stay selected.
  if (value && flag != 2) {
    QVector<uint32_t> keep;
    for (uint32_t sid : doc_->selection())
      if (!ids.contains(sid)) keep.push_back(sid);
    doc_->setSelection(keep);
  }
  doc_->notifyNodesChanged(ids, ChangeDisplay);
  doc_->notifyStructureChanged();
  doc_->endEdit();
}

void LayerExplorer::showMenu(const QPoint& pos) {
  QTreeWidgetItem* it = tree_->itemAt(pos);
  QMenu menu(this);
  uint32_t layer = it ? currentLayerOfItem(it) : 0;
  menu.addAction(tr("Create New Layer"), this, [this] { createLayer(tr("Layer%1").arg(doc_->scene().layers.size(), 3, 10, QChar('0')), true); });
  if (it) {
    menu.addAction(tr("Set Current Layer"), this, [this, layer] { setCurrentLayer(layer); });
    menu.addAction(tr("Add Selected Objects"), this, [this, layer] { moveToLayer(doc_->selection(), layer); });
    menu.addAction(tr("Rename"), this, [this, it] { tree_->editItem(it->parent() ? it->parent() : it, kName); })
        ->setEnabled(layer != 0);
    menu.addAction(tr("Delete Layer"), this, [this, layer] { deleteLayer(layer); })->setEnabled(layer != 0);
  }
  menu.exec(tree_->viewport()->mapToGlobal(pos));
}

}  // namespace veraa
