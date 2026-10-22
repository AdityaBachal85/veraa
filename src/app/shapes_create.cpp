// Interactive creation of shapes (Line, Freehand, drag-to-size splines, Text, Section), TextPlus,
// AEC objects (doors, windows, stairs, railing, wall, foliage) and BlobMesh, plus the shared
// helpers declared in shapes_app.h.
#include <QApplication>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>

#include <cmath>

#include "aec.h"
#include "editorstate.h"
#include "glscene.h"
#include "mainwindow.h"
#include "shape2d.h"
#include "shapes.h"
#include "shapes_app.h"
#include "viewport.h"

namespace veraa {
namespace shapesapp {

std::function<bool(QWidget*, const QString&)> askYesNo = [](QWidget* parent, const QString& question) {
  return QMessageBox::question(parent, QCoreApplication::translate("Shapes", "Veraa"), question,
                               QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) == QMessageBox::Yes;
};

BezierShape evaluatedShape(const Node& n, int upTo) {
  ShapeObject* so = n.shapeObject();
  if (!so) return BezierShape();
  GeomState st;
  st.shape = so->buildShape();
  st.isShape = true;
  if (so->params.has("steps")) st.shapeSteps = so->params.getInt("steps");
  if (so->params.has("optimize")) st.shapeOptimize = so->params.getBool("optimize");
  int count = upTo < 0 ? static_cast<int>(n.modifiers.size()) : std::min(upTo, static_cast<int>(n.modifiers.size()));
  for (int i = 0; i < count; ++i)
    if (n.modifiers[i] && n.modifiers[i]->enabled) n.modifiers[i]->applyState(st);
  return st.isShape ? st.shape : BezierShape();
}

CompoundOperand captureOperand(const Node& picked, const Node& compoundNode, const CompoundRole& role) {
  CompoundOperand op;
  op.role = role.role;
  op.sourceId = picked.id;
  op.name = picked.name;
  op.isShape = role.shape;
  Mat4 xf = role.space == OperandSpace::Local ? Mat4()
                                              : compoundNode.objectWorldMatrix().inverse() * picked.objectWorldMatrix();
  if (role.shape) {
    op.shape = evaluatedShape(picked);
    op.shape.transform(xf);
  } else {
    op.mesh = picked.mesh();
    op.mesh.transform(xf);
  }
  return op;
}

bool hitNodePlane(const Viewport& vp, const Node& n, float px, float py, float z, Vec3& local) {
  Mat4 m = n.objectWorldMatrix();
  Mat4 inv = m.inverse();
  Ray r = vp.camera().rayAt(px, py);
  Vec3 o = inv.transformPoint(r.o), d = inv.transformDir(r.d);
  if (std::fabs(d.z) < 1e-9f) return false;
  float t = (z - o.z) / d.z;
  local = o + d * t;
  return true;
}

namespace {

class PickTool : public ViewportTool {
 public:
  PickTool(QString prompt, std::function<bool(const Node&)> accept, std::function<void(Node&)> picked, bool multiple)
      : prompt_(std::move(prompt)), accept_(std::move(accept)), picked_(std::move(picked)), multiple_(multiple) {}
  bool mousePress(Viewport& vp, QMouseEvent* e) override {
    if (e->button() == Qt::RightButton) {
      ending_ = true;  // ends on release, so the release doesn't open the quad menu
      return true;
    }
    if (e->button() != Qt::LeftButton) return false;
    uint32_t id = vp.pickNode(static_cast<float>(e->position().x()), static_cast<float>(e->position().y()));
    Node* n = id ? vp.document()->scene().findNode(id) : nullptr;
    if (n && accept_(*n)) {
      auto keep = vp.editorState()->viewportToolShared();  // stay alive while the callback runs
      picked_(*n);
      if (!multiple_) vp.editorState()->setViewportTool(nullptr);
    }
    return true;
  }
  bool mouseRelease(Viewport& vp, QMouseEvent* e) override {
    if (e->button() == Qt::RightButton && ending_) {
      ending_ = false;
      vp.editorState()->setViewportTool(nullptr);
      return true;
    }
    return e->button() == Qt::LeftButton;
  }
  QString prompt() const override { return prompt_; }

