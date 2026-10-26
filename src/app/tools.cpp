// Object tools: Align, Quick Align, Normal Align, Align to View, Mirror, Array, Snapshot,
// Spacing Tool, the Group menu and Select and Link / Unlink.
#include <QAction>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QToolBar>
#include <QUndoStack>
#include <QVBoxLayout>

#include <cmath>
#include <functional>

#include "dialogs.h"
#include "document.h"
#include "editorstate.h"
#include "mainwindow.h"
#include "object.h"
#include "paramform.h"
#include "registry.h"
#include "selftest.h"
#include "viewport.h"
#include "viewportpanel.h"

namespace veraa {

namespace {

// World bounds of a node and everything linked below it.
Aabb hierarchyBounds(const Scene& scene, const Node& n) {
  Aabb b;
  if (n.kind() == ObjectKind::Geometry || n.kind() == ObjectKind::Shape) b = n.worldBounds();
  for (Node* c : scene.children(n.id)) {
    Aabb cb = hierarchyBounds(scene, *c);
    if (cb.valid()) {
      b.expand(cb.lo);
      b.expand(cb.hi);
    }
  }
  return b;
}

MainWindow*& window() {
  static MainWindow* w = nullptr;
  return w;
}

void prompt(const QString& text) {
  if (window()) window()->editorState()->setPrompt(text);
}

// Viewport tool that waits for a click on an object and calls `picked` with its id.
class PickObjectTool : public ViewportTool {
 public:
  PickObjectTool(QString prompt, std::function<bool(uint32_t)> filter, std::function<void(Viewport&, uint32_t, QMouseEvent*)> picked)
      : prompt_(std::move(prompt)), filter_(std::move(filter)), picked_(std::move(picked)) {}
  bool mousePress(Viewport& vp, QMouseEvent* e) override {
    if (e->button() == Qt::RightButton) {
      vp.editorState()->setViewportTool(nullptr);
      return true;
    }
    if (e->button() != Qt::LeftButton) return false;
    uint32_t id = vp.pickNode(static_cast<float>(e->position().x()), static_cast<float>(e->position().y()));
    if (!id || (filter_ && !filter_(id))) return true;
    auto keep = vp.editorState()->viewportToolShared();  // stay alive while the callback runs
    vp.editorState()->setViewportTool(nullptr);
    picked_(vp, id, e);
    return true;
  }
  bool mouseRelease(Viewport&, QMouseEvent* e) override { return e->button() != Qt::MiddleButton; }
  bool mouseMove(Viewport& vp, QMouseEvent* e) override {
    uint32_t id = vp.pickNode(static_cast<float>(e->position().x()), static_cast<float>(e->position().y()));
    vp.setCursor(id && (!filter_ || filter_(id)) ? Qt::PointingHandCursor : Qt::ArrowCursor);
    return false;
  }
  bool cancel() override { return false; }
  QString prompt() const override { return prompt_; }
  void drawOverlay(Viewport&, QPainter& p) override {
    p.setPen(QColor(120, 220, 255));
    p.drawText(QRectF(8, 24, 400, 18), Qt::AlignLeft | Qt::AlignVCenter, prompt_);
  }

 private:
  QString prompt_;
  std::function<bool(uint32_t)> filter_;
  std::function<void(Viewport&, uint32_t, QMouseEvent*)> picked_;
};

QVector<Node*> selectedTopLevel(SceneDocument* doc) {
  // Nodes whose parent is not also selected (so transforms are not applied twice).
  QVector<Node*> out;
  for (Node* n : doc->selectedNodes()) {
    bool parentSelected = false;
    for (Node* p = n->parent(); p; p = p->parent()) parentSelected |= doc->isSelected(p->id);
    if (!parentSelected) out.push_back(n);
  }
  return out;
}

// ---------------------------------------------------------------------------------------------
// Align

struct AlignOptions {
  bool pos[3] = {true, true, true};
  int current = 1;  // 0 min, 1 center, 2 pivot, 3 max
  int target = 1;
  bool orient[3] = {false, false, false};
  bool scale[3] = {false, false, false};
};

float alignRef(const Node& n, const Aabb& b, int axis, int mode) {
  switch (mode) {
    case 0: return b.valid() ? b.lo[axis] : n.worldPosition()[axis];
    case 2: return n.worldPosition()[axis];
    case 3: return b.valid() ? b.hi[axis] : n.worldPosition()[axis];
    default: return b.valid() ? b.center()[axis] : n.worldPosition()[axis];
  }
}

void applyAlign(SceneDocument* doc, const std::vector<std::pair<uint32_t, Transform>>& start, uint32_t targetId,
                const AlignOptions& o) {
  Scene& scene = doc->scene();
  Node* target = scene.findNode(targetId);
  if (!target) return;
  Aabb tb = hierarchyBounds(scene, *target);
  QVector<uint32_t> ids;
  for (const auto& [id, xf] : start) {
    Node* n = scene.findNode(id);
    if (!n) continue;
    n->transform = xf;
    // Orientation and scale first (they change the bounds).
    if (o.orient[0] || o.orient[1] || o.orient[2]) {
      Mat4 tw = target->worldMatrix(), nw = n->worldMatrix();
      Vec3 ns(length(nw.column(0)), length(nw.column(1)), length(nw.column(2)));
      Mat4 r;
      for (int c = 0; c < 3; ++c) {
        Vec3 axis = o.orient[c] ? normalize(tw.column(c)) : normalize(nw.column(c));
        for (int k = 0; k < 3; ++k) r.m[k][c] = axis[k];
      }
      // Re-orthogonalize when only some axes were taken from the target.
      Vec3 x(r.m[0][0], r.m[1][0], r.m[2][0]), y(r.m[0][1], r.m[1][1], r.m[2][1]);
      Vec3 z = normalize(cross(x, y));
      y = normalize(cross(z, x));
      x = normalize(x);
      Mat4 w;
      for (int k = 0; k < 3; ++k) {
        w.m[k][0] = x[k] * ns.x;
        w.m[k][1] = y[k] * ns.y;
        w.m[k][2] = z[k] * ns.z;
        w.m[k][3] = nw.m[k][3];
      }
      n->setWorldMatrix(w);
    }
    for (int k = 0; k < 3; ++k)
      if (o.scale[k]) n->transform.scale[k] = target->transform.scale[k];
    Aabb nb = hierarchyBounds(scene, *n);
    Vec3 p = n->worldPosition();
    for (int k = 0; k < 3; ++k)
      if (o.pos[k]) p[k] += alignRef(*target, tb, k, o.target) - alignRef(*n, nb, k, o.current);
    n->setWorldPosition(p);
    ids.push_back(id);
  }
  doc->notifyNodesChanged(ids, ChangeTransform);
}

class AlignDialog : public QDialog {
 public:
  AlignDialog(SceneDocument* doc, uint32_t targetId, QWidget* parent) : QDialog(parent), doc_(doc), target_(targetId) {
    setObjectName("AlignDialog");
    Node* t = doc->scene().findNode(targetId);
    setWindowTitle(tr("Align Selection (%1)").arg(t ? QString::fromStdString(t->name) : QString()));
    for (Node* n : selectedTopLevel(doc)) start_.push_back({n->id, n->transform});
    auto* v = new QVBoxLayout(this);
    auto* posBox = new QGroupBox(tr("Align Position (World):"), this);
    auto* pg = new QGridLayout(posBox);
    const char* axes[3] = {"X Position", "Y Position", "Z Position"};
    for (int k = 0; k < 3; ++k) {
      pos_[k] = new QCheckBox(tr(axes[k]), posBox);
      pos_[k]->setObjectName(QString("align_pos%1").arg(k));
      pos_[k]->setChecked(true);
      pg->addWidget(pos_[k], 0, k);
    }
    const char* modes[4] = {"Minimum", "Center", "Pivot Point", "Maximum"};
    auto* cur = new QGroupBox(tr("Current Object:"), posBox);
    auto* tgt = new QGroupBox(tr("Target Object:"), posBox);
    auto* cl = new QVBoxLayout(cur);
    auto* tl = new QVBoxLayout(tgt);
    curGroup_ = new QButtonGroup(this);
    tgtGroup_ = new QButtonGroup(this);
    for (int i = 0; i < 4; ++i) {
      auto* a = new QRadioButton(tr(modes[i]), cur);
      auto* b = new QRadioButton(tr(modes[i]), tgt);
      a->setObjectName(QString("align_cur%1").arg(i));
      b->setObjectName(QString("align_tgt%1").arg(i));
      curGroup_->addButton(a, i);
      tgtGroup_->addButton(b, i);
      cl->addWidget(a);
      tl->addWidget(b);
    }
    curGroup_->button(1)->setChecked(true);
    tgtGroup_->button(1)->setChecked(true);
    pg->addWidget(cur, 1, 0, 1, 2);
    pg->addWidget(tgt, 1, 2);
    v->addWidget(posBox);
    auto* oBox = new QGroupBox(tr("Align Orientation (Local):"), this);
    auto* ol = new QHBoxLayout(oBox);
    auto* sBox = new QGroupBox(tr("Match Scale:"), this);
    auto* sl = new QHBoxLayout(sBox);
    for (int k = 0; k < 3; ++k) {
      orient_[k] = new QCheckBox(QString("%1 Axis").arg("XYZ"[k]), oBox);
      scale_[k] = new QCheckBox(QString("%1 Axis").arg("XYZ"[k]), sBox);
      ol->addWidget(orient_[k]);
      sl->addWidget(scale_[k]);
    }
    v->addWidget(oBox);
    v->addWidget(sBox);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this);
    v->addWidget(bb);
    auto update = [this] { preview(); };
    for (int k = 0; k < 3; ++k) {
      connect(pos_[k], &QCheckBox::toggled, this, update);
      connect(orient_[k], &QCheckBox::toggled, this, update);
      connect(scale_[k], &QCheckBox::toggled, this, update);
    }
    connect(curGroup_, &QButtonGroup::idClicked, this, update);
    connect(tgtGroup_, &QButtonGroup::idClicked, this, update);
    connect(bb, &QDialogButtonBox::accepted, this, [this] {
      commit();
      accept();
    });
    connect(bb->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this] {
      // Apply keeps the current alignment and starts again from it.
      commit();
      start_.clear();
      for (Node* n : selectedTopLevel(doc_)) start_.push_back({n->id, n->transform});
      for (int k = 0; k < 3; ++k) {
        QSignalBlocker b1(pos_[k]), b2(orient_[k]), b3(scale_[k]);
        pos_[k]->setChecked(false);
        orient_[k]->setChecked(false);
        scale_[k]->setChecked(false);
      }
    });
    connect(bb, &QDialogButtonBox::rejected, this, [this] {
      restore();
      reject();
    });
    preview();
  }

