#include "commandpanel.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

#include "icons.h"
#include "paramform.h"

namespace veraa {

namespace {

QScrollArea* scrollWrap(QWidget* content, QWidget* parent) {
  auto* sa = new QScrollArea(parent);
  sa->setWidget(content);
  sa->setWidgetResizable(true);
  sa->setFrameShape(QFrame::NoFrame);
  sa->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  return sa;
}

QString display(const std::string& type) { return QString::fromStdString(objectDisplayName(type)); }

}  // namespace

CommandPanel::CommandPanel(SceneDocument* doc, EditorState* state, QWidget* parent)
    : QTabWidget(parent), doc_(doc), state_(state) {
  setMinimumWidth(250);
  addTab(buildCreateTab(), icons::create(), tr("Create"));
  addTab(buildModifyTab(), icons::modify(), tr("Modify"));
  setIconSize(QSize(16, 16));

  connect(state_, &EditorState::createTypeChanged, this, [this](const QString&) { syncCreate(); });
  connect(doc_, &SceneDocument::selectionChanged, this, [this] {
    syncCreate();
    rebuildModify();
  });
  connect(doc_, &SceneDocument::structureChanged, this, [this] {
    // Node objects may have been replaced (undo/redo): drop all references, then rebuild.
    createForm_->setBlock(nullptr);
    createFormNode_ = 0;
    modForm_->setBlock(nullptr);
    modNode_ = 0;
    syncCreate();
    rebuildModify();
  });
  connect(doc_, &SceneDocument::nodesChanged, this, [this](const QVector<uint32_t>& ids, int flags) {
    if (ids.contains(createFormNode_)) {
      createForm_->refresh();
      if (Node* n = doc_->scene().findNode(createFormNode_)) {
        QSignalBlocker b(createName_);
        createName_->setText(QString::fromStdString(n->name));
        createColor_->setColor(n->wireColor);
      }
    }
    if (ids.contains(modNode_)) {
      if (flags & ChangeName) {
        if (Node* n = doc_->scene().findNode(modNode_)) {
          QSignalBlocker b(name_);
          name_->setText(QString::fromStdString(n->name));
        }
      }
      refreshModifyValues();
    }
  });
  syncCreate();
  rebuildModify();
}

void CommandPanel::showCreate() { setCurrentIndex(0); }
void CommandPanel::showModify() { setCurrentIndex(1); }

// ---------------------------------------------------------------------------------------------
// Create tab

QWidget* CommandPanel::buildCreateTab() {
  auto* content = new QWidget;
  auto* v = new QVBoxLayout(content);
  v->setContentsMargins(4, 4, 4, 4);
  v->setSpacing(4);

  auto* cats = new QHBoxLayout;
  cats->setSpacing(2);
  const QIcon catIcons[3] = {icons::geometry(), icons::lights(), icons::cameras()};
  const QString catTips[3] = {tr("Geometry"), tr("Lights"), tr("Cameras")};
  for (int i = 0; i < 3; ++i) {
    auto* b = new QToolButton(content);
    b->setIcon(catIcons[i]);
    b->setIconSize(QSize(22, 22));
    b->setCheckable(true);
    b->setAutoRaise(true);
    b->setToolTip(catTips[i]);
    connect(b, &QToolButton::clicked, this, [this, i] { setCategory(i); });
    catButtons_[i] = b;
    cats->addWidget(b);
  }
  cats->addStretch();
  v->addLayout(cats);

  auto* typeRoll = new Rollout(tr("Object Type"), content);
  auto* grid = new QWidget(typeRoll->body());
  typeGrid_ = new QGridLayout(grid);
  typeGrid_->setContentsMargins(0, 0, 0, 0);
  typeGrid_->setSpacing(3);
  typeRoll->form()->addRow(grid);
  typeGroup_ = new QButtonGroup(this);
  typeGroup_->setExclusive(false);
  v->addWidget(typeRoll);

  nameRollout_ = new Rollout(tr("Name and Color"), content);
  auto* nameRow = new QWidget(nameRollout_->body());
  auto* nh = new QHBoxLayout(nameRow);
  nh->setContentsMargins(0, 0, 0, 0);
  createName_ = new QLineEdit(nameRow);
  createColor_ = new ColorSwatch(nameRow);
  createColor_->setFixedWidth(28);
  nh->addWidget(createName_, 1);
  nh->addWidget(createColor_);
  nameRollout_->form()->addRow(nameRow);
  v->addWidget(nameRollout_);
  connect(createName_, &QLineEdit::editingFinished, this, [this] {
    Node* n = doc_->scene().findNode(createFormNode_);
    if (!n || createName_->text().isEmpty() || n->name == createName_->text().toStdString()) return;
    doc_->beginEdit(tr("Rename"));
    n->name = createName_->text().toStdString();
    doc_->notifyNodeChanged(n, ChangeName);
    doc_->endEdit();
  });
  connect(createColor_, &ColorSwatch::colorEdited, this, [this](const Vec3& c) {
    Node* n = doc_->scene().findNode(createFormNode_);
    if (!n) return;
    doc_->softEdit(tr("Object Color"), QString("color/%1").arg(n->id));
    n->wireColor = c;
    doc_->notifyNodeChanged(n, ChangeDisplay);
  });

  createForm_ = new ParamForm(content);
  v->addWidget(createForm_);
  v->addStretch();

  setCategory(0);
  return scrollWrap(content, this);
}

void CommandPanel::setCategory(int c) {
  category_ = c;
  for (int i = 0; i < 3; ++i) catButtons_[i]->setChecked(i == c);
  rebuildTypeButtons();
}

void CommandPanel::rebuildTypeButtons() {
  for (QAbstractButton* b : typeGroup_->buttons()) {
    typeGroup_->removeButton(b);
    b->hide();
    b->deleteLater();
  }
  std::vector<std::string> types;
  if (category_ == 0) types = geometryTypes();
  else if (category_ == 1) types = lightTypes();
  else types = {"Camera"};
  int i = 0;
  for (const auto& t : types) {
    auto* b = new QPushButton(display(t), typeGrid_->parentWidget());
    b->setCheckable(true);
    b->setMinimumHeight(24);
    QString type = QString::fromStdString(t);
    b->setProperty("objectType", type);
    b->setChecked(state_->createType() == type);
    connect(b, &QPushButton::clicked, this, [this, type](bool on) {
      state_->setCreateType(on ? type : QString());
      if (on)
        state_->setPrompt(tr("Click and drag in a viewport to create a %1").arg(display(type.toStdString())));
    });
    typeGroup_->addButton(b);
    typeGrid_->addWidget(b, i / 2, i % 2);
    ++i;
  }
}

void CommandPanel::syncCreate() {
  QString ct = state_->createType();
  for (QAbstractButton* b : typeGroup_->buttons()) {
    QSignalBlocker blk(b);
    b->setChecked(b->property("objectType").toString() == ct);
  }
  if (!ct.isEmpty()) {
    // Jump to the category of the active creation type.
    int cat = ct == "Camera" ? 2 : (std::find(lightTypes().begin(), lightTypes().end(), ct.toStdString()) !=
                                            lightTypes().end()
                                        ? 1
                                        : 0);
    if (cat != category_) setCategory(cat);
    showCreate();
  }
  // Parameters of the object being (or just) created.
  Node* n = doc_->selection().size() == 1 ? doc_->firstSelected() : nullptr;
  bool show = n && !ct.isEmpty() && QString::fromStdString(n->object->type()) == ct;
  nameRollout_->setVisible(show);
  if (!show) {
    if (createFormNode_) {
      createForm_->setBlock(nullptr);
      createFormNode_ = 0;
    }
    return;
  }
  QSignalBlocker b(createName_);
  createName_->setText(QString::fromStdString(n->name));
  createColor_->setColor(n->wireColor);
  if (createFormNode_ != n->id || !createForm_->block()) {
    createFormNode_ = n->id;
    wireForm(createForm_, n->id, -1);
    createForm_->setBlock(&n->object->params);
  }
}

// ---------------------------------------------------------------------------------------------
// Modify tab

QWidget* CommandPanel::buildModifyTab() {
  auto* content = new QWidget;
  auto* v = new QVBoxLayout(content);
  v->setContentsMargins(4, 4, 4, 4);
  v->setSpacing(4);

  auto* nameRow = new QHBoxLayout;
  name_ = new QLineEdit(content);
  color_ = new ColorSwatch(content);
  color_->setFixedWidth(28);
  nameRow->addWidget(name_, 1);
  nameRow->addWidget(color_);
  v->addLayout(nameRow);

  modList_ = new QComboBox(content);
  modList_->addItem(tr("Modifier List"));
  for (const auto& t : modifierTypes())
    modList_->addItem(QString::fromStdString(modifierDisplayName(t)), QString::fromStdString(t));
  v->addWidget(modList_);

  stack_ = new QListWidget(content);
  stack_->setMinimumHeight(110);
  stack_->setMaximumHeight(170);
  v->addWidget(stack_);

  auto* btns = new QHBoxLayout;
  btns->setSpacing(2);
  delMod_ = new QToolButton(content);
  delMod_->setIcon(icons::trash());
  delMod_->setToolTip(tr("Remove modifier from the stack"));
  upMod_ = new QToolButton(content);
  upMod_->setIcon(icons::arrowUp());
  upMod_->setToolTip(tr("Move modifier up"));
  downMod_ = new QToolButton(content);
  downMod_->setIcon(icons::arrowDown());
  downMod_->setToolTip(tr("Move modifier down"));
  btns->addWidget(delMod_);
  btns->addWidget(upMod_);
  btns->addWidget(downMod_);
  btns->addStretch();
  v->addLayout(btns);

  modInfo_ = new QLabel(content);
  modInfo_->setWordWrap(true);
  modInfo_->setStyleSheet("color: #a8a8a8;");
  v->addWidget(modInfo_);

  modForm_ = new ParamForm(content);
  v->addWidget(modForm_);
  v->addStretch();

  connect(name_, &QLineEdit::editingFinished, this, [this] {
    auto nodes = doc_->selectedNodes();
    if (nodes.size() != 1 || name_->text().isEmpty() || nodes[0]->name == name_->text().toStdString()) return;
    doc_->beginEdit(tr("Rename"));
    nodes[0]->name = name_->text().toStdString();
    doc_->notifyNodeChanged(nodes[0], ChangeName);
    doc_->endEdit();
  });
  connect(color_, &ColorSwatch::colorEdited, this, [this](const Vec3& c) {
    auto nodes = doc_->selectedNodes();
    if (nodes.isEmpty()) return;
    doc_->softEdit(tr("Object Color"), "color/selection");
    QVector<uint32_t> ids;
    for (Node* n : nodes) {
      n->wireColor = c;
      ids.push_back(n->id);
    }
    doc_->notifyNodesChanged(ids, ChangeDisplay);
  });
  connect(modList_, &QComboBox::activated, this, [this](int i) {
    if (i <= 0) return;
    QString type = modList_->itemData(i).toString();
    QSignalBlocker b(modList_);
    modList_->setCurrentIndex(0);
    addModifier(type);
  });
  connect(stack_, &QListWidget::currentRowChanged, this, [this](int row) {
    if (rebuilding_ || row < 0) return;
    selectStackRow(row);
  });
  connect(stack_, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
    if (rebuilding_) return;
    Node* n = doc_->scene().findNode(modNode_);
    if (!n) return;
    int row = stack_->row(item);
    int mods = static_cast<int>(n->modifiers.size());
    if (row >= mods) return;
    int idx = mods - 1 - row;
    bool on = item->checkState() == Qt::Checked;
    if (n->modifiers[idx]->enabled == on) return;
    doc_->beginEdit(on ? tr("Enable Modifier") : tr("Disable Modifier"));
    n->modifiers[idx]->enabled = on;
    doc_->notifyNodeChanged(n, ChangeGeometry);
    doc_->endEdit();
  });
  connect(delMod_, &QToolButton::clicked, this, [this] {
    Node* n = doc_->scene().findNode(modNode_);
    int idx = currentStackIndex();
    if (!n || idx < 0) return;
    uint32_t id = n->id;
    doc_->beginEdit(tr("Delete Modifier"));
    n->modifiers.erase(n->modifiers.begin() + idx);
    stackChoice_[id] = idx - 1 >= 0 ? idx - 1 : -1;
    doc_->notifyNodeChanged(n, ChangeGeometry);
    doc_->endEdit();
    rebuildModify();
  });
  auto move = [this](int dir) {
    Node* n = doc_->scene().findNode(modNode_);
    int idx = currentStackIndex();
    if (!n || idx < 0) return;
    int to = idx + dir;
    if (to < 0 || to >= static_cast<int>(n->modifiers.size())) return;
    doc_->beginEdit(tr("Reorder Modifiers"));
    std::swap(n->modifiers[idx], n->modifiers[to]);
    stackChoice_[n->id] = to;
    doc_->notifyNodeChanged(n, ChangeGeometry);
    doc_->endEdit();
    rebuildModify();
  };
  connect(upMod_, &QToolButton::clicked, this, [move] { move(+1); });
  connect(downMod_, &QToolButton::clicked, this, [move] { move(-1); });