 private:
  bool ending_ = false;
  QString prompt_;
  std::function<bool(const Node&)> accept_;
  std::function<void(Node&)> picked_;
  bool multiple_;
};

}  // namespace

std::shared_ptr<ViewportTool> makePickTool(const QString& prompt, std::function<bool(const Node&)> accept,
                                           std::function<void(Node&)> picked, bool multiple) {
  return std::make_shared<PickTool>(prompt, std::move(accept), std::move(picked), multiple);
}

namespace {

QString tr(const char* s) { return QCoreApplication::translate("Shapes", s); }

// Creation frame of the viewport's construction plane.
Mat4 creationFrame(const Viewport& vp) { return Mat4::fromEulerDeg(vp.camera().creationRotation()); }

float wpp(const Viewport& vp, const Vec3& at) { return vp.camera().worldPerPixel(at); }

// ---------------------------------------------------------------------------------------------
// Line: click = corner knot, click-drag = Bezier knot, right-click ends, clicking the first knot
// asks to close the spline. Backspace removes the last knot; Shift keeps segments orthogonal.

class LineTool : public ViewportTool {
 public:
  explicit LineTool(QString type) : type_(std::move(type)) {}

  bool mousePress(Viewport& vp, QMouseEvent* e) override {
    float px = static_cast<float>(e->position().x()), py = static_cast<float>(e->position().y());
    SceneDocument* doc = vp.document();
    if (e->button() == Qt::RightButton) {
      if (!nodeId_) return false;  // not drawing: let the viewport leave creation mode
      finish(vp, false);
      return true;
    }
    if (e->button() != Qt::LeftButton) return false;
    Vec3 hit;
    if (!vp.hitConstructionPlane(px, py, hit)) return true;
    Node* n = node(doc);
    if (!n) {
      doc->beginEdit(tr("Create %1").arg(QString::fromStdString(objectDisplayName(type_.toStdString()))));
      n = doc->createNode(type_);
      if (!n) {
        doc->cancelEdit();
        return true;
      }
      nodeId_ = n->id;
      n->transform.position = hit;
      n->transform.rotation = vp.camera().creationRotation();
      knots_ = {cornerKnot(Vec3()), cornerKnot(Vec3())};
      doc->select(n->id, false);
    } else {
      Vec3 local = toLocal(*n, hit, e->modifiers() & Qt::ShiftModifier);
      // A click on the first knot closes the spline (after asking).
      float sx = 0, sy = 0;
      int fixed = static_cast<int>(knots_.size()) - 1;
      if (fixed >= 2 && vp.camera().project(n->objectWorldMatrix().transformPoint(knots_[0].p), sx, sy) &&
          std::hypot(sx - px, sy - py) < 8.0f) {
        if (askYesNo(&vp, tr("Close spline?"))) {
          knots_.pop_back();
          closed_ = true;
          finish(vp, true);
          return true;
        }
      }
      knots_.back() = cornerKnot(local);
      knots_.push_back(cornerKnot(local));
    }
    pressing_ = true;
    pressPos_ = e->position();
    pressIdx_ = static_cast<int>(knots_.size()) - 2;
    apply(doc);
    return true;
  }

  bool mouseMove(Viewport& vp, QMouseEvent* e) override {
    Node* n = node(vp.document());
    if (!n) return false;
    float px = static_cast<float>(e->position().x()), py = static_cast<float>(e->position().y());
    Vec3 hit;
    if (!vp.hitConstructionPlane(px, py, hit)) return true;
    bool shift = e->modifiers() & Qt::ShiftModifier;
    if (pressing_ && (e->buttons() & Qt::LeftButton)) {
      if ((e->position() - pressPos_).manhattanLength() > 3) {
        // Dragging pulls Bezier handles out of the knot just placed.
        Knot& k = knots_[pressIdx_];
        Vec3 out = toLocal(*n, hit, false);
        k.type = KnotType::Bezier;
        k.out = out;
        k.in = k.p * 2.0f - out;
        knots_.back() = cornerKnot(k.p);
        apply(vp.document());
      }
    } else {
      knots_.back() = cornerKnot(toLocal(*n, hit, shift));
      apply(vp.document());
    }
    return true;
  }

  bool mouseRelease(Viewport&, QMouseEvent* e) override {
    if (e->button() == Qt::LeftButton) pressing_ = false;
    return nodeId_ != 0;
  }

  bool keyPress(Viewport& vp, QKeyEvent* e) override {
    if (!nodeId_) return false;
    if (e->key() == Qt::Key_Backspace && knots_.size() > 2) {
      knots_.erase(knots_.end() - 2);
      apply(vp.document());
      return true;
    }
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
      finish(vp, false);
      return true;
    }
    return false;
  }

  void drawLines(Viewport& vp, LineBatch& lines) override {
    Node* n = node(vp.document());
    if (!n || knots_.empty()) return;
    Mat4 m = n->objectWorldMatrix();
    Vec3 p = m.transformPoint(knots_[0].p);
    float s = wpp(vp, p) * 4.0f;
    Vec3 r = vp.camera().right() * s, u = vp.camera().up() * s;
    QColor c(255, 220, 0);
    lines.line(p - r - u, p + r - u, c);
    lines.line(p + r - u, p + r + u, c);
    lines.line(p + r + u, p - r + u, c);
    lines.line(p - r + u, p - r - u, c);
  }

