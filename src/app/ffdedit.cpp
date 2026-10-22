// FFD modifiers in the Modify panel: the lattice display, the Control Points sub-object
// tool (click/region selection and move/rotate/scale with the gizmo) and Reset.
#include <QApplication>
#include <QFormLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QUndoStack>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <set>

#include "commandpanel.h"
#include "document.h"
#include "editorstate.h"
#include "mainwindow.h"
#include "meshmods.h"
#include "paramform.h"
#include "polyedit.h"
#include "selftest.h"
#include "viewport.h"
#include "viewportpanel.h"

namespace veraa {

namespace {

struct FfdTarget {
  SceneDocument* doc = nullptr;
  EditorState* state = nullptr;
  uint32_t nodeId = 0;
  int stackIndex = -1;

  Node* node() const { return doc ? doc->scene().findNode(nodeId) : nullptr; }
  FfdModifier* mod() const {
    Node* n = node();
    if (!n || stackIndex < 0 || stackIndex >= static_cast<int>(n->modifiers.size())) return nullptr;
    return dynamic_cast<FfdModifier*>(n->modifiers[stackIndex].get());
  }
  bool valid() const { return mod() != nullptr; }
  Mat4 objectToWorld() const {
    Node* n = node();
    return n ? n->objectWorldMatrix() : Mat4();
  }
  // Makes sure the lattice frame is current.
  void evaluate() const {
    if (Node* n = node()) n->mesh();
  }
  bool same(const FfdTarget& o) const { return nodeId == o.nodeId && stackIndex == o.stackIndex; }
};

FfdTarget& shownLattice() {
  static FfdTarget t;
  return t;
}

std::set<int>& selectedPoints() {
  static std::set<int> s;
  return s;
}

std::vector<QPointF> projectPoints(Viewport& vp, const FfdTarget& t, std::vector<char>* ok = nullptr) {
  FfdModifier* m = t.mod();
  std::vector<QPointF> out;
  if (!m) return out;
  Mat4 ow = t.objectToWorld();
  int n = m->pointCount();
  out.resize(n);
  if (ok) ok->assign(n, 0);
  for (int i = 0; i < n; ++i) {
    float x, y;
    if (vp.camera().project(ow.transformPoint(m->point(i)), x, y)) {
      out[i] = QPointF(x, y);
      if (ok) (*ok)[i] = 1;
    }
  }
  return out;
}

class FfdDecorator : public ViewportDecorator {
 public:
  void drawLines(Viewport& vp, LineBatch& lines) override {
    const FfdTarget& t = shownLattice();
    FfdModifier* m = t.mod();
    if (!m || vp.editorState()->creating()) return;
    t.evaluate();
    Mat4 ow = t.objectToWorld();
    QColor c(255, 140, 20);
    for (const auto& [a, b] : m->latticeEdges()) lines.line(ow.transformPoint(m->point(a)), ow.transformPoint(m->point(b)), c);
  }
  void drawOverlay(Viewport& vp, QPainter& p) override {
    const FfdTarget& t = shownLattice();
    FfdModifier* m = t.mod();
    if (!m || vp.editorState()->creating()) return;
    std::vector<char> ok;
    auto pts = projectPoints(vp, t, &ok);
    for (size_t i = 0; i < pts.size(); ++i) {
      if (!ok[i]) continue;
      bool sel = selectedPoints().count(static_cast<int>(i)) > 0;
      p.fillRect(QRectF(pts[i].x() - 3, pts[i].y() - 3, 6, 6), sel ? QColor(255, 40, 40) : QColor(20, 20, 20));
      p.setPen(QColor(255, 160, 40));
      p.drawRect(QRectF(pts[i].x() - 3, pts[i].y() - 3, 6, 6));
    }
  }
};

class FfdTool : public ViewportTool {
 public:
  explicit FfdTool(const FfdTarget& t) : target_(t) {}
  const FfdTarget& target() const { return target_; }

  int pickPoint(Viewport& vp, float px, float py) const {
    std::vector<char> ok;
    auto pts = projectPoints(vp, target_, &ok);
    int best = -1;
    float bestD = 9.0f;
    for (size_t i = 0; i < pts.size(); ++i) {
      if (!ok[i]) continue;
      float d = static_cast<float>(std::hypot(pts[i].x() - px, pts[i].y() - py));
      if (d < bestD) {
        bestD = d;
        best = static_cast<int>(i);
      }
    }
    return best;
  }