  return scrollWrap(content, this);
}

int CommandPanel::currentStackIndex() const {
  Node* n = doc_->scene().findNode(modNode_);
  if (!n) return -1;
  int mods = static_cast<int>(n->modifiers.size());
  int row = stack_->currentRow();
  if (row < 0 || row >= mods) return -1;
  return mods - 1 - row;
}

void CommandPanel::wireForm(ParamForm* form, uint32_t nodeId, int stackIndex) {
  form->beforeChange = [this, nodeId, stackIndex](const std::string& key) {
    doc_->softEdit(tr("Change %1").arg(QString::fromStdString(key)),
                   QString("%1/%2/%3").arg(nodeId).arg(stackIndex).arg(QString::fromStdString(key)));
  };
  form->afterChange = [this, nodeId](const std::string&) {
    if (Node* n = doc_->scene().findNode(nodeId)) doc_->notifyNodeChanged(n, ChangeGeometry);
  };
  QString file = doc_->filePath();
  form->browseDir = file.isEmpty() ? QString() : QFileInfo(file).absolutePath();
}

void CommandPanel::rebuildModify() {
  rebuilding_ = true;
  auto nodes = doc_->selectedNodes();
  Node* n = nodes.size() == 1 ? nodes[0] : nullptr;
  modNode_ = n ? n->id : 0;
  stack_->clear();
  name_->setEnabled(!nodes.isEmpty());
  color_->setEnabled(!nodes.isEmpty());
  {
    QSignalBlocker b(name_);
    name_->setText(n ? QString::fromStdString(n->name)
                     : nodes.isEmpty() ? QString() : tr("%1 Objects Selected").arg(nodes.size()));
  }
  name_->setReadOnly(!n);
  if (!nodes.isEmpty()) color_->setColor(nodes[0]->wireColor);

  bool anyGeometry = false;
  for (Node* s : nodes) anyGeometry |= s->kind() == ObjectKind::Geometry;
  modList_->setEnabled(anyGeometry);

  if (!n) {
    modForm_->setBlock(nullptr);
    modInfo_->setText(nodes.isEmpty() ? tr("Select an object to modify it.")
                                      : tr("Multiple objects selected. Modifiers chosen from the list are added to each "
                                           "selected object."));
    delMod_->setEnabled(false);
    upMod_->setEnabled(false);
    downMod_->setEnabled(false);
    rebuilding_ = false;
    return;
  }
  modInfo_->clear();
  int mods = static_cast<int>(n->modifiers.size());
  for (int i = mods - 1; i >= 0; --i) {
    auto* item = new QListWidgetItem(QString::fromStdString(modifierDisplayName(n->modifiers[i]->type())), stack_);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(n->modifiers[i]->enabled ? Qt::Checked : Qt::Unchecked);
  }
  auto* base = new QListWidgetItem(display(n->object->type()), stack_);
  QFont f = base->font();
  f.setBold(true);
  base->setFont(f);
  int choice = stackChoice_.value(n->id, mods > 0 ? mods - 1 : -1);
  if (choice >= mods) choice = mods - 1;
  int row = choice < 0 ? mods : mods - 1 - choice;
  stack_->setCurrentRow(row);
  rebuilding_ = false;
  selectStackRow(row);
}