  bool cancel() override {
    if (!nodeId_ || !doc_) return false;
    doc_->cancelEdit();
    nodeId_ = 0;
    return true;
  }

  QString prompt() const override {
    return tr("Click to place corner vertices, drag for Bezier vertices; right-click to finish, click the first "
              "vertex to close");
  }

 private:
  Node* node(SceneDocument* doc) {
    doc_ = doc;
    return nodeId_ ? doc->scene().findNode(nodeId_) : nullptr;
  }

  Vec3 toLocal(const Node& n, const Vec3& world, bool ortho) const {
    Vec3 l = n.objectWorldMatrix().inverse().transformPoint(world);
    l.z = 0;
    if (ortho && knots_.size() >= 2) {
      const Vec3& prev = knots_[knots_.size() - 2].p;
      Vec3 d = l - prev;
      if (std::fabs(d.x) > std::fabs(d.y)) l.y = prev.y;
      else l.x = prev.x;
    }
    return l;
  }

  void apply(SceneDocument* doc) {
    Node* n = node(doc);
    if (!n) return;
    auto* obj = dynamic_cast<EditableSplineObject*>(n->object.get());
    if (!obj) return;
    Spline s;
    s.knots = knots_;
    s.closed = closed_;
    s.updateAutoHandles();
    obj->shape.splines = {s};
    obj->touch();
    doc->notifyNodeChanged(n, ChangeGeometry);
  }

  void finish(Viewport& vp, bool closedNow) {
    SceneDocument* doc = vp.document();
    if (!closedNow && knots_.size() > 1) knots_.pop_back();  // drop the rubber-band knot
    if (knots_.size() >= 2) {
      apply(doc);
      doc->endEdit();
    } else {
      doc->cancelEdit();
    }
    nodeId_ = 0;
    knots_.clear();
    vp.editorState()->setViewportTool(nullptr);
  }

  QString type_;
  SceneDocument* doc_ = nullptr;
  uint32_t nodeId_ = 0;
  std::vector<Knot> knots_;
  bool closed_ = false;
  bool pressing_ = false;
  QPointF pressPos_;
  int pressIdx_ = 0;
};

// ---------------------------------------------------------------------------------------------
// Freehand: drag to draw; the stroke becomes smooth knots (closed when it ends at its start).

void simplify(const std::vector<Vec3>& pts, size_t a, size_t b, float tol, std::vector<bool>& keep) {
  if (b <= a + 1) return;
  Vec3 ab = pts[b] - pts[a];
  float len2 = length2(ab);
  float best = -1;
  size_t idx = a;
  for (size_t i = a + 1; i < b; ++i) {
    Vec3 ap = pts[i] - pts[a];
    float d = len2 > 0 ? length(ap - ab * clampf(dot(ap, ab) / len2, 0.0f, 1.0f)) : length(ap);
    if (d > best) {
      best = d;
      idx = i;
    }
  }
  if (best > tol) {
    keep[idx] = true;
    simplify(pts, a, idx, tol, keep);
    simplify(pts, idx, b, tol, keep);
  }
}

class FreehandTool : public ViewportTool {
 public:
  bool mousePress(Viewport& vp, QMouseEvent* e) override {
    if (e->button() != Qt::LeftButton) return false;
    Vec3 hit;
    if (!vp.hitConstructionPlane(static_cast<float>(e->position().x()), static_cast<float>(e->position().y()), hit))
      return true;
    doc_ = vp.document();
    doc_->beginEdit(tr("Create Freehand"));
    Node* n = doc_->createNode("Freehand");
    if (!n) {
      doc_->cancelEdit();
      return true;
    }
    nodeId_ = n->id;
    n->transform.position = hit;
    n->transform.rotation = vp.camera().creationRotation();
    pts_ = {Vec3()};
    step_ = wpp(vp, hit) * 4.0f;
    doc_->select(n->id, false);
    return true;
  }
  bool mouseMove(Viewport& vp, QMouseEvent* e) override {
    Node* n = nodeId_ ? vp.document()->scene().findNode(nodeId_) : nullptr;
    if (!n || !(e->buttons() & Qt::LeftButton)) return nodeId_ != 0;
    Vec3 hit;
    if (!vp.hitConstructionPlane(static_cast<float>(e->position().x()), static_cast<float>(e->position().y()), hit))
      return true;
    Vec3 l = n->objectWorldMatrix().inverse().transformPoint(hit);
    l.z = 0;
    if (length(l - pts_.back()) >= step_) {
      pts_.push_back(l);
      apply(*n, false);
    }
    return true;
  }
  bool mouseRelease(Viewport& vp, QMouseEvent* e) override {
    if (e->button() != Qt::LeftButton || !nodeId_) return false;
    Node* n = vp.document()->scene().findNode(nodeId_);
    if (n && pts_.size() >= 3) {
      apply(*n, true);
      doc_->endEdit();
    } else {
      doc_->cancelEdit();
    }
    nodeId_ = 0;
    vp.editorState()->setViewportTool(nullptr);
    return true;
  }
  bool cancel() override {
    if (!nodeId_) return false;
    doc_->cancelEdit();
    nodeId_ = 0;
    return true;
  }
  QString prompt() const override { return tr("Drag to draw a freehand spline"); }

