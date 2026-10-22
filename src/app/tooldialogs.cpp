#include "tooldialogs.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextBrowser>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <cmath>
#include <map>
#include <set>

#include "editorstate.h"
#include "interfacedata.h"
#include "mainwindow.h"
#include "paramform.h"
#include "uistate.h"
#include "units.h"
#include "viewportpanel.h"

namespace veraa {

namespace {

QPushButton* button(const QString& text, QWidget* parent, const std::function<void()>& fn) {
  auto* b = new QPushButton(text, parent);
  QObject::connect(b, &QPushButton::clicked, parent, fn);
  return b;
}

void noHelp(QDialog* d) { d->setWindowFlag(Qt::WindowContextHelpButtonHint, false); }

}  // namespace

// ---------------------------------------------------------------------------------------------
// Transform Type-In

TransformTypeInDialog::TransformTypeInDialog(MainWindow* w) : QDialog(w), w_(w) {
  noHelp(this);
  setAttribute(Qt::WA_DeleteOnClose, false);
  auto* h = new QHBoxLayout(this);
  auto* absBox = new QGroupBox(this);
  auto* offBox = new QGroupBox(this);
  auto* af = new QFormLayout(absBox);
  auto* of = new QFormLayout(offBox);
  absTitle_ = new QLabel(tr("Absolute:World"), absBox);
  offTitle_ = new QLabel(tr("Offset:World"), offBox);
  af->addRow(absTitle_);
  of->addRow(offTitle_);
  const char* names[3] = {"X:", "Y:", "Z:"};
  for (int i = 0; i < 3; ++i) {
    abs_[i] = new UnitSpinBox(absBox);
    off_[i] = new UnitSpinBox(offBox);
    for (UnitSpinBox* sb : {abs_[i], off_[i]}) {
      sb->setRange(-1e9, 1e9);
      sb->setDecimals(3);
      sb->setKeyboardTracking(false);
      sb->setMinimumWidth(110);
      installSpinnerScrub(sb);
    }
    af->addRow(names[i], abs_[i]);
    of->addRow(names[i], off_[i]);
    connect(abs_[i], QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, i](double v) {
      if (!updating_) apply(false, i, v);
    });
    connect(off_[i], QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, i](double v) {
      if (!updating_) apply(true, i, v);
    });
  }
  h->addWidget(absBox);
  h->addWidget(offBox);
  SceneDocument* doc = w->document();
  connect(doc, &SceneDocument::selectionChanged, this, &TransformTypeInDialog::refresh);
  connect(doc, &SceneDocument::nodesChanged, this, [this](const QVector<uint32_t>&, int f) {
    if (f & ChangeTransform) refresh();
  });
  connect(w->editorState(), &EditorState::toolChanged, this, &TransformTypeInDialog::refresh);
  refresh();
}

void TransformTypeInDialog::refresh() {
  if (updating_) return;
  updating_ = true;
  TransformTool tool = w_->editorState()->tool();
  QString title = tool == TransformTool::Rotate ? tr("Rotate Transform Type-In")
                  : tool == TransformTool::Scale ? tr("Scale Transform Type-In")
                                                 : tr("Move Transform Type-In");
  setWindowTitle(title);
  bool length = tool == TransformTool::Move || tool == TransformTool::Select;
  absTitle_->setText(tool == TransformTool::Scale ? tr("Absolute:Local") : tr("Absolute:World"));
  offTitle_->setText(tool == TransformTool::Scale ? tr("Offset:World (%)") : tr("Offset:World"));
  auto nodes = w_->document()->selectedNodes();
  for (int i = 0; i < 3; ++i) {
    for (UnitSpinBox* sb : {abs_[i], off_[i]}) {
      sb->setLengthMode(length);
      sb->setSuffix(tool == TransformTool::Rotate ? QString::fromUtf8("°") : tool == TransformTool::Scale ? "%" : "");
      sb->setEnabled(!nodes.isEmpty());
    }
    double v = 0;
    if (!nodes.isEmpty()) {
      Node* n = nodes[0];
      v = length ? n->worldPosition()[i]
          : tool == TransformTool::Rotate ? n->transform.rotation[i]
                                          : n->transform.scale[i] * 100.0;
    }
    abs_[i]->setValue(v);
    off_[i]->setValue(tool == TransformTool::Scale ? 100.0 : 0.0);
  }
  updating_ = false;
}

void TransformTypeInDialog::apply(bool offset, int axis, double v) {
  // The status bar type-in implements the same math; route through it with the matching mode.
  UiState& ui = UiState::instance();
  bool wasAbs = ui.absoluteTypeIn();
  ui.setAbsoluteTypeIn(!offset);
  SceneDocument* doc = w_->document();
  auto nodes = doc->selectedNodes();
  if (!nodes.isEmpty()) {
    TransformTool tool = w_->editorState()->tool();
    doc->beginEdit(tr("Transform Type-In"));
    QVector<uint32_t> ids;
    for (Node* n : nodes) {
      if (tool == TransformTool::Rotate) {
        if (!offset) {
          n->transform.rotation[axis] = static_cast<float>(v);
        } else {
          Vec3 wa(axis == 0 ? 1.0f : 0.0f, axis == 1 ? 1.0f : 0.0f, axis == 2 ? 1.0f : 0.0f);
          Vec3 la = normalize(n->parentWorldMatrix().inverse().transformDir(wa));
          n->transform.rotation =
              eulerDegFromMatrix(Mat4::rotateAxis(la, radians(static_cast<float>(v))) * Mat4::fromEulerDeg(n->transform.rotation));
        }
      } else if (tool == TransformTool::Scale) {
        float f = static_cast<float>(v) / 100.0f;
        if (!offset) n->transform.scale[axis] = f;
        else n->transform.scale[axis] *= f;
      } else {
        Vec3 p = n->worldPosition();
        if (!offset) p[axis] = static_cast<float>(v);
        else p[axis] += static_cast<float>(v);
        n->setWorldPosition(p);
      }
      ids.push_back(n->id);
    }
    doc->notifyNodesChanged(ids, ChangeTransform);
    doc->endEdit();
  }
  ui.setAbsoluteTypeIn(wasAbs);
  refresh();
}

// ---------------------------------------------------------------------------------------------
// Transform Toolbox