  AlignOptions options() const {
    AlignOptions o;
    for (int k = 0; k < 3; ++k) {
      o.pos[k] = pos_[k]->isChecked();
      o.orient[k] = orient_[k]->isChecked();
      o.scale[k] = scale_[k]->isChecked();
    }
    o.current = curGroup_->checkedId();
    o.target = tgtGroup_->checkedId();
    return o;
  }

  void preview() { applyAlign(doc_, start_, target_, options()); }
  void restore() {
    QVector<uint32_t> ids;
    for (const auto& [id, xf] : start_)
      if (Node* n = doc_->scene().findNode(id)) {
        n->transform = xf;
        ids.push_back(id);
      }
    doc_->notifyNodesChanged(ids, ChangeTransform);
  }
  void commit() {
    AlignOptions o = options();
    restore();
    doc_->beginEdit(tr("Align"));
    applyAlign(doc_, start_, target_, o);
    doc_->endEdit();
  }

 private:
  SceneDocument* doc_;
  uint32_t target_;
  std::vector<std::pair<uint32_t, Transform>> start_;
  QCheckBox* pos_[3];
  QCheckBox* orient_[3];
  QCheckBox* scale_[3];
  QButtonGroup* curGroup_;
  QButtonGroup* tgtGroup_;
};

void startAlign(MainWindow& w) {
  SceneDocument* doc = w.document();
  if (doc->selection().isEmpty()) {
    prompt(QObject::tr("Select objects to align first"));
    return;
  }
  MainWindow* win = &w;
  w.editorState()->setViewportTool(std::make_shared<PickObjectTool>(
      QObject::tr("Align: pick the target object"), [doc](uint32_t id) { return !doc->isSelected(id); },
      [win](Viewport&, uint32_t id, QMouseEvent*) {
        auto* dlg = new AlignDialog(win->document(), id, win);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();
      }));
}

void startQuickAlign(MainWindow& w) {
  SceneDocument* doc = w.document();
  if (doc->selection().isEmpty()) {
    prompt(QObject::tr("Select objects to align first"));
    return;
  }
  w.editorState()->setViewportTool(std::make_shared<PickObjectTool>(
      QObject::tr("Quick Align: pick the target object"), [doc](uint32_t id) { return !doc->isSelected(id); },
      [doc](Viewport&, uint32_t id, QMouseEvent*) {
        Node* target = doc->scene().findNode(id);
        if (!target) return;
        auto nodes = selectedTopLevel(doc);
        Vec3 center = nodes.size() == 1 ? nodes[0]->worldPosition() : doc->selectionCenter();
        Vec3 d = target->worldPosition() - center;
        doc->beginEdit(QObject::tr("Quick Align"));
        QVector<uint32_t> ids;
        for (Node* n : nodes) {
          n->setWorldPosition(n->worldPosition() + d);
          ids.push_back(n->id);
        }
        doc->notifyNodesChanged(ids, ChangeTransform);
        doc->endEdit();
      }));
}

// Normal Align: pick a face on the source, then on the target.
class NormalAlignTool : public ViewportTool {
 public:
  explicit NormalAlignTool(SceneDocument* doc) : doc_(doc) {}
  static bool hitFace(Viewport& vp, uint32_t id, float px, float py, Vec3& p, Vec3& n) {
    Node* node = vp.document()->scene().findNode(id);
    if (!node || node->triMesh().empty()) return false;
    Mat4 m = node->objectWorldMatrix(), inv = m.inverse();
    Ray r = vp.camera().rayAt(px, py);
    Ray local{inv.transformPoint(r.o), inv.transformDir(r.d)};
    const TriMesh& t = node->triMesh();
    float best = kInfinity;
    for (size_t i = 0; i + 2 < t.indices.size(); i += 3) {
      Vec3 a = t.positions[t.indices[i]], b = t.positions[t.indices[i + 1]], c = t.positions[t.indices[i + 2]];
      Vec3 e1 = b - a, e2 = c - a, pv = cross(local.d, e2);
      float det = dot(e1, pv);
      if (std::fabs(det) < 1e-12f) continue;
      Vec3 s = local.o - a;
      float u = dot(s, pv) / det;
      if (u < 0 || u > 1) continue;
      Vec3 q = cross(s, e1);
      float v = dot(local.d, q) / det;
      if (v < 0 || u + v > 1) continue;
      float tt = dot(e2, q) / det;
      if (tt > 1e-5f && tt < best) {
        best = tt;
        p = m.transformPoint(local.o + local.d * tt);
        Vec3 ln = cross(e1, e2);
        // Normals transform with the inverse transpose.
        n = normalize(Vec3(inv.m[0][0] * ln.x + inv.m[1][0] * ln.y + inv.m[2][0] * ln.z,
                           inv.m[0][1] * ln.x + inv.m[1][1] * ln.y + inv.m[2][1] * ln.z,
                           inv.m[0][2] * ln.x + inv.m[1][2] * ln.y + inv.m[2][2] * ln.z));
        if (m.determinant3x3() < 0) n = -n;
      }
    }
    return best < kInfinity;
  }
  bool mousePress(Viewport& vp, QMouseEvent* e) override {
    if (e->button() == Qt::RightButton) {
      vp.editorState()->setViewportTool(nullptr);
      return true;
    }
    if (e->button() != Qt::LeftButton) return false;
    float px = static_cast<float>(e->position().x()), py = static_cast<float>(e->position().y());
    uint32_t id = vp.pickNode(px, py);
    if (!id) return true;
    Vec3 p, n;
    if (!hitFace(vp, id, px, py, p, n)) return true;
    if (stage_ == 0) {
      if (!doc_->isSelected(id)) {
        vp.editorState()->setPrompt(QObject::tr("Normal Align: pick a face on a selected object"));
        return true;
      }
      src_ = id;
      srcP_ = p;
      srcN_ = n;
      stage_ = 1;
      vp.editorState()->setPrompt(QObject::tr("Normal Align: pick a face on the target object"));
      return true;
    }
    if (id == src_) return true;
    // Rotate so the source normal faces the target normal, and move the points together.
    Vec3 to = -n, from = srcN_;
    Vec3 axis = cross(from, to);
    float s = length(axis), c = dot(from, to);
    Mat4 rot;
    if (s > 1e-6f) rot = Mat4::rotateAxis(axis / s, std::atan2(s, c));
    else if (c < 0) {
      Vec3 u, v;
      makeBasis(from, u, v);
      rot = Mat4::rotateAxis(u, kPi);
    }
    Mat4 w = Mat4::translate(p) * rot * Mat4::translate(-srcP_);
    doc_->beginEdit(QObject::tr("Normal Align"));
    QVector<uint32_t> ids;
    for (Node* node : selectedTopLevel(doc_)) {
      node->setWorldMatrix(w * node->worldMatrix());
      ids.push_back(node->id);
    }
    doc_->notifyNodesChanged(ids, ChangeTransform);
    doc_->endEdit();
    vp.editorState()->setViewportTool(nullptr);
    return true;
  }
  bool mouseRelease(Viewport&, QMouseEvent* e) override { return e->button() != Qt::MiddleButton; }
  QString prompt() const override { return QObject::tr("Normal Align: pick a face on the source object"); }
  void drawLines(Viewport&, LineBatch& lines) override {
    if (stage_ == 1) lines.line(srcP_, srcP_ + srcN_ * 30.0f, QColor(60, 160, 255));
  }

