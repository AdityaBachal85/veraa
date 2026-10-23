#include "dialogs.h"

#include <algorithm>

#include <QAction>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QPlainTextEdit>
#include <QTabWidget>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QRadioButton>
#include <QSpinBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include "icons.h"
#include "mainwindow.h"

namespace veraa {

SelectByNameDialog::SelectByNameDialog(SceneDocument* doc, QWidget* parent) : QDialog(parent) {
  setWindowTitle(tr("Select From Scene"));
  setWindowFlag(Qt::WindowContextHelpButtonHint, false);
  resize(360, 460);
  auto* v = new QVBoxLayout(this);
  filter_ = new QLineEdit(this);
  filter_->setPlaceholderText(tr("Find..."));
  filter_->setClearButtonEnabled(true);
  list_ = new QListWidget(this);
  list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  for (const auto& n : doc->scene().nodes) {
    if (n->hidden) continue;
    QIcon icon = n->kind() == ObjectKind::Light    ? icons::light()
                 : n->kind() == ObjectKind::Camera ? icons::camera()
                                                   : icons::object();
    auto* it = new QListWidgetItem(icon, QString::fromStdString(n->name), list_);
    it->setData(Qt::UserRole, n->id);
    it->setSelected(doc->isSelected(n->id));
  }
  auto* tools = new QHBoxLayout;
  auto* all = new QPushButton(tr("All"), this);
  auto* none = new QPushButton(tr("None"), this);
  auto* invert = new QPushButton(tr("Invert"), this);
  tools->addWidget(all);
  tools->addWidget(none);
  tools->addWidget(invert);
  auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  box->button(QDialogButtonBox::Ok)->setText(tr("Select"));
  v->addWidget(filter_);
  v->addWidget(list_);
  v->addLayout(tools);
  v->addWidget(box);
  connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(list_, &QListWidget::itemDoubleClicked, this, &QDialog::accept);
  connect(all, &QPushButton::clicked, list_, &QListWidget::selectAll);
  connect(none, &QPushButton::clicked, list_, &QListWidget::clearSelection);
  connect(invert, &QPushButton::clicked, this, [this] {
    for (int i = 0; i < list_->count(); ++i) list_->item(i)->setSelected(!list_->item(i)->isSelected());
  });
  connect(filter_, &QLineEdit::textChanged, this, [this](const QString& f) {
    for (int i = 0; i < list_->count(); ++i)
      list_->item(i)->setHidden(!f.isEmpty() && !list_->item(i)->text().contains(f, Qt::CaseInsensitive));
  });
  filter_->setFocus();
}

QVector<uint32_t> SelectByNameDialog::chosen() const {
  QVector<uint32_t> ids;
  for (int i = 0; i < list_->count(); ++i)
    if (list_->item(i)->isSelected() && !list_->item(i)->isHidden()) ids.push_back(list_->item(i)->data(Qt::UserRole).toUInt());
  return ids;
}

ObjImportDialog::ObjImportDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle(tr("OBJ Import Options"));
  setWindowFlag(Qt::WindowContextHelpButtonHint, false);
  auto* form = new QFormLayout(this);
  yUp_ = new QCheckBox(tr("File is Y-up (convert to Z-up)"), this);
  yUp_->setChecked(true);
  materials_ = new QCheckBox(tr("Import materials (.mtl)"), this);
  materials_->setChecked(true);
  scale_ = new QDoubleSpinBox(this);
  scale_->setRange(0.0001, 100000);
  scale_->setDecimals(4);
  scale_->setValue(1.0);
  scale_->setToolTip(tr("Example: 100 converts meters to centimeters"));
  form->addRow(yUp_);
  form->addRow(materials_);
  form->addRow(tr("Scale:"), scale_);
  auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  form->addRow(box);
  connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

ObjImportOptions ObjImportDialog::options() const {
  ObjImportOptions o;
  o.yUp = yUp_->isChecked();
  o.importMaterials = materials_->isChecked();
  o.scale = static_cast<float>(scale_->value());
  return o;
}

CloneOptionsDialog::CloneOptionsDialog(const QString& defaultName, bool showCopies, QWidget* parent)
    : QDialog(parent) {
  setWindowTitle(tr("Clone Options"));
  setWindowFlag(Qt::WindowContextHelpButtonHint, false);
  auto* v = new QVBoxLayout(this);
  auto* objBox = new QGroupBox(tr("Object"), this);
  auto* ov = new QVBoxLayout(objBox);
  auto* copy = new QRadioButton(tr("Copy"), objBox);
  copy->setChecked(true);
  auto* instance = new QRadioButton(tr("Instance"), objBox);
  auto* reference = new QRadioButton(tr("Reference"), objBox);
  instance->setObjectName("clone_instance");
  reference->setObjectName("clone_reference");
  instance->setToolTip(tr("Shares the object and its modifiers: editing one changes all"));
  reference->setToolTip(tr("Shares the base object; each copy keeps its own modifiers"));
  modeGroup_ = new QButtonGroup(this);
  modeGroup_->addButton(copy, Copy);
  modeGroup_->addButton(instance, Instance);
  modeGroup_->addButton(reference, Reference);
  ov->addWidget(copy);
  ov->addWidget(instance);
  ov->addWidget(reference);
  v->addWidget(objBox);
  auto* form = new QFormLayout;
  if (showCopies) {
    copies_ = new QSpinBox(this);
    copies_->setRange(1, 1000);
    copies_->setValue(1);
    form->addRow(tr("Number of copies:"), copies_);
  }
  name_ = new QLineEdit(defaultName, this);
  form->addRow(tr("Name:"), name_);
  v->addLayout(form);
  auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  v->addWidget(box);
  connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

int CloneOptionsDialog::copies() const { return copies_ ? copies_->value() : 1; }
void CloneOptionsDialog::setMode(int m) {
  if (QAbstractButton* b = modeGroup_->button(m)) b->setChecked(true);
}
QString CloneOptionsDialog::name() const { return name_->text().trimmed(); }

CloneOptionsDialog::Mode CloneOptionsDialog::mode() const {
  return static_cast<Mode>(std::max(0, modeGroup_->checkedId()));
}

void applyCloneMode(Scene& scene, uint32_t sourceId, uint32_t cloneId, CloneOptionsDialog::Mode mode) {
  if (mode == CloneOptionsDialog::Copy) return;
  Node* src = scene.findNode(sourceId);
  Node* dst = scene.findNode(cloneId);
  if (!src || !dst) return;
  dst->object = src->object;
  if (mode == CloneOptionsDialog::Instance) {
    dst->modifiers = src->modifiers;
  } else {
    dst->modifiers.clear();
    for (const auto& m : src->modifiers)
      if (m) dst->modifiers.push_back(m->clone());
  }
  dst->invalidate();
}

ObjectPropertiesDialog::ObjectPropertiesDialog(SceneDocument* doc, Node* node, QWidget* parent) : QDialog(parent) {
  setWindowTitle(tr("Object Properties"));
  setWindowFlag(Qt::WindowContextHelpButtonHint, false);
  auto* v = new QVBoxLayout(this);
  auto* tabs = new QTabWidget(this);
  v->addWidget(tabs);

  // ---- General ----
  auto* general = new QWidget(tabs);
  auto* gv = new QVBoxLayout(general);
  auto* info = new QGroupBox(tr("Object Information"), general);
  auto* form = new QFormLayout(info);
  auto* name = new QLineEdit(QString::fromStdString(node->name), info);
  form->addRow(tr("Name:"), name);
  form->addRow(tr("Type:"), new QLabel(QString::fromStdString(objectDisplayName(node->object->type())), info));
  Aabb b = node->worldBounds();
  if (b.valid()) {
    Vec3 e = b.extent();
    form->addRow(tr("Dimensions:"),
                 new QLabel(QString("X: %1  Y: %2  Z: %3").arg(e.x, 0, 'f', 2).arg(e.y, 0, 'f', 2).arg(e.z, 0, 'f', 2), info));
  }
  if (node->kind() == ObjectKind::Geometry) {
    const PolyMesh& m = node->mesh();
    form->addRow(tr("Vertices:"), new QLabel(QLocale().toString(static_cast<qulonglong>(m.positions.size())), info));
    form->addRow(tr("Faces:"), new QLabel(QLocale().toString(static_cast<qulonglong>(m.faces.size())), info));
    const Material* mat = doc->scene().findMaterial(node->materialId);
    form->addRow(tr("Material:"), new QLabel(mat ? QString::fromStdString(mat->name) : tr("None"), info));
  }
  Node* parentNode = node->parent();
  form->addRow(tr("Parent:"), new QLabel(parentNode ? QString::fromStdString(parentNode->name) : tr("Scene Root"), info));
  form->addRow(tr("Children:"), new QLabel(QString::number(doc->scene().children(node->id).size()), info));
  auto* layer = new QComboBox(info);
  for (const Layer& l : doc->scene().layers) layer->addItem(QString::fromStdString(l.name), l.id);
  layer->setCurrentIndex(std::max(0, layer->findData(node->layerId)));
  form->addRow(tr("Layer:"), layer);
  gv->addWidget(info);

  auto* flagsRow = new QHBoxLayout;
  auto* interactivity = new QGroupBox(tr("Interactivity / Display"), general);
  auto* iv = new QVBoxLayout(interactivity);
  auto* render = new QGroupBox(tr("Rendering Control"), general);
  auto* rv = new QVBoxLayout(render);
  struct Flag {
    QCheckBox* box;
    bool Node::*member;
  };
  QList<Flag> flags;
  auto addFlag = [&](QGroupBox* group, QVBoxLayout* l, const QString& text, bool Node::*member) {
    auto* c = new QCheckBox(text, group);
    c->setChecked(node->*member);
    l->addWidget(c);
    flags.push_back({c, member});
  };
  addFlag(interactivity, iv, tr("Hide"), &Node::hidden);
  addFlag(interactivity, iv, tr("Freeze"), &Node::frozen);
  addFlag(interactivity, iv, tr("See-Through"), &Node::seeThrough);
  addFlag(interactivity, iv, tr("Display as Box"), &Node::displayAsBox);
  addFlag(interactivity, iv, tr("Backface Cull"), &Node::backfaceCull);
  addFlag(render, rv, tr("Renderable"), &Node::renderable);
  addFlag(render, rv, tr("Visible to Camera"), &Node::visibleToCamera);
  addFlag(render, rv, tr("Visible to Reflection/Refraction"), &Node::visibleToReflections);
  addFlag(render, rv, tr("Receive Shadows"), &Node::receiveShadows);
  addFlag(render, rv, tr("Cast Shadows"), &Node::castShadows);
  addFlag(render, rv, tr("Motion Blur"), &Node::motionBlur);
  auto* gb = new QSpinBox(render);
  gb->setRange(0, 65535);
  gb->setValue(node->gBufferId);
  auto* gbRow = new QHBoxLayout;
  gbRow->addWidget(new QLabel(tr("Object ID:"), render));
  gbRow->addWidget(gb);
  rv->addLayout(gbRow);
  flagsRow->addWidget(interactivity);
  flagsRow->addWidget(render);
  gv->addLayout(flagsRow);
  tabs->addTab(general, tr("General"));

  // ---- User Defined ----
  auto* user = new QWidget(tabs);
  auto* uv = new QVBoxLayout(user);
  uv->addWidget(new QLabel(tr("User Defined Properties:"), user));
  auto* props = new QPlainTextEdit(QString::fromStdString(node->userProps), user);
  props->setPlaceholderText(tr("key = value, one per line"));
  uv->addWidget(props);
  tabs->addTab(user, tr("User Defined"));

  auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  v->addWidget(box);
  connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
  uint32_t id = node->id;
  connect(box, &QDialogButtonBox::accepted, this, [this, doc, id, name, layer, flags, gb, props] {
    Node* n = doc->scene().findNode(id);
    if (n) {
      doc->beginEdit(tr("Object Properties"));
      if (!name->text().trimmed().isEmpty()) n->name = name->text().trimmed().toStdString();
      n->userProps = props->toPlainText().toStdString();
      // Flags, layer and object ID go to every selected object (like 3ds Max).
      QVector<Node*> targets = doc->selectedNodes();
      if (!targets.contains(n)) targets.push_back(n);
      QVector<uint32_t> ids;
      for (Node* t : targets) {
        for (const Flag& f : flags) t->*(f.member) = f.box->isChecked();
        t->layerId = layer->currentData().toUInt();
        t->gBufferId = gb->value();
        ids.push_back(t->id);
      }
      QVector<uint32_t> keep;
      for (uint32_t sid : doc->selection())
        if (Node* t = doc->scene().findNode(sid); t && !t->hidden && !t->frozen) keep.push_back(sid);
      doc->setSelection(keep);
      doc->notifyNodesChanged(ids, ChangeName | ChangeDisplay);
      doc->notifyStructureChanged();
      doc->endEdit();
    }
    accept();
  });
}

void showShortcutsDialog(QWidget* parent) {
  QDialog dlg(parent);
  dlg.setWindowTitle(QObject::tr("Keyboard Shortcuts"));
  dlg.setWindowFlag(Qt::WindowContextHelpButtonHint, false);
  dlg.resize(520, 640);
  auto* v = new QVBoxLayout(&dlg);
  const char* rows[][2] = {
      {"Q / W / E / R", "Select / Move / Rotate / Scale"},
      {"F5 / F6 / F7 / F8", "Constrain to X / Y / Z / cycle planes"},
      {"Shift + drag (Move)", "Clone while moving"},
      {"Ctrl + click / Alt + click", "Add to / remove from selection"},
      {"Ctrl+A / Ctrl+D / Ctrl+I", "Select all / none / invert"},
      {"H", "Select by name"},
      {"Delete", "Delete selection"},
      {"Ctrl+V", "Clone selection"},
      {"Ctrl+Z / Ctrl+Y", "Undo / Redo"},
      {"Middle mouse drag", "Pan view"},
      {"Alt + middle mouse drag", "Orbit view"},
      {"Ctrl + Alt + middle mouse drag", "Zoom view"},
      {"Mouse wheel", "Zoom toward cursor"},
      {"Z / Ctrl+Shift+Z", "Zoom extents selected / all viewports"},
      {"Shift+Z", "Undo view change"},
      {"Alt+W", "Maximize / restore active viewport"},
      {"T / B / F / L / P / U", "Top / Bottom / Front / Left / Perspective / Orthographic view"},
      {"C", "Camera view"},
      {"Ctrl+C", "Create camera from perspective view"},
      {"F3 / F4", "Wireframe <-> shaded / Edged faces"},
      {"G", "Toggle grid"},
      {"M", "Material Editor"},
      {"F10", "Render Setup"},
      {"Shift+Q / F9", "Render"},
      {"Esc", "Cancel creation or drag"},
      {"Right-click (while creating)", "Stop creating objects"},
      {"Ctrl+N / Ctrl+O / Ctrl+S", "New / Open / Save"},
  };
  QList<QPair<QString, QString>> list;
  // Every command with a hotkey (current assignments, including Hotkey Editor changes).
  if (auto* w = qobject_cast<MainWindow*>(parent)) {
    QHash<QString, QString> keys = w->hotkeys();
    for (auto it = keys.begin(); it != keys.end(); ++it) {
      QAction* a = w->command(it.key());
      if (it.value().isEmpty() || !a) continue;
      QString key = QKeySequence(it.value(), QKeySequence::PortableText).toString(QKeySequence::NativeText);
      list.push_back({key, a->text().remove('&').section('\t', 0, 0)});
    }
    std::sort(list.begin(), list.end(), [](const auto& x, const auto& y) { return x.second.toLower() < y.second.toLower(); });
    // Mouse and modifier gestures are not commands; keep them from the fixed list.
    for (const auto& r : rows) {
      QString k = QString::fromUtf8(r[0]);
      if (k.contains("mouse", Qt::CaseInsensitive) || k.contains("click", Qt::CaseInsensitive) ||
          k.contains("drag", Qt::CaseInsensitive) || k.contains("wheel", Qt::CaseInsensitive))
        list.push_back({k, QObject::tr(r[1])});
    }
  } else {
    for (const auto& r : rows) list.push_back({QString::fromUtf8(r[0]), QObject::tr(r[1])});
  }
  auto* filter = new QLineEdit(&dlg);
  filter->setPlaceholderText(QObject::tr("Find..."));
  filter->setClearButtonEnabled(true);
  v->addWidget(filter);
  auto* table = new QTableWidget(list.size(), 2, &dlg);
  table->setHorizontalHeaderLabels({QObject::tr("Shortcut"), QObject::tr("Action")});
  table->verticalHeader()->hide();
  table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  table->horizontalHeader()->setStretchLastSection(true);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  for (int i = 0; i < table->rowCount(); ++i) {
    table->setItem(i, 0, new QTableWidgetItem(list[i].first));
    table->setItem(i, 1, new QTableWidgetItem(list[i].second));
  }
  QObject::connect(filter, &QLineEdit::textChanged, table, [table](const QString& f) {
    for (int i = 0; i < table->rowCount(); ++i)
      table->setRowHidden(i, !f.isEmpty() && !table->item(i, 0)->text().contains(f, Qt::CaseInsensitive) &&
                                 !table->item(i, 1)->text().contains(f, Qt::CaseInsensitive));
  });
  v->addWidget(table);
  auto* box = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
  QObject::connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  v->addWidget(box);
  dlg.exec();
}

}  // namespace veraa