  Vec3 selectionCenter() const {
    FfdModifier* m = target_.mod();
    Mat4 ow = target_.objectToWorld();
    Aabb b;
    if (m)
      for (int i : selectedPoints())
        if (i < m->pointCount()) b.expand(ow.transformPoint(m->point(i)));
    return b.valid() ? b.center() : Vec3();
  }

  bool gizmoShown(Viewport& vp) const {
    return !selectedPoints().empty() && vp.editorState()->tool() != TransformTool::Select && !vp.editorState()->creating();
  }

  void setupGizmo(Viewport& vp) {
    Vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    gizmo_.setup(vp.camera(), vp.editorState()->tool(), dragging_ ? center_ : selectionCenter(), axes);
  }

  void begin(Viewport& vp, GizmoHandle h, float px, float py) {
    FfdModifier* m = target_.mod();
    if (!m) return;
    setupGizmo(vp);
    center_ = gizmo_.origin;
    start_.clear();
    for (int i = 0; i < m->pointCount(); ++i) start_.push_back(m->offset(i));
    gizmoDrag_.begin(vp.camera(), gizmo_, h, px, py);
    if (vp.editorState()->tool() != TransformTool::Rotate) vp.editorState()->setConstraint(h);
    dragging_ = true;
  }

  void update(Viewport& vp, float px, float py) {
    FfdModifier* m = target_.mod();
    if (!m) return;
    EditorState* st = vp.editorState();
    const ViewCamera& cam = vp.camera();
    Mat4 w;
    TransformTool tool = st->tool();
    if (tool == TransformTool::Move) {
      Vec3 d = gizmoDrag_.translation(cam, px, py);
      Vec3 snapped;
      if (st->snapPoint(vp, px, py, center_ + d, snapped)) d = snapped - center_;
      w = Mat4::translate(d);
    } else if (tool == TransformTool::Rotate) {
      Vec3 axis;
      float angle = st->snapAngle(gizmoDrag_.rotation(cam, px, py, axis));
      w = Mat4::translate(center_) * Mat4::rotateAxis(axis, angle) * Mat4::translate(-center_);
    } else {
      Vec3 f = st->snapPercent(gizmoDrag_.scaling(cam, px, py));
      w = Mat4::translate(center_) * Mat4::scale(f) * Mat4::translate(-center_);
    }
    Mat4 ow = target_.objectToWorld();
    Mat4 local = ow.inverse() * w * ow;
    for (int i : selectedPoints()) {
      if (i >= static_cast<int>(start_.size())) continue;
      Vec3 p0 = m->restPoint(i) + start_[i];
      m->setOffset(i, local.transformPoint(p0) - m->restPoint(i));
    }
    target_.doc->notifyNodesChanged({target_.nodeId}, ChangeGeometry);
  }

  void end(bool commit) {
    dragging_ = false;
    FfdModifier* m = target_.mod();
    if (!m) return;
    std::vector<Vec3> finalOffsets;
    for (int i = 0; i < m->pointCount(); ++i) finalOffsets.push_back(m->offset(i));
    for (size_t i = 0; i < start_.size(); ++i) m->setOffset(static_cast<int>(i), start_[i]);
    if (commit) {
      target_.doc->beginEdit(QObject::tr("Move Control Points"));
      if (FfdModifier* again = target_.mod())
        for (size_t i = 0; i < finalOffsets.size(); ++i) again->setOffset(static_cast<int>(i), finalOffsets[i]);
      target_.doc->notifyNodesChanged({target_.nodeId}, ChangeGeometry);
      target_.doc->endEdit();
    } else {
      target_.doc->notifyNodesChanged({target_.nodeId}, ChangeGeometry);
    }
  }