 private:
  SceneDocument* doc_;
  int stage_ = 0;
  uint32_t src_ = 0;
  Vec3 srcP_, srcN_;
};

void alignToView(MainWindow& w) {
  SceneDocument* doc = w.document();
  auto nodes = selectedTopLevel(doc);
  if (nodes.isEmpty()) {
    prompt(QObject::tr("Select objects first"));
    return;
  }
  QDialog dlg(&w);
  dlg.setWindowTitle(QObject::tr("Align to View"));
  auto* f = new QFormLayout(&dlg);
  auto* axis = new QComboBox(&dlg);
  axis->addItems({"X", "Y", "Z"});
  axis->setCurrentIndex(2);
  auto* flip = new QCheckBox(QObject::tr("Flip"), &dlg);
  f->addRow(QObject::tr("Align axis:"), axis);
  f->addRow(flip);
  auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
  QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  f->addRow(bb);
  if (dlg.exec() != QDialog::Accepted) return;
  Viewport* vp = w.viewports()->active();
  vp->updateCamera();
  Vec3 toViewer = -vp->camera().forward();
  if (flip->isChecked()) toViewer = -toViewer;
  int a = axis->currentIndex();
  doc->beginEdit(QObject::tr("Align to View"));
  QVector<uint32_t> ids;
  for (Node* n : nodes) {
    Mat4 m = n->worldMatrix();
    Vec3 cur = normalize(m.column(a));
    Vec3 ax = cross(cur, toViewer);
    float s = length(ax), c = dot(cur, toViewer);
    Mat4 r;
    if (s > 1e-6f) r = Mat4::rotateAxis(ax / s, std::atan2(s, c));
    else if (c < 0) r = Mat4::rotateAxis(normalize(m.column((a + 1) % 3)), kPi);
    Vec3 p = m.translation();
    n->setWorldMatrix(Mat4::translate(p) * r * Mat4::translate(-p) * m);
    ids.push_back(n->id);
  }
  doc->notifyNodesChanged(ids, ChangeTransform);
  doc->endEdit();
}

// ---------------------------------------------------------------------------------------------
// Mirror

class MirrorDialog : public QDialog {
 public:
  MirrorDialog(SceneDocument* doc, QWidget* parent) : QDialog(parent), doc_(doc) {
    setObjectName("MirrorDialog");
    setWindowTitle(tr("Mirror: World Coordinates"));
    for (Node* n : selectedTopLevel(doc)) start_.push_back({n->id, n->transform});
    center_ = doc->selectionCenter();
    auto* v = new QVBoxLayout(this);
    auto* axisBox = new QGroupBox(tr("Mirror Axis:"), this);
    auto* ag = new QGridLayout(axisBox);
    axes_ = new QButtonGroup(this);
    const char* names[6] = {"X", "Y", "Z", "XY", "YZ", "ZX"};
    for (int i = 0; i < 6; ++i) {
      auto* r = new QRadioButton(names[i], axisBox);
      r->setObjectName(QString("mirror_axis%1").arg(i));
      axes_->addButton(r, i);
      ag->addWidget(r, i % 3, i / 3);
    }
    axes_->button(0)->setChecked(true);
    offset_ = new QDoubleSpinBox(axisBox);
    offset_->setObjectName("mirror_offset");
    offset_->setRange(-1e7, 1e7);
    ag->addWidget(new QLabel(tr("Offset:"), axisBox), 3, 0);
    ag->addWidget(offset_, 3, 1);
    v->addWidget(axisBox);
    auto* cloneBox = new QGroupBox(tr("Clone Selection:"), this);
    auto* cl = new QVBoxLayout(cloneBox);
    clone_ = new QButtonGroup(this);
    const char* modes[4] = {"No Clone", "Copy", "Instance", "Reference"};
    for (int i = 0; i < 4; ++i) {
      auto* r = new QRadioButton(tr(modes[i]), cloneBox);
      r->setObjectName(QString("mirror_clone%1").arg(i));
      clone_->addButton(r, i);
      cl->addWidget(r);
    }
    clone_->button(0)->setChecked(true);
    v->addWidget(cloneBox);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    v->addWidget(bb);
    connect(axes_, &QButtonGroup::idClicked, this, [this] { preview(); });
    connect(offset_, &QDoubleSpinBox::valueChanged, this, [this] { preview(); });
    connect(bb, &QDialogButtonBox::accepted, this, [this] {
      commit();
      accept();
    });
    connect(bb, &QDialogButtonBox::rejected, this, [this] {
      restore();
      reject();
    });
    preview();
  }

  Mat4 mirrorMatrix() const {
    int a = axes_->checkedId();
    Vec3 s(1.0f);
    if (a == 0 || a == 3 || a == 5) s.x = -1;
    if (a == 1 || a == 3 || a == 4) s.y = -1;
    if (a == 2 || a == 4 || a == 5) s.z = -1;
    if (a >= 3) {
      // Two-axis mirrors flip one of the axes back so the result stays a mirror (like 3ds Max
      // XY = mirror X and Y, which is a rotation by 180 degrees about Z).
    }
    Vec3 off;
    float o = static_cast<float>(offset_->value());
    if (a == 0) off.x = o;
    else if (a == 1) off.y = o;
    else if (a == 2) off.z = o;
    else if (a == 3) off = Vec3(o, o, 0);
    else if (a == 4) off = Vec3(0, o, o);
    else off = Vec3(o, 0, o);
    return Mat4::translate(center_ + off) * Mat4::scale(s) * Mat4::translate(-center_);
  }

  void apply(const std::vector<uint32_t>& ids) {
    Mat4 m = mirrorMatrix();
    QVector<uint32_t> changed;
    for (size_t i = 0; i < ids.size() && i < start_.size(); ++i)
      if (Node* n = doc_->scene().findNode(ids[i])) {
        Node* orig = doc_->scene().findNode(start_[i].first);
        Transform xf = start_[i].second;
        if (orig) {
          Transform saved = orig->transform;
          orig->transform = xf;
          Mat4 world = orig->worldMatrix();
          orig->transform = saved;
          n->setWorldMatrix(m * world);
        }
        changed.push_back(n->id);
      }
    doc_->notifyNodesChanged(changed, ChangeTransform);
  }

  void preview() {
    restore();
    if (clone_->checkedId() == 0) {
      std::vector<uint32_t> ids;
      for (const auto& s : start_) ids.push_back(s.first);
      apply(ids);
    }
    // Clones are only created on OK (the preview shows the mirror on the originals).
  }

  void restore() {
    QVector<uint32_t> ids;
    for (const auto& [id, xf] : start_)
      if (Node* n = doc_->scene().findNode(id)) {
        n->transform = xf;
        ids.push_back(id);
      }
    doc_->notifyNodesChanged(ids, ChangeTransform);
  }

  void commit() {
    restore();
    int mode = clone_->checkedId();
    doc_->beginEdit(tr("Mirror"));
    std::vector<uint32_t> targets;
    if (mode == 0) {
      for (const auto& s : start_) targets.push_back(s.first);
    } else {
      QVector<uint32_t> created;
      for (const auto& s : start_) {
        Node* src = doc_->scene().findNode(s.first);
        if (!src) continue;
        auto copy = src->clone();
        copy->name = doc_->scene().uniqueNodeName(src->name);
        Node* added = doc_->scene().addNode(std::move(copy));
        applyCloneMode(doc_->scene(), src->id, added->id, static_cast<CloneOptionsDialog::Mode>(mode - 1));
        targets.push_back(added->id);
        created.push_back(added->id);
      }
      doc_->notifyStructureChanged();
      doc_->setSelection(created);
    }
    apply(targets);
    doc_->endEdit();
  }

 private:
  SceneDocument* doc_;
  std::vector<std::pair<uint32_t, Transform>> start_;
  Vec3 center_;
  QButtonGroup* axes_;
  QButtonGroup* clone_;
  QDoubleSpinBox* offset_;
};

// ---------------------------------------------------------------------------------------------
// Array

struct ArraySettings {
  Vec3 move, rotate, scale{100, 100, 100};
  bool totals = false;
  int type = 0;  // 0 copy, 1 instance, 2 reference
  int dims = 1;
  int count[3] = {10, 1, 1};
  Vec3 row2, row3;
  bool aroundWorld = false;
};