TransformToolbox::TransformToolbox(MainWindow* w) : QDialog(w), w_(w) {
  noHelp(this);
  setWindowTitle(tr("Transform Toolbox"));
  auto* v = new QVBoxLayout(this);

  auto* rot = new QGroupBox(tr("Rotate"), this);
  auto* rg = new QGridLayout(rot);
  angle_ = new QDoubleSpinBox(rot);
  angle_->setRange(-360, 360);
  angle_->setValue(90);
  angle_->setSuffix(QString::fromUtf8("°"));
  rg->addWidget(angle_, 0, 0, 1, 3);
  const char* axes[3] = {"X", "Y", "Z"};
  for (int a = 0; a < 3; ++a) {
    rg->addWidget(button(QString("%1 -").arg(axes[a]), rot, [this, a] { rotateSelection(a, -static_cast<float>(angle_->value())); }), 1, a);
    rg->addWidget(button(QString("%1 +").arg(axes[a]), rot, [this, a] { rotateSelection(a, static_cast<float>(angle_->value())); }), 2, a);
  }
  v->addWidget(rot);

  auto* sc = new QGroupBox(tr("Scale"), this);
  auto* sh = new QHBoxLayout(sc);
  percent_ = new QDoubleSpinBox(sc);
  percent_->setRange(1, 10000);
  percent_->setValue(50);
  percent_->setSuffix("%");
  sh->addWidget(percent_);
  sh->addWidget(button(tr("Shrink"), sc, [this] { scaleSelection(1.0f - static_cast<float>(percent_->value()) / 100.0f); }));
  sh->addWidget(button(tr("Grow"), sc, [this] { scaleSelection(1.0f + static_cast<float>(percent_->value()) / 100.0f); }));
  v->addWidget(sc);

  auto* align = new QGroupBox(tr("Align Pivot"), this);
  auto* ah = new QHBoxLayout(align);
  ah->addWidget(button(tr("Center"), align, [this] { alignPivot(0); }));
  ah->addWidget(button(tr("Min"), align, [this] { alignPivot(1); }));
  ah->addWidget(button(tr("Max"), align, [this] { alignPivot(2); }));
  v->addWidget(align);

  auto* size = new QGroupBox(tr("Object Size"), this);
  auto* zh = new QHBoxLayout(size);
  sizeAxis_ = new QComboBox(size);
  sizeAxis_->addItems({"X", "Y", "Z", tr("All")});
  size_ = new UnitSpinBox(size);
  size_->setRange(0.001, 1e9);
  size_->setValue(100);
  zh->addWidget(sizeAxis_);
  zh->addWidget(size_);
  zh->addWidget(button(tr("Set Size"), size, [this] { setSize(sizeAxis_->currentIndex(), size_->value()); }));
  v->addWidget(size);
}

void TransformToolbox::rotateSelection(int axis, float deg) {
  SceneDocument* doc = w_->document();
  auto nodes = doc->selectedNodes();
  if (nodes.isEmpty()) return;
  doc->beginEdit(tr("Rotate"));
  QVector<uint32_t> ids;
  for (Node* n : nodes) {
    Vec3 wa(axis == 0 ? 1.0f : 0.0f, axis == 1 ? 1.0f : 0.0f, axis == 2 ? 1.0f : 0.0f);
    Vec3 la = normalize(n->parentWorldMatrix().inverse().transformDir(wa));
    n->transform.rotation = eulerDegFromMatrix(Mat4::rotateAxis(la, radians(deg)) * Mat4::fromEulerDeg(n->transform.rotation));
    ids.push_back(n->id);
  }
  doc->notifyNodesChanged(ids, ChangeTransform);
  doc->endEdit();
}

void TransformToolbox::scaleSelection(float f) {
  SceneDocument* doc = w_->document();
  auto nodes = doc->selectedNodes();
  if (nodes.isEmpty() || f <= 0) return;
  doc->beginEdit(tr("Scale"));
  QVector<uint32_t> ids;
  for (Node* n : nodes) {
    n->transform.scale = n->transform.scale * f;
    ids.push_back(n->id);
  }
  doc->notifyNodesChanged(ids, ChangeTransform);
  doc->endEdit();
}

void TransformToolbox::alignPivot(int mode) {
  // Moves each pivot to the center / minimum / maximum of its object's bounding box without
  // moving the geometry (adjusts the object offset).
  SceneDocument* doc = w_->document();
  auto nodes = doc->selectedNodes();
  if (nodes.isEmpty()) return;
  doc->beginEdit(tr("Align Pivot"));
  QVector<uint32_t> ids;
  for (Node* n : nodes) {
    Aabb b = n->worldBounds();
    if (!b.valid()) continue;
    Vec3 target = mode == 0 ? b.center() : mode == 1 ? Vec3(b.center().x, b.center().y, b.lo.z) : Vec3(b.center().x, b.center().y, b.hi.z);
    Mat4 geom = n->objectWorldMatrix();
    n->setWorldPosition(target);
    // Keep the geometry where it was: objectOffset = inverse(new node world) * old geometry world.
    n->objectOffset = decomposeTransform(n->worldMatrix().inverse() * geom);
    for (Node* c : doc->scene().children(n->id)) c->invalidate();
    n->invalidate();
    ids.push_back(n->id);
  }
  doc->notifyNodesChanged(ids, ChangeTransform | ChangeGeometry);
  doc->endEdit();
}

void TransformToolbox::setSize(int axis, double size) {
  SceneDocument* doc = w_->document();
  auto nodes = doc->selectedNodes();
  if (nodes.isEmpty()) return;
  doc->beginEdit(tr("Object Size"));
  QVector<uint32_t> ids;
  for (Node* n : nodes) {
    Aabb b = n->worldBounds();
    if (!b.valid()) continue;
    Vec3 e = b.extent();
    for (int a = 0; a < 3; ++a) {
      if (axis != 3 && a != axis) continue;
      if (e[a] > 1e-6f) n->transform.scale[a] *= static_cast<float>(size) / e[a];
    }
    ids.push_back(n->id);
  }
  doc->notifyNodesChanged(ids, ChangeTransform);
  doc->endEdit();
}

// ---------------------------------------------------------------------------------------------
// Selection sets