 private:
  void apply(Node& n, bool final) {
    auto* obj = dynamic_cast<EditableSplineObject*>(n.object.get());
    if (!obj) return;
    std::vector<Vec3> pts = pts_;
    bool closed = final && pts.size() > 3 && length(pts.front() - pts.back()) < step_ * 3.0f;
    if (closed) pts.pop_back();
    std::vector<bool> keep(pts.size(), false);
    keep.front() = keep.back() = true;
    if (final) simplify(pts, 0, pts.size() - 1, step_ * 0.5f, keep);
    else std::fill(keep.begin(), keep.end(), true);
    Spline s;
    for (size_t i = 0; i < pts.size(); ++i)
      if (keep[i]) {
        Knot k = cornerKnot(pts[i]);
        k.type = final ? KnotType::Smooth : KnotType::Corner;
        s.knots.push_back(k);
      }
    s.closed = closed && s.knots.size() >= 3;
    s.updateAutoHandles();
    obj->shape.splines = {s};
    obj->touch();
    doc_->notifyNodeChanged(&n, ChangeGeometry);
  }

  SceneDocument* doc_ = nullptr;
  uint32_t nodeId_ = 0;
  std::vector<Vec3> pts_;
  float step_ = 1.0f;
};

// ---------------------------------------------------------------------------------------------
// Wall: click points (right-click ends, clicking the first point asks to close the wall).

class WallTool : public ViewportTool {
 public:
  bool mousePress(Viewport& vp, QMouseEvent* e) override {
    SceneDocument* doc = vp.document();
    doc_ = doc;
    float px = static_cast<float>(e->position().x()), py = static_cast<float>(e->position().y());
    if (e->button() == Qt::RightButton) {
      if (!nodeId_) return false;
      finish(vp, false);
      return true;
    }
    if (e->button() != Qt::LeftButton) return false;
    Vec3 hit;
    if (!vp.hitConstructionPlane(px, py, hit)) return true;
    Node* n = nodeId_ ? doc->scene().findNode(nodeId_) : nullptr;
    if (!n) {
      doc->beginEdit(tr("Create Wall"));
      n = doc->createNode("Wall");
      if (!n) {
        doc->cancelEdit();
        return true;
      }
      nodeId_ = n->id;
      n->transform.position = hit;
      n->transform.rotation = vp.camera().creationRotation();
      pts_ = {Vec2(), Vec2()};
      doc->select(n->id, false);
    } else {
      Vec3 l = local(*n, hit, e->modifiers() & Qt::ShiftModifier);
      float sx = 0, sy = 0;
      int fixed = static_cast<int>(pts_.size()) - 1;
      if (fixed >= 3 &&
          vp.camera().project(n->objectWorldMatrix().transformPoint(Vec3(pts_[0].x, pts_[0].y, 0)), sx, sy) &&
          std::hypot(sx - px, sy - py) < 8.0f && askYesNo(&vp, tr("Close wall?"))) {
        pts_.pop_back();
        closed_ = true;
        finish(vp, true);
        return true;
      }
      pts_.back() = Vec2(l.x, l.y);
      pts_.emplace_back(l.x, l.y);
    }
    apply();
    return true;
  }
  bool mouseMove(Viewport& vp, QMouseEvent* e) override {
    Node* n = nodeId_ ? vp.document()->scene().findNode(nodeId_) : nullptr;
    if (!n) return false;
    Vec3 hit;
    if (!vp.hitConstructionPlane(static_cast<float>(e->position().x()), static_cast<float>(e->position().y()), hit))
      return true;
    Vec3 l = local(*n, hit, e->modifiers() & Qt::ShiftModifier);
    pts_.back() = Vec2(l.x, l.y);
    apply();
    return true;
  }
  bool mouseRelease(Viewport&, QMouseEvent* e) override { return nodeId_ != 0 && e->button() == Qt::LeftButton; }
  bool mouseDoubleClick(Viewport& vp, QMouseEvent* e) override {
    if (!nodeId_ || e->button() != Qt::LeftButton) return false;
    finish(vp, false);
    return true;
  }
  bool cancel() override {
    if (!nodeId_) return false;
    doc_->cancelEdit();
    nodeId_ = 0;
    return true;
  }
  QString prompt() const override {
    return tr("Click to place wall points; right-click or double-click to finish, click the first point to close");
  }

