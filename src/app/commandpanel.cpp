#include "commandpanel.h"

#include "extension.h"

#include <QButtonGroup>
#include <QCheckBox>
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
      Node* cn = doc_->scene().findNode(createFormNode_);
      if (!cn || !cn->object || cn->object.get() != createFormObject_) {
        // The object was replaced (Convert To, ...): its parameters are gone.
        createForm_->setBlock(nullptr);
        createFormNode_ = 0;
        createFormObject_ = nullptr;
      } else {
        createForm_->refresh();
      }
    }
    if (ids.contains(createFormNode_)) {
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
  const QIcon catIcons[7] = {icons::geometry(), icons::shapes(),  icons::lights(), icons::cameras(),
                             icons::helpers(),  icons::spaceWarps(), icons::systems()};
  const QString catTips[7] = {tr("Geometry"), tr("Shapes"),      tr("Lights"), tr("Cameras"),
                              tr("Helpers"),  tr("Space Warps"), tr("Systems")};
  for (int i = 0; i < 7; ++i) {
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

  subCombo_ = new QComboBox(content);
  subCombo_->setObjectName("CreateListCombo");
  v->addWidget(subCombo_);
  connect(subCombo_, &QComboBox::currentTextChanged, this, [this](const QString& sub) {
    if (sub == subcategory_) return;
    subcategory_ = sub;
    rebuildTypeButtons();
  });

  auto* typeRoll = new Rollout(tr("Object Type"), content);
  // AutoGrid: new objects are built on the surface under the cursor (enabled while creating).
  autoGrid_ = new QCheckBox(tr("AutoGrid"), typeRoll->body());
  autoGrid_->setObjectName("AutoGrid");
  autoGrid_->setToolTip(tr("Create objects on the surface of other objects"));
  autoGrid_->setEnabled(false);
  connect(autoGrid_, &QCheckBox::toggled, this, [this](bool on) { state_->setAutoGrid(on); });
  auto* autoRow = new QHBoxLayout;
  autoRow->addStretch(1);
  autoRow->addWidget(autoGrid_);
  autoRow->addStretch(1);
  typeRoll->form()->addRow(autoRow);
  listNote_ = new QLabel(typeRoll->body());
  listNote_->setWordWrap(true);
  listNote_->setStyleSheet("color: #a8a8a8;");
  listNote_->hide();
  typeRoll->form()->addRow(listNote_);
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
  createColor_->setColor(Vec3(0.78f, 0.16f, 0.16f));
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
  for (int i = 0; i < 7; ++i) catButtons_[i]->setChecked(i == c);
  QSignalBlocker block(subCombo_);
  subCombo_->clear();
  lists_ = createLists(createCategories().value(c));
  for (const CreateList& l : lists_) subCombo_->addItem(l.name);
  subCombo_->setVisible(subCombo_->count() > 0);
  subcategory_ = subCombo_->currentText();
  rebuildTypeButtons();
}

void CommandPanel::setCreateList(const QString& name) {
  int at = subCombo_->findText(name);
  if (at >= 0) subCombo_->setCurrentIndex(at);
}

QStringList CommandPanel::createButtonLabels() const {
  QStringList out;
  for (QAbstractButton* b : typeGroup_->buttons()) out << b->text();
  return out;
}

void CommandPanel::rebuildTypeButtons() {
  for (QAbstractButton* b : typeGroup_->buttons()) {
    typeGroup_->removeButton(b);
    b->hide();
    b->deleteLater();
  }
  const CreateList* list = nullptr;
  for (const CreateList& l : lists_)
    if (l.name == subcategory_) list = &l;
  listNote_->setVisible(list && list->buttons.empty());
  if (list && list->buttons.empty()) listNote_->setText(tr("%1: not available in Veraa yet.").arg(list->name));
  if (!list) return;
  int i = 0;
  for (const CreateButton& cb : list->buttons) {
    auto* b = new QPushButton(cb.label, typeGrid_->parentWidget());
    b->setMinimumHeight(24);
    QString type = cb.type, label = cb.label;
    b->setProperty("objectType", type);
    if (type.isEmpty()) {
      b->setProperty("notAvailable", true);
      b->setToolTip(tr("%1 (not available yet)").arg(label));
      connect(b, &QPushButton::clicked, this, [this, label] { Q_EMIT featureNotAvailable(label); });
    } else {
      b->setCheckable(true);
      connect(b, &QPushButton::clicked, this, [this, type, label](bool on) {
        if (on) beginCreate(type, label);
        else state_->setCreateType(QString());
      });
    }
    typeGroup_->addButton(b);
    typeGrid_->addWidget(b, i / 2, i % 2);
    ++i;
  }
  syncCreate();
}

void CommandPanel::beginCreate(const QString& type, const QString& label) {
  createLabel_ = label;
  createLabelType_ = type;
  state_->setCreateType(type);
  if (!type.isEmpty())
    state_->setPrompt(tr("Click and drag in a viewport to create a %1")
                          .arg(label.isEmpty() ? display(type.toStdString()) : label));
}

void CommandPanel::syncCreate() {
  QString ct = state_->createType();
  if (ct != createLabelType_) createLabel_.clear();  // creation started elsewhere (menu, script)
  // The button of the active type: the one that started it, else the first of that type.
  auto buttonFor = [this, &ct]() -> QAbstractButton* {
    QAbstractButton* first = nullptr;
    for (QAbstractButton* b : typeGroup_->buttons()) {
      if (ct.isEmpty() || b->property("objectType").toString() != ct) continue;
      if (!createLabel_.isEmpty() && b->text() == createLabel_) return b;
      if (!first) first = b;
    }
    return first;
  };
  if (!ct.isEmpty() && !buttonFor()) {
    // Jump to the list that has the active creation type (the starting button's list first).
    bool found = false;
    for (int pass = 0; pass < 2 && !found; ++pass)
      for (int c = 0; c < createCategories().size() && !found; ++c)
        for (const CreateList& l : createLists(createCategories()[c]))
          for (const CreateButton& b : l.buttons)
            if (!found && b.type == ct && (pass == 1 || createLabel_.isEmpty() || b.label == createLabel_)) {
              if (c != category_) setCategory(c);
              setCreateList(l.name);
              found = true;
            }
  }
  QAbstractButton* active = buttonFor();
  for (QAbstractButton* b : typeGroup_->buttons()) {
    QSignalBlocker blk(b);
    b->setChecked(b == active);
  }
  autoGrid_->setEnabled(!ct.isEmpty());
  if (!ct.isEmpty()) showCreate();
  // Name and color of the object being (or just) created; empty otherwise.
  Node* n = doc_->selection().size() == 1 ? doc_->firstSelected() : nullptr;
  bool show = n && !ct.isEmpty() && QString::fromStdString(n->object->type()) == ct;
  createName_->setEnabled(show);
  createColor_->setEnabled(show);
  if (!show) {
    QSignalBlocker b(createName_);
    createName_->clear();
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
    createFormObject_ = n->object.get();
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

  modExtraHost_ = new QWidget(content);
  auto* extraLayout = new QVBoxLayout(modExtraHost_);
  extraLayout->setContentsMargins(0, 0, 0, 0);
  extraLayout->setSpacing(4);
  v->addWidget(modExtraHost_);

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

namespace {
std::vector<const void*> stackIdentity(const Node* n) {
  std::vector<const void*> ids;
  if (!n) return ids;
  ids.push_back(n->object.get());
  for (const auto& m : n->modifiers) ids.push_back(m.get());
  return ids;
}
}  // namespace

void CommandPanel::rebuildModify() {
  rebuilding_ = true;
  auto nodes = doc_->selectedNodes();
  Node* n = nodes.size() == 1 ? nodes[0] : nullptr;
  modNode_ = n ? n->id : 0;
  modBuiltFor_ = stackIdentity(n);
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
  for (Node* s : nodes) anyGeometry |= s->kind() == ObjectKind::Geometry || s->kind() == ObjectKind::Shape;
  modList_->setEnabled(anyGeometry);

  if (!n) {
    modForm_->setBlock(nullptr);
    if (modExtra_) {  // type-specific rollouts belong to the previous selection
      modExtra_->hide();
      modExtra_->deleteLater();
      modExtra_ = nullptr;
    }
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

  // Type-specific rollouts (Editable Poly tools, Edit Spline, FFD, ...).
  if (modExtra_) {
    modExtra_->hide();
    modExtra_->deleteLater();
    modExtra_ = nullptr;
  }
  QString type = QString::fromStdString(idx < 0 ? n->object->type() : n->modifiers[idx]->type());
  if (ModifyRolloutFactory factory = modifyRolloutsFor(type)) {
    ModifyPanelContext ctx{doc_, state_, n->id, idx};
    modExtra_ = factory(ctx, modExtraHost_);
    if (modExtra_) modExtraHost_->layout()->addWidget(modExtra_);
  }
}

void CommandPanel::refreshModifyValues() {
  Node* n = doc_->scene().findNode(modNode_);
  if (!n) return;
  if (static_cast<int>(stack_->count()) != static_cast<int>(n->modifiers.size()) + 1 ||
      stackIdentity(n) != modBuiltFor_) {
    rebuildModify();
    return;
  }
  modForm_->refresh();
}

void CommandPanel::addModifier(const QString& type) {
  auto nodes = doc_->selectedNodes();
  QVector<uint32_t> ids;
  for (Node* n : nodes)
    if (n->kind() == ObjectKind::Geometry || n->kind() == ObjectKind::Shape) ids.push_back(n->id);
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