SelectionSetsDialog::SelectionSetsDialog(SceneDocument* doc, QWidget* parent) : QDialog(parent), doc_(doc) {
  noHelp(this);
  setWindowTitle(tr("Named Selection Sets"));
  resize(420, 460);
  auto* v = new QVBoxLayout(this);
  auto* bar = new QHBoxLayout;
  bar->addWidget(button(tr("Create New Set"), this, [this] {
    bool ok = false;
    QString name = QInputDialog::getText(this, tr("Create New Set"), tr("Name:"), QLineEdit::Normal,
                                         tr("Selection Set %1").arg(selectionSetNames(doc_->scene()).size() + 1), &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    setSelectionSet(doc_, name.trimmed(), doc_->selection());
    rebuild();
  }));
  bar->addWidget(button(tr("Remove"), this, [this] {
    if (currentSet().isEmpty()) return;
    removeSelectionSet(doc_, currentSet());
    rebuild();
  }));
  bar->addWidget(button(tr("Add Selected"), this, [this] {
    QString s = currentSet();
    if (s.isEmpty()) return;
    QVector<uint32_t> ids = selectionSetMembers(doc_->scene(), s);
    for (uint32_t id : doc_->selection())
      if (!ids.contains(id)) ids.push_back(id);
    setSelectionSet(doc_, s, ids);
    rebuild();
  }));
  bar->addWidget(button(tr("Subtract Selected"), this, [this] {
    QString s = currentSet();
    if (s.isEmpty()) return;
    QVector<uint32_t> ids = selectionSetMembers(doc_->scene(), s);
    for (uint32_t id : doc_->selection()) ids.removeAll(id);
    setSelectionSet(doc_, s, ids);
    rebuild();
  }));
  v->addLayout(bar);
  auto* bar2 = new QHBoxLayout;
  bar2->addWidget(button(tr("Select Objects in Set"), this, [this] {
    if (!currentSet().isEmpty()) doc_->setSelection(selectionSetMembers(doc_->scene(), currentSet()));
  }));
  bar2->addWidget(button(tr("Rename"), this, [this] {
    QString s = currentSet();
    if (s.isEmpty()) return;
    bool ok = false;
    QString name = QInputDialog::getText(this, tr("Rename Set"), tr("Name:"), QLineEdit::Normal, s, &ok);
    if (ok) renameSelectionSet(doc_, s, name.trimmed());
    rebuild();
  }));
  bar2->addStretch(1);
  v->addLayout(bar2);
  tree_ = new QTreeWidget(this);
  tree_->setHeaderHidden(true);
  v->addWidget(tree_, 1);
  auto* box = new QDialogButtonBox(QDialogButtonBox::Close, this);
  connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
  v->addWidget(box);
  rebuild();
}

QString SelectionSetsDialog::currentSet() const {
  QTreeWidgetItem* it = tree_->currentItem();
  if (!it) return QString();
  while (it->parent()) it = it->parent();
  return it->text(0);
}

void SelectionSetsDialog::rebuild() {
  QString cur = currentSet();
  tree_->clear();
  for (const QString& name : selectionSetNames(doc_->scene())) {
    auto* item = new QTreeWidgetItem(tree_, {name});
    for (uint32_t id : selectionSetMembers(doc_->scene(), name))
      if (Node* n = doc_->scene().findNode(id)) new QTreeWidgetItem(item, {QString::fromStdString(n->name)});
    if (name == cur) tree_->setCurrentItem(item);
    item->setExpanded(true);
  }
}

// ---------------------------------------------------------------------------------------------
// Display Floater

DisplayFloater::DisplayFloater(MainWindow* w) : QDialog(w), w_(w) {
  noHelp(this);
  setWindowTitle(tr("Display Floater"));
  SceneDocument* doc = w->document();
  auto* tabs = new QTabWidget(this);
  auto* v = new QVBoxLayout(this);
  v->addWidget(tabs);

  auto setFlag = [doc](const QString& label, const std::function<bool(Node&)>& fn) {
    doc->beginEdit(label);
    QVector<uint32_t> ids;
    for (const auto& n : doc->scene().nodes)
      if (fn(*n)) ids.push_back(n->id);
    doc->notifyNodesChanged(ids, ChangeDisplay);
    doc->endEdit();
  };

  // Hide/Freeze tab.
  auto* hf = new QWidget(tabs);
  auto* g = new QGridLayout(hf);
  g->addWidget(new QLabel(tr("<b>Hide</b>"), hf), 0, 0);
  g->addWidget(new QLabel(tr("<b>Freeze</b>"), hf), 0, 1);
  g->addWidget(button(tr("Selected"), hf, [doc, setFlag] {
                 setFlag(tr("Hide Selected"), [doc](Node& n) {
                   if (!doc->isSelected(n.id)) return false;
                   n.hidden = true;
                   return true;
                 });
                 doc->clearSelection();
               }), 1, 0);
  g->addWidget(button(tr("Unselected"), hf, [doc, setFlag] {
                 setFlag(tr("Hide Unselected"), [doc](Node& n) {
                   if (doc->isSelected(n.id) || n.hidden) return false;
                   n.hidden = true;
                   return true;
                 });
               }), 2, 0);
  g->addWidget(button(tr("Unhide All"), hf, [doc] { doc->unhideAll(); }), 3, 0);
  g->addWidget(button(tr("Selected"), hf, [doc, setFlag] {
                 setFlag(tr("Freeze Selected"), [doc](Node& n) {
                   if (!doc->isSelected(n.id)) return false;
                   n.frozen = true;
                   return true;
                 });
                 doc->clearSelection();
               }), 1, 1);
  g->addWidget(button(tr("Unselected"), hf, [doc, setFlag] {
                 setFlag(tr("Freeze Unselected"), [doc](Node& n) {
                   if (doc->isSelected(n.id) || n.frozen) return false;
                   n.frozen = true;
                   return true;
                 });
               }), 2, 1);
  g->addWidget(button(tr("Unfreeze All"), hf, [setFlag] {
                 setFlag(tr("Unfreeze All"), [](Node& n) {
                   if (!n.frozen) return false;
                   n.frozen = false;
                   return true;
                 });
               }), 3, 1);
  g->setRowStretch(4, 1);
  tabs->addTab(hf, tr("Hide/Freeze"));

  // Object Level tab: hide by category.
  auto* ol = new QWidget(tabs);
  auto* og = new QVBoxLayout(ol);
  og->addWidget(new QLabel(tr("Hide by Category"), ol));
  const struct {
    const char* name;
    ObjectKind kind;
  } cats[] = {{"Geometry", ObjectKind::Geometry}, {"Shapes", ObjectKind::Shape},   {"Lights", ObjectKind::Light},
              {"Cameras", ObjectKind::Camera},   {"Helpers", ObjectKind::Helper}, {"Space Warps", ObjectKind::SpaceWarp}};
  for (const auto& c : cats) {
    auto* cb = new QCheckBox(tr(c.name), ol);
    ObjectKind kind = c.kind;
    connect(cb, &QCheckBox::toggled, this, [doc, kind, setFlag, cb](bool on) {
      setFlag(on ? tr("Hide %1").arg(cb->text()) : tr("Unhide %1").arg(cb->text()), [kind, on](Node& n) {
        if (n.kind() != kind || n.hidden == on) return false;
        n.hidden = on;
        return true;
      });
      Q_UNUSED(doc);
    });
    og->addWidget(cb);
  }
  og->addWidget(new QLabel(tr("Display Properties (selection)"), ol));
  const struct {
    const char* name;
    bool Node::*flag;
  } props[] = {{"Display as Box", &Node::displayAsBox}, {"Backface Cull", &Node::backfaceCull}, {"See-Through", &Node::seeThrough}};
  for (const auto& p : props) {
    auto* row = new QHBoxLayout;
    row->addWidget(new QLabel(tr(p.name), ol), 1);
    bool Node::*flag = p.flag;
    QString name = tr(p.name);
    row->addWidget(button(tr("On"), ol, [doc, setFlag, flag, name] {
      setFlag(name, [doc, flag](Node& n) {
        if (!doc->isSelected(n.id)) return false;
        n.*flag = true;
        return true;
      });
    }));
    row->addWidget(button(tr("Off"), ol, [doc, setFlag, flag, name] {
      setFlag(name, [doc, flag](Node& n) {
        if (!doc->isSelected(n.id)) return false;
        n.*flag = false;
        return true;
      });
    }));
    og->addLayout(row);
  }
  og->addStretch(1);
  tabs->addTab(ol, tr("Object Level"));
}

// ---------------------------------------------------------------------------------------------
// Scene states

SceneStatesDialog::SceneStatesDialog(SceneDocument* doc, QWidget* parent) : QDialog(parent), doc_(doc) {
  noHelp(this);
  setWindowTitle(tr("Manage Scene States"));
  resize(420, 380);
  auto* h = new QHBoxLayout(this);
  list_ = new QListWidget(this);
  h->addWidget(list_, 1);
  auto* v = new QVBoxLayout;
  const char* names[6] = {"Hidden", "Frozen", "Transforms", "Materials", "Layers", "Display"};
  auto* partsBox = new QGroupBox(tr("Parts to save"), this);
  auto* pv = new QVBoxLayout(partsBox);
  for (int i = 0; i < 6; ++i) {
    parts_[i] = new QCheckBox(tr(names[i]), partsBox);
    parts_[i]->setChecked(true);
    pv->addWidget(parts_[i]);
  }
  v->addWidget(partsBox);
  v->addWidget(button(tr("Save..."), this, [this] {
    bool ok = false;
    QString name = QInputDialog::getText(this, tr("Save Scene State"), tr("Name:"), QLineEdit::Normal,
                                         tr("State %1").arg(list_->count() + 1), &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    int mask = 0;
    for (int i = 0; i < 6; ++i)
      if (parts_[i]->isChecked()) mask |= 1 << i;
    saveSceneState(doc_, name.trimmed(), mask);
    rebuild();
  }));
  v->addWidget(button(tr("Restore"), this, [this] {
    if (list_->currentItem()) restoreSceneState(doc_, list_->currentItem()->text());
  }));
  v->addWidget(button(tr("Delete"), this, [this] {
    if (!list_->currentItem()) return;
    deleteSceneState(doc_, list_->currentItem()->text());
    rebuild();
  }));
  v->addStretch(1);
  v->addWidget(button(tr("Close"), this, [this] { accept(); }));
  h->addLayout(v);
  connect(list_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* it) { restoreSceneState(doc_, it->text()); });
  rebuild();
}

void SceneStatesDialog::rebuild() {
  list_->clear();
  for (const QString& n : sceneStateNames(doc_->scene())) list_->addItem(n);
}

// ---------------------------------------------------------------------------------------------
// Light Lister

LightListerDialog::LightListerDialog(SceneDocument* doc, QWidget* parent) : QDialog(parent), doc_(doc) {
  noHelp(this);
  setWindowTitle(tr("Light Lister"));
  resize(640, 320);
  auto* v = new QVBoxLayout(this);
  table_ = new QTableWidget(0, 6, this);
  table_->setHorizontalHeaderLabels({tr("On"), tr("Name"), tr("Type"), tr("Multiplier"), tr("Color"), tr("Shadows")});
  table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
  table_->verticalHeader()->hide();
  v->addWidget(table_);
  auto* bar = new QHBoxLayout;
  bar->addWidget(button(tr("Refresh"), this, [this] { rebuild(); }));
  bar->addStretch(1);
  v->addLayout(bar);
  connect(doc_, &SceneDocument::structureChanged, this, &LightListerDialog::rebuild);
  connect(doc_, &SceneDocument::nodesChanged, this, [this](const QVector<uint32_t>&, int f) {
    if ((f & (ChangeGeometry | ChangeName)) && !rebuilding_ && !doc_->inEdit()) rebuild();
  });
  rebuild();
}

void LightListerDialog::rebuild() {
  rebuilding_ = true;
  table_->setRowCount(0);
  for (const auto& up : doc_->scene().nodes) {
    Node* n = up.get();
    if (!n->light()) continue;
    int row = table_->rowCount();
    table_->insertRow(row);
    uint32_t id = n->id;
    ParamBlock& p = n->object->params;
    auto edit = [this, id](const QString& label, const std::function<void(Node&)>& fn) {
      Node* node = doc_->scene().findNode(id);
      if (!node) return;
      rebuilding_ = true;
      doc_->beginEdit(label);
      fn(*node);
      node->object->touch();
      node->invalidate();
      doc_->notifyNodeChanged(node, ChangeGeometry | ChangeName);
      doc_->endEdit();
      rebuilding_ = false;
    };
    auto* on = new QCheckBox(table_);
    on->setChecked(!p.has("enabled") || p.getBool("enabled"));
    connect(on, &QCheckBox::toggled, this, [edit](bool c) { edit(tr("Light On"), [c](Node& nd) { nd.object->params.setNumber("enabled", c ? 1 : 0); }); });
    table_->setCellWidget(row, 0, on);
    auto* name = new QLineEdit(QString::fromStdString(n->name), table_);
    connect(name, &QLineEdit::editingFinished, this, [edit, name] {
      QString t = name->text().trimmed();
      if (!t.isEmpty()) edit(tr("Rename Light"), [t](Node& nd) { nd.name = t.toStdString(); });
    });
    table_->setCellWidget(row, 1, name);
    table_->setItem(row, 2, new QTableWidgetItem(QString::fromStdString(objectDisplayName(n->object->type()))));
    auto* mult = new QDoubleSpinBox(table_);
    mult->setRange(0, 1e6);
    mult->setDecimals(3);
    mult->setValue(p.has("intensity") ? p.getFloat("intensity") : 1.0);
    mult->setKeyboardTracking(false);
    connect(mult, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [edit](double val) { edit(tr("Light Multiplier"), [val](Node& nd) { nd.object->params.setNumber("intensity", val); }); });
    table_->setCellWidget(row, 3, mult);
    auto* color = new ColorSwatch(table_);
    color->setColor(p.has("color") ? p.getColor("color") : Vec3(1.0f));
    connect(color, &ColorSwatch::colorEdited, this,
            [edit](const Vec3& c) { edit(tr("Light Color"), [c](Node& nd) { nd.object->params.setColor("color", c); }); });
    table_->setCellWidget(row, 4, color);
    auto* shadows = new QCheckBox(table_);
    shadows->setChecked(!p.has("castShadows") || p.getBool("castShadows"));
    connect(shadows, &QCheckBox::toggled, this,
            [edit](bool c) { edit(tr("Light Shadows"), [c](Node& nd) { nd.object->params.setNumber("castShadows", c ? 1 : 0); }); });
    table_->setCellWidget(row, 5, shadows);
  }
  rebuilding_ = false;
}

// ---------------------------------------------------------------------------------------------
// Rename Objects

RenameObjectsDialog::RenameObjectsDialog(SceneDocument* doc, QWidget* parent) : QDialog(parent), doc_(doc) {
  noHelp(this);
  setWindowTitle(tr("Rename Objects"));
  auto* g = new QGridLayout(this);
  int r = 0;
  useBase_ = new QCheckBox(tr("Base Name"), this);
  base_ = new QLineEdit(tr("Object"), this);
  g->addWidget(useBase_, r, 0);
  g->addWidget(base_, r++, 1);
  usePrefix_ = new QCheckBox(tr("Prefix"), this);
  prefix_ = new QLineEdit(this);
  g->addWidget(usePrefix_, r, 0);
  g->addWidget(prefix_, r++, 1);
  removeFirst_ = new QCheckBox(tr("Remove First"), this);
  firstN_ = new QSpinBox(this);
  firstN_->setRange(0, 100);
  g->addWidget(removeFirst_, r, 0);
  g->addWidget(firstN_, r++, 1);
  useSuffix_ = new QCheckBox(tr("Suffix"), this);
  suffix_ = new QLineEdit(this);
  g->addWidget(useSuffix_, r, 0);
  g->addWidget(suffix_, r++, 1);
  removeLast_ = new QCheckBox(tr("Remove Last"), this);
  lastN_ = new QSpinBox(this);
  lastN_->setRange(0, 100);
  g->addWidget(removeLast_, r, 0);
  g->addWidget(lastN_, r++, 1);
  numbered_ = new QCheckBox(tr("Numbered"), this);
  auto* nh = new QHBoxLayout;
  baseNumber_ = new QSpinBox(this);
  baseNumber_->setRange(0, 1000000);
  baseNumber_->setPrefix(tr("Base: "));
  step_ = new QSpinBox(this);
  step_->setRange(1, 1000);
  step_->setPrefix(tr("Step: "));
  nh->addWidget(baseNumber_);
  nh->addWidget(step_);
  g->addWidget(numbered_, r, 0);
  g->addLayout(nh, r++, 1);
  auto* box = new QDialogButtonBox(this);
  QPushButton* renameBtn = box->addButton(tr("Rename"), QDialogButtonBox::AcceptRole);
  box->addButton(QDialogButtonBox::Close);
  g->addWidget(box, r, 0, 1, 2);
  connect(renameBtn, &QPushButton::clicked, this, [this] {
    int n = apply(doc_->selection());
    if (n == 0) QMessageBox::information(this, windowTitle(), tr("Select the objects to rename first."));
  });
  connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

int RenameObjectsDialog::apply(const QVector<uint32_t>& ids) {
  if (ids.isEmpty()) return 0;
  doc_->beginEdit(tr("Rename Objects"));
  int count = 0, number = baseNumber_->value();
  for (uint32_t id : ids) {
    Node* n = doc_->scene().findNode(id);
    if (!n) continue;
    QString name = useBase_->isChecked() ? base_->text() : QString::fromStdString(n->name);
    if (removeFirst_->isChecked()) name = name.mid(firstN_->value());
    if (removeLast_->isChecked()) name.chop(std::min(lastN_->value(), static_cast<int>(name.size())));
    if (usePrefix_->isChecked()) name = prefix_->text() + name;
    if (useSuffix_->isChecked()) name += suffix_->text();
    if (numbered_->isChecked()) {
      name += QString("%1").arg(number, 3, 10, QChar('0'));
      number += step_->value();
    }
    if (name.isEmpty()) continue;
    n->name = name.toStdString();
    ++count;
  }
  doc_->notifyNodesChanged(ids, ChangeName);
  doc_->endEdit();
  return count;
}

// ---------------------------------------------------------------------------------------------
// Color Clipboard

ColorClipboard::ColorClipboard(SceneDocument* doc, QWidget* parent) : QDialog(parent), doc_(doc) {
  noHelp(this);
  setWindowTitle(tr("Color Clipboard"));
  auto* v = new QVBoxLayout(this);
  auto* g = new QGridLayout;
  QSettings s("Veraa", "Veraa");
  for (int i = 0; i < 12; ++i) {
    swatches_[i] = new ColorSwatch(this);
    swatches_[i]->setMinimumSize(44, 22);
    swatches_[i]->setColor(vec3FromJson(json::parse(s.value(QString("colorClipboard/%1").arg(i), "[0.6,0.6,0.6]").toString().toStdString(), nullptr, false), Vec3(0.6f)));
    connect(swatches_[i], &ColorSwatch::colorEdited, this, [i](const Vec3& c) {
      QSettings st("Veraa", "Veraa");
      st.setValue(QString("colorClipboard/%1").arg(i), QString::fromStdString(vec3ToJson(c).dump()));
    });
    auto* copy = new QToolButton(this);
    copy->setText(tr("Copy"));
    copy->setToolTip(tr("Copy the selected object's color into this swatch"));
    connect(copy, &QToolButton::clicked, this, [this, i] {
      if (Node* n = doc_->firstSelected()) {
        swatches_[i]->setColor(n->wireColor);
        Q_EMIT swatches_[i]->colorEdited(n->wireColor);
      }
    });
    auto* paste = new QToolButton(this);
    paste->setText(tr("Paste"));
    paste->setToolTip(tr("Apply this color to the selected objects"));
    connect(paste, &QToolButton::clicked, this, [this, i] {
      auto nodes = doc_->selectedNodes();
      if (nodes.isEmpty()) return;
      doc_->beginEdit(tr("Paste Color"));
      QVector<uint32_t> ids;
      for (Node* n : nodes) {
        n->wireColor = swatches_[i]->color();
        ids.push_back(n->id);
      }
      doc_->notifyNodesChanged(ids, ChangeDisplay);
      doc_->endEdit();
    });
    g->addWidget(swatches_[i], i / 2, (i % 2) * 3);
    g->addWidget(copy, i / 2, (i % 2) * 3 + 1);
    g->addWidget(paste, i / 2, (i % 2) * 3 + 2);
  }
  v->addLayout(g);
}

// ---------------------------------------------------------------------------------------------
// Mesh Inspector

MeshInspectorDialog::Stats MeshInspectorDialog::inspect(const PolyMesh& m) {
  Stats s;
  s.vertices = m.positions.size();
  s.faces = m.faces.size();
  s.triangles = m.triangleCount();
  std::map<std::pair<int, int>, int> edgeUse;
  std::vector<bool> used(m.positions.size(), false);
  for (const auto& f : m.faces) {
    std::set<int> unique(f.v.begin(), f.v.end());
    if (f.v.size() < 3 || unique.size() < 3) ++s.degenerateFaces;
    for (size_t i = 0; i < f.v.size(); ++i) {
      int a = f.v[i], b = f.v[(i + 1) % f.v.size()];
      if (a >= 0 && a < static_cast<int>(used.size())) used[a] = true;
      if (a == b) continue;
      ++edgeUse[{std::min(a, b), std::max(a, b)}];
    }
  }
  for (const auto& [e, c] : edgeUse) {
    if (c == 1) ++s.openEdges;
    else if (c > 2) ++s.nonManifoldEdges;
  }
  for (bool u : used)
    if (!u) ++s.isolatedVertices;
  return s;
}

MeshInspectorDialog::MeshInspectorDialog(SceneDocument* doc, QWidget* parent) : QDialog(parent) {
  noHelp(this);
  setWindowTitle(tr("Mesh Inspector"));
  resize(620, 300);
  auto* v = new QVBoxLayout(this);
  auto* table = new QTableWidget(0, 8, this);
  table->setHorizontalHeaderLabels({tr("Object"), tr("Vertices"), tr("Faces"), tr("Triangles"), tr("Open Edges"),
                                    tr("Non-manifold"), tr("Degenerate"), tr("Isolated Verts")});
  table->verticalHeader()->hide();
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  auto nodes = doc->selectedNodes();
  if (nodes.isEmpty())
    for (const auto& n : doc->scene().nodes) nodes.push_back(n.get());
  for (Node* n : nodes) {
    if (n->kind() != ObjectKind::Geometry) continue;
    Stats s = inspect(n->mesh());
    int row = table->rowCount();
    table->insertRow(row);
    table->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(n->name)));
    size_t vals[7] = {s.vertices, s.faces, s.triangles, s.openEdges, s.nonManifoldEdges, s.degenerateFaces, s.isolatedVertices};
    for (int c = 0; c < 7; ++c) {
      auto* it = new QTableWidgetItem(QLocale().toString(static_cast<qulonglong>(vals[c])));
      if (c >= 3 && vals[c] > 0) it->setForeground(QColor(240, 170, 60));
      table->setItem(row, c + 1, it);
    }
  }
  table->resizeColumnsToContents();
  v->addWidget(table);
  auto* box = new QDialogButtonBox(QDialogButtonBox::Close, this);
  connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
  v->addWidget(box);
}