// Creates the array copies (inside an open edit); returns the new node ids.
QVector<uint32_t> buildArray(SceneDocument* doc, const QVector<uint32_t>& sources, const ArraySettings& s) {
  QVector<uint32_t> created;
  Scene& scene = doc->scene();
  int n1 = std::max(1, s.count[0]);
  int n2 = s.dims >= 2 ? std::max(1, s.count[1]) : 1;
  int n3 = s.dims >= 3 ? std::max(1, s.count[2]) : 1;
  Vec3 mv = s.move, rt = s.rotate, sc = s.scale;
  if (s.totals && n1 > 1) {
    mv = mv / static_cast<float>(n1 - 1);
    rt = rt / static_cast<float>(n1 - 1);
    for (int k = 0; k < 3; ++k) sc[k] = 100.0f * std::pow(sc[k] / 100.0f, 1.0f / (n1 - 1));
  }
  for (uint32_t sid : sources) {
    Node* src = scene.findNode(sid);
    if (!src) continue;
    Mat4 base = src->worldMatrix();
    Vec3 pivot = base.translation();
    for (int k = 0; k < n3; ++k)
      for (int j = 0; j < n2; ++j)
        for (int i = 0; i < n1; ++i) {
          if (i == 0 && j == 0 && k == 0) continue;  // the original
          float fi = static_cast<float>(i);
          Mat4 r = Mat4::fromEulerDeg(rt * fi);
          Vec3 f(std::pow(sc.x / 100.0f, fi), std::pow(sc.y / 100.0f, fi), std::pow(sc.z / 100.0f, fi));
          Vec3 c = s.aroundWorld ? Vec3() : pivot;
          Mat4 xf = Mat4::translate(mv * fi + s.row2 * static_cast<float>(j) + s.row3 * static_cast<float>(k)) *
                    Mat4::translate(c) * r * Mat4::translate(-c);
          Mat4 world = xf * Mat4::translate(pivot) * Mat4::scale(f) * Mat4::translate(-pivot) * base;
          auto copy = src->clone();
          Node* added = scene.addNode(std::move(copy));
          applyCloneMode(scene, sid, added->id, static_cast<CloneOptionsDialog::Mode>(s.type));
          added->setWorldMatrix(world);
          created.push_back(added->id);
        }
  }
  doc->notifyStructureChanged();
  return created;
}

class ArrayDialog : public QDialog {
 public:
  ArrayDialog(SceneDocument* doc, QWidget* parent) : QDialog(parent), doc_(doc) {
    setObjectName("ArrayDialog");
    setWindowTitle(tr("Array"));
    for (Node* n : selectedTopLevel(doc)) sources_.push_back(n->id);
    auto* v = new QVBoxLayout(this);
    auto* tb = new QGroupBox(tr("Array Transformation: World Coordinates"), this);
    auto* g = new QGridLayout(tb);
    const char* rows[3] = {"Move", "Rotate", "Scale"};
    for (int c = 0; c < 3; ++c) g->addWidget(new QLabel(QString("%1").arg("XYZ"[c]), tb), 0, c + 1, Qt::AlignCenter);
    for (int r = 0; r < 3; ++r) {
      g->addWidget(new QLabel(tr(rows[r]), tb), r + 1, 0);
      for (int c = 0; c < 3; ++c) {
        auto* s = new QDoubleSpinBox(tb);
        s->setObjectName(QString("array_%1%2").arg(rows[r]).arg("XYZ"[c]));
        s->setRange(r == 2 ? 0.01 : -1e7, 1e7);
        s->setDecimals(2);
        s->setValue(r == 2 ? 100.0 : 0.0);
        s->setKeyboardTracking(false);
        installSpinnerScrub(s);
        spin_[r][c] = s;
        g->addWidget(s, r + 1, c + 1);
        connect(s, &QDoubleSpinBox::valueChanged, this, [this] { changed(); });
      }
    }
    totals_ = new QCheckBox(tr("Values are totals (not increments)"), tb);
    world_ = new QCheckBox(tr("Rotate around the world origin"), tb);
    g->addWidget(totals_, 4, 0, 1, 4);
    g->addWidget(world_, 5, 0, 1, 4);
    v->addWidget(tb);
    auto* typeBox = new QGroupBox(tr("Type of Object"), this);
    auto* tl = new QHBoxLayout(typeBox);
    type_ = new QButtonGroup(this);
    const char* types[3] = {"Copy", "Instance", "Reference"};
    for (int i = 0; i < 3; ++i) {
      auto* r = new QRadioButton(tr(types[i]), typeBox);
      type_->addButton(r, i);
      tl->addWidget(r);
    }
    type_->button(0)->setChecked(true);
    v->addWidget(typeBox);
    auto* dimBox = new QGroupBox(tr("Array Dimensions"), this);
    auto* dg = new QGridLayout(dimBox);
    dims_ = new QButtonGroup(this);
    for (int d = 0; d < 3; ++d) {
      auto* r = new QRadioButton(QString("%1D").arg(d + 1), dimBox);
      dims_->addButton(r, d + 1);
      dg->addWidget(r, d, 0);
      count_[d] = new QSpinBox(dimBox);
      count_[d]->setObjectName(QString("array_count%1").arg(d + 1));
      count_[d]->setRange(1, 100000);
      count_[d]->setValue(d == 0 ? 10 : 1);
      dg->addWidget(count_[d], d, 1);
      connect(count_[d], &QSpinBox::valueChanged, this, [this] { changed(); });
      if (d > 0)
        for (int c = 0; c < 3; ++c) {
          auto* s = new QDoubleSpinBox(dimBox);
          s->setRange(-1e7, 1e7);
          s->setObjectName(QString("array_row%1%2").arg(d + 1).arg("XYZ"[c]));
          installSpinnerScrub(s);
          (d == 1 ? row2_ : row3_)[c] = s;
          dg->addWidget(s, d, 2 + c);
          connect(s, &QDoubleSpinBox::valueChanged, this, [this] { changed(); });
        }
    }
    dims_->button(1)->setChecked(true);
    total_ = new QLabel(dimBox);
    dg->addWidget(total_, 3, 0, 1, 5);
    v->addWidget(dimBox);
    preview_ = new QPushButton(tr("Preview"), this);
    preview_->setCheckable(true);
    preview_->setObjectName("array_preview");
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    auto* row = new QHBoxLayout();
    row->addWidget(preview_);
    row->addStretch();
    row->addWidget(bb);
    v->addLayout(row);
    connect(dims_, &QButtonGroup::idClicked, this, [this] { changed(); });
    connect(type_, &QButtonGroup::idClicked, this, [this] { changed(); });
    connect(totals_, &QCheckBox::toggled, this, [this] { changed(); });
    connect(world_, &QCheckBox::toggled, this, [this] { changed(); });
    connect(preview_, &QPushButton::toggled, this, [this] { changed(); });
    connect(bb, &QDialogButtonBox::accepted, this, [this] {
      clearPreview();
      doc_->beginEdit(tr("Array"));
      QVector<uint32_t> made = buildArray(doc_, sources_, settings());
      doc_->endEdit();
      prompt(tr("Array created %1 object(s)").arg(made.size()));
      accept();
    });
    connect(bb, &QDialogButtonBox::rejected, this, [this] {
      clearPreview();
      reject();
    });
    changed();
  }

  ArraySettings settings() const {
    ArraySettings s;
    for (int c = 0; c < 3; ++c) {
      s.move[c] = static_cast<float>(spin_[0][c]->value());
      s.rotate[c] = static_cast<float>(spin_[1][c]->value());
      s.scale[c] = static_cast<float>(spin_[2][c]->value());
      s.row2[c] = static_cast<float>(row2_[c]->value());
      s.row3[c] = static_cast<float>(row3_[c]->value());
    }
    s.totals = totals_->isChecked();
    s.aroundWorld = world_->isChecked();
    s.type = type_->checkedId();
    s.dims = dims_->checkedId();
    for (int d = 0; d < 3; ++d) s.count[d] = count_[d]->value();
    return s;
  }

  void changed() {
    ArraySettings s = settings();
    long total = s.count[0];
    if (s.dims >= 2) total *= s.count[1];
    if (s.dims >= 3) total *= s.count[2];
    total_->setText(tr("Total in Array: %1").arg(total * std::max<long>(1, sources_.size())));
    clearPreview();
    if (preview_->isChecked()) {
      previewIds_ = buildArray(doc_, sources_, s);
    }
  }

  void clearPreview() {
    if (previewIds_.isEmpty()) return;
    for (uint32_t id : previewIds_) doc_->scene().removeNode(id);
    previewIds_.clear();
    doc_->notifyStructureChanged();
  }