  bool mousePress(Viewport& vp, QMouseEvent* e) override {
    if (vp.editorState()->creating() || !target_.valid()) return false;
    if (e->button() == Qt::RightButton && dragging_) {
      end(false);
      return true;
    }
    if (e->button() != Qt::LeftButton) return false;
    float px = static_cast<float>(e->position().x()), py = static_cast<float>(e->position().y());
    press_ = cur_ = e->position().toPoint();
    mods_ = e->modifiers();
    bool plain = !(mods_ & (Qt::ControlModifier | Qt::AltModifier));
    EditorState* st = vp.editorState();
    if (plain && st->tool() != TransformTool::Select) {
      if (gizmoShown(vp)) {
        setupGizmo(vp);
        GizmoHandle h = gizmo_.hitTest(vp.camera(), px, py);
        if (h != GizmoHandle::None) {
          begin(vp, h, px, py);
          return true;
        }
      }
      int i = pickPoint(vp, px, py);
      if (i >= 0) {
        if (!selectedPoints().count(i)) selectedPoints() = {i};
        GizmoHandle h = st->constraint();
        if (st->tool() == TransformTool::Rotate)
          h = handleAxisMask(h) == 1 ? GizmoHandle::X : handleAxisMask(h) == 2 ? GizmoHandle::Y : GizmoHandle::Z;
        begin(vp, h, px, py);
        return true;
      }
    }
    pressed_ = true;
    region_ = false;
    return true;
  }

  bool mouseMove(Viewport& vp, QMouseEvent* e) override {
    if (!target_.valid()) return false;
    float px = static_cast<float>(e->position().x()), py = static_cast<float>(e->position().y());
    if (dragging_) {
      update(vp, px, py);
      return true;
    }
    if (pressed_) {
      cur_ = e->position().toPoint();
      if ((cur_ - press_).manhattanLength() > 3) region_ = true;
      return true;
    }
    if (gizmoShown(vp) && e->buttons() == Qt::NoButton) {
      setupGizmo(vp);
      GizmoHandle h = gizmo_.hitTest(vp.camera(), px, py);
      if (h != hot_) {
        hot_ = h;
        return true;
      }
    }
    return false;
  }

  bool mouseRelease(Viewport& vp, QMouseEvent* e) override {
    if (e->button() != Qt::LeftButton) return false;
    if (dragging_) {
      end(true);
      return true;
    }
    if (!pressed_) return false;
    pressed_ = false;
    bool ctrl = mods_ & Qt::ControlModifier, alt = mods_ & Qt::AltModifier;
    std::set<int> hits;
    if (region_) {
      std::vector<char> ok;
      auto pts = projectPoints(vp, target_, &ok);
      QRectF r = QRectF(QRect(press_, e->position().toPoint()).normalized());
      for (size_t i = 0; i < pts.size(); ++i)
        if (ok[i] && r.contains(pts[i])) hits.insert(static_cast<int>(i));
    } else {
      int i = pickPoint(vp, static_cast<float>(press_.x()), static_cast<float>(press_.y()));
      if (i >= 0) hits.insert(i);
    }
    std::set<int>& sel = selectedPoints();
    if (alt) {
      for (int i : hits) sel.erase(i);
    } else if (ctrl) {
      for (int i : hits)
        if (!region_ && sel.count(i)) sel.erase(i);
        else sel.insert(i);
    } else {
      sel = hits;
    }
    region_ = false;
    if (target_.state) target_.state->requestRedraw();
    return true;
  }

  void drawOverlay(Viewport& vp, QPainter& p) override {
    if (gizmoShown(vp)) {
      setupGizmo(vp);
      drawGizmoOverlay(p, vp.camera(), gizmo_, dragging_ ? gizmoDrag_.handle() : hot_);
    }
    if (pressed_ && region_) {
      p.setPen(QPen(QColor(255, 255, 255), 1, Qt::DashLine));
      p.drawRect(QRect(press_, cur_).normalized());
    }
    p.setPen(QColor(255, 200, 80));
    p.drawText(QRectF(8, 24, 300, 18), Qt::AlignLeft | Qt::AlignVCenter, QObject::tr("Sub-object: Control Points"));
  }

  bool cancel() override {
    if (dragging_) {
      end(false);
      return true;
    }
    return false;
  }

  QString prompt() const override { return QObject::tr("Control Points: click or drag to select, drag to transform"); }

