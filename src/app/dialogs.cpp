#include "dialogs.h"

#include <QCheckBox>
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
  instance->setEnabled(false);
  reference->setEnabled(false);
  instance->setToolTip(tr("Instances are planned for a later version"));
  reference->setToolTip(tr("References are planned for a later version"));
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
QString CloneOptionsDialog::name() const { return name_->text().trimmed(); }

ObjectPropertiesDialog::ObjectPropertiesDialog(SceneDocument* doc, Node* node, QWidget* parent) : QDialog(parent) {
  setWindowTitle(tr("Object Properties"));
  setWindowFlag(Qt::WindowContextHelpButtonHint, false);
  auto* form = new QFormLayout(this);
  auto* name = new QLineEdit(QString::fromStdString(node->name), this);
  form->addRow(tr("Name:"), name);
  form->addRow(tr("Type:"), new QLabel(QString::fromStdString(objectDisplayName(node->object->type())), this));
  if (node->kind() == ObjectKind::Geometry) {
    const PolyMesh& m = node->mesh();
    form->addRow(tr("Vertices:"), new QLabel(QLocale().toString(static_cast<qulonglong>(m.positions.size())), this));
    form->addRow(tr("Polygons:"), new QLabel(QLocale().toString(static_cast<qulonglong>(m.faces.size())), this));
    form->addRow(tr("Triangles:"), new QLabel(QLocale().toString(static_cast<qulonglong>(m.triangleCount())), this));
    Aabb b = node->worldBounds();
    Vec3 e = b.extent();
    form->addRow(tr("Dimensions:"), new QLabel(QString("%1 x %2 x %3").arg(e.x, 0, 'f', 2).arg(e.y, 0, 'f', 2).arg(e.z, 0, 'f', 2), this));
    const Material* mat = doc->scene().findMaterial(node->materialId);
    form->addRow(tr("Material:"), new QLabel(mat ? QString::fromStdString(mat->name) : tr("None"), this));
  }
  auto* renderable = new QCheckBox(tr("Renderable"), this);
  renderable->setChecked(node->renderable);
  auto* hidden = new QCheckBox(tr("Hidden"), this);
  hidden->setChecked(node->hidden);
  form->addRow(renderable);
  form->addRow(hidden);
  auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  form->addRow(box);
  connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
  uint32_t id = node->id;
  connect(box, &QDialogButtonBox::accepted, this, [this, doc, id, name, renderable, hidden] {
    Node* n = doc->scene().findNode(id);
    if (n) {
      doc->beginEdit(tr("Object Properties"));
      if (!name->text().isEmpty()) n->name = name->text().toStdString();
      n->renderable = renderable->isChecked();
      n->hidden = hidden->isChecked();
      doc->notifyNodeChanged(n, ChangeName | ChangeDisplay);
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
  auto* table = new QTableWidget(static_cast<int>(sizeof(rows) / sizeof(rows[0])), 2, &dlg);
  table->setHorizontalHeaderLabels({QObject::tr("Shortcut"), QObject::tr("Action")});
  table->verticalHeader()->hide();
  table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  table->horizontalHeader()->setStretchLastSection(true);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  for (int i = 0; i < table->rowCount(); ++i) {
    table->setItem(i, 0, new QTableWidgetItem(QString::fromUtf8(rows[i][0])));
    table->setItem(i, 1, new QTableWidgetItem(QObject::tr(rows[i][1])));
  }
  v->addWidget(table);
  auto* box = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
  QObject::connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  v->addWidget(box);
  dlg.exec();
}

}  // namespace veraa