 private:
  Vec3 local(const Node& n, const Vec3& world, bool ortho) const {
    Vec3 l = n.objectWorldMatrix().inverse().transformPoint(world);
    if (ortho && pts_.size() >= 2) {
      Vec2 prev = pts_[pts_.size() - 2];
      if (std::fabs(l.x - prev.x) > std::fabs(l.y - prev.y)) l.y = prev.y;
      else l.x = prev.x;
    }
    return l;
  }
  void apply() {
    Node* n = nodeId_ ? doc_->scene().findNode(nodeId_) : nullptr;
    auto* w = n ? dynamic_cast<WallObject*>(n->object.get()) : nullptr;
    if (!w) return;
    // Skip the rubber-band point while it sits on the previous one.
    w->points = pts_;
    if (w->points.size() >= 2 && w->points.back() == w->points[w->points.size() - 2]) w->points.pop_back();
    if (w->points.size() < 2) w->points = {Vec2(), Vec2(0.01f, 0)};
    w->closed = closed_;
    w->touch();
    doc_->notifyNodeChanged(n, ChangeGeometry);
  }
  void finish(Viewport& vp, bool closedNow) {
    if (!closedNow && pts_.size() > 1) pts_.pop_back();
    // Drop repeated points (double-click lands twice).
    std::vector<Vec2> clean;
    for (const auto& p : pts_)
      if (clean.empty() || std::hypot(p.x - clean.back().x, p.y - clean.back().y) > 1e-3f) clean.push_back(p);
    pts_ = clean;
    if (pts_.size() >= 2) {
      apply();
      doc_->endEdit();
    } else {
      doc_->cancelEdit();
    }
    nodeId_ = 0;
    vp.editorState()->setViewportTool(nullptr);
  }

  SceneDocument* doc_ = nullptr;
  uint32_t nodeId_ = 0;
  std::vector<Vec2> pts_;
  bool closed_ = false;
};

// ---------------------------------------------------------------------------------------------
// Staged drag creation: stage 0 is a drag on the construction plane, later stages follow the
// mouse (up/down) and advance with a click.

struct StageInput {
  Node* node = nullptr;
  ParamBlock* p = nullptr;
  Vec3 p0;       // first point (world)
  Mat4 frame;    // creation frame rotation
  Vec3 d;        // stage 0: current point in the creation frame, relative to p0
  Vec3 dStage0;  // stage 0 result (kept for later stages)
  float dy = 0;  // later stages: world units moved up since the stage started
  Vec3 hitD;     // later stages: current plane point in the creation frame (relative to p0)
};

using StageFn = std::function<void(StageInput&)>;

struct StagedSpec {
  std::vector<StageFn> stages;
  std::vector<QString> prompts;
  bool clickOnly = false;           // created by a single click with default sizes
  std::function<void(Node&)> init;  // after creation, before stage 0
  std::function<void(SceneDocument*, Node&)> done;
};

// Rotates the node about its creation-frame Z so its local +X follows direction d.
void yawTo(StageInput& in, const Vec3& d, float offsetDeg = 0) {
  if (d.x * d.x + d.y * d.y < 1e-12f) return;
  float a = std::atan2(d.y, d.x) + radians(offsetDeg);
  in.node->transform.rotation = eulerDegFromMatrix(in.frame * Mat4::rotateZ(a));
}

void centerOn(StageInput& in, const Vec3& d) {
  in.node->transform.position = in.p0 + in.frame.transformDir(Vec3(d.x * 0.5f, d.y * 0.5f, 0));
}

class StagedTool : public ViewportTool {
 public:
  StagedTool(QString type, StagedSpec spec) : type_(std::move(type)), spec_(std::move(spec)) {}

  bool mousePress(Viewport& vp, QMouseEvent* e) override {
    doc_ = vp.document();
    float px = static_cast<float>(e->position().x()), py = static_cast<float>(e->position().y());
    if (e->button() == Qt::RightButton) {
      if (stage_ < 0) return false;
      cancel();
      vp.editorState()->setViewportTool(nullptr);
      return true;
    }
    if (e->button() != Qt::LeftButton) return false;
    if (stage_ >= 1) {
      advance(vp, py);
      return true;
    }
    Vec3 hit;
    if (!vp.hitConstructionPlane(px, py, hit)) return true;
    doc_->beginEdit(tr("Create %1").arg(QString::fromStdString(objectDisplayName(type_.toStdString()))));
    Node* n = doc_->createNode(type_);
    if (!n) {
      doc_->cancelEdit();
      return true;
    }
    nodeId_ = n->id;
    in_ = StageInput();
    in_.p0 = hit;
    in_.frame = creationFrame(vp);
    n->transform.position = hit;
    n->transform.rotation = vp.camera().creationRotation();
    if (spec_.init) spec_.init(*n);
    doc_->select(n->id, false);
    doc_->notifyNodeChanged(n, ChangeAll);
    pressPos_ = e->position();
    if (spec_.clickOnly) {
      finish(vp);
      return true;
    }
    stage_ = 0;
    if (!spec_.prompts.empty()) vp.editorState()->setPrompt(spec_.prompts[0]);
    return true;
  }

