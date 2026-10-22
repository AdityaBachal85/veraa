#include "commandtabs.h"

#include <algorithm>
#include <QButtonGroup>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <cmath>

#include "dialogs.h"
#include "editorstate.h"
#include "extension.h"
#include "glscene.h"
#include "interfacedata.h"
#include "mainwindow.h"
#include "paramform.h"
#include "scriptui.h"
#include "tooldialogs.h"
#include "uistate.h"
#include "units.h"
#include "viewport.h"
#include "edittools.h"

namespace veraa {

namespace {

QWidget* scrollPage(QScrollArea* area, QVBoxLayout** layout) {
  area->setWidgetResizable(true);
  area->setFrameShape(QFrame::NoFrame);
  auto* page = new QWidget(area);
  auto* v = new QVBoxLayout(page);
  v->setContentsMargins(2, 2, 2, 2);
  v->setSpacing(2);
  area->setWidget(page);
  *layout = v;
  return page;
}

QPushButton* addButton(QFormLayout* form, const QString& text, QObject* ctx, const std::function<void()>& fn) {
  auto* b = new QPushButton(text, form->parentWidget());
  QObject::connect(b, &QPushButton::clicked, ctx, fn);
  form->addRow(b);
  return b;
}

// Changes a node's pivot (node transform) without moving its geometry or its children.
void setPivotKeepGeometry(Scene& scene, Node* n, const Mat4& newWorld) {
  Mat4 geom = n->objectWorldMatrix();
  std::vector<std::pair<Node*, Mat4>> kids;
  for (Node* c : scene.children(n->id)) kids.push_back({c, c->worldMatrix()});
  n->setWorldMatrix(newWorld);
  n->objectOffset = decomposeTransform(n->worldMatrix().inverse() * geom);
  n->invalidate();
  for (auto& [c, w] : kids) c->setWorldMatrix(w);
}

Mat4 composeTRS(const Vec3& pos, const Vec3 axes[3], const Vec3& scale) {
  Mat4 m;
  for (int r = 0; r < 3; ++r) {
    m.m[r][0] = axes[0][r] * scale.x;
    m.m[r][1] = axes[1][r] * scale.y;
    m.m[r][2] = axes[2][r] * scale.z;
    m.m[r][3] = pos[r];
  }
  return m;
}

// Draws the pivot axes of selected objects while a pivot mode is active.
class PivotDecorator : public ViewportDecorator {
 public:
  explicit PivotDecorator(HierarchyPanel* p) : panel_(p) {}
  void drawLines(Viewport& vp, LineBatch& lines) override {
    if (!panel_ || panel_->pivotMode() == PivotEditMode::None) return;
    for (Node* n : vp.document()->selectedNodes()) {
      Mat4 w = n->worldMatrix();
      Vec3 o = w.translation();
      float s = vp.camera().worldPerPixel(o) * 60.0f;
      lines.line(o, o + normalize(w.column(0)) * s, QColor(230, 70, 70));
      lines.line(o, o + normalize(w.column(1)) * s, QColor(80, 210, 80));
      lines.line(o, o + normalize(w.column(2)) * s, QColor(80, 120, 240));
    }
  }

