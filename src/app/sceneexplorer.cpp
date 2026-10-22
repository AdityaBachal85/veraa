#include "sceneexplorer.h"

#include <QHeaderView>
#include <QLineEdit>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "icons.h"

namespace veraa {

namespace {
constexpr int kIdRole = Qt::UserRole + 1;
}

SceneExplorer::SceneExplorer(SceneDocument* doc, QWidget* parent) : QWidget(parent), doc_(doc) {
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(2, 2, 2, 2);
  v->setSpacing(2);
  filter_ = new QLineEdit(this);
  filter_->setPlaceholderText(tr("Find..."));
  filter_->setClearButtonEnabled(true);
  v->addWidget(filter_);
  tree_ = new QTreeWidget(this);
  tree_->setColumnCount(2);
  tree_->setHeaderLabels({tr("Name"), tr("Type")});
  tree_->setRootIsDecorated(false);
  tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  tree_->setEditTriggers(QAbstractItemView::EditKeyPressed | QAbstractItemView::SelectedClicked);
  tree_->setUniformRowHeights(true);
  tree_->header()->setStretchLastSection(false);
  tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  tree_->setIconSize(QSize(16, 16));
  v->addWidget(tree_);

  connect(filter_, &QLineEdit::textChanged, this, &SceneExplorer::applyFilter);
  // Rebuild later: edits can originate from this tree's own signals.
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
    for (QTreeWidgetItem* it : tree_->selectedItems()) ids.push_back(it->data(0, kIdRole).toUInt());
    doc_->setSelection(ids);
  });
  connect(tree_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* it, int col) {
    if (syncing_ || col != 0) return;
    Node* n = doc_->scene().findNode(it->data(0, kIdRole).toUInt());
    if (!n) return;
    bool visible = it->checkState(0) == Qt::Checked;
    std::string name = it->text(0).toStdString();
    if (n->hidden == !visible && n->name == name) return;
    doc_->beginEdit(n->hidden != !visible ? (visible ? tr("Unhide") : tr("Hide")) : tr("Rename"));
    n->hidden = !visible;
    if (!name.empty()) n->name = name;
    doc_->notifyNodeChanged(n, ChangeDisplay | ChangeName);
    doc_->endEdit();
  });
  rebuild();
}

void SceneExplorer::rebuild() {
  rebuildPending_ = false;
  syncing_ = true;
  tree_->clear();
  for (const auto& n : doc_->scene().nodes) {
    auto* it = new QTreeWidgetItem(tree_);
    it->setText(0, QString::fromStdString(n->name));
    it->setText(1, QString::fromStdString(objectDisplayName(n->object ? n->object->type() : "")));
    it->setData(0, kIdRole, n->id);
    it->setFlags(it->flags() | Qt::ItemIsEditable | Qt::ItemIsUserCheckable);
    it->setCheckState(0, n->hidden ? Qt::Unchecked : Qt::Checked);
    it->setToolTip(0, tr("Checkbox: visible in viewports. Double-click or F2 to rename."));
    QIcon icon = n->kind() == ObjectKind::Light    ? icons::light()
                 : n->kind() == ObjectKind::Camera ? icons::camera()
                                                   : icons::object();
    it->setIcon(0, icon);
    if (n->hidden) {
      it->setForeground(0, QColor(130, 130, 130));
      it->setForeground(1, QColor(110, 110, 110));
    }
  }
  syncing_ = false;
  syncSelection();
  applyFilter();
}

void SceneExplorer::syncSelection() {
  syncing_ = true;
  tree_->clearSelection();
  QTreeWidgetItem* last = nullptr;
  for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
    QTreeWidgetItem* it = tree_->topLevelItem(i);
    if (doc_->isSelected(it->data(0, kIdRole).toUInt())) {
      it->setSelected(true);
      last = it;
    }
  }
  if (last) tree_->scrollToItem(last);
  syncing_ = false;
}

void SceneExplorer::applyFilter() {
  QString f = filter_->text().trimmed();
  for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
    QTreeWidgetItem* it = tree_->topLevelItem(i);
    it->setHidden(!f.isEmpty() && !it->text(0).contains(f, Qt::CaseInsensitive));
  }
}

}  // namespace veraa