  bool mouseMove(Viewport& vp, QMouseEvent* e) override {
    if (stage_ < 0) return false;
    Node* n = doc_->scene().findNode(nodeId_);
    if (!n) return true;
    float px = static_cast<float>(e->position().x()), py = static_cast<float>(e->position().y());
    in_.node = n;
    in_.p = &n->object->params;
    Vec3 hit;
    bool onPlane = vp.hitConstructionPlane(px, py, hit);
    if (stage_ == 0) {
      if (!(e->buttons() & Qt::LeftButton) || !onPlane) return true;
      in_.d = in_.frame.transposed().transformDir(hit - in_.p0);
      in_.dStage0 = in_.d;
    } else {
      in_.dy = (stageY_ - py) * wpp(vp, n->transform.position);
      if (onPlane) in_.hitD = in_.frame.transposed().transformDir(hit - in_.p0);
    }
    spec_.stages[stage_](in_);
    n->object->touch();
    doc_->notifyNodeChanged(n, ChangeGeometry | ChangeTransform);
    return true;
  }

  bool mouseRelease(Viewport& vp, QMouseEvent* e) override {
    if (stage_ != 0 || e->button() != Qt::LeftButton) return stage_ >= 0;
    if ((e->position() - pressPos_).manhattanLength() < 3) {
      cancel();  // a click without dragging creates nothing
      vp.editorState()->setViewportTool(nullptr);
      return true;
    }
    advance(vp, static_cast<float>(e->position().y()));
    return true;
  }

  bool cancel() override {
    if (stage_ < 0 || !doc_) return false;
    doc_->cancelEdit();
    stage_ = -1;
    nodeId_ = 0;
    return true;
  }

  QString prompt() const override { return spec_.prompts.empty() ? QString() : spec_.prompts[0]; }

 private:
  void advance(Viewport& vp, float py) {
    ++stage_;
    if (stage_ >= static_cast<int>(spec_.stages.size())) {
      finish(vp);
      return;
    }
    stageY_ = py;
    if (stage_ < static_cast<int>(spec_.prompts.size())) vp.editorState()->setPrompt(spec_.prompts[stage_]);
  }

  void finish(Viewport& vp) {
    Node* n = doc_->scene().findNode(nodeId_);
    if (n && spec_.done) spec_.done(doc_, *n);
    doc_->endEdit();
    stage_ = -1;
    nodeId_ = 0;
    vp.editorState()->setPrompt(tr("Click and drag to create another %1, right-click to stop")
                                    .arg(QString::fromStdString(objectDisplayName(type_.toStdString()))));
    vp.editorState()->setViewportTool(nullptr);
  }