 private:
  QPointer<HierarchyPanel> panel_;
};

}  // namespace

// ---------------------------------------------------------------------------------------------
// Hierarchy

HierarchyPanel::HierarchyPanel(MainWindow* w) : w_(w), doc_(w->document()) {
  QVBoxLayout* v;
  QWidget* page = scrollPage(this, &v);
  auto* tabs = new QHBoxLayout;
  auto* pivotBtn = new QPushButton(tr("Pivot"), page);
  auto* linkBtn = new QPushButton(tr("Link Info"), page);
  for (QPushButton* b : {pivotBtn, linkBtn}) {
    b->setCheckable(true);
    b->setAutoExclusive(true);
    tabs->addWidget(b);
  }
  pivotBtn->setChecked(true);
  v->addLayout(tabs);
  pages_ = new QStackedWidget(page);
  v->addWidget(pages_);
  v->addStretch(1);
  connect(pivotBtn, &QPushButton::clicked, this, [this] { pages_->setCurrentIndex(0); });
  connect(linkBtn, &QPushButton::clicked, this, [this] {
    pages_->setCurrentIndex(1);
    refreshLinkInfo();
  });

  // ---- Pivot page ----
  auto* pivotPage = new QWidget(pages_);
  auto* pv = new QVBoxLayout(pivotPage);
  pv->setContentsMargins(0, 0, 0, 0);
  auto* adjust = new Rollout(tr("Adjust Pivot"), pivotPage);
  const char* modeNames[3] = {"Affect Pivot Only", "Affect Object Only", "Affect Hierarchy Only"};
  for (int i = 0; i < 3; ++i) {
    modeButtons_[i] = new QPushButton(tr(modeNames[i]), adjust->body());
    modeButtons_[i]->setCheckable(true);
    modeButtons_[i]->setObjectName(QString(modeNames[i]).remove(' '));
    adjust->form()->addRow(modeButtons_[i]);
    auto mode = static_cast<PivotEditMode>(i + 1);
    connect(modeButtons_[i], &QPushButton::toggled, this, [this, mode](bool on) {
      if (on) setPivotMode(mode);
      else if (mode_ == mode) setPivotMode(PivotEditMode::None);
    });
  }
  adjust->form()->addRow(new QLabel(tr("Alignment:"), adjust->body()));
  addButton(adjust->form(), tr("Center to Object"), this, [this] { centerPivotToObject(); });
  addButton(adjust->form(), tr("Align to Object"), this, [this] { alignPivotToObject(); });
  addButton(adjust->form(), tr("Align to World"), this, [this] { alignPivotToWorld(); });
  addButton(adjust->form(), tr("Reset Pivot"), this, [this] { resetPivot(); });
  pv->addWidget(adjust);

  auto* working = new Rollout(tr("Working Pivot"), pivotPage);
  auto* useWp = new QPushButton(tr("Use Working Pivot"), working->body());
  useWp->setCheckable(true);
  working->form()->addRow(useWp);
  connect(useWp, &QPushButton::toggled, this, [this](bool on) {
    UiState& ui = UiState::instance();
    if (on) {
      if (!hasWorkingPivot(doc_->scene())) {
        Transform t;
        if (Node* n = doc_->firstSelected()) t = decomposeTransform(n->worldMatrix());
        setWorkingPivot(doc_, &t);
      }
      ui.setRefCoordSys(RefCoordSys::Working);
      ui.setPivotCenter(PivotCenter::Coordinate);
    } else {
      ui.setRefCoordSys(RefCoordSys::View);
      ui.setPivotCenter(PivotCenter::PivotPoint);
    }
  });
  addButton(working->form(), tr("Place at Selection Pivot"), this, [this] {
    Node* n = doc_->firstSelected();
    if (!n) return;
    Transform t = decomposeTransform(n->worldMatrix());
    t.scale = Vec3(1.0f);
    setWorkingPivot(doc_, &t);
    w_->setPrompt(tr("Working pivot placed at %1").arg(QString::fromStdString(n->name)));
  });
  addButton(working->form(), tr("Place at Selection Center"), this, [this] {
    if (doc_->selection().isEmpty()) return;
    Transform t;
    t.position = doc_->selectionBounds().center();
    setWorkingPivot(doc_, &t);
  });
  addButton(working->form(), tr("Reset Working Pivot"), this, [this] { setWorkingPivot(doc_, nullptr); });
  pv->addWidget(working);

  auto* xform = new Rollout(tr("Adjust Transform"), pivotPage);
  xform->form()->addRow(new QLabel(tr("Reset:"), xform->body()));
  addButton(xform->form(), tr("Transform"), this, [this] { resetTransform(); });
  addButton(xform->form(), tr("Scale"), this, [this] { resetScale(); });
  pv->addWidget(xform);
  pages_->addWidget(pivotPage);

  // ---- Link Info page ----
  auto* linkPage = new QWidget(pages_);
  auto* lv = new QVBoxLayout(linkPage);
  lv->setContentsMargins(0, 0, 0, 0);
  auto* locks = new Rollout(tr("Locks"), linkPage);
  auto* lg = new QGridLayout;
  const char* groups[3] = {"Move", "Rotate", "Scale"};
  for (int g = 0; g < 3; ++g) {
    lg->addWidget(new QLabel(tr(groups[g]), locks->body()), g, 0);
    for (int a = 0; a < 3; ++a) {
      int bit = g * 3 + a;
      locks_[bit] = new QCheckBox(QString(QChar('X' + a)), locks->body());
      lg->addWidget(locks_[bit], g, a + 1);
      connect(locks_[bit], &QCheckBox::toggled, this, [this] {
        if (refreshing_) return;
        int mask = 0;
        for (int i = 0; i < 9; ++i)
          if (locks_[i]->isChecked()) mask |= 1 << i;
        for (uint32_t id : doc_->selection()) setTransformLocks(doc_, id, mask);
      });
    }
  }
  locks->form()->addRow(lg);
  lv->addWidget(locks);
  auto* info = new Rollout(tr("Link"), linkPage);
  linkInfo_ = new QLabel(info->body());
  linkInfo_->setWordWrap(true);
  info->form()->addRow(linkInfo_);
  addButton(info->form(), tr("Select Parent"), this, [this] {
    Node* n = doc_->firstSelected();
    if (n && n->parent()) doc_->setSelection({n->parent()->id});
  });
  addButton(info->form(), tr("Select Children"), this, [this] {
    QVector<uint32_t> ids;
    for (uint32_t id : doc_->selection())
      for (Node* c : doc_->scene().children(id)) ids.push_back(c->id);
    if (!ids.isEmpty()) doc_->setSelection(ids);
  });
  addButton(info->form(), tr("Unlink Selection"), this, [this] {
    if (QAction* a = w_->command("tools.unlink")) a->trigger();
  });
  lv->addWidget(info);
  pages_->addWidget(linkPage);

  connect(doc_, &SceneDocument::selectionChanged, this, [this] {
    cacheTransforms();
    refreshLinkInfo();
  });
  connect(doc_, &SceneDocument::structureChanged, this, [this] {
    cacheTransforms();
    refreshLinkInfo();
  });
  connect(doc_->undoStack(), &QUndoStack::indexChanged, this, [this] { cacheTransforms(); });
  connect(doc_, &SceneDocument::nodesChanged, this, [this](const QVector<uint32_t>& ids, int flags) {
    if (compensating_ || mode_ == PivotEditMode::None || !(flags & ChangeTransform)) return;
    if (doc_->inEdit()) compensate(ids);
    else cacheTransforms();
  });
  w->editorState()->addDecorator(std::make_shared<PivotDecorator>(this));
}

void HierarchyPanel::setPivotMode(PivotEditMode m) {
  mode_ = m;
  for (int i = 0; i < 3; ++i) {
    QSignalBlocker b(modeButtons_[i]);
    modeButtons_[i]->setChecked(static_cast<int>(m) == i + 1);
  }
  cacheTransforms();
  w_->setPrompt(m == PivotEditMode::AffectPivotOnly    ? tr("Transforms move only the pivot")
                : m == PivotEditMode::AffectObjectOnly ? tr("Transforms move only the object, not its pivot")
                : m == PivotEditMode::AffectHierarchyOnly
                    ? tr("Transforms move only the links to the children")
                    : QString());
  w_->editorState()->requestRedraw();
}

void HierarchyPanel::cacheTransforms() {
  if (compensating_) return;
  nodeWorld_.clear();
  geomWorld_.clear();
  for (const auto& n : doc_->scene().nodes) {
    nodeWorld_.insert(n->id, n->worldMatrix());
    geomWorld_.insert(n->id, n->objectWorldMatrix());
  }
}

void HierarchyPanel::compensate(const QVector<uint32_t>& ids) {
  Scene& scene = doc_->scene();
  compensating_ = true;
  QVector<uint32_t> changed;
  for (uint32_t id : ids) {
    Node* n = scene.findNode(id);
    if (!n || !nodeWorld_.contains(id)) continue;
    Mat4 oldW = nodeWorld_.value(id), oldG = geomWorld_.value(id);
    Mat4 newW = n->worldMatrix();
    std::vector<Node*> kids = scene.children(id);
    if (mode_ == PivotEditMode::AffectPivotOnly) {
      // The pivot moved; keep the geometry and the children where they were.
      n->objectOffset = decomposeTransform(newW.inverse() * oldG);
      for (Node* c : kids)
        if (nodeWorld_.contains(c->id)) c->setWorldMatrix(nodeWorld_.value(c->id));
    } else if (mode_ == PivotEditMode::AffectObjectOnly) {
      // The geometry moves; the pivot (and so the children) stays.
      Mat4 delta = newW * oldW.inverse();
      n->setWorldMatrix(oldW);
      n->objectOffset = decomposeTransform(oldW.inverse() * delta * oldG);
    } else if (mode_ == PivotEditMode::AffectHierarchyOnly) {
      // Only the links move: the node stays, its children are carried by the change.
      Mat4 delta = newW * oldW.inverse();
      n->setWorldMatrix(oldW);
      for (Node* c : kids)
        if (nodeWorld_.contains(c->id)) {
          Mat4 cw = nodeWorld_.value(c->id);
          c->setWorldMatrix(cw);
          c->setWorldPosition(delta.transformPoint(cw.translation()));
          changed.push_back(c->id);
        }
    }
    n->invalidate();
    changed.push_back(id);
    for (Node* c : kids) changed.push_back(c->id);
  }
  doc_->notifyNodesChanged(changed, ChangeTransform | ChangeGeometry);
  compensating_ = false;
}

void HierarchyPanel::centerPivotToObject() {
  auto nodes = doc_->selectedNodes();
  if (nodes.isEmpty()) return;
  doc_->beginEdit(tr("Center Pivot to Object"));
  QVector<uint32_t> ids;
  for (Node* n : nodes) {
    Aabb b = n->worldBounds();
    if (!b.valid()) continue;
    Mat4 w = n->worldMatrix();
    w.m[0][3] = b.center().x;
    w.m[1][3] = b.center().y;
    w.m[2][3] = b.center().z;
    setPivotKeepGeometry(doc_->scene(), n, w);
    ids.push_back(n->id);
  }
  doc_->notifyNodesChanged(ids, ChangeTransform | ChangeGeometry);
  doc_->endEdit();
}

void HierarchyPanel::alignPivotToObject() {
  auto nodes = doc_->selectedNodes();
  if (nodes.isEmpty()) return;
  doc_->beginEdit(tr("Align Pivot to Object"));
  QVector<uint32_t> ids;
  for (Node* n : nodes) {
    Mat4 g = n->objectWorldMatrix();
    Mat4 w = n->worldMatrix();
    Vec3 axes[3] = {normalize(g.column(0)), normalize(g.column(1)), normalize(g.column(2))};
    Vec3 s(length(w.column(0)), length(w.column(1)), length(w.column(2)));
    setPivotKeepGeometry(doc_->scene(), n, composeTRS(w.translation(), axes, s));
    ids.push_back(n->id);
  }
  doc_->notifyNodesChanged(ids, ChangeTransform | ChangeGeometry);
  doc_->endEdit();
}

void HierarchyPanel::alignPivotToWorld() {
  auto nodes = doc_->selectedNodes();
  if (nodes.isEmpty()) return;
  doc_->beginEdit(tr("Align Pivot to World"));
  QVector<uint32_t> ids;
  for (Node* n : nodes) {
    Mat4 w = n->worldMatrix();
    Vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    Vec3 s(length(w.column(0)), length(w.column(1)), length(w.column(2)));
    setPivotKeepGeometry(doc_->scene(), n, composeTRS(w.translation(), axes, s));
    ids.push_back(n->id);
  }
  doc_->notifyNodesChanged(ids, ChangeTransform | ChangeGeometry);
  doc_->endEdit();
}

void HierarchyPanel::resetPivot() {
  auto nodes = doc_->selectedNodes();
  if (nodes.isEmpty()) return;
  doc_->beginEdit(tr("Reset Pivot"));
  QVector<uint32_t> ids;
  for (Node* n : nodes) {
    // The pivot goes back to where the object's own origin is.
    setPivotKeepGeometry(doc_->scene(), n, n->objectWorldMatrix());
    n->objectOffset = Transform();
    ids.push_back(n->id);
  }
  doc_->notifyNodesChanged(ids, ChangeTransform | ChangeGeometry);
  doc_->endEdit();
}

void HierarchyPanel::resetTransform() {
  auto nodes = doc_->selectedNodes();
  if (nodes.isEmpty()) return;
  doc_->beginEdit(tr("Reset Transform"));
  QVector<uint32_t> ids;
  for (Node* n : nodes) {
    Vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    setPivotKeepGeometry(doc_->scene(), n, composeTRS(n->worldPosition(), axes, Vec3(1.0f)));
    ids.push_back(n->id);
  }
  doc_->notifyNodesChanged(ids, ChangeTransform | ChangeGeometry);
  doc_->endEdit();
}

void HierarchyPanel::resetScale() {
  auto nodes = doc_->selectedNodes();
  if (nodes.isEmpty()) return;
  doc_->beginEdit(tr("Reset Scale"));
  QVector<uint32_t> ids;
  for (Node* n : nodes) {
    Mat4 w = n->worldMatrix();
    Vec3 axes[3] = {normalize(w.column(0)), normalize(w.column(1)), normalize(w.column(2))};
    setPivotKeepGeometry(doc_->scene(), n, composeTRS(w.translation(), axes, Vec3(1.0f)));
    ids.push_back(n->id);
  }
  doc_->notifyNodesChanged(ids, ChangeTransform | ChangeGeometry);
  doc_->endEdit();
}

void HierarchyPanel::refreshLinkInfo() {
  if (!linkInfo_) return;
  refreshing_ = true;
  Node* n = doc_->firstSelected();
  int mask = n ? transformLocks(doc_->scene(), n->id) : 0;
  for (int i = 0; i < 9; ++i) {
    locks_[i]->setEnabled(n != nullptr);
    locks_[i]->setChecked((mask >> i) & 1);
  }
  if (!n) {
    linkInfo_->setText(tr("Select an object"));
  } else {
    Node* p = n->parent();
    linkInfo_->setText(tr("Object: %1\nParent: %2\nChildren: %3")
                           .arg(QString::fromStdString(n->name))
                           .arg(p ? QString::fromStdString(p->name) : tr("World"))
                           .arg(doc_->scene().children(n->id).size()));
  }
  refreshing_ = false;
}

// ---------------------------------------------------------------------------------------------
// Display

DisplayPanel::DisplayPanel(MainWindow* w) : w_(w), doc_(w->document()) {
  QVBoxLayout* v;
  QWidget* page = scrollPage(this, &v);
  UiState& ui = UiState::instance();

  auto* color = new Rollout(tr("Display Color"), page);
  auto* wireGroup = new QButtonGroup(color);
  auto* shadedGroup = new QButtonGroup(color);
  wireObject_ = new QRadioButton(tr("Object Color"), color->body());
  wireMaterial_ = new QRadioButton(tr("Material Color"), color->body());
  shadedObject_ = new QRadioButton(tr("Object Color"), color->body());
  shadedMaterial_ = new QRadioButton(tr("Material Color"), color->body());
  wireGroup->addButton(wireObject_);
  wireGroup->addButton(wireMaterial_);
  shadedGroup->addButton(shadedObject_);
  shadedGroup->addButton(shadedMaterial_);
  color->form()->addRow(tr("Wireframe:"), wireObject_);
  color->form()->addRow(QString(), wireMaterial_);
  color->form()->addRow(tr("Shaded:"), shadedObject_);
  color->form()->addRow(QString(), shadedMaterial_);
  auto applyColors = [this] {
    if (!refreshing_) UiState::instance().setDisplayColors(wireObject_->isChecked(), shadedObject_->isChecked());
  };
  for (QRadioButton* r : {wireObject_, wireMaterial_, shadedObject_, shadedMaterial_})
    connect(r, &QRadioButton::toggled, this, applyColors);
  v->addWidget(color);

  auto* cat = new Rollout(tr("Hide by Category"), page);
  const char* names[6] = {"Geometry", "Shapes", "Lights", "Cameras", "Helpers", "Space Warps"};
  for (int i = 0; i < 6; ++i) {
    categories_[i] = new QCheckBox(tr(names[i]), cat->body());
    categories_[i]->setObjectName(QString("HideCategory%1").arg(i));
    cat->form()->addRow(categories_[i]);
    connect(categories_[i], &QCheckBox::toggled, this, [this, i](bool on) {
      if (refreshing_) return;
      UiState& u = UiState::instance();
      int mask = u.hiddenCategories();
      mask = on ? (mask | (1 << i)) : (mask & ~(1 << i));
      u.setHiddenCategories(mask);
    });
  }
  auto* catBtns = new QHBoxLayout;
  for (const char* label : {"All", "None", "Invert"}) {
    auto* b = new QPushButton(tr(label), cat->body());
    QString l = label;
    connect(b, &QPushButton::clicked, this, [l] {
      UiState& u = UiState::instance();
      u.setHiddenCategories(l == "All" ? 63 : l == "None" ? 0 : (~u.hiddenCategories()) & 63);
    });
    catBtns->addWidget(b);
  }
  cat->form()->addRow(catBtns);
  v->addWidget(cat);

  auto setFlag = [this](const QString& label, const std::function<bool(Node&)>& fn) {
    doc_->beginEdit(label);
    QVector<uint32_t> ids;
    for (const auto& n : doc_->scene().nodes)
      if (fn(*n)) ids.push_back(n->id);
    doc_->notifyNodesChanged(ids, ChangeDisplay);
    doc_->endEdit();
  };
  auto byName = [this](const QString& title, const std::function<bool(const Node&)>& filter,
                       const std::function<void(const QVector<uint32_t>&)>& apply) {
    QDialog dlg(this);
    dlg.setWindowTitle(title);
    auto* dv = new QVBoxLayout(&dlg);
    auto* list = new QListWidget(&dlg);
    list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    for (const auto& n : doc_->scene().nodes)
      if (filter(*n)) {
        auto* it = new QListWidgetItem(QString::fromStdString(n->name), list);
        it->setData(Qt::UserRole, n->id);
      }
    dv->addWidget(list);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    dv->addWidget(box);
    connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return;
    QVector<uint32_t> ids;
    for (QListWidgetItem* it : list->selectedItems()) ids.push_back(it->data(Qt::UserRole).toUInt());
    if (!ids.isEmpty()) apply(ids);
  };
  auto byHit = [this](const QString& prompt, const std::function<void(uint32_t)>& apply) {
    w_->editorState()->setViewportTool(makePickObjectTool(doc_, prompt, apply));
    w_->setPrompt(prompt);
  };

  auto* hide = new Rollout(tr("Hide"), page);
  addButton(hide->form(), tr("Hide Selected"), this, [this] { doc_->hideSelected(); });
  addButton(hide->form(), tr("Hide Unselected"), this, [this, setFlag] {
    setFlag(tr("Hide Unselected"), [this](Node& n) {
      if (doc_->isSelected(n.id) || n.hidden) return false;
      n.hidden = true;
      return true;
    });
  });
  addButton(hide->form(), tr("Hide by Name..."), this, [this, byName, setFlag] {
    byName(tr("Hide Objects"), [](const Node& n) { return !n.hidden; }, [this, setFlag](const QVector<uint32_t>& ids) {
      setFlag(tr("Hide by Name"), [ids](Node& n) {
        if (!ids.contains(n.id)) return false;
        n.hidden = true;
        return true;
      });
      Q_UNUSED(this);
    });
  });
  addButton(hide->form(), tr("Hide by Hit"), this, [this, byHit, setFlag] {
    byHit(tr("Click objects to hide; right-click to stop"), [setFlag](uint32_t id) {
      setFlag(tr("Hide by Hit"), [id](Node& n) {
        if (n.id != id) return false;
        n.hidden = true;
        return true;
      });
    });
  });
  addButton(hide->form(), tr("Unhide All"), this, [this] { doc_->unhideAll(); });
  addButton(hide->form(), tr("Unhide by Name..."), this, [byName, setFlag] {
    byName(tr("Unhide Objects"), [](const Node& n) { return n.hidden; }, [setFlag](const QVector<uint32_t>& ids) {
      setFlag(tr("Unhide by Name"), [ids](Node& n) {
        if (!ids.contains(n.id)) return false;
        n.hidden = false;
        return true;
      });
    });
  });
  hideFrozen_ = new QCheckBox(tr("Hide Frozen Objects"), hide->body());
  hide->form()->addRow(hideFrozen_);
  connect(hideFrozen_, &QCheckBox::toggled, this, [this](bool on) {
    if (!refreshing_) UiState::instance().setHideFrozen(on);
  });
  v->addWidget(hide);

  auto* freeze = new Rollout(tr("Freeze"), page);
  addButton(freeze->form(), tr("Freeze Selected"), this, [this, setFlag] {
    setFlag(tr("Freeze Selected"), [this](Node& n) {
      if (!doc_->isSelected(n.id)) return false;
      n.frozen = true;
      return true;
    });
    doc_->clearSelection();
  });
  addButton(freeze->form(), tr("Freeze Unselected"), this, [this, setFlag] {
    setFlag(tr("Freeze Unselected"), [this](Node& n) {
      if (doc_->isSelected(n.id) || n.frozen) return false;
      n.frozen = true;
      return true;
    });
  });
  addButton(freeze->form(), tr("Freeze by Name..."), this, [byName, setFlag] {
    byName(tr("Freeze Objects"), [](const Node& n) { return !n.frozen; }, [setFlag](const QVector<uint32_t>& ids) {
      setFlag(tr("Freeze by Name"), [ids](Node& n) {
        if (!ids.contains(n.id)) return false;
        n.frozen = true;
        return true;
      });
    });
  });
  addButton(freeze->form(), tr("Freeze by Hit"), this, [byHit, setFlag] {
    byHit(tr("Click objects to freeze; right-click to stop"), [setFlag](uint32_t id) {
      setFlag(tr("Freeze by Hit"), [id](Node& n) {
        if (n.id != id) return false;
        n.frozen = true;
        return true;
      });
    });
  });
  addButton(freeze->form(), tr("Unfreeze All"), this, [setFlag] {
    setFlag(tr("Unfreeze All"), [](Node& n) {
      if (!n.frozen) return false;
      n.frozen = false;
      return true;
    });
  });
  addButton(freeze->form(), tr("Unfreeze by Name..."), this, [byName, setFlag] {
    byName(tr("Unfreeze Objects"), [](const Node& n) { return n.frozen; }, [setFlag](const QVector<uint32_t>& ids) {
      setFlag(tr("Unfreeze by Name"), [ids](Node& n) {
        if (!ids.contains(n.id)) return false;
        n.frozen = false;
        return true;
      });
    });
  });
  v->addWidget(freeze);

  auto* props = new Rollout(tr("Display Properties"), page);
  const struct {
    const char* name;
    bool Node::*flag;
  } flags[3] = {{"Display as Box", &Node::displayAsBox}, {"Backface Cull", &Node::backfaceCull}, {"See-Through", &Node::seeThrough}};
  for (int i = 0; i < 3; ++i) {
    props_[i] = new QCheckBox(tr(flags[i].name), props->body());
    props->form()->addRow(props_[i]);
    bool Node::*flag = flags[i].flag;
    QString name = tr(flags[i].name);
    connect(props_[i], &QCheckBox::toggled, this, [this, flag, name, setFlag](bool on) {
      if (refreshing_) return;
      setFlag(name, [this, flag, on](Node& n) {
        if (!doc_->isSelected(n.id)) return false;
        n.*flag = on;
        return true;
      });
    });
  }
  auto* frozenGray = new QCheckBox(tr("Show Frozen in Gray"), props->body());
  frozenGray->setChecked(ui.showFrozenInGray());
  connect(frozenGray, &QCheckBox::toggled, this, [](bool on) { UiState::instance().setShowFrozenInGray(on); });
  props->form()->addRow(frozenGray);
  v->addWidget(props);
  v->addStretch(1);

  connect(doc_, &SceneDocument::selectionChanged, this, &DisplayPanel::refresh);
  connect(doc_, &SceneDocument::nodesChanged, this, [this](const QVector<uint32_t>&, int f) {
    if (f & ChangeDisplay) refresh();
  });
  connect(&UiState::instance(), &UiState::changed, this, &DisplayPanel::refresh);
  refresh();
}

void DisplayPanel::refresh() {
  refreshing_ = true;
  const UiState& ui = UiState::instance();
  wireObject_->setChecked(ui.wireUsesObjectColor());
  wireMaterial_->setChecked(!ui.wireUsesObjectColor());
  shadedObject_->setChecked(ui.shadedUsesObjectColor());
  shadedMaterial_->setChecked(!ui.shadedUsesObjectColor());
  for (int i = 0; i < 6; ++i) categories_[i]->setChecked(ui.categoryHidden(i));
  hideFrozen_->setChecked(ui.hideFrozen());
  auto nodes = doc_->selectedNodes();
  bool Node::*flags[3] = {&Node::displayAsBox, &Node::backfaceCull, &Node::seeThrough};
  for (int i = 0; i < 3; ++i) {
    props_[i]->setEnabled(!nodes.isEmpty());
    props_[i]->setChecked(!nodes.isEmpty() && nodes[0]->*flags[i]);
  }
  refreshing_ = false;
}

// ---------------------------------------------------------------------------------------------
// Utilities

namespace {
const char* const kUtilities[] = {"Collapse",          "Color Clipboard", "Measure",
                                  "Reset XForm",       "Rescale World Units", "Polygon Counter",
                                  "Bitmap/Photometric Path Editor", "Python Script", "Object Info"};
}

UtilitiesPanel::UtilitiesPanel(MainWindow* w) : w_(w), doc_(w->document()) {
  QVBoxLayout* v;
  QWidget* page = scrollPage(this, &v);
  auto* util = new Rollout(tr("Utilities"), page);
  auto* top = new QHBoxLayout;
  auto* more = new QPushButton(tr("More..."), util->body());
  auto* sets = new QPushButton(tr("Sets"), util->body());
  top->addWidget(more);
  top->addWidget(sets);
  util->form()->addRow(top);
  buttons_ = new QWidget(util->body());
  util->form()->addRow(buttons_);
  v->addWidget(util);
  utilityHost_ = new QVBoxLayout;
  v->addLayout(utilityHost_);
  v->addStretch(1);
  connect(more, &QPushButton::clicked, this, [this] {
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Utilities"));
    auto* dv = new QVBoxLayout(&dlg);
    auto* list = new QListWidget(&dlg);
    list->addItems(utilityNames());
    dv->addWidget(list);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    dv->addWidget(box);
    connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    connect(list, &QListWidget::itemDoubleClicked, &dlg, &QDialog::accept);
    if (dlg.exec() == QDialog::Accepted && list->currentItem()) openUtility(list->currentItem()->text());
  });
  connect(sets, &QPushButton::clicked, this, [this] {
    // Configure Button Sets: choose which utilities have buttons.
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Configure Button Sets"));
    auto* dv = new QVBoxLayout(&dlg);
    QSettings s("Veraa", "Veraa");
    QStringList shown = s.value("utilities/buttons", utilityNames()).toStringList();
    QList<QCheckBox*> boxes;
    for (const QString& n : utilityNames()) {
      auto* c = new QCheckBox(n, &dlg);
      c->setChecked(shown.contains(n));
      dv->addWidget(c);
      boxes << c;
    }
    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    dv->addWidget(box);
    connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return;
    QStringList chosen;
    for (QCheckBox* c : boxes)
      if (c->isChecked()) chosen << c->text();
    s.setValue("utilities/buttons", chosen);
    rebuildButtons();
  });
  rebuildButtons();
}

QStringList UtilitiesPanel::utilityNames() const {
  QStringList out;
  for (const char* n : kUtilities) out << tr(n);
  return out;
}

void UtilitiesPanel::rebuildButtons() {
  delete buttons_->layout();
  for (QWidget* c : buttons_->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) delete c;
  auto* g = new QGridLayout(buttons_);
  g->setContentsMargins(0, 0, 0, 0);
  g->setSpacing(2);
  QStringList shown = QSettings("Veraa", "Veraa").value("utilities/buttons", utilityNames()).toStringList();
  int i = 0;
  for (const QString& n : utilityNames()) {
    if (!shown.contains(n)) continue;
    auto* b = new QPushButton(n == tr("Bitmap/Photometric Path Editor") ? tr("Bitmap Paths") : n, buttons_);
    b->setToolTip(n);
    connect(b, &QPushButton::clicked, this, [this, n] { openUtility(n); });
    g->addWidget(b, i / 2, i % 2);
    ++i;
  }
}

void UtilitiesPanel::openUtility(const QString& name) {
  if (current_) {
    delete current_;
    current_ = nullptr;
  }
  if (name == tr("Color Clipboard")) {
    if (QAction* a = w_->command("tools.colorClipboard")) a->trigger();
    return;
  }
  current_ = buildUtility(name);
  currentName_ = name;
  if (current_) utilityHost_->addWidget(current_);
}

void UtilitiesPanel::measure(const Node& n, double* area, double* volume, Vec3* center) {
  const TriMesh& t = n.triMesh();
  Mat4 m = n.objectWorldMatrix();
  double a = 0, vol = 0;
  Vec3 c;
  double wsum = 0;
  for (size_t i = 0; i + 2 < t.indices.size(); i += 3) {
    Vec3 p0 = m.transformPoint(t.positions[t.indices[i]]);
    Vec3 p1 = m.transformPoint(t.positions[t.indices[i + 1]]);
    Vec3 p2 = m.transformPoint(t.positions[t.indices[i + 2]]);
    double ta = 0.5 * length(cross(p1 - p0, p2 - p0));
    a += ta;
    double tv = dot(p0, cross(p1, p2)) / 6.0;  // signed tetrahedron with the origin
    vol += tv;
    c = c + (p0 + p1 + p2) * static_cast<float>(tv / 4.0);
    wsum += tv;
  }
  if (area) *area = a;
  if (volume) *volume = std::fabs(vol);
  if (center) *center = std::fabs(wsum) > 1e-12 ? c / static_cast<float>(wsum) : n.worldBounds().center();
}

void UtilitiesPanel::collapse(SceneDocument* doc, const QVector<uint32_t>& ids) {
  doc->beginEdit(tr("Collapse"));
  QVector<uint32_t> done;
  for (uint32_t id : ids) {
    Node* n = doc->scene().findNode(id);
    if (!n || n->kind() != ObjectKind::Geometry) continue;
    auto poly = std::make_shared<EditablePolyObject>();
    poly->mesh = n->mesh();
    n->object = poly;
    n->modifiers.clear();
    n->invalidate();
    done.push_back(id);
  }
  doc->notifyNodesChanged(done, ChangeGeometry);
  doc->notifyStructureChanged();
  doc->endEdit();
}

void UtilitiesPanel::resetXForm(SceneDocument* doc, const QVector<uint32_t>& ids) {
  doc->beginEdit(tr("Reset XForm"));
  QVector<uint32_t> done;
  for (uint32_t id : ids) {
    Node* n = doc->scene().findNode(id);
    if (!n || n->kind() != ObjectKind::Geometry) continue;
    Mat4 world = n->worldMatrix();
    Mat4 geom = n->objectWorldMatrix();
    std::vector<std::pair<Node*, Mat4>> kids;
    for (Node* c : doc->scene().children(id)) kids.push_back({c, c->worldMatrix()});
    // New node transform: position only; the old rotation, scale and pivot offset go into the mesh.
    Mat4 t = Mat4::translate(world.translation());
    PolyMesh mesh = n->mesh();
    mesh.transform(t.inverse() * geom);
    auto poly = std::make_shared<EditablePolyObject>();
    poly->mesh = mesh;
    n->object = poly;
    n->modifiers.clear();
    n->objectOffset = Transform();
    n->setWorldMatrix(t);
    n->invalidate();
    for (auto& [c, w] : kids) c->setWorldMatrix(w);
    done.push_back(id);
  }
  doc->notifyNodesChanged(done, ChangeGeometry | ChangeTransform);
  doc->notifyStructureChanged();
  doc->endEdit();
}

void UtilitiesPanel::rescale(SceneDocument* doc, double factor, bool selectionOnly) {
  if (factor <= 0) return;
  doc->beginEdit(tr("Rescale World Units"));
  QVector<uint32_t> ids;
  float f = static_cast<float>(factor);
  for (const auto& n : doc->scene().nodes) {
    if (selectionOnly && !doc->isSelected(n->id)) continue;
    // Roots carry the scale to their children (children are relative to them).
    Node* p = n->parent();
    if (p && (!selectionOnly || doc->isSelected(p->id))) continue;
    Vec3 pos = n->worldPosition();
    n->transform.scale = n->transform.scale * f;
    n->setWorldPosition(pos * f);
    ids.push_back(n->id);
  }
  doc->notifyNodesChanged(ids, ChangeTransform);
  doc->endEdit();
}

QWidget* UtilitiesPanel::buildUtility(const QString& name) {
  auto* r = new Rollout(name, widget());
  QFormLayout* f = r->form();
  if (name == tr("Collapse")) {
    f->addRow(new QLabel(tr("Collapses the modifier stacks of the selected objects into Editable Poly meshes."), r->body()));
    addButton(f, tr("Collapse Selected"), this, [this] { collapse(doc_, doc_->selection()); });
  } else if (name == tr("Measure")) {
    auto* info = new QLabel(r->body());
    info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    f->addRow(info);
    auto update = [this, info] {
      Node* n = doc_->firstSelected();
      if (!n || n->kind() != ObjectKind::Geometry) {
        info->setText(tr("Select a geometry object"));
        return;
      }
      double area = 0, volume = 0;
      Vec3 c;
      measure(*n, &area, &volume, &c);
      const Units& u = Units::instance();
      Vec3 e = n->worldBounds().extent();
      double cm2 = u.toDisplay(1.0) * u.toDisplay(1.0);
      info->setText(tr("%1\nSurface Area: %2 %3²\nVolume: %4 %3³\nCenter of Mass: %5, %6, %7\nDimensions: %8 x %9 x %10")
                        .arg(QString::fromStdString(n->name))
                        .arg(area * cm2, 0, 'f', 3)
                        .arg(u.suffix().isEmpty() ? tr("units") : u.suffix())
                        .arg(volume * cm2 * u.toDisplay(1.0), 0, 'f', 3)
                        .arg(u.format(c.x), u.format(c.y), u.format(c.z))
                        .arg(u.format(e.x), u.format(e.y), u.format(e.z)));
    };
    connect(doc_, &SceneDocument::selectionChanged, info, update);
    connect(doc_, &SceneDocument::nodesChanged, info, update);
    update();
  } else if (name == tr("Reset XForm")) {
    f->addRow(new QLabel(tr("Bakes rotation, scale and pivot offset into the mesh, leaving a world-aligned pivot."), r->body()));
    addButton(f, tr("Reset Selected"), this, [this] { resetXForm(doc_, doc_->selection()); });
  } else if (name == tr("Rescale World Units")) {
    auto* factor = new QDoubleSpinBox(r->body());
    factor->setRange(0.0001, 100000);
    factor->setDecimals(4);
    factor->setValue(1.0);
    f->addRow(tr("Scale factor:"), factor);
    auto* selOnly = new QCheckBox(tr("Affect selection only"), r->body());
    f->addRow(selOnly);
    addButton(f, tr("Rescale"), this, [this, factor, selOnly] { rescale(doc_, factor->value(), selOnly->isChecked()); });
  } else if (name == tr("Polygon Counter")) {
    auto* budget = new QSpinBox(r->body());
    budget->setRange(1, 100000000);
    budget->setValue(100000);
    f->addRow(tr("Budget:"), budget);
    auto* sel = new QLabel(r->body());
    auto* selBar = new QProgressBar(r->body());
    auto* all = new QLabel(r->body());
    auto* allBar = new QProgressBar(r->body());
    f->addRow(tr("Selected:"), sel);
    f->addRow(selBar);
    f->addRow(tr("All objects:"), all);
    f->addRow(allBar);
    auto update = [this, budget, sel, selBar, all, allBar] {
      size_t sp = 0, st = 0, ap = 0, at = 0;
      for (const auto& n : doc_->scene().nodes) {
        if (n->kind() != ObjectKind::Geometry) continue;
        size_t p = n->mesh().faces.size(), t = n->triMesh().triangleCount();
        ap += p;
        at += t;
        if (doc_->isSelected(n->id)) {
          sp += p;
          st += t;
        }
      }
      sel->setText(tr("%1 polygons, %2 triangles").arg(sp).arg(st));
      all->setText(tr("%1 polygons, %2 triangles").arg(ap).arg(at));
      for (QProgressBar* b : {selBar, allBar}) b->setRange(0, budget->value());
      selBar->setValue(static_cast<int>(std::min<size_t>(sp, budget->value())));
      allBar->setValue(static_cast<int>(std::min<size_t>(ap, budget->value())));
    };
    connect(doc_, &SceneDocument::selectionChanged, sel, update);
    connect(doc_, &SceneDocument::structureChanged, sel, update);
    connect(doc_, &SceneDocument::nodesChanged, sel, update);
    connect(budget, QOverload<int>::of(&QSpinBox::valueChanged), sel, update);
    update();
  } else if (name == tr("Bitmap/Photometric Path Editor")) {
    auto* table = new QTableWidget(0, 3, r->body());
    table->setHorizontalHeaderLabels({tr("Owner"), tr("Path"), tr("Status")});
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table->verticalHeader()->hide();
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setMinimumHeight(180);
    f->addRow(table);
    struct Ref {
      ParamBlock* block;
      std::string key;
      QString owner;
    };
    auto refs = std::make_shared<std::vector<Ref>>();
    auto rebuild = [this, table, refs] {
      refs->clear();
      Scene& s = doc_->scene();
      for (const auto& m : s.materials)
        for (const ParamDef& d : m->params.defs())
          if (d.type == ParamType::File && !m->params.getString(d.key).empty())
            refs->push_back({&m->params, d.key, QString::fromStdString(m->name)});
      for (const ParamDef& d : s.renderSettings.defs())
        if (d.type == ParamType::File && !s.renderSettings.getString(d.key).empty())
          refs->push_back({&s.renderSettings, d.key, tr("Environment")});
      table->setRowCount(0);
      for (const Ref& ref : *refs) {
        int row = table->rowCount();
        table->insertRow(row);
        QString path = QString::fromStdString(ref.block->getString(ref.key));
        bool found = QFileInfo::exists(QString::fromStdString(s.resolvePath(path.toStdString())));
        table->setItem(row, 0, new QTableWidgetItem(ref.owner + " / " + QString::fromStdString(ref.key)));
        table->setItem(row, 1, new QTableWidgetItem(path));
        auto* st = new QTableWidgetItem(found ? tr("Found") : tr("Missing"));
        st->setForeground(found ? QColor(120, 220, 120) : QColor(240, 110, 90));
        table->setItem(row, 2, st);
      }
    };
    auto rows = [table] {
      QSet<int> out;
      for (QTableWidgetItem* it : table->selectedItems()) out.insert(it->row());
      if (out.isEmpty())
        for (int i = 0; i < table->rowCount(); ++i) out.insert(i);
      return out;
    };
    auto change = [this, refs, rows, rebuild](const QString& label, const std::function<QString(const QString&)>& fn) {
      QSet<int> sel = rows();
      doc_->beginEdit(label);
      for (int i : sel) {
        if (i < 0 || i >= static_cast<int>(refs->size())) continue;
        Ref& ref = (*refs)[i];
        ref.block->setString(ref.key, fn(QString::fromStdString(ref.block->getString(ref.key))).toStdString());
      }
      doc_->notifyMaterialsChanged();
      doc_->notifyRenderSettingsChanged();
      doc_->endEdit();
      rebuild();
    };
    addButton(f, tr("Strip Path"), this, [change] {
      change(tr("Strip Paths"), [](const QString& p) { return QFileInfo(p).fileName(); });
    });
    addButton(f, tr("Set Path..."), this, [this, change] {
      QString dir = QFileDialog::getExistingDirectory(this, tr("Choose the folder holding the files"));
      if (dir.isEmpty()) return;
      change(tr("Set Paths"), [dir](const QString& p) { return QDir(dir).filePath(QFileInfo(p).fileName()); });
    });
    addButton(f, tr("Make Relative to Scene"), this, [this, change] {
      if (doc_->filePath().isEmpty()) {
        QMessageBox::information(this, tr("Bitmap Paths"), tr("Save the scene first."));
        return;
      }
      QDir base = QFileInfo(doc_->filePath()).absoluteDir();
      change(tr("Make Paths Relative"), [base](const QString& p) {
        return QFileInfo(p).isAbsolute() ? base.relativeFilePath(p) : p;
      });
    });
    addButton(f, tr("Refresh"), this, rebuild);
    rebuild();
  } else if (name == tr("Python Script")) {
    addButton(f, tr("Open Listener"), this, [this] {
      if (QAction* a = w_->command("scripting.listener")) a->trigger();
    });
    addButton(f, tr("New Script"), this, [this] {
      if (QAction* a = w_->command("scripting.new")) a->trigger();
    });
    addButton(f, tr("Run Script..."), this, [this] {
      if (QAction* a = w_->command("scripting.run")) a->trigger();
    });
  } else if (name == tr("Object Info")) {
    addButton(f, tr("Object Properties..."), this, [this] {
      if (QAction* a = w_->command("edit.objectProperties")) a->trigger();
    });
    addButton(f, tr("Mesh Inspector..."), this, [this] {
      if (QAction* a = w_->command("tools.meshInspector")) a->trigger();
    });
  }
  addButton(f, tr("Close"), this, [this] {
    QMetaObject::invokeMethod(this, [this] {
      delete current_;
      current_ = nullptr;
    }, Qt::QueuedConnection);
  });
  return r;
}

}  // namespace veraa