 private:
  SceneDocument* doc_;
  QVector<uint32_t> sources_, previewIds_;
  QDoubleSpinBox* spin_[3][3];
  QDoubleSpinBox* row2_[3];
  QDoubleSpinBox* row3_[3];
  QCheckBox* totals_;
  QCheckBox* world_;
  QButtonGroup* type_;
  QButtonGroup* dims_;
  QSpinBox* count_[3];
  QLabel* total_;
  QPushButton* preview_;
};

// ---------------------------------------------------------------------------------------------
// Snapshot

void snapshot(MainWindow& w) {
  SceneDocument* doc = w.document();
  auto nodes = doc->selectedNodes();
  if (nodes.isEmpty()) {
    prompt(QObject::tr("Select objects to snapshot"));
    return;
  }
  Scene& scene = doc->scene();
  QDialog dlg(&w);
  dlg.setWindowTitle(QObject::tr("Snapshot"));
  auto* f = new QFormLayout(&dlg);
  auto* single = new QRadioButton(QObject::tr("Single"), &dlg);
  auto* range = new QRadioButton(QObject::tr("Range"), &dlg);
  single->setChecked(true);
  auto* from = new QSpinBox(&dlg);
  auto* to = new QSpinBox(&dlg);
  auto* copies = new QSpinBox(&dlg);
  from->setRange(-100000, 100000);
  to->setRange(-100000, 100000);
  from->setValue(scene.time.startFrame);
  to->setValue(scene.time.endFrame);
  copies->setRange(1, 10000);
  copies->setValue(10);
  auto* method = new QComboBox(&dlg);
  method->addItems({QObject::tr("Copy"), QObject::tr("Instance"), QObject::tr("Reference"), QObject::tr("Mesh")});
  method->setCurrentIndex(3);
  f->addRow(single);
  f->addRow(range);
  f->addRow(QObject::tr("From:"), from);
  f->addRow(QObject::tr("To:"), to);
  f->addRow(QObject::tr("Copies:"), copies);
  f->addRow(QObject::tr("Clone Method:"), method);
  auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
  QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  f->addRow(bb);
  if (dlg.exec() != QDialog::Accepted) return;
  std::vector<double> frames;
  double saved = scene.time.currentFrame;
  if (single->isChecked()) {
    frames.push_back(saved);
  } else {
    int n = copies->value();
    for (int i = 0; i < n; ++i)
      frames.push_back(n == 1 ? from->value() : from->value() + (to->value() - from->value()) * static_cast<double>(i) / (n - 1));
  }
  QVector<uint32_t> ids;
  for (Node* n : nodes) ids.push_back(n->id);
  doc->beginEdit(QObject::tr("Snapshot"));
  QVector<uint32_t> created;
  for (double fr : frames) {
    scene.time.currentFrame = fr;
    for (auto& n : scene.nodes) n->invalidate();
    for (uint32_t id : ids) {
      Node* src = scene.findNode(id);
      if (!src || (src->kind() != ObjectKind::Geometry && src->kind() != ObjectKind::Shape)) continue;
      std::unique_ptr<Node> copy;
      if (method->currentIndex() == 3) {
        auto poly = std::make_shared<EditablePolyObject>();
        poly->setMesh(src->mesh());
        copy = std::make_unique<Node>();
        copy->object = poly;
        copy->materialId = src->materialId;
        copy->wireColor = src->wireColor;
      } else {
        copy = src->clone();
      }
      copy->name = scene.uniqueNodeName(src->name);
      copy->parentId = 0;
      Mat4 world = src->objectWorldMatrix();
      Node* added = scene.addNode(std::move(copy));
      if (method->currentIndex() != 3) applyCloneMode(scene, id, added->id, static_cast<CloneOptionsDialog::Mode>(method->currentIndex()));
      if (method->currentIndex() != 3) added->objectOffset = Transform();
      added->setWorldMatrix(world);
      created.push_back(added->id);
    }
  }
  scene.time.currentFrame = saved;
  for (auto& n : scene.nodes) n->invalidate();
  doc->notifyStructureChanged();
  doc->endEdit();
  prompt(QObject::tr("Snapshot created %1 object(s)").arg(created.size()));
}

// ---------------------------------------------------------------------------------------------
// Spacing Tool

// Points evenly spaced along a polyline (world space), with tangents.
void spacePoints(const std::vector<Vec3>& line, int count, float startOff, float endOff, std::vector<Vec3>& pts,
                 std::vector<Vec3>& tangents) {
  std::vector<float> acc(line.size(), 0.0f);
  for (size_t i = 1; i < line.size(); ++i) acc[i] = acc[i - 1] + length(line[i] - line[i - 1]);
  float total = line.empty() ? 0.0f : acc.back();
  float a = std::clamp(startOff, 0.0f, total), b = std::clamp(total - endOff, a, total);
  for (int i = 0; i < count; ++i) {
    float s = count == 1 ? (a + b) * 0.5f : a + (b - a) * i / (count - 1);
    size_t k = 1;
    while (k + 1 < line.size() && acc[k] < s) ++k;
    float seg = acc[k] - acc[k - 1];
    float t = seg > 1e-9f ? (s - acc[k - 1]) / seg : 0.0f;
    pts.push_back(lerp(line[k - 1], line[k], t));
    tangents.push_back(normalize(line[k] - line[k - 1]));
  }
}

class SpacingDialog : public QDialog {
 public:
  SpacingDialog(MainWindow& w) : QDialog(&w), w_(w) {
    setObjectName("SpacingDialog");
    setWindowTitle(tr("Spacing Tool"));
    SceneDocument* doc = w.document();
    auto* f = new QFormLayout(this);
    path_ = new QComboBox(this);
    path_->setObjectName("spacing_path");
    for (const auto& n : doc->scene().nodes)
      if (!n->shapeLines().empty()) path_->addItem(QString::fromStdString(n->name), n->id);
    auto* pick = new QPushButton(tr("Pick Path"), this);
    connect(pick, &QPushButton::clicked, this, [this] {
      QPointer<SpacingDialog> self(this);
      SceneDocument* d = w_.document();
      w_.editorState()->setViewportTool(std::make_shared<PickObjectTool>(
          tr("Pick a shape to use as the path"),
          [d](uint32_t id) {
            Node* n = d->scene().findNode(id);
            return n && !n->shapeLines().empty();
          },
          [self](Viewport&, uint32_t id, QMouseEvent*) {
            if (!self) return;
            int i = self->path_->findData(id);
            if (i >= 0) self->path_->setCurrentIndex(i);
          }));
    });
    auto* row = new QHBoxLayout();
    row->addWidget(path_, 1);
    row->addWidget(pick);
    f->addRow(tr("Path:"), row);
    count_ = new QSpinBox(this);
    count_->setObjectName("spacing_count");
    count_->setRange(1, 100000);
    count_->setValue(5);
    f->addRow(tr("Count:"), count_);
    start_ = new QDoubleSpinBox(this);
    end_ = new QDoubleSpinBox(this);
    start_->setRange(0, 1e7);
    end_->setRange(0, 1e7);
    f->addRow(tr("Start Offset:"), start_);
    f->addRow(tr("End Offset:"), end_);
    follow_ = new QCheckBox(tr("Follow (align to the path)"), this);
    f->addRow(follow_);
    type_ = new QComboBox(this);
    type_->addItems({tr("Copy"), tr("Instance"), tr("Reference")});
    f->addRow(tr("Type of Object:"), type_);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Close, this);
    bb->button(QDialogButtonBox::Apply)->setObjectName("spacing_apply");
    connect(bb->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this] { apply(); });
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    f->addRow(bb);
  }

  void apply() {
    SceneDocument* doc = w_.document();
    Scene& scene = doc->scene();
    QVector<uint32_t> sources;
    for (Node* n : selectedTopLevel(doc)) sources.push_back(n->id);
    Node* path = scene.findNode(path_->currentData().toUInt());
    if (sources.isEmpty() || !path) {
      prompt(tr("Select the object(s) to distribute and choose a path"));
      return;
    }
    std::vector<Vec3> line;
    Mat4 pm = path->objectWorldMatrix();
    for (const auto& p : path->shapeLines()[0]) line.push_back(pm.transformPoint(p));
    std::vector<Vec3> pts, tans;
    spacePoints(line, count_->value(), static_cast<float>(start_->value()), static_cast<float>(end_->value()), pts, tans);
    doc->beginEdit(tr("Spacing Tool"));
    QVector<uint32_t> created;
    for (size_t i = 0; i < pts.size(); ++i) {
      uint32_t sid = sources[static_cast<int>(i % sources.size())];
      Node* src = scene.findNode(sid);
      if (!src) continue;
      auto copy = src->clone();
      copy->parentId = 0;
      Node* added = scene.addNode(std::move(copy));
      applyCloneMode(scene, sid, added->id, static_cast<CloneOptionsDialog::Mode>(type_->currentIndex()));
      Mat4 world = src->worldMatrix();
      if (follow_->isChecked()) {
        // Local X follows the path direction.
        Vec3 x = tans[i], z(0, 0, 1);
        if (std::fabs(dot(x, z)) > 0.99f) z = Vec3(0, 1, 0);
        Vec3 y = normalize(cross(z, x));
        z = cross(x, y);
        Vec3 s(length(world.column(0)), length(world.column(1)), length(world.column(2)));
        Mat4 m;
        for (int k = 0; k < 3; ++k) {
          m.m[k][0] = x[k] * s.x;
          m.m[k][1] = y[k] * s.y;
          m.m[k][2] = z[k] * s.z;
        }
        world = m;
      }
      for (int k = 0; k < 3; ++k) world.m[k][3] = pts[i][k];
      added->setWorldMatrix(world);
      created.push_back(added->id);
    }
    doc->notifyStructureChanged();
    doc->endEdit();
    prompt(tr("Spacing Tool placed %1 object(s)").arg(created.size()));
  }

 private:
  MainWindow& w_;
  QComboBox* path_;
  QSpinBox* count_;
  QDoubleSpinBox* start_;
  QDoubleSpinBox* end_;
  QCheckBox* follow_;
  QComboBox* type_;
};