void CommandPanel::selectStackRow(int row) {
  Node* n = doc_->scene().findNode(modNode_);
  if (!n) return;
  int mods = static_cast<int>(n->modifiers.size());
  int idx = row >= mods ? -1 : mods - 1 - row;
  stackChoice_[n->id] = idx;
  delMod_->setEnabled(idx >= 0);
  upMod_->setEnabled(idx >= 0 && idx < mods - 1);
  downMod_->setEnabled(idx > 0);
  ParamBlock* block = idx < 0 ? &n->object->params : &n->modifiers[idx]->params;
  wireForm(modForm_, n->id, idx);
  modForm_->setBlock(block);
}

void CommandPanel::refreshModifyValues() {
  Node* n = doc_->scene().findNode(modNode_);
  if (!n) return;
  if (static_cast<int>(stack_->count()) != static_cast<int>(n->modifiers.size()) + 1) {
    rebuildModify();
    return;
  }
  modForm_->refresh();
}

void CommandPanel::addModifier(const QString& type) {
  auto nodes = doc_->selectedNodes();
  QVector<uint32_t> ids;
  for (Node* n : nodes)
    if (n->kind() == ObjectKind::Geometry) ids.push_back(n->id);
  if (ids.isEmpty()) return;
  doc_->beginEdit(tr("Add %1").arg(QString::fromStdString(modifierDisplayName(type.toStdString()))));
  for (uint32_t id : ids) {
    Node* n = doc_->scene().findNode(id);
    auto mod = createModifier(type.toStdString());
    if (!mod) continue;
    int at = static_cast<int>(n->modifiers.size());
    if (ids.size() == 1) at = std::min(stackChoice_.value(id, at - 1) + 1, at);
    n->modifiers.insert(n->modifiers.begin() + at, std::move(mod));
    stackChoice_[id] = at;
  }
  doc_->notifyNodesChanged(ids, ChangeGeometry);
  doc_->endEdit();
  showModify();
  rebuildModify();
}

}  // namespace veraa