 private:
  FfdTarget target_;
  Gizmo gizmo_;
  GizmoDrag gizmoDrag_;
  GizmoHandle hot_ = GizmoHandle::None;
  bool dragging_ = false, pressed_ = false, region_ = false;
  QPoint press_, cur_;
  Qt::KeyboardModifiers mods_;
  Vec3 center_;
  std::vector<Vec3> start_;
};

class FfdPanel;
QPointer<FfdPanel>& currentFfdPanel() {
  static QPointer<FfdPanel> p;
  return p;
}

class FfdPanel : public QWidget {
 public:
  FfdPanel(const ModifyPanelContext& ctx, QWidget* parent) : QWidget(parent), ctx_(ctx) {
    setObjectName("FfdPanel");
    currentFfdPanel() = this;
    FfdTarget t{ctx.doc, ctx.state, ctx.nodeId, ctx.stackIndex};
    if (!shownLattice().same(t)) selectedPoints().clear();
    shownLattice() = t;
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    auto* r = new Rollout(tr("FFD Parameters"), this);
    v->addWidget(r);
    points_ = new QPushButton(tr("Control Points"), r->body());
    points_->setObjectName("ffd_controlPoints");
    points_->setCheckable(true);
    points_->setToolTip(tr("Sub-object level: select and transform control points (1)"));
    r->form()->addRow(points_);
    auto* reset = new QPushButton(tr("Reset"), r->body());
    reset->setObjectName("ffd_reset");
    r->form()->addRow(reset);
    auto* all = new QPushButton(tr("Select All Points"), r->body());
    all->setObjectName("ffd_selectAll");
    r->form()->addRow(all);
    info_ = new QLabel(r->body());
    r->form()->addRow(info_);
    connect(points_, &QPushButton::toggled, this, [this](bool on) { setPointsMode(on); });
    connect(reset, &QPushButton::clicked, this, [this] {
      FfdTarget tt = target();
      if (!tt.valid()) return;
      ctx_.doc->beginEdit(tr("Reset FFD"));
      tt.mod()->resetPoints();
      tt.mod()->touch();
      ctx_.doc->notifyNodesChanged({ctx_.nodeId}, ChangeGeometry);
      ctx_.doc->endEdit();
    });
    connect(all, &QPushButton::clicked, this, [this] {
      FfdTarget tt = target();
      if (!tt.valid()) return;
      selectedPoints().clear();
      for (int i = 0; i < tt.mod()->pointCount(); ++i) selectedPoints().insert(i);
      setPointsMode(true);
      ctx_.state->requestRedraw();
    });
    connect(ctx_.state, &EditorState::viewportToolChanged, this, [this] {
      QSignalBlocker b(points_);
      auto tool = std::dynamic_pointer_cast<FfdTool>(ctx_.state->viewportToolShared());
      points_->setChecked(tool && tool->target().same(target()));
    });
    connect(ctx_.doc, &SceneDocument::nodesChanged, this, [this](const QVector<uint32_t>& ids, int) {
      if (ids.contains(ctx_.nodeId)) refresh();
    });
    // Coming back to an FFD that was in Control Points mode keeps the mode.
    auto tool = std::dynamic_pointer_cast<FfdTool>(ctx_.state->viewportToolShared());
    if (tool && tool->target().same(t)) {
      QSignalBlocker b(points_);
      points_->setChecked(true);
    }
    refresh();
    ctx_.state->requestRedraw();
  }

  ~FfdPanel() override {
    // A newer panel (rebuilt after undo, or another FFD) takes over the lattice display.
    if (currentFfdPanel() != this && !currentFfdPanel().isNull()) return;
    FfdTarget t = target();
    if (shownLattice().same(t)) shownLattice() = FfdTarget();
    auto tool = std::dynamic_pointer_cast<FfdTool>(ctx_.state->viewportToolShared());
    if (tool && tool->target().same(t)) ctx_.state->setViewportTool(nullptr);
  }

  FfdTarget target() const { return FfdTarget{ctx_.doc, ctx_.state, ctx_.nodeId, ctx_.stackIndex}; }

  void setPointsMode(bool on) {
    auto tool = std::dynamic_pointer_cast<FfdTool>(ctx_.state->viewportToolShared());
    bool active = tool && tool->target().same(target());
    if (on && !active) ctx_.state->setViewportTool(std::make_shared<FfdTool>(target()));
    if (!on && active) ctx_.state->setViewportTool(nullptr);
    QSignalBlocker b(points_);
    points_->setChecked(on);
  }

  void refresh() {
    FfdTarget t = target();
    if (!t.valid()) return;
    int a, b, c;
    t.mod()->dims(a, b, c);
    info_->setText(tr("Lattice %1 x %2 x %3, %4 selected").arg(a).arg(b).arg(c).arg(selectedPoints().size()));
  }