// ---------------------------------------------------------------------------------------------
// Groups

void groupSelection(MainWindow& w) {
  SceneDocument* doc = w.document();
  auto nodes = selectedTopLevel(doc);
  if (nodes.isEmpty()) {
    prompt(QObject::tr("Select objects to group"));
    return;
  }
  bool ok = false;
  QString name = QInputDialog::getText(&w, QObject::tr("Group"), QObject::tr("Group name:"), QLineEdit::Normal,
                                       QString::fromStdString(doc->scene().uniqueNodeName("Group")), &ok);
  if (!ok || name.isEmpty()) return;
  Scene& scene = doc->scene();
  Aabb b;
  for (Node* n : nodes) {
    Aabb nb = hierarchyBounds(scene, *n);
    if (nb.valid()) {
      b.expand(nb.lo);
      b.expand(nb.hi);
    } else {
      b.expand(n->worldPosition());
    }
  }
  doc->beginEdit(QObject::tr("Group"));
  Node* head = scene.createNode("GroupHead");
  head->name = name.toStdString();
  head->groupHead = true;
  head->groupOpen = false;
  head->transform.position = b.valid() ? b.center() : Vec3();
  head->wireColor = Vec3(0.9f, 0.3f, 0.6f);
  // Groups inherit the parent of the first member (nested groups).
  uint32_t commonParent = nodes[0]->parentId;
  if (commonParent) scene.setParent(head->id, commonParent);
  if (Node* p = scene.findNode(commonParent); p && (p->groupHead)) head->groupMember = true;
  for (Node* n : nodes) {
    scene.setParent(n->id, head->id);
    n->groupMember = true;
  }
  uint32_t hid = head->id;
  doc->notifyStructureChanged();
  doc->setSelection({hid});
  doc->endEdit();
}

Node* groupHeadOf(Node* n) {
  for (Node* p = n; p; p = p->parent())
    if (p->groupHead) return p;
  return nullptr;
}

QVector<Node*> selectedGroupHeads(SceneDocument* doc) {
  QVector<Node*> heads;
  for (Node* n : doc->selectedNodes())
    if (n->groupHead && !heads.contains(n)) heads.push_back(n);
  return heads;
}

void ungroup(MainWindow& w, bool recursive) {
  SceneDocument* doc = w.document();
  auto heads = selectedGroupHeads(doc);
  if (heads.isEmpty()) {
    prompt(QObject::tr("Select a group"));
    return;
  }
  Scene& scene = doc->scene();
  doc->beginEdit(recursive ? QObject::tr("Explode") : QObject::tr("Ungroup"));
  QVector<uint32_t> released;
  std::vector<uint32_t> todo;
  for (Node* h : heads) todo.push_back(h->id);
  while (!todo.empty()) {
    uint32_t hid = todo.back();
    todo.pop_back();
    Node* h = scene.findNode(hid);
    if (!h) continue;
    uint32_t parent = h->parentId;
    for (Node* c : scene.children(hid)) {
      c->groupMember = false;
      scene.setParent(c->id, parent);
      if (Node* p = scene.findNode(parent); p && p->groupHead) c->groupMember = true;
      if (recursive && c->groupHead) todo.push_back(c->id);
      else released.push_back(c->id);
    }
    scene.removeNode(hid);
  }
  doc->notifyStructureChanged();
  doc->setSelection(released);
  doc->endEdit();
}

void openGroups(MainWindow& w, bool open, bool recursive) {
  SceneDocument* doc = w.document();
  QVector<Node*> heads = selectedGroupHeads(doc);
  if (!open) {
    // Close: the group of any selected member.
    for (Node* n : doc->selectedNodes())
      if (Node* h = groupHeadOf(n->groupHead ? n : n->parent()); h && !heads.contains(h)) heads.push_back(h);
  }
  if (heads.isEmpty()) {
    prompt(QObject::tr("Select a group"));
    return;
  }
  Scene& scene = doc->scene();
  doc->beginEdit(open ? QObject::tr("Open Group") : QObject::tr("Close Group"));
  QVector<uint32_t> sel;
  std::vector<uint32_t> todo;
  for (Node* h : heads) todo.push_back(h->id);
  while (!todo.empty()) {
    Node* h = scene.findNode(todo.back());
    todo.pop_back();
    if (!h) continue;
    h->groupOpen = open;
    sel.push_back(h->id);
    if (recursive)
      for (Node* c : scene.children(h->id))
        if (c->groupHead) todo.push_back(c->id);
  }
  doc->notifyStructureChanged();
  doc->setSelection(sel);
  doc->endEdit();
}

void attachToGroup(MainWindow& w) {
  SceneDocument* doc = w.document();
  if (doc->selection().isEmpty()) {
    prompt(QObject::tr("Select objects to attach to a group"));
    return;
  }
  w.editorState()->setViewportTool(std::make_shared<PickObjectTool>(
      QObject::tr("Pick the group to attach to"),
      [doc](uint32_t id) {
        Node* n = doc->scene().findNode(id);
        return n && n->groupHead && !doc->isSelected(id);
      },
      [doc](Viewport&, uint32_t id, QMouseEvent*) {
        Node* head = doc->scene().findNode(id);
        if (!head) return;
        doc->beginEdit(QObject::tr("Attach to Group"));
        for (Node* n : selectedTopLevel(doc)) {
          if (n->id == id || doc->scene().isAncestor(n->id, id)) continue;
          doc->scene().setParent(n->id, id);
          n->groupMember = true;
        }
        doc->notifyStructureChanged();
        doc->setSelection({id});
        doc->endEdit();
      }));
}

void detachFromGroup(MainWindow& w) {
  SceneDocument* doc = w.document();
  Scene& scene = doc->scene();
  QVector<uint32_t> ids;
  for (Node* n : doc->selectedNodes())
    if (n->groupMember) ids.push_back(n->id);
  if (ids.isEmpty()) {
    prompt(QObject::tr("Open the group and select the members to detach"));
    return;
  }
  doc->beginEdit(QObject::tr("Detach from Group"));
  for (uint32_t id : ids) {
    Node* n = scene.findNode(id);
    Node* head = n ? n->parent() : nullptr;
    uint32_t up = head ? head->parentId : 0;
    n->groupMember = false;
    scene.setParent(id, up);
    if (Node* p = scene.findNode(up); p && p->groupHead) n->groupMember = true;
  }
  doc->notifyStructureChanged();
  doc->setSelection(ids);
  doc->endEdit();
}

// Draws group brackets (selected closed groups) and pink boxes (open groups).
class GroupDecorator : public ViewportDecorator {
 public:
  void drawLines(Viewport& vp, LineBatch& lines) override {
    SceneDocument* doc = vp.document();
    const Scene& scene = doc->scene();
    for (const auto& up : scene.nodes) {
      const Node& n = *up;
      if (!n.groupHead || !scene.isNodeVisible(n)) continue;
      bool sel = doc->isSelected(n.id);
      bool memberSelected = false;
      for (Node* c : scene.children(n.id)) memberSelected |= doc->isSelected(c->id);
      if (!sel && !(n.groupOpen && memberSelected) && !n.groupOpen) continue;
      Aabb b = hierarchyBounds(scene, n);
      if (!b.valid()) continue;
      QColor col = n.groupOpen ? QColor(255, 110, 200) : QColor(255, 255, 255);
      auto corner = [&](int i) {
        return Vec3((i & 1) ? b.hi.x : b.lo.x, (i & 2) ? b.hi.y : b.lo.y, (i & 4) ? b.hi.z : b.lo.z);
      };
      Vec3 ext = b.extent() * 0.25f;
      if (n.groupOpen) {
        static const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        for (const auto& e : edges) lines.line(corner(e[0]), corner(e[1]), col);
      } else {
        for (int i = 0; i < 8; ++i) {
          Vec3 c = corner(i);
          for (int k = 0; k < 3; ++k) {
            Vec3 d;
            d[k] = ((i >> k) & 1) ? -ext[k] : ext[k];
            lines.line(c, c + d, col);
          }
        }
      }
    }
  }
};