// ---------------------------------------------------------------------------------------------
// Viewport Configuration

namespace {

class LayoutButton : public QToolButton {
 public:
  LayoutButton(int layout, QWidget* parent) : QToolButton(parent), layout_(layout) {
    setCheckable(true);
    setFixedSize(54, 42);
    setToolTip(ViewportPanel::layoutName(layout));
  }

 protected:
  void paintEvent(QPaintEvent* e) override {
    QToolButton::paintEvent(e);
    QPainter p(this);
    QRectF r = QRectF(rect()).adjusted(6, 6, -6, -6);
    p.setPen(QPen(isChecked() ? QColor(245, 197, 24) : QColor(200, 200, 200), 1));
    for (const QRectF& c : ViewportPanel::layoutCells(layout_))
      p.drawRect(QRectF(r.x() + c.x() * r.width(), r.y() + c.y() * r.height(), c.width() * r.width(), c.height() * r.height()));
  }

 private:
  int layout_;
};

}  // namespace

ViewportConfigDialog::ViewportConfigDialog(ViewportPanel* panel, QWidget* parent, int tab) : QDialog(parent) {
  noHelp(this);
  setWindowTitle(tr("Viewport Configuration"));
  auto* v = new QVBoxLayout(this);
  auto* tabs = new QTabWidget(this);
  v->addWidget(tabs);
  Viewport* vp = panel->active();

  // ---- Layout ----
  auto* lay = new QWidget(tabs);
  auto* lv = new QVBoxLayout(lay);
  auto* grid = new QGridLayout;
  auto* group = new QButtonGroup(lay);
  for (int i = 0; i < ViewportPanel::layoutCount(); ++i) {
    auto* b = new LayoutButton(i, lay);
    b->setObjectName(QString("Layout%1").arg(i));
    b->setChecked(panel->viewLayout() == i);
    group->addButton(b, i);
    grid->addWidget(b, i / 7, i % 7);
  }
  lv->addLayout(grid);
  auto* slots = new QFormLayout;
  const ViewType types[] = {ViewType::Top,  ViewType::Bottom, ViewType::Front,       ViewType::Back,
                            ViewType::Left, ViewType::Right,  ViewType::Perspective, ViewType::User};
  QList<QComboBox*> combos;
  for (int i = 0; i < panel->count(); ++i) {
    auto* c = new QComboBox(lay);
    for (ViewType t : types) c->addItem(t == ViewType::User ? tr("Orthographic") : viewTypeName(t), static_cast<int>(t));
    int at = c->findData(static_cast<int>(panel->viewport(i)->camera().type));
    c->setCurrentIndex(at < 0 ? 6 : at);
    slots->addRow(tr("Viewport %1:").arg(i + 1), c);
    combos << c;
  }
  lv->addLayout(slots);
  lv->addStretch(1);
  tabs->addTab(lay, tr("Layout"));

  // ---- Display (visual style and lighting) ----
  UiState& ui = UiState::instance();
  auto* disp = new QWidget(tabs);
  auto* df = new QFormLayout(disp);
  auto* shading = new QComboBox(disp);
  shading->addItems({tr("Wireframe Override"), tr("Default Shading"), tr("Facets"), tr("Clay"), tr("Bounding Box")});
  shading->setCurrentIndex(static_cast<int>(vp->shading()));
  df->addRow(tr("Visual style (active viewport):"), shading);
  auto* edged = new QCheckBox(tr("Edged Faces"), disp);
  edged->setChecked(vp->edgedFaces());
  df->addRow(edged);
  auto* quality = new QComboBox(disp);
  quality->addItems({tr("Standard"), tr("High Quality"), tr("Performance")});
  quality->setCurrentIndex(static_cast<int>(vp->quality()));
  df->addRow(tr("Quality:"), quality);
  auto* lighting = new QComboBox(disp);
  lighting->addItems({tr("Default Lights"), tr("Scene Lights")});
  lighting->setCurrentIndex(static_cast<int>(vp->lighting()));
  df->addRow(tr("Lighting:"), lighting);
  auto* safe = new QCheckBox(tr("Show Safe Frame in Active View"), disp);
  safe->setChecked(vp->safeFrame());
  df->addRow(safe);
  auto* shadeSel = new QCheckBox(tr("Shade Selected Objects"), disp);
  shadeSel->setChecked(ui.shadeSelected());
  df->addRow(shadeSel);
  auto* brackets = new QCheckBox(tr("Selection Brackets"), disp);
  brackets->setChecked(ui.showSelectionBrackets());
  df->addRow(brackets);
  auto* frozenGray = new QCheckBox(tr("Show Frozen in Gray"), disp);
  frozenGray->setChecked(ui.showFrozenInGray());
  df->addRow(frozenGray);
  auto* cube = new QCheckBox(tr("Show the ViewCube"), disp);
  cube->setChecked(ui.showViewCube());
  df->addRow(cube);
  auto* gizmo = new QSpinBox(disp);
  gizmo->setRange(30, 250);
  gizmo->setValue(static_cast<int>(ui.gizmoSize()));
  gizmo->setSuffix(tr(" px"));
  df->addRow(tr("Gizmo size:"), gizmo);
  auto* wireColor = new QComboBox(disp);
  wireColor->addItems({tr("Object Color"), tr("Material Color")});
  wireColor->setCurrentIndex(ui.wireUsesObjectColor() ? 0 : 1);
  df->addRow(tr("Wireframe display color:"), wireColor);
  auto* shadedColor = new QComboBox(disp);
  shadedColor->addItems({tr("Object Color"), tr("Material Color")});
  shadedColor->setCurrentIndex(ui.shadedUsesObjectColor() ? 0 : 1);
  df->addRow(tr("Shaded display color:"), shadedColor);
  tabs->addTab(disp, tr("Display"));

  // ---- Background ----
  auto* bgw = new QWidget(tabs);
  auto* bf = new QFormLayout(bgw);
  ViewportBackground bg = vp->background();
  auto* mode = new QComboBox(bgw);
  mode->addItems({tr("Use Gradient Color"), tr("Use Solid Color"), tr("Use Files"), tr("Use Environment Background")});
  mode->setCurrentIndex(static_cast<int>(bg.mode));
  bf->addRow(tr("Background:"), mode);
  auto* solid = new ColorSwatch(bgw);
  solid->setColor(Vec3(bg.solid.redF(), bg.solid.greenF(), bg.solid.blueF()));
  bf->addRow(tr("Solid color:"), solid);
  auto* top = new ColorSwatch(bgw);
  top->setColor(Vec3(bg.top.redF(), bg.top.greenF(), bg.top.blueF()));
  bf->addRow(tr("Gradient top:"), top);
  auto* bottom = new ColorSwatch(bgw);
  bottom->setColor(Vec3(bg.bottom.redF(), bg.bottom.greenF(), bg.bottom.blueF()));
  bf->addRow(tr("Gradient bottom:"), bottom);
  auto* fileRow = new QHBoxLayout;
  auto* file = new QLineEdit(bg.imagePath, bgw);
  auto* browse = new QPushButton(tr("Files..."), bgw);
  fileRow->addWidget(file, 1);
  fileRow->addWidget(browse);
  bf->addRow(tr("Image:"), fileRow);
  connect(browse, &QPushButton::clicked, this, [this, file, mode] {
    QString f = QFileDialog::getOpenFileName(this, tr("Viewport Background Image"), file->text(),
                                             tr("Images (*.png *.jpg *.jpeg *.bmp *.tif *.tiff *.exr *.hdr);;All files (*)"));
    if (!f.isEmpty()) {
      file->setText(f);
      mode->setCurrentIndex(2);
    }
  });
  auto* applyAll = new QCheckBox(tr("Apply to all viewports"), bgw);
  bf->addRow(applyAll);
  tabs->addTab(bgw, tr("Background"));
  tabs->setCurrentIndex(std::clamp(tab, 0, 2));

  auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this);
  v->addWidget(box);
  auto apply = [=] {
    UiState& ui = UiState::instance();
    panel->setViewLayout(group->checkedId() < 0 ? panel->viewLayout() : group->checkedId());
    for (int i = 0; i < combos.size(); ++i) {
      auto t = static_cast<ViewType>(combos[i]->currentData().toInt());
      if (panel->viewport(i)->camera().type != t && panel->viewport(i)->camera().type != ViewType::Camera)
        panel->viewport(i)->setViewType(t);
    }
    Viewport* a = panel->active();
    a->setShading(static_cast<ShadingMode>(shading->currentIndex()));
    a->setEdgedFaces(edged->isChecked());
    a->setQuality(static_cast<ViewportQuality>(quality->currentIndex()));
    a->setLighting(static_cast<ViewportLighting>(lighting->currentIndex()));
    a->setSafeFrame(safe->isChecked());
    ui.setShadeSelected(shadeSel->isChecked());
    ui.setShowSelectionBrackets(brackets->isChecked());
    ui.setShowFrozenInGray(frozenGray->isChecked());
    ui.setShowViewCube(cube->isChecked());
    ui.setGizmoSize(static_cast<float>(gizmo->value()));
    ui.setDisplayColors(wireColor->currentIndex() == 0, shadedColor->currentIndex() == 0);
    ViewportBackground nb;
    nb.mode = static_cast<ViewportBackground::Mode>(mode->currentIndex());
    auto qc = [](const Vec3& c) { return QColor::fromRgbF(c.x, c.y, c.z); };
    nb.solid = qc(solid->color());
    nb.top = qc(top->color());
    nb.bottom = qc(bottom->color());
    nb.imagePath = file->text();
    if (applyAll->isChecked())
      for (int i = 0; i < panel->count(); ++i) panel->viewport(i)->setBackground(nb);
    else
      a->setBackground(nb);
  };
  connect(box->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, apply);
  connect(box, &QDialogButtonBox::accepted, this, [this, apply] {
    apply();
    accept();
  });
  connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