 private:
  ModifyPanelContext ctx_;
  QPushButton* points_ = nullptr;
  QLabel* info_ = nullptr;
};

void installFfd(MainWindow& w) {
  w.editorState()->addDecorator(std::make_shared<FfdDecorator>());
  auto factory = [](const ModifyPanelContext& ctx, QWidget* parent) -> QWidget* { return new FfdPanel(ctx, parent); };
  for (const char* t : {"FFD2", "FFD3", "FFD4", "FFDBox", "FFDCyl"}) registerModifyRollouts(t, factory);
}

// ---------------------------------------------------------------------------------------------
// Self test: FFD point move through the UI.

void testFfd(MainWindow& w) {
  using testing::check;
  using testing::pump;
  SceneDocument* doc = w.document();
  EditorState* st = w.editorState();
  Viewport* vp = w.viewports()->viewport(1);  // Front
  w.viewports()->setActive(vp);
  doc->beginEdit("Create Box");
  Node* box = doc->createNode("Box");
  box->object->params.setNumber("heightSegs", 4);
  box->invalidate();
  doc->endEdit();
  uint32_t id = box->id;
  doc->setSelection({id});
  w.commandPanel()->addModifier("FFD2");
  pump(50);
  box = doc->scene().findNode(id);
  check(box && box->modifiers.size() == 1 && dynamic_cast<FfdModifier*>(box->modifiers[0].get()), "FFD 2x2x2 added");
  QPushButton* cp = nullptr;
  for (QPushButton* b : w.findChildren<QPushButton*>("ffd_controlPoints"))
    if (b->isVisible()) cp = b;
  check(cp != nullptr, "Control Points button");
  if (cp) cp->click();
  pump(30);
  check(std::dynamic_pointer_cast<FfdTool>(st->viewportToolShared()) != nullptr, "Control Points mode active");
  vp->zoomExtents(false);
  vp->camera().zoom(1.6f, vp->width() * 0.5f, vp->height() * 0.5f);
  pump(30);
  // Region-select the top four points (they overlap in the front view).
  Mat4 ow = box->objectWorldMatrix();
  QPointF a = testing::projectPoint(vp, ow.transformPoint(Vec3(-80, 0, 120)));
  QPointF b = testing::projectPoint(vp, ow.transformPoint(Vec3(80, 0, 80)));
  vp->simulateMouse(QEvent::MouseButtonPress, a, Qt::LeftButton, Qt::LeftButton);
  for (int i = 1; i <= 8; ++i) vp->simulateMouse(QEvent::MouseMove, a + (b - a) * (i / 8.0), Qt::NoButton, Qt::LeftButton);
  vp->simulateMouse(QEvent::MouseButtonRelease, b, Qt::LeftButton, Qt::NoButton);
  pump(30);
  check(selectedPoints().size() == 4, "region selects the four top control points");
  // Drag them up 40 units with the Z axis of the move gizmo.
  st->setTool(TransformTool::Move);
  pump(30);
  vp->updateCamera();
  Vec3 c = ow.transformPoint(Vec3(0, 0, 100));
  float wpp = vp->camera().worldPerPixel(c);
  QPointF h = testing::projectPoint(vp, c + Vec3(0, 0, wpp * 60.0f));
  QPointF to = h + QPointF(0, -40.0f / wpp);
  vp->simulateMouse(QEvent::MouseMove, h, Qt::NoButton, Qt::NoButton);
  vp->simulateMouse(QEvent::MouseButtonPress, h, Qt::LeftButton, Qt::LeftButton);
  for (int i = 1; i <= 8; ++i) vp->simulateMouse(QEvent::MouseMove, h + (to - h) * (i / 8.0), Qt::NoButton, Qt::LeftButton);
  vp->simulateMouse(QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
  pump(50);
  box = doc->scene().findNode(id);
  check(box && std::fabs(box->mesh().bounds().hi.z - 140.0f) < 1.0f, "moving FFD points raises the top by 40");
  if (QString dir = qEnvironmentVariable("VERAA_SCREENSHOT_DIR"); !dir.isEmpty())
    testing::saveGrab(&w, dir + "/ffd_01_points_moved.png");
  doc->undoStack()->undo();
  pump(50);
  box = doc->scene().findNode(id);
  check(box && std::fabs(box->mesh().bounds().hi.z - 100.0f) < 1e-3f, "undo restores the lattice");
  st->setViewportTool(nullptr);
  st->setTool(TransformTool::Select);
}

}  // namespace

VERAA_APP_EXTENSION(ffdEdit, 210, installFfd);
VERAA_SELF_TEST(ffdControlPoints, testFfd);

}  // namespace veraa