// ---------------------------------------------------------------------------------------------
// Select and Link

class SelectAndLinkTool : public ViewportTool {
 public:
  bool mousePress(Viewport& vp, QMouseEvent* e) override {
    if (e->button() == Qt::RightButton) {
      vp.editorState()->setViewportTool(nullptr);
      return true;
    }
    if (e->button() != Qt::LeftButton || vp.editorState()->creating()) return false;
    uint32_t id = vp.pickNode(static_cast<float>(e->position().x()), static_cast<float>(e->position().y()));
    if (!id) return true;
    SceneDocument* doc = vp.document();
    if (!doc->isSelected(id)) doc->select(id, false);
    children_.clear();
    for (Node* n : selectedTopLevel(doc)) children_.push_back(n->id);
    dragging_ = true;
    from_ = to_ = e->position().toPoint();
    return true;
  }
  bool mouseMove(Viewport& vp, QMouseEvent* e) override {
    if (!dragging_) return false;
    to_ = e->position().toPoint();
    target_ = vp.pickNode(static_cast<float>(to_.x()), static_cast<float>(to_.y()));
    vp.setCursor(target_ ? Qt::DragLinkCursor : Qt::ForbiddenCursor);
    return true;
  }
  bool mouseRelease(Viewport& vp, QMouseEvent* e) override {
    if (!dragging_ || e->button() != Qt::LeftButton) return dragging_;
    dragging_ = false;
    vp.unsetCursor();
    SceneDocument* doc = vp.document();
    uint32_t parent = vp.pickNode(static_cast<float>(e->position().x()), static_cast<float>(e->position().y()));
    // Dropping on one of the dragged objects links the others to it.
    children_.removeAll(parent);
    if (!parent || children_.isEmpty()) return true;
    doc->beginEdit(QObject::tr("Link"));
    int linked = 0;
    for (uint32_t c : children_)
      if (doc->scene().setParent(c, parent)) ++linked;
    doc->notifyStructureChanged();
    doc->endEdit();
    Node* p = doc->scene().findNode(parent);
    vp.editorState()->setPrompt(QObject::tr("Linked %1 object(s) to %2").arg(linked).arg(p ? QString::fromStdString(p->name) : QString()));
    return true;
  }
  void drawOverlay(Viewport&, QPainter& p) override {
    if (!dragging_) return;
    p.setPen(QPen(QColor(255, 255, 255), 1, Qt::DashLine));
    p.drawLine(from_, to_);
  }
  bool cancel() override {
    bool was = dragging_;
    dragging_ = false;
    return was;
  }
  QString prompt() const override { return QObject::tr("Select and Link: drag from the child to the parent"); }

 private:
  bool dragging_ = false;
  QVector<uint32_t> children_;
  uint32_t target_ = 0;
  QPoint from_, to_;
};

void unlinkSelection(MainWindow& w) {
  SceneDocument* doc = w.document();
  QVector<uint32_t> ids;
  for (Node* n : doc->selectedNodes())
    if (n->parentId && !n->groupMember) ids.push_back(n->id);
  if (ids.isEmpty()) return;
  doc->beginEdit(QObject::tr("Unlink"));
  for (uint32_t id : ids) doc->scene().setParent(id, 0);
  doc->notifyStructureChanged();
  doc->endEdit();
}

// ---------------------------------------------------------------------------------------------
// Installer

QIcon drawnIcon(const std::function<void(QPainter&)>& fn) {
  QPixmap pm(24, 24);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(QPen(QColor(220, 220, 220), 1.8));
  fn(p);
  return QIcon(pm);
}

void installTools(MainWindow& w) {
  window() = &w;
  w.editorState()->addDecorator(std::make_shared<GroupDecorator>());
  auto make = [&](const QString& id, const QString& text, const QKeySequence& key, std::function<void()> fn,
                  const QIcon& icon = QIcon()) {
    auto* a = new QAction(icon, text, &w);
    if (!key.isEmpty()) a->setShortcut(key);
    w.addAction(a);
    w.registerCommand(id, a);
    QObject::connect(a, &QAction::triggered, &w, [fn] { fn(); });
    return a;
  };
  MainWindow* win = &w;
  QIcon linkIcon = drawnIcon([](QPainter& p) {
    p.drawEllipse(QRectF(3, 8, 10, 8));
    p.drawEllipse(QRectF(11, 8, 10, 8));
  });
  QIcon unlinkIcon = drawnIcon([](QPainter& p) {
    p.drawEllipse(QRectF(2, 8, 9, 8));
    p.drawEllipse(QRectF(13, 8, 9, 8));
    p.setPen(QPen(QColor(230, 80, 70), 2));
    p.drawLine(QPointF(12, 4), QPointF(12, 20));
  });
  QIcon mirrorIcon = drawnIcon([](QPainter& p) {
    p.drawPolygon(QPolygonF({QPointF(3, 18), QPointF(10, 6), QPointF(10, 18)}));
    p.drawPolygon(QPolygonF({QPointF(21, 18), QPointF(14, 6), QPointF(14, 18)}));
  });
  QIcon alignIcon = drawnIcon([](QPainter& p) {
    p.drawRect(QRectF(4, 4, 8, 6));
    p.drawRect(QRectF(4, 14, 14, 6));
    p.setPen(QPen(QColor(255, 200, 80), 1.6));
    p.drawLine(QPointF(4, 2), QPointF(4, 22));
  });
  QAction* link = make("tools.selectAndLink", QObject::tr("Select and Link"), QKeySequence(),
                       [win] { win->editorState()->setViewportTool(std::make_shared<SelectAndLinkTool>()); }, linkIcon);
  QAction* unlink = make("tools.unlink", QObject::tr("Unlink Selection"), QKeySequence(), [win] { unlinkSelection(*win); }, unlinkIcon);
  QAction* mirror = make("edit.mirror", QObject::tr("Mirror..."), QKeySequence(), [win] {
    if (win->document()->selection().isEmpty()) {
      prompt(QObject::tr("Select objects to mirror"));
      return;
    }
    MirrorDialog dlg(win->document(), win);
    dlg.exec();
  }, mirrorIcon);
  QAction* array = make("tools.array", QObject::tr("Array..."), QKeySequence(), [win] {
    if (win->document()->selection().isEmpty()) {
      prompt(QObject::tr("Select objects to array"));
      return;
    }
    ArrayDialog dlg(win->document(), win);
    dlg.exec();
  });
  QAction* align = make("tools.align", QObject::tr("Align..."), QKeySequence("Alt+A"), [win] { startAlign(*win); }, alignIcon);
  QAction* quick = make("tools.quickAlign", QObject::tr("Quick Align"), QKeySequence("Shift+A"), [win] { startQuickAlign(*win); });
  QAction* normal = make("tools.normalAlign", QObject::tr("Normal Align..."), QKeySequence("Alt+N"), [win] {
    if (win->document()->selection().isEmpty()) {
      prompt(QObject::tr("Select the object to align first"));
      return;
    }
    win->editorState()->setViewportTool(std::make_shared<NormalAlignTool>(win->document()));
  });
  QAction* toView = make("tools.alignToView", QObject::tr("Align to View..."), QKeySequence(), [win] { alignToView(*win); });
  QAction* snap = make("tools.snapshot", QObject::tr("Snapshot..."), QKeySequence(), [win] { snapshot(*win); });
  QAction* spacing = make("tools.spacing", QObject::tr("Spacing Tool..."), QKeySequence("Shift+I"), [win] {
    auto* dlg = new SpacingDialog(*win);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
  });
  QMenu* tools = w.menuAt("Tools");
  tools->addSeparator();
  tools->addAction(mirror);
  tools->addAction(array);
  QMenu* alignMenu = w.menuAt("Tools/Align");
  alignMenu->addAction(align);
  alignMenu->addAction(quick);
  alignMenu->addAction(normal);
  alignMenu->addAction(toView);
  tools->addAction(snap);
  tools->addAction(spacing);
  QMenu* edit = w.menuAt("Edit");
  edit->addSeparator();
  edit->addAction(link);
  edit->addAction(unlink);
  QToolBar* tb = w.mainToolBar();
  tb->addSeparator();
  tb->addAction(link);
  tb->addAction(unlink);
  tb->addAction(mirror);
  tb->addAction(align);

  QMenu* group = w.menuAt("Group");
  group->addAction(make("group.group", QObject::tr("Group..."), QKeySequence(), [win] { groupSelection(*win); }));
  group->addAction(make("group.ungroup", QObject::tr("Ungroup"), QKeySequence(), [win] { ungroup(*win, false); }));
  group->addAction(make("group.open", QObject::tr("Open"), QKeySequence(), [win] { openGroups(*win, true, false); }));
  group->addAction(make("group.openRecursively", QObject::tr("Open Recursively"), QKeySequence(), [win] { openGroups(*win, true, true); }));
  group->addAction(make("group.close", QObject::tr("Close"), QKeySequence(), [win] { openGroups(*win, false, false); }));
  group->addAction(make("group.attach", QObject::tr("Attach"), QKeySequence(), [win] { attachToGroup(*win); }));
  group->addAction(make("group.detach", QObject::tr("Detach"), QKeySequence(), [win] { detachFromGroup(*win); }));
  group->addAction(make("group.explode", QObject::tr("Explode"), QKeySequence(), [win] { ungroup(*win, true); }));
}