// ---------------------------------------------------------------------------------------------
// Workspaces

ManageWorkspacesDialog::ManageWorkspacesDialog(MainWindow* w) : QDialog(w), w_(w) {
  noHelp(this);
  setWindowTitle(tr("Manage Workspaces"));
  resize(380, 300);
  auto* h = new QHBoxLayout(this);
  list_ = new QListWidget(this);
  h->addWidget(list_, 1);
  auto* v = new QVBoxLayout;
  v->addWidget(button(tr("Save as New Workspace..."), this, [this] {
    bool ok = false;
    QString name = QInputDialog::getText(this, tr("New Workspace"), tr("Name:"), QLineEdit::Normal,
                                         tr("Workspace %1").arg(list_->count() + 1), &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    w_->saveWorkspace(name.trimmed());
    rebuild();
  }));
  v->addWidget(button(tr("Save Over Selected"), this, [this] {
    if (list_->currentItem()) w_->saveWorkspace(list_->currentItem()->text());
  }));
  v->addWidget(button(tr("Switch To"), this, [this] {
    if (list_->currentItem()) w_->loadWorkspace(list_->currentItem()->text());
  }));
  v->addWidget(button(tr("Delete"), this, [this] {
    if (!list_->currentItem()) return;
    w_->deleteWorkspace(list_->currentItem()->text());
    rebuild();
  }));
  v->addStretch(1);
  v->addWidget(button(tr("Close"), this, [this] { accept(); }));
  h->addLayout(v);
  rebuild();
}

void ManageWorkspacesDialog::rebuild() {
  list_->clear();
  list_->addItems(w_->workspaceNames());
}

// ---------------------------------------------------------------------------------------------
// Mirror

MirrorDialog::MirrorDialog(SceneDocument* doc, QWidget* parent) : QDialog(parent), doc_(doc) {
  noHelp(this);
  setWindowTitle(tr("Mirror: World Coordinates"));
  auto* v = new QVBoxLayout(this);
  auto* axisBox = new QGroupBox(tr("Mirror Axis"), this);
  auto* ag = new QGridLayout(axisBox);
  auto* axes = new QButtonGroup(this);
  const char* names[6] = {"X", "Y", "Z", "XY", "YZ", "ZX"};
  for (int i = 0; i < 6; ++i) {
    auto* r = new QRadioButton(names[i], axisBox);
    axes->addButton(r, i);
    ag->addWidget(r, i % 3, i / 3);
    if (i == 0) r->setChecked(true);
  }
  auto* offset = new UnitSpinBox(axisBox);
  offset->setRange(-1e9, 1e9);
  ag->addWidget(new QLabel(tr("Offset:"), axisBox), 3, 0);
  ag->addWidget(offset, 3, 1);
  v->addWidget(axisBox);
  auto* cloneBox = new QGroupBox(tr("Clone Selection"), this);
  auto* cv = new QVBoxLayout(cloneBox);
  auto* clones = new QButtonGroup(this);
  const char* cnames[4] = {"No Clone", "Copy", "Instance", "Reference"};
  for (int i = 0; i < 4; ++i) {
    auto* r = new QRadioButton(tr(cnames[i]), cloneBox);
    clones->addButton(r, i);
    cv->addWidget(r);
    if (i == 0) r->setChecked(true);
  }
  v->addWidget(cloneBox);
  auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  v->addWidget(box);
  connect(box, &QDialogButtonBox::accepted, this, [this, axes, offset, clones] {
    mirror(doc_, axes->checkedId(), static_cast<float>(offset->value()), clones->checkedId());
    accept();
  });
  connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void MirrorDialog::mirror(SceneDocument* doc, int axis, float offset, int clone) {
  auto nodes = doc->selectedNodes();
  if (nodes.isEmpty()) return;
  doc->beginEdit(tr("Mirror"));
  Vec3 c = doc->selectionCenter();
  Vec3 s(1.0f);
  Vec3 off;
  const int masks[6] = {1, 2, 4, 3, 6, 5};
  int m = masks[std::clamp(axis, 0, 5)];
  for (int a = 0; a < 3; ++a)
    if (m & (1 << a)) {
      s[a] = -1.0f;
      off[a] = offset;
    }
  Mat4 mirrorM = Mat4::translate(c + off) * Mat4::scale(s) * Mat4::translate(-c);
  QVector<uint32_t> targets;
  if (clone == 0) {
    for (Node* n : nodes) targets.push_back(n->id);
  } else {
    for (Node* src : nodes) {
      std::unique_ptr<Node> copy;
      if (clone == 1) {
        copy = src->clone();
      } else {
        copy = src->instance();
        if (clone == 3) {
          copy->modifiers.clear();
          for (const auto& mod : src->modifiers)
            if (mod) copy->modifiers.push_back(mod->clone());
        }
      }
      Node* added = doc->scene().addNode(std::move(copy));
      added->parentId = 0;
      added->setWorldMatrix(src->worldMatrix());
      targets.push_back(added->id);
    }
  }
  for (uint32_t id : targets)
    if (Node* n = doc->scene().findNode(id)) n->setWorldMatrix(mirrorM * n->worldMatrix());
  if (clone != 0) {
    doc->notifyStructureChanged();
    doc->setSelection(targets);
  }
  doc->notifyNodesChanged(targets, ChangeTransform);
  doc->endEdit();
}

// ---------------------------------------------------------------------------------------------
// Help pages

namespace {
void showHtml(QWidget* parent, const QString& title, const QString& html) {
  QDialog dlg(parent);
  noHelp(&dlg);
  dlg.setWindowTitle(title);
  dlg.resize(640, 560);
  auto* v = new QVBoxLayout(&dlg);
  auto* text = new QTextBrowser(&dlg);
  text->setOpenExternalLinks(true);
  text->setHtml(html);
  v->addWidget(text);
  auto* box = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
  QObject::connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  v->addWidget(box);
  dlg.exec();
}
}  // namespace

void showUserGuide(QWidget* parent) {
  showHtml(parent, QObject::tr("User Guide"),
           QObject::tr("<h2>Veraa user guide</h2>"
                       "<h3>Interface</h3><p>The menu bar is at the top with the Workspaces selector on its right. "
                       "Below it is the main toolbar: undo, linking, the selection filter, selection tools and "
                       "regions, transforms, the reference coordinate system, pivot center, snaps, named "
                       "selection sets, mirror and align, explorers, graph editors and rendering.</p>"
                       "<p>The Axis Constraints toolbar on the left restricts transforms to X, Y, Z or a plane. "
                       "The Scene and Layer Explorers are docked on the left, the command panel (Create, Modify, "
                       "Hierarchy, Motion, Display, Utilities) on the right.</p>"
                       "<h3>Viewports</h3><p>Click the [+], view, quality and shading labels in a viewport's top "
                       "left corner for its menus. Use the ViewCube (top right) to turn the view: click a face, "
                       "edge or corner, drag the cube to orbit, drag the compass ring to spin, click the house "
                       "for the home view. The navigation controls in the bottom right zoom, pan and orbit.</p>"
                       "<h3>Status bar</h3><p>Type Python into the mini listener (bottom left) and press Enter. "
                       "The X/Y/Z fields show and set the selection's position, rotation or scale; toggle "
                       "Absolute/Offset mode with the button to their left. Space locks the selection.</p>"
                       "<h3>Files</h3><p>File > Import reads OBJ, FBX, glTF/GLB, STL, PLY and 3DS. File > Export "
                       "writes OBJ (with MTL), FBX, glTF/GLB, STL and PLY.</p>"
                       "<p>Help > Keyboard Shortcuts lists every shortcut; Customize > Hotkey Editor changes them.</p>"));
}

void showWhatsNew(QWidget* parent) {
  showHtml(parent, QObject::tr("What's New"),
           QObject::tr("<h2>What's new in Veraa %1</h2><ul>"
                       "<li>Full main toolbar with selection filter, region shapes (rectangle, circle, fence, "
                       "lasso, paint), window/crossing, reference coordinate systems and pivot centers.</li>"
                       "<li>ViewCube, viewport label menus, 14 viewport layouts, visual styles (Default, Facets, "
                       "Clay, Bounding Box, Wireframe), scene lighting, backgrounds and safe frames.</li>"
                       "<li>Two-row status bar with a Python mini listener, transform type-in and navigation "
                       "controls.</li>"
                       "<li>Hierarchy, Display and Utilities command panels; Scene and Layer Explorers.</li>"
                       "<li>Hold/Fetch, Transform Type-In, Transform Toolbox, selection sets, scene states, "
                       "Light Lister, Rename Objects, Mesh Inspector.</li>"
                       "<li>Hotkey Editor, workspaces, units setup, project folders and preferences.</li>"
                       "<li>FBX, glTF, STL, PLY and 3DS import; FBX, glTF, STL, PLY and OBJ export.</li>"
                       "<li>Python scripting with a listener, editor and macro recorder.</li></ul>")
               .arg(VERAA_VERSION));
}

}  // namespace veraa