  QString type_;
  StagedSpec spec_;
  SceneDocument* doc_ = nullptr;
  uint32_t nodeId_ = 0;
  int stage_ = -1;
  float stageY_ = 0;
  QPointF pressPos_;
  StageInput in_;
};

float r2d(const Vec3& d) { return std::sqrt(d.x * d.x + d.y * d.y); }

void setMin(ParamBlock& p, const char* key, float v) { p.setNumber(key, std::max(v, 0.01f)); }

StageFn cornerRect(const char* lengthKey = "length", const char* widthKey = "width") {
  return [lengthKey, widthKey](StageInput& in) {
    setMin(*in.p, widthKey, std::fabs(in.d.x));
    setMin(*in.p, lengthKey, std::fabs(in.d.y));
    centerOn(in, in.d);
  };
}

StageFn radiusFrom(const char* key) {
  return [key](StageInput& in) { setMin(*in.p, key, r2d(in.d)); };
}

StageFn thicknessStage() {
  return [](StageInput& in) {
    float limit = std::min(in.p->getFloat("length"), in.p->getFloat("width")) * 0.49f;
    in.p->setNumber("thickness", clampf(std::fabs(in.dy), 0.01f, std::max(0.01f, limit)));
  };
}

// Arc: drag the chord, then move to bend it through a third point.
StageFn arcStage0() {
  return [](StageInput& in) {
    float r = r2d(in.d) * 0.5f;
    setMin(*in.p, "radius", r);
    float a = degrees(std::atan2(in.d.y, in.d.x));
    in.p->setNumber("from", a + 180.0f);
    in.p->setNumber("to", a + 360.0f);
    centerOn(in, in.d);
  };
}

StageFn arcStage1() {
  return [](StageInput& in) {
    Vec2 a(0, 0), b(in.dStage0.x, in.dStage0.y), c(in.hitD.x, in.hitD.y);
    float D = 2 * (a.x * (b.y - c.y) + b.x * (c.y - a.y) + c.x * (a.y - b.y));
    if (std::fabs(D) < 1e-6f) return;
    float b2 = b.x * b.x + b.y * b.y, c2 = c.x * c.x + c.y * c.y;
    Vec2 o((b2 * (c.y - a.y) + c2 * (a.y - b.y)) / D, (b2 * (a.x - c.x) + c2 * (b.x - a.x)) / D);
    float r = std::hypot(o.x, o.y);
    auto ang = [&](const Vec2& p) { return degrees(std::atan2(p.y - o.y, p.x - o.x)); };
    float ta = ang(a), tb = ang(b), tc = ang(c);
    auto mod360 = [](float x) { return std::fmod(std::fmod(x, 360.0f) + 360.0f, 360.0f); };
    bool ccw = mod360(tc - ta) < mod360(tb - ta);
    float from = ccw ? ta : tb, to = ccw ? ta + mod360(tb - ta) : tb + mod360(ta - tb);
    setMin(*in.p, "radius", r);
    in.p->setNumber("from", from);
    in.p->setNumber("to", to);
    in.node->transform.position = in.p0 + in.frame.transformDir(Vec3(o.x, o.y, 0));
  };
}

void registerSpec(const char* type, StagedSpec spec) {
  QString t = type;
  registerCreationTool(t, [t, spec] { return std::make_shared<StagedTool>(t, spec); });
}

void zeroKeys(Node& n, std::initializer_list<const char*> keys) {
  for (const char* k : keys)
    if (n.object->params.has(k)) n.object->params.setNumber(k, 0.01);
}

void installCreationTools(MainWindow& w) {
  // Font folders next to the executable (packaged builds) and in the source tree.
  QString app = QCoreApplication::applicationDirPath();
  for (const QString& d : {app + "/fonts", app + "/assets/fonts", app + "/../assets/fonts", app + "/../../assets/fonts"})
    addFontSearchPath(d.toStdString());

  registerCreationTool("Line", [] { return std::make_shared<LineTool>("Line"); });
  registerCreationTool("Freehand", [] { return std::make_shared<FreehandTool>(); });
  registerCreationTool("Wall", [] { return std::make_shared<WallTool>(); });

  const QString dragRect = tr("Drag to set the length and width");
  const QString dragRadius = tr("Drag to set the radius");
  const QString moveThick = tr("Move the mouse to set the thickness, click to finish");
  auto rectInit = [](Node& n) { zeroKeys(n, {"length", "width"}); };
  registerSpec("Rectangle", {{cornerRect()}, {dragRect}, false, rectInit, {}});
  registerSpec("Ellipse", {{cornerRect()}, {dragRect}, false, rectInit, {}});
  auto radiusInit = [](Node& n) { zeroKeys(n, {"radius", "radius1"}); };
  registerSpec("Circle", {{radiusFrom("radius")}, {dragRadius}, false, radiusInit, {}});
  registerSpec("NGon", {{radiusFrom("radius")}, {dragRadius}, false, radiusInit, {}});
  auto secondRadius = [](StageInput& in) {
    in.p->setNumber("radius2", std::max(0.01f, in.p->getFloat("radius1") + in.dy));
  };
  registerSpec("Donut",
               {{radiusFrom("radius1"), secondRadius},
                {dragRadius, tr("Move the mouse to set Radius 2, click to finish")},
                false,
                [](Node& n) { zeroKeys(n, {"radius1", "radius2"}); },
                {}});
  registerSpec("Star",
               {{[](StageInput& in) {
                  setMin(*in.p, "radius1", r2d(in.d));
                  in.p->setNumber("radius2", in.p->getFloat("radius1") * 0.5f);
                },
                 secondRadius},
                {dragRadius, tr("Move the mouse to set Radius 2, click to finish")},
                false,
                radiusInit,
                {}});
  registerSpec("Arc", {{arcStage0(), arcStage1()},
                       {tr("Drag from one end of the arc to the other"),
                        tr("Move the mouse to bend the arc, click to finish")},
                       false,
                       [](Node& n) { zeroKeys(n, {"radius"}); },
                       {}});
  registerSpec("Helix", {{[](StageInput& in) {
                            setMin(*in.p, "radius1", r2d(in.d));
                            setMin(*in.p, "radius2", r2d(in.d));
                          },
                          [](StageInput& in) { in.p->setNumber("height", in.dy); },
                          [](StageInput& in) {
                            in.p->setNumber("radius2", std::max(0.0f, in.p->getFloat("radius1") + in.dy));
                          }},
                         {dragRadius, tr("Move the mouse to set the height, click"),
                          tr("Move the mouse to set Radius 2, click to finish")},
                         false,
                         [](Node& n) {
                           zeroKeys(n, {"radius1", "radius2"});
                           n.object->params.setNumber("height", 0);
                         },
                         {}});
  registerSpec("Egg", {{[](StageInput& in) {
                          setMin(*in.p, "length", r2d(in.d) * 2.0f);
                          setMin(*in.p, "width", r2d(in.d) * 1.5f);
                          in.p->setNumber("thickness", std::min(in.p->getFloat("thickness"), r2d(in.d) * 0.2f));
                        }},
                       {tr("Drag to set the size of the egg")},
                       false,
                       [](Node& n) { zeroKeys(n, {"length", "width"}); },
                       {}});
  for (const char* t : {"WalledRectangle", "Channel", "AngleShape", "Tee", "WideFlange"})
    registerSpec(t, {{cornerRect(), thicknessStage()},
                     {dragRect, moveThick},
                     false,
                     [](Node& n) {
                       zeroKeys(n, {"length", "width"});
                       n.object->params.setNumber("thickness", 0.01);
                     },
                     {}});
  registerSpec("Text", {{}, {}, true, {}, {}});
  registerSpec("Section", {{cornerRect()},
                           {tr("Drag to set the size of the section plane")},
                           false,
                           rectInit,
                           [](SceneDocument* doc, Node& n) { updateSection(doc, &n); }});
  registerSpec("TextPlus", {{}, {}, true, {}, {}});
  registerSpec("Foliage", {{}, {}, true, {}, {}});
  registerSpec("BlobMesh", {{}, {}, true, {}, {}});

  // Doors and windows: drag the width, then move for the depth and the height.
  auto widthAlong = [](StageInput& in) {
    setMin(*in.p, "width", r2d(in.d));
    yawTo(in, in.d);
  };
  auto depthStage = [](StageInput& in) { setMin(*in.p, "depth", std::fabs(in.dy)); };
  auto heightStage = [](StageInput& in) { setMin(*in.p, "height", in.dy); };
  const QString dragWidth = tr("Drag to set the width");
  const QString moveDepth = tr("Move the mouse to set the depth, click");
  const QString moveHeight = tr("Move the mouse to set the height, click to finish");
  for (const char* t : {"PivotDoor", "SlidingDoor", "BiFoldDoor", "AwningWindow", "CasementWindow", "FixedWindow",
                        "PivotedWindow", "ProjectedWindow", "SlidingWindow"})
    registerSpec(t, {{widthAlong, depthStage, heightStage},
                     {dragWidth, moveDepth, moveHeight},
                     false,
                     [](Node& n) { zeroKeys(n, {"width", "depth", "height"}); },
                     {}});
  registerSpec("Railing", {{[](StageInput& in) {
                              setMin(*in.p, "length", r2d(in.d));
                              yawTo(in, in.d);
                            },
                            [](StageInput& in) { setMin(*in.p, "height", in.dy); }},
                           {tr("Drag to set the railing length"), moveHeight},
                           false,
                           [](Node& n) { zeroKeys(n, {"length", "height"}); },
                           {}});
  // Stairs climb along local +Y: the drag sets the run, then width and height.
  auto runAlong = [](const char* key, const char* key2) {
    return [key, key2](StageInput& in) {
      setMin(*in.p, key, r2d(in.d));
      if (key2) setMin(*in.p, key2, r2d(in.d));
      yawTo(in, in.d, -90.0f);
    };
  };
  auto widthStage = [](StageInput& in) { setMin(*in.p, "width", std::fabs(in.dy)); };
  const QString moveWidth = tr("Move the mouse to set the width, click");
  registerSpec("StraightStair", {{runAlong("length", nullptr), widthStage, heightStage},
                                 {tr("Drag to set the length"), moveWidth, moveHeight},
                                 false,
                                 [](Node& n) { zeroKeys(n, {"length", "width", "height"}); },
                                 {}});
  for (const char* t : {"LTypeStair", "UTypeStair"})
    registerSpec(t, {{runAlong("length1", "length2"), widthStage, heightStage},
                     {tr("Drag to set the length of the flights"), moveWidth, moveHeight},
                     false,
                     [](Node& n) { zeroKeys(n, {"length1", "length2", "width", "height"}); },
                     {}});
  registerSpec("SpiralStair", {{radiusFrom("radius"), heightStage},
                               {dragRadius, moveHeight},
                               false,
                               [](Node& n) { zeroKeys(n, {"radius", "height"}); },
                               {}});
  (void)w;
}

}  // namespace
}  // namespace shapesapp

VERAA_APP_EXTENSION(shapesCreation, 200, shapesapp::installCreationTools);

}  // namespace veraa