// ---------------------------------------------------------------------------------------------
// Self tests: align, array count, mirror, group, link.

void testTools(MainWindow& w) {
  using testing::check;
  using testing::pump;
  SceneDocument* doc = w.document();
  EditorState* st = w.editorState();
  doc->beginEdit("Create");
  Node* a = doc->createNode("Box");
  Node* b = doc->createNode("Sphere");
  b->transform.position = Vec3(300, 120, 40);
  doc->endEdit();
  uint32_t ida = a->id, idb = b->id;

  // Align: box centered on the sphere (X, Y, Z center to center).
  doc->setSelection({ida});
  AlignDialog* dlg = new AlignDialog(doc, idb, &w);
  dlg->commit();
  delete dlg;
  Node* na = doc->scene().findNode(ida);
  Node* nb = doc->scene().findNode(idb);
  Vec3 ca = na->worldBounds().center(), cb = nb->worldBounds().center();
  check(length(ca - cb) < 1e-3f, "Align moves the box center onto the sphere center");
  doc->undoStack()->undo();
  pump(20);
  check(length(doc->scene().findNode(ida)->worldPosition()) < 1e-3f, "undo restores the aligned object");

  // Align through the pick tool and the dialog's OK (min X of box to max X of sphere).
  Viewport* top = w.viewports()->viewport(0);
  w.viewports()->setActive(top);
  top->zoomExtents(false);
  pump(30);
  doc->setSelection({ida});
  if (QAction* al = w.command("tools.align")) al->trigger();
  QPointF sp = testing::projectPoint(top, Vec3(300, 120, 40));
  top->simulateMouse(QEvent::MouseButtonPress, sp, Qt::LeftButton, Qt::LeftButton);
  top->simulateMouse(QEvent::MouseButtonRelease, sp, Qt::LeftButton, Qt::NoButton);
  pump(50);
  auto* open = static_cast<AlignDialog*>(w.findChild<QDialog*>("AlignDialog"));
  check(open != nullptr, "picking the target opens the Align dialog");
  if (open) {
    open->findChild<QRadioButton*>("align_cur0")->click();
    open->findChild<QRadioButton*>("align_tgt3")->click();
    open->findChild<QCheckBox*>("align_pos1")->setChecked(false);
    open->findChild<QCheckBox*>("align_pos2")->setChecked(false);
    pump(20);
    na = doc->scene().findNode(ida);
    check(std::fabs(na->worldBounds().lo.x - doc->scene().findNode(idb)->worldBounds().hi.x) < 1e-2f,
          "Align previews min X onto the target's max X");
    if (auto* bb = open->findChild<QDialogButtonBox*>()) bb->button(QDialogButtonBox::Ok)->click();
    pump(30);
  }
  na = doc->scene().findNode(ida);
  check(std::fabs(na->worldPosition().y) < 1e-3f, "Align leaves unchecked axes alone");
  doc->undoStack()->undo();
  pump(20);

  // Array: 10 copies in 1D, then 3 rows in 2D.
  doc->setSelection({ida});
  size_t before = doc->scene().nodes.size();
  ArraySettings s;
  s.move = Vec3(120, 0, 0);
  s.count[0] = 10;
  doc->beginEdit("Array");
  QVector<uint32_t> made = buildArray(doc, {ida}, s);
  doc->endEdit();
  check(made.size() == 9 && doc->scene().nodes.size() == before + 9, "1D array of 10 makes 9 copies");
  Node* last = doc->scene().findNode(made.back());
  check(last && std::fabs(last->worldPosition().x - 1080.0f) < 1e-3f, "array copies step by the increment");
  doc->undoStack()->undo();
  pump(20);
  s.dims = 2;
  s.count[1] = 3;
  s.row2 = Vec3(0, 150, 0);
  s.type = 1;
  doc->beginEdit("Array");
  made = buildArray(doc, {ida}, s);
  doc->endEdit();
  check(made.size() == 29, "2D array 10 x 3 makes 29 copies");
  Node* inst = doc->scene().findNode(made[0]);
  check(inst && inst->object == doc->scene().findNode(ida)->object, "Instance copies share the object");
  doc->undoStack()->undo();
  pump(20);

  // Array dialog: count spinner drives the total.
  doc->setSelection({ida});
  {
    ArrayDialog ad(doc, &w);
    ad.findChild<QSpinBox*>("array_count1")->setValue(4);
    ad.findChild<QDoubleSpinBox*>("array_MoveX")->setValue(150);
    ad.findChild<QPushButton*>("array_preview")->setChecked(true);
    pump(20);
    check(doc->scene().nodes.size() == before + 3, "Array preview shows the copies");
    if (auto* bb = ad.findChild<QDialogButtonBox*>()) bb->button(QDialogButtonBox::Ok)->click();
    pump(20);
    check(doc->scene().nodes.size() == before + 3, "Array OK keeps the copies");
  }
  doc->undoStack()->undo();
  pump(20);
  check(doc->scene().nodes.size() == before, "undo removes the array");

  // Mirror with a copy across X.
  doc->setSelection({idb});
  {
    MirrorDialog md(doc, &w);
    md.findChild<QRadioButton*>("mirror_clone1")->click();
    md.commit();
  }
  check(doc->scene().nodes.size() == before + 1, "Mirror with Copy adds a mirrored copy");
  Node* mirrored = doc->firstSelected();
  check(mirrored && mirrored->id != idb && mirrored->transform.scale.x < 0, "the copy is mirrored on X");
  doc->undoStack()->undo();
  pump(20);

  // Group, open/close and selection redirect.
  doc->setSelection({ida, idb});
  doc->beginEdit("Group");
  {
    Node* head = doc->scene().createNode("GroupHead");
    head->groupHead = true;
    head->name = "TestGroup";
    for (uint32_t id : {ida, idb}) {
      doc->scene().setParent(id, head->id);
      doc->scene().findNode(id)->groupMember = true;
    }
    doc->notifyStructureChanged();
  }
  doc->endEdit();
  Node* head = doc->scene().findNodeByName("TestGroup");
  doc->setSelection({ida});
  check(head && doc->selection().size() == 1 && doc->selection()[0] == head->id, "clicking a member of a closed group selects the group");
  doc->setSelection({head->id});
  openGroups(w, true, false);
  doc->setSelection({ida});
  check(doc->selection().size() == 1 && doc->selection()[0] == ida, "members of an open group can be selected");
  openGroups(w, false, false);
  check(!doc->scene().findNodeByName("TestGroup")->groupOpen, "Close closes the group");
  doc->setSelection({doc->scene().findNodeByName("TestGroup")->id});
  ungroup(w, false);
  check(!doc->scene().findNodeByName("TestGroup") && !doc->scene().findNode(ida)->groupMember, "Ungroup releases the members");

  // Select and Link by dragging from the box to the sphere.
  top->zoomExtents(false);
  pump(30);
  st->setViewportTool(std::make_shared<SelectAndLinkTool>());
  QPointF from = testing::projectPoint(top, doc->scene().findNode(ida)->worldBounds().center());
  QPointF to = testing::projectPoint(top, doc->scene().findNode(idb)->worldBounds().center());
  top->simulateMouse(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
  for (int i = 1; i <= 5; ++i) top->simulateMouse(QEvent::MouseMove, from + (to - from) * (i / 5.0), Qt::NoButton, Qt::LeftButton);
  top->simulateMouse(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
  pump(20);
  check(doc->scene().findNode(ida)->parentId == idb, "Select and Link links the box to the sphere");
  st->setViewportTool(nullptr);
  doc->setSelection({ida});
  unlinkSelection(w);
  check(doc->scene().findNode(ida)->parentId == 0, "Unlink removes the link");
}

}  // namespace

VERAA_APP_EXTENSION(objectTools, 240, installTools);
VERAA_SELF_TEST(objectTools, testTools);

}  // namespace veraa
