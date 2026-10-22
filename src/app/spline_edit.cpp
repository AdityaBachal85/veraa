// Editable Spline / Edit Spline sub-object editing: Vertex, Segment and Spline levels (keys 1-3)
// with selection, moving (knots, segments, splines and Bezier handles) and the editing tools
// (knot types, Refine, Insert, Weld, Fuse, Connect, Break, Fillet, Chamfer, Make First, Cross
// Insert, Divide, Detach, Hide, Outline, Boolean, Mirror, Reverse, Close, Explode, Trim, Extend,
// Attach, Attach Mult., Delete).
#include <QButtonGroup>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <set>

#include "glscene.h"
#include "paramform.h"
#include "shape_modifiers.h"
#include "shapes_app.h"
#include "spline_ops.h"
#include "viewport.h"

namespace veraa {
namespace shapesapp {

namespace {

using splineops::KnotRef;
using splineops::SegmentRef;

QString tr(const char* s) { return QCoreApplication::translate("Shapes", s); }

enum class Mode { Select, Refine, Insert, Connect, CrossInsert, Trim, Extend, Boolean, Attach };

class SplineEditTool : public ViewportTool {
 public:
  SplineEditTool(SceneDocument* doc, EditorState* st, uint32_t nodeId, int stackIndex)
      : doc_(doc), st_(st), nodeId_(nodeId), stackIndex_(stackIndex) {}

  // ---- Session state ----
  int level() const { return level_; }
  Mode mode() const { return mode_; }
  uint32_t nodeId() const { return nodeId_; }
  int stackIndex() const { return stackIndex_; }
  void setLevel(int l) {
    if (l == level_) l = 0;  // pressing the active level again leaves sub-object mode
    level_ = l;
    mode_ = Mode::Select;
    if (level_ > 0) st_->setViewportTool(self_.lock());
    else if (st_->viewportTool() == this) st_->setViewportTool(nullptr);
    st_->setPrompt(prompt());
    changed();
  }
  void setMode(Mode m) {
    mode_ = mode_ == m ? Mode::Select : m;
    connectFrom_ = -1;
    if (level_ == 0) setLevel(mode_ == Mode::Trim || mode_ == Mode::Extend || mode_ == Mode::Boolean ? 3 : 1);
    if (mode_ != Mode::Select && st_->viewportTool() != this) st_->setViewportTool(self_.lock());
    st_->setPrompt(prompt());
    changed();
  }
  void setSelf(const std::shared_ptr<SplineEditTool>& s) { self_ = s; }
  int addListener(std::function<void()> f) {
    listeners_.emplace_back(++nextListener_, std::move(f));
    return nextListener_;
  }
  void removeListener(int id) {
    listeners_.erase(std::remove_if(listeners_.begin(), listeners_.end(), [id](const auto& p) { return p.first == id; }),
                     listeners_.end());
  }
  int liveRollouts = 0;

  Node* node() const { return doc_->scene().findNode(nodeId_); }

  // The shape being edited (read). For Edit Spline without edits this is its incoming shape.
  BezierShape currentShape() const {
    Node* n = node();
    if (!n) return BezierShape();
    if (stackIndex_ < 0) {
      auto* es = dynamic_cast<EditableSplineObject*>(n->object.get());
      return es ? es->shape : BezierShape();
    }
    auto* m = editMod(*n);
    if (!m) return BezierShape();
    return m->hasEdits ? m->shape : evaluatedShape(*n, stackIndex_);
  }

  // Runs an edit as one undo step. fn returns false to cancel.
  bool edit(const QString& label, const std::function<bool(BezierShape&)>& fn) {
    Node* n = node();
    if (!n) return false;
    doc_->beginEdit(label);
    BezierShape* s = writable(*n);
    if (!s || !fn(*s)) {
      doc_->cancelEdit();
      return false;
    }
    touch(*n);
    doc_->endEdit();
    validateSelection();
    changed();
    return true;
  }

  // ---- Selection ----
  std::set<KnotRef> knots;
  std::set<SegmentRef> segs;
  std::set<int> splines;
  std::set<SegmentRef> hidden;
  float weldThreshold = 0.1f;
  int booleanOp = 0;  // shape2d::BoolOp: 0 union, 1 subtraction, 2 intersection

  std::vector<KnotRef> selKnots() const { return {knots.begin(), knots.end()}; }
  std::vector<SegmentRef> selSegs() const { return {segs.begin(), segs.end()}; }
  std::vector<int> selSplines() const { return {splines.begin(), splines.end()}; }
  void clearSelection() {
    knots.clear();
    segs.clear();
    splines.clear();
  }
  void validateSelection() {
    BezierShape s = currentShape();
    int ns = static_cast<int>(s.splines.size());
    auto validKnot = [&](const KnotRef& k) {
      return k.spline >= 0 && k.spline < ns && k.knot >= 0 && k.knot < static_cast<int>(s.splines[k.spline].knots.size());
    };
    auto validSeg = [&](const SegmentRef& k) {
      return k.spline >= 0 && k.spline < ns && k.knot >= 0 && k.knot < s.splines[k.spline].segmentCount();
    };
    for (auto it = knots.begin(); it != knots.end();) it = validKnot(*it) ? std::next(it) : knots.erase(it);
    for (auto it = segs.begin(); it != segs.end();) it = validSeg(*it) ? std::next(it) : segs.erase(it);
    for (auto it = hidden.begin(); it != hidden.end();) it = validSeg(*it) ? std::next(it) : hidden.erase(it);
    for (auto it = splines.begin(); it != splines.end();) it = (*it >= 0 && *it < ns) ? std::next(it) : splines.erase(it);
  }

  // ---- ViewportTool ----
  bool mousePress(Viewport& vp, QMouseEvent* e) override;
  bool mouseMove(Viewport& vp, QMouseEvent* e) override;
  bool mouseRelease(Viewport& vp, QMouseEvent* e) override;
  bool keyPress(Viewport& vp, QKeyEvent* e) override;
  void drawLines(Viewport& vp, LineBatch& lines) override;
  void drawOverlay(Viewport& vp, QPainter& p) override;
  bool cancel() override;
  QString prompt() const override;

  void changed() {
    auto copy = listeners_;
    for (auto& [id, f] : copy) f();
    st_->requestRedraw();
  }

 private:
  EditSplineModifier* editMod(Node& n) const {
    if (stackIndex_ < 0 || stackIndex_ >= static_cast<int>(n.modifiers.size())) return nullptr;
    return dynamic_cast<EditSplineModifier*>(n.modifiers[stackIndex_].get());
  }
  BezierShape* writable(Node& n) {
    if (stackIndex_ < 0) {
      auto* es = dynamic_cast<EditableSplineObject*>(n.object.get());
      return es ? &es->shape : nullptr;
    }
    auto* m = editMod(n);
    if (!m) return nullptr;
    if (!m->hasEdits) {
      m->shape = evaluatedShape(n, stackIndex_);
      m->hasEdits = true;
    }
    return &m->shape;
  }
  void touch(Node& n) {
    if (stackIndex_ < 0) n.object->touch();
    else if (auto* m = editMod(n)) m->touch();
    doc_->notifyNodeChanged(&n, ChangeGeometry);
  }

  // Screen-space picking.
  bool screen(const Viewport& vp, const Mat4& m, const Vec3& local, float& x, float& y) const {
    return vp.camera().project(m.transformPoint(local), x, y);
  }
  float segmentScreenDistance(const Viewport& vp, const Mat4& m, const Spline& sp, int seg, float px, float py,
                              float* tOut) const {
    float best = 1e30f, bestT = 0;
    const int N = 16;
    float ax = 0, ay = 0;
    bool aok = screen(vp, m, sp.evaluate(seg, 0), ax, ay);
    for (int i = 1; i <= N; ++i) {
      float t1 = static_cast<float>(i) / N;
      float bx = 0, by = 0;
      bool bok = screen(vp, m, sp.evaluate(seg, t1), bx, by);
      if (aok && bok) {
        float vx = bx - ax, vy = by - ay, l2 = vx * vx + vy * vy;
        float u = l2 > 0 ? clampf(((px - ax) * vx + (py - ay) * vy) / l2, 0.0f, 1.0f) : 0.0f;
        float d = std::hypot(ax + vx * u - px, ay + vy * u - py);
        if (d < best) {
          best = d;
          bestT = (static_cast<float>(i - 1) + u) / N;
        }
      }
      ax = bx;
      ay = by;
      aok = bok;
    }
    if (tOut) *tOut = bestT;
    return best;
  }
  bool pickKnot(const Viewport& vp, const BezierShape& s, float px, float py, KnotRef& out) const {
    Mat4 m = node()->objectWorldMatrix();
    float best = 8.0f;
    bool found = false;
    for (int si = 0; si < static_cast<int>(s.splines.size()); ++si)
      for (int k = 0; k < static_cast<int>(s.splines[si].knots.size()); ++k) {
        float x, y;
        if (!screen(vp, m, s.splines[si].knots[k].p, x, y)) continue;
        float d = std::hypot(x - px, y - py);
        if (d < best) {
          best = d;
          out = {si, k};
          found = true;
        }
      }
    return found;
  }
  bool pickSegment(const Viewport& vp, const BezierShape& s, float px, float py, SegmentRef& out, float* t,
                   float tolerance = 6.0f) const {
    Mat4 m = node()->objectWorldMatrix();
    float best = tolerance;
    bool found = false;
    for (int si = 0; si < static_cast<int>(s.splines.size()); ++si)
      for (int g = 0; g < s.splines[si].segmentCount(); ++g) {
        if (hidden.count({si, g})) continue;
        float tt = 0;
        float d = segmentScreenDistance(vp, m, s.splines[si], g, px, py, &tt);
        if (d < best) {
          best = d;
          out = {si, g};
          if (t) *t = tt;
          found = true;
        }
      }
    return found;
  }
  // Handle (in/out) of a selected Bezier knot under the cursor: returns knot and which (0 in, 1 out).
  bool pickHandle(const Viewport& vp, const BezierShape& s, float px, float py, KnotRef& k, int& which) const {
    Mat4 m = node()->objectWorldMatrix();
    for (const KnotRef& r : knots) {
      if (r.spline >= static_cast<int>(s.splines.size()) || r.knot >= static_cast<int>(s.splines[r.spline].knots.size()))
        continue;
      const Knot& kn = s.splines[r.spline].knots[r.knot];
      if (kn.type != KnotType::Bezier && kn.type != KnotType::BezierCorner) continue;
      for (int h = 0; h < 2; ++h) {
        float x, y;
        if (!screen(vp, m, h ? kn.out : kn.in, x, y)) continue;
        if (std::hypot(x - px, y - py) < 7.0f) {
          k = r;
          which = h;
          return true;
        }
      }
    }
    return false;
  }

  void selectAt(const BezierShape& s, const Viewport& vp, float px, float py, Qt::KeyboardModifiers mods, bool& hit);
  void regionSelect(const Viewport& vp, const QRect& r, Qt::KeyboardModifiers mods);
  void applyDrag(const Viewport& vp, float px, float py);

  SceneDocument* doc_;
  EditorState* st_;
  uint32_t nodeId_;
  int stackIndex_;
  std::weak_ptr<SplineEditTool> self_;
  int level_ = 0;
  Mode mode_ = Mode::Select;
  std::vector<std::pair<int, std::function<void()>>> listeners_;
  int nextListener_ = 0;

  // Drag state.
  enum class Drag { None, Move, Handle, Region } drag_ = Drag::None;
  bool dragMoved_ = false;
  QPointF pressPos_, curPos_;
  Qt::KeyboardModifiers pressMods_;
  BezierShape dragOrig_;
  Vec3 dragStartLocal_;
  float dragZ_ = 0;
  KnotRef handleKnot_;
  int handleWhich_ = 0;
  int connectFrom_ = -1;  // Connect: first end knot (spline index); knot in connectKnot_
  KnotRef connectKnot_;
};

void SplineEditTool::selectAt(const BezierShape& s, const Viewport& vp, float px, float py, Qt::KeyboardModifiers mods,
                              bool& hit) {
  bool ctrl = mods & Qt::ControlModifier, alt = mods & Qt::AltModifier;
  auto applySel = [&](auto& set, const auto& item) {
    if (alt) set.erase(item);
    else if (ctrl) {
      if (set.count(item)) set.erase(item);
      else set.insert(item);
    } else if (!set.count(item)) {
      set.clear();
      set.insert(item);
    }
  };
  hit = false;
  if (level_ == 1) {
    KnotRef k;
    if (pickKnot(vp, s, px, py, k)) {
      applySel(knots, k);
      hit = true;
    }
  } else if (level_ == 2) {
    SegmentRef g;
    if (pickSegment(vp, s, px, py, g, nullptr)) {
      applySel(segs, g);
      hit = true;
    }
  } else if (level_ == 3) {
    SegmentRef g;
    if (pickSegment(vp, s, px, py, g, nullptr)) {
      applySel(splines, g.spline);
      hit = true;
    }
  }
}

void SplineEditTool::regionSelect(const Viewport& vp, const QRect& r, Qt::KeyboardModifiers mods) {
  bool ctrl = mods & Qt::ControlModifier, alt = mods & Qt::AltModifier;
  BezierShape s = currentShape();
  Mat4 m = node()->objectWorldMatrix();
  auto inside = [&](const Vec3& local) {
    float x, y;
    return screen(vp, m, local, x, y) && r.contains(QPoint(static_cast<int>(x), static_cast<int>(y)));
  };
  auto update = [&](auto& set, const auto& items) {
    if (!ctrl && !alt) set.clear();
    for (const auto& it : items) {
      if (alt) set.erase(it);
      else set.insert(it);
    }
  };
  if (level_ == 1) {
    std::vector<KnotRef> in;
    for (int si = 0; si < static_cast<int>(s.splines.size()); ++si)
      for (int k = 0; k < static_cast<int>(s.splines[si].knots.size()); ++k)
        if (inside(s.splines[si].knots[k].p)) in.push_back({si, k});
    update(knots, in);
  } else if (level_ == 2) {
    std::vector<SegmentRef> in;
    for (int si = 0; si < static_cast<int>(s.splines.size()); ++si)
      for (int g = 0; g < s.splines[si].segmentCount(); ++g)
        if (!hidden.count({si, g}) && inside(s.splines[si].evaluate(g, 0.5f))) in.push_back({si, g});
    update(segs, in);
  } else if (level_ == 3) {
    std::vector<int> in;
    for (int si = 0; si < static_cast<int>(s.splines.size()); ++si)
      for (const auto& k : s.splines[si].knots)
        if (inside(k.p)) {
          in.push_back(si);
          break;
        }
    update(splines, in);
  }
}

bool SplineEditTool::mousePress(Viewport& vp, QMouseEvent* e) {
  Node* n = node();
  if (!n) {
    st_->setViewportTool(nullptr);
    return false;
  }
  if (e->button() == Qt::RightButton) {
    if (mode_ != Mode::Select) {
      setMode(mode_);  // toggles back to Select
      return true;
    }
    return false;  // quad menu
  }
  if (e->button() != Qt::LeftButton || level_ == 0) return false;
  float px = static_cast<float>(e->position().x()), py = static_cast<float>(e->position().y());
  BezierShape s = currentShape();
  validateSelection();
  Mat4 M = n->objectWorldMatrix();
  pressPos_ = curPos_ = e->position();
  pressMods_ = e->modifiers();
  Vec3 local;
  switch (mode_) {
    case Mode::Refine:
    case Mode::Insert: {
      SegmentRef g;
      float t = 0;
      if (!pickSegment(vp, s, px, py, g, &t, 8.0f)) return true;
      KnotRef added;
      bool ok = edit(mode_ == Mode::Refine ? tr("Refine") : tr("Insert"), [&](BezierShape& w) {
        added = splineops::refine(w, g, clampf(t, 0.02f, 0.98f));
        return true;
      });
      if (ok && mode_ == Mode::Insert) {
        // The inserted vertex follows the mouse until the button is released.
        knots = {added};
        doc_->beginEdit(tr("Move Vertex"));
        Node* nn = node();
        BezierShape* w = nn ? writable(*nn) : nullptr;
        if (w) dragOrig_ = *w;
        dragZ_ = dragOrig_.splines[added.spline].knots[added.knot].p.z;
        hitNodePlane(vp, *nn, px, py, dragZ_, dragStartLocal_);
        drag_ = Drag::Move;
        dragMoved_ = true;
      } else if (ok) {
        knots = {added};
      }
      return true;
    }
    case Mode::Connect: {
      KnotRef k;
      if (!pickKnot(vp, s, px, py, k)) return true;
      const Spline& sp = s.splines[k.spline];
      bool isEnd = !sp.closed && (k.knot == 0 || k.knot == static_cast<int>(sp.knots.size()) - 1);
      if (!isEnd) return true;
      if (connectFrom_ < 0) {
        connectFrom_ = k.spline;
        connectKnot_ = k;
        st_->setPrompt(tr("Click another end vertex to connect to"));
      } else {
        KnotRef a = connectKnot_;
        connectFrom_ = -1;
        if (edit(tr("Connect"), [&](BezierShape& w) { return splineops::connect(w, a, k); })) clearSelection();
        st_->setPrompt(prompt());
      }
      st_->requestRedraw();
      return true;
    }
    case Mode::CrossInsert: {
      // The two splines nearest to the click.
      std::vector<std::pair<float, int>> near;
      for (int si = 0; si < static_cast<int>(s.splines.size()); ++si) {
        float best = 1e30f;
        for (int g = 0; g < s.splines[si].segmentCount(); ++g)
          best = std::min(best, segmentScreenDistance(vp, M, s.splines[si], g, px, py, nullptr));
        near.emplace_back(best, si);
      }
      std::sort(near.begin(), near.end());
      if (near.size() >= 2 && near[1].first < 12.0f) {
        int a = near[0].second, b = near[1].second;
        int count = 0;
        edit(tr("Cross Insert"), [&](BezierShape& w) {
          count = splineops::crossInsert(w, a, b);
          return count > 0;
        });
        st_->setPrompt(tr("%1 crossing(s) inserted").arg(count));
      }
      return true;
    }
    case Mode::Trim:
    case Mode::Extend: {
      SegmentRef g;
      if (!pickSegment(vp, s, px, py, g, nullptr, 8.0f) || !hitNodePlane(vp, *n, px, py, 0, local)) return true;
      bool ext = mode_ == Mode::Extend;
      edit(ext ? tr("Extend") : tr("Trim"),
           [&](BezierShape& w) { return ext ? splineops::extend(w, g.spline, local) : splineops::trim(w, g.spline, local); });
      clearSelection();
      return true;
    }
    case Mode::Boolean: {
      SegmentRef g;
      if (splines.size() != 1 || !pickSegment(vp, s, px, py, g, nullptr, 8.0f)) return true;
      int target = *splines.begin();
      if (g.spline == target) return true;
      edit(tr("Spline Boolean"), [&](BezierShape& w) {
        return splineops::booleanSplines(w, target, {g.spline}, static_cast<shape2d::BoolOp>(booleanOp));
      });
      clearSelection();
      setMode(Mode::Boolean);  // one operand per Boolean press
      return true;
    }
    case Mode::Attach: {
      uint32_t id = vp.pickNode(px, py);
      Node* other = id ? doc_->scene().findNode(id) : nullptr;
      if (!other || other == n || other->kind() != ObjectKind::Shape || !other->isShapeResult()) return true;
      BezierShape add = evaluatedShape(*other);
      add.transform(M.inverse() * other->objectWorldMatrix());
      uint32_t gone = other->id;
      edit(tr("Attach"), [&](BezierShape& w) {
        for (auto& sp : add.splines) w.splines.push_back(sp);
        doc_->scene().removeNode(gone);
        doc_->notifyStructureChanged();
        return true;
      });
      return true;
    }
    case Mode::Select:
      break;
  }
  // Bezier handles of selected knots come first.
  if (level_ == 1 && pickHandle(vp, s, px, py, handleKnot_, handleWhich_)) {
    doc_->beginEdit(tr("Move Handle"));
    BezierShape* w = writable(*n);
    dragOrig_ = w ? *w : s;
    dragZ_ = dragOrig_.splines[handleKnot_.spline].knots[handleKnot_.knot].p.z;
    hitNodePlane(vp, *n, px, py, dragZ_, dragStartLocal_);
    drag_ = Drag::Handle;
    dragMoved_ = false;
    return true;
  }
  bool hit = false;
  selectAt(s, vp, px, py, e->modifiers(), hit);
  changed();
  if (hit && !(e->modifiers() & (Qt::ControlModifier | Qt::AltModifier))) {
    doc_->beginEdit(level_ == 1 ? tr("Move Vertex") : level_ == 2 ? tr("Move Segment") : tr("Move Spline"));
    BezierShape* w = writable(*n);
    dragOrig_ = w ? *w : s;
    dragZ_ = 0;
    if (level_ == 1 && !knots.empty()) {
      KnotRef k = *knots.begin();
      dragZ_ = dragOrig_.splines[k.spline].knots[k.knot].p.z;
    }
    hitNodePlane(vp, *n, px, py, dragZ_, dragStartLocal_);
    drag_ = Drag::Move;
    dragMoved_ = false;
    return true;
  }
  if (!hit) drag_ = Drag::Region;
  return true;
}

void SplineEditTool::applyDrag(const Viewport& vp, float px, float py) {
  Node* n = node();
  if (!n) return;
  Vec3 local;
  if (!hitNodePlane(vp, *n, px, py, dragZ_, local)) return;
  Vec3 delta = local - dragStartLocal_;
  delta.z = 0;
  BezierShape* w = writable(*n);
  if (!w) return;
  *w = dragOrig_;
  Mat4 M = n->objectWorldMatrix();
  if (drag_ == Drag::Handle) {
    Knot& k = w->splines[handleKnot_.spline].knots[handleKnot_.knot];
    const Knot& o = dragOrig_.splines[handleKnot_.spline].knots[handleKnot_.knot];
    Vec3 h = (handleWhich_ ? o.out : o.in) + delta;
    Vec3 snapped;
    if (st_->snapPoint(vp, px, py, M.transformPoint(h), snapped)) h = M.inverse().transformPoint(snapped);
    (handleWhich_ ? k.out : k.in) = h;
    if (k.type == KnotType::Bezier) {
      // Keep the other handle opposite and the same length.
      (handleWhich_ ? k.in : k.out) = k.p * 2.0f - h;
    }
  } else {
    // Collect the knots to move at this level.
    std::set<KnotRef> move;
    if (level_ == 1) move = knots;
    else if (level_ == 2)
      for (const auto& g : segs) {
        const Spline& sp = w->splines[g.spline];
        move.insert({g.spline, g.knot});
        move.insert({g.spline, (g.knot + 1) % static_cast<int>(sp.knots.size())});
      }
    else
      for (int si : splines)
        for (int k = 0; k < static_cast<int>(w->splines[si].knots.size()); ++k) move.insert({si, k});
    if (move.empty()) return;
    // Snap the first moved knot.
    const KnotRef& first = *move.begin();
    Vec3 target = dragOrig_.splines[first.spline].knots[first.knot].p + delta, snapped;
    if (st_->snapPoint(vp, px, py, M.transformPoint(target), snapped))
      delta = M.inverse().transformPoint(snapped) - dragOrig_.splines[first.spline].knots[first.knot].p;
    for (const auto& r : move) {
      Knot& k = w->splines[r.spline].knots[r.knot];
      k.p += delta;
      k.in += delta;
      k.out += delta;
    }
    w->updateAutoHandles();
  }
  touch(*n);
}

bool SplineEditTool::mouseMove(Viewport& vp, QMouseEvent* e) {
  if (drag_ == Drag::None) return false;
  curPos_ = e->position();
  if ((curPos_ - pressPos_).manhattanLength() > 2) dragMoved_ = true;
  if (drag_ == Drag::Region) {
    st_->requestRedraw();
    return true;
  }
  if (dragMoved_) applyDrag(vp, static_cast<float>(curPos_.x()), static_cast<float>(curPos_.y()));
  return true;
}

bool SplineEditTool::mouseRelease(Viewport& vp, QMouseEvent* e) {
  if (e->button() != Qt::LeftButton || drag_ == Drag::None) return drag_ != Drag::None;
  if (drag_ == Drag::Region) {
    if ((e->position() - pressPos_).manhattanLength() > 3)
      regionSelect(vp, QRect(pressPos_.toPoint(), e->position().toPoint()).normalized(), pressMods_);
    else if (!(pressMods_ & (Qt::ControlModifier | Qt::AltModifier)))
      clearSelection();
    drag_ = Drag::None;
    changed();
    return true;
  }
  drag_ = Drag::None;
  if (dragMoved_) doc_->endEdit();
  else doc_->cancelEdit();
  changed();
  return true;
}

bool SplineEditTool::keyPress(Viewport&, QKeyEvent* e) {
  if (e->modifiers() & (Qt::ControlModifier | Qt::AltModifier)) return false;
  int key = e->key();
  if (key == Qt::Key_1 || key == Qt::Key_2 || key == Qt::Key_3) {
    setLevel(key - Qt::Key_1 + 1);
    return true;
  }
  if (key != Qt::Key_Delete && key != Qt::Key_Backspace) return false;
  if (level_ == 1 && !knots.empty()) {
    edit(tr("Delete Vertex"), [&](BezierShape& w) {
      splineops::deleteKnots(w, selKnots());
      return true;
    });
  } else if (level_ == 2 && !segs.empty()) {
    edit(tr("Delete Segment"), [&](BezierShape& w) {
      splineops::deleteSegments(w, selSegs());
      return true;
    });
  } else if (level_ == 3 && !splines.empty()) {
    edit(tr("Delete Spline"), [&](BezierShape& w) {
      splineops::deleteSplines(w, selSplines());
      return true;
    });
  } else {
    return false;
  }
  clearSelection();
  changed();
  return true;
}

bool SplineEditTool::cancel() {
  if (drag_ == Drag::Move || drag_ == Drag::Handle) {
    drag_ = Drag::None;
    doc_->cancelEdit();
    return true;
  }
  if (drag_ == Drag::Region) {
    drag_ = Drag::None;
    return true;
  }
  if (mode_ != Mode::Select) {
    mode_ = Mode::Select;
    connectFrom_ = -1;
    changed();
    return true;
  }
  return false;
}

QString SplineEditTool::prompt() const {
  switch (mode_) {
    case Mode::Refine: return tr("Click segments to add vertices; right-click to stop");
    case Mode::Insert: return tr("Click a segment to insert a vertex and drag it; right-click to stop");
    case Mode::Connect: return tr("Click an end vertex, then another end vertex to connect them");
    case Mode::CrossInsert: return tr("Click where two splines cross to add vertices on both");
    case Mode::Trim: return tr("Click a spline piece between crossings to trim it");
    case Mode::Extend: return tr("Click near an open end to extend it to the next spline");
    case Mode::Boolean: return tr("Click the spline to combine with the selected spline");
    case Mode::Attach: return tr("Click shapes to attach them; right-click to stop");
    case Mode::Select: break;
  }
  switch (level_) {
    case 1: return tr("Vertex: click or drag to select, drag vertices or handles to move them");
    case 2: return tr("Segment: click or drag to select, drag to move");
    case 3: return tr("Spline: click or drag to select, drag to move");
    default: return QString();
  }
}

void SplineEditTool::drawLines(Viewport& vp, LineBatch& lines) {
  Node* n = node();
  if (!n) return;
  BezierShape s = currentShape();
  Mat4 M = n->objectWorldMatrix();
  auto box = [&](const Vec3& local, float px, const QColor& c) {
    Vec3 p = M.transformPoint(local);
    float sz = vp.camera().worldPerPixel(p) * px;
    Vec3 r = vp.camera().right() * sz, u = vp.camera().up() * sz;
    lines.line(p - r - u, p + r - u, c);
    lines.line(p + r - u, p + r + u, c);
    lines.line(p + r + u, p - r + u, c);
    lines.line(p - r + u, p - r - u, c);
  };
  auto segLine = [&](const Spline& sp, int g, const QColor& c) {
    Vec3 a = M.transformPoint(sp.evaluate(g, 0));
    for (int i = 1; i <= 16; ++i) {
      Vec3 b = M.transformPoint(sp.evaluate(g, static_cast<float>(i) / 16));
      lines.line(a, b, c);
      a = b;
    }
  };
  const QColor red(255, 40, 40), white(235, 235, 235), yellow(255, 220, 0), green(90, 220, 90);
  for (int si = 0; si < static_cast<int>(s.splines.size()); ++si) {
    const Spline& sp = s.splines[si];
    if (level_ == 3 && splines.count(si))
      for (int g = 0; g < sp.segmentCount(); ++g) segLine(sp, g, red);
    if (level_ == 2)
      for (int g = 0; g < sp.segmentCount(); ++g)
        if (segs.count({si, g})) segLine(sp, g, red);
    if (level_ == 1) {
      for (int k = 0; k < static_cast<int>(sp.knots.size()); ++k) {
        bool sel = knots.count({si, k}) != 0;
        box(sp.knots[k].p, k == 0 ? 5.0f : 3.0f, sel ? red : (k == 0 ? yellow : white));
        const Knot& kn = sp.knots[k];
        if (sel && (kn.type == KnotType::Bezier || kn.type == KnotType::BezierCorner)) {
          lines.line(M.transformPoint(kn.in), M.transformPoint(kn.p), green);
          lines.line(M.transformPoint(kn.p), M.transformPoint(kn.out), green);
          box(kn.in, 2.5f, green);
          box(kn.out, 2.5f, green);
        }
      }
    }
  }
  if (mode_ == Mode::Connect && connectFrom_ >= 0 && connectKnot_.spline < static_cast<int>(s.splines.size()) &&
      connectKnot_.knot < static_cast<int>(s.splines[connectKnot_.spline].knots.size()))
    box(s.splines[connectKnot_.spline].knots[connectKnot_.knot].p, 7.0f, green);
}

void SplineEditTool::drawOverlay(Viewport&, QPainter& p) {
  if (drag_ != Drag::Region) return;
  p.setPen(QPen(QColor(255, 255, 255), 1, Qt::DashLine));
  p.setBrush(Qt::NoBrush);
  p.drawRect(QRectF(pressPos_, curPos_).normalized());
}

// The active session (one per node and stack entry), kept while its rollout is shown.
std::weak_ptr<SplineEditTool> g_session;

std::shared_ptr<SplineEditTool> sessionFor(const ModifyPanelContext& ctx) {
  auto s = g_session.lock();
  if (s && s->nodeId() == ctx.nodeId && s->stackIndex() == ctx.stackIndex) return s;
  s = std::make_shared<SplineEditTool>(ctx.doc, ctx.state, ctx.nodeId, ctx.stackIndex);
  s->setSelf(s);
  g_session = s;
  return s;
}

// Holds the session alive while any rollout shows it (and the editor uses it as its tool).
std::shared_ptr<SplineEditTool> g_sessionHold;

}  // namespace

bool setSplineSubObjectLevel(EditorState*, int level) {
  auto s = g_session.lock();
  if (!s) return false;
  if (level != s->level()) s->setLevel(level);
  return true;
}

QWidget* buildSplineEditRollout(const ModifyPanelContext& ctx, QWidget* parent) {
  Node* n = ctx.doc->scene().findNode(ctx.nodeId);
  if (!n) return nullptr;
  if (ctx.stackIndex < 0 && !dynamic_cast<EditableSplineObject*>(n->object.get())) return nullptr;
  auto session = sessionFor(ctx);
  g_sessionHold = session;
  ++session->liveRollouts;

  auto* root = new QWidget(parent);
  auto* v = new QVBoxLayout(root);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(4);

  // ---- Selection ----
  auto* selRoll = new Rollout(tr("Selection"), root);
  auto* levels = new QHBoxLayout;
  QPushButton* levelBtn[3];
  const char* names[3] = {"Vertex", "Segment", "Spline"};
  for (int i = 0; i < 3; ++i) {
    levelBtn[i] = new QPushButton(tr(names[i]), selRoll->body());
    levelBtn[i]->setCheckable(true);
    levelBtn[i]->setToolTip(tr("%1 sub-object level (%2)").arg(tr(names[i])).arg(i + 1));
    levels->addWidget(levelBtn[i]);
    QObject::connect(levelBtn[i], &QPushButton::clicked, root, [session, i] { session->setLevel(i + 1); });
  }
  selRoll->form()->addRow(levels);
  auto* selInfo = new QLabel(selRoll->body());
  selRoll->form()->addRow(selInfo);
  v->addWidget(selRoll);

  // ---- Geometry ----
  auto* geo = new Rollout(tr("Geometry"), root);
  QFormLayout* f = geo->form();
  auto* attach = new QPushButton(tr("Attach"), geo->body());
  attach->setCheckable(true);
  auto* attachMult = new QPushButton(tr("Attach Mult."), geo->body());
  auto* attachRow = new QHBoxLayout;
  attachRow->addWidget(attach);
  attachRow->addWidget(attachMult);
  f->addRow(attachRow);

  // Vertex tools.
  auto* vertexBox = new QWidget(geo->body());
  auto* vg = new QGridLayout(vertexBox);
  vg->setContentsMargins(0, 0, 0, 0);
  vg->setSpacing(2);
  const char* typeNames[4] = {"Corner", "Smooth", "Bezier", "Bezier Corner"};
  for (int t = 0; t < 4; ++t) {
    auto* b = new QPushButton(tr(typeNames[t]), vertexBox);
    b->setToolTip(tr("Set the selected vertices to %1").arg(tr(typeNames[t])));
    vg->addWidget(b, t / 2, t % 2);
    QObject::connect(b, &QPushButton::clicked, root, [session, t] {
      if (session->knots.empty()) return;
      session->edit(QCoreApplication::translate("Shapes", "Vertex Type"), [&](BezierShape& w) {
        splineops::setKnotType(w, session->selKnots(), static_cast<KnotType>(t));
        return true;
      });
    });
  }
  auto modeButton = [&](const QString& text, Mode m, QWidget* host) {
    auto* b = new QPushButton(text, host);
    b->setCheckable(true);
    QObject::connect(b, &QPushButton::clicked, root, [session, m] { session->setMode(m); });
    return std::make_pair(b, m);
  };
  std::vector<std::pair<QPushButton*, Mode>> modeButtons;
  modeButtons.push_back(modeButton(tr("Refine"), Mode::Refine, vertexBox));
  modeButtons.push_back(modeButton(tr("Insert"), Mode::Insert, vertexBox));
  modeButtons.push_back(modeButton(tr("Connect"), Mode::Connect, vertexBox));
  modeButtons.push_back(modeButton(tr("Cross Insert"), Mode::CrossInsert, vertexBox));
  vg->addWidget(modeButtons[0].first, 2, 0);
  vg->addWidget(modeButtons[1].first, 2, 1);
  vg->addWidget(modeButtons[2].first, 3, 0);
  vg->addWidget(modeButtons[3].first, 3, 1);
  auto simple = [&](const QString& text, QWidget* host, std::function<void()> fn) {
    auto* b = new QPushButton(text, host);
    QObject::connect(b, &QPushButton::clicked, root, fn);
    return b;
  };
  auto* weldSpin = new QDoubleSpinBox(vertexBox);
  weldSpin->setRange(0, 1e6);
  weldSpin->setDecimals(3);
  weldSpin->setValue(session->weldThreshold);
  installSpinnerScrub(weldSpin);
  QPointer<QLabel> infoP = selInfo;
  auto* weld = simple(tr("Weld"), vertexBox, [session, weldSpin, infoP] {
    int welded = 0;
    session->edit(QCoreApplication::translate("Shapes", "Weld"), [&](BezierShape& w) {
      welded = splineops::weld(w, session->selKnots(), static_cast<float>(weldSpin->value()));
      return welded > 0;
    });
    session->clearSelection();
    session->changed();
    if (infoP) infoP->setText(QCoreApplication::translate("Shapes", "%1 vertices welded").arg(welded));
  });
  vg->addWidget(weld, 4, 0);
  vg->addWidget(weldSpin, 4, 1);
  auto knotOp = [&](const QString& text, std::function<void(BezierShape&, const std::vector<KnotRef>&)> op, bool clear) {
    return simple(text, vertexBox, [session, op, text, clear] {
      if (session->knots.empty()) return;
      session->edit(text, [&](BezierShape& w) {
        op(w, session->selKnots());
        return true;
      });
      if (clear) session->clearSelection();
      session->changed();
    });
  };
  vg->addWidget(knotOp(tr("Fuse"), [](BezierShape& w, const auto& k) { splineops::fuse(w, k); }, false), 5, 0);
  vg->addWidget(knotOp(tr("Break"), [](BezierShape& w, const auto& k) { splineops::breakKnots(w, k); }, true), 5, 1);
  vg->addWidget(knotOp(tr("Make First"), [](BezierShape& w, const auto& k) { if (!k.empty()) splineops::makeFirst(w, k.front()); }, true), 6, 0);
  vg->addWidget(knotOp(tr("Delete"), [](BezierShape& w, const auto& k) { splineops::deleteKnots(w, k); }, true), 6, 1);
  auto* filletSpin = new QDoubleSpinBox(vertexBox);
  filletSpin->setRange(0, 1e6);
  filletSpin->setValue(10);
  installSpinnerScrub(filletSpin);
  auto* chamferSpin = new QDoubleSpinBox(vertexBox);
  chamferSpin->setRange(0, 1e6);
  chamferSpin->setValue(10);
  installSpinnerScrub(chamferSpin);
  vg->addWidget(simple(tr("Fillet"), vertexBox,
                       [session, filletSpin] {
                         if (session->knots.empty() || filletSpin->value() <= 0) return;
                         session->edit(QCoreApplication::translate("Shapes", "Fillet"), [&](BezierShape& w) {
                           splineops::fillet(w, session->selKnots(), static_cast<float>(filletSpin->value()));
                           return true;
                         });
                         session->clearSelection();
                         session->changed();
                       }),
                7, 0);
  vg->addWidget(filletSpin, 7, 1);
  vg->addWidget(simple(tr("Chamfer"), vertexBox,
                       [session, chamferSpin] {
                         if (session->knots.empty() || chamferSpin->value() <= 0) return;
                         session->edit(QCoreApplication::translate("Shapes", "Chamfer"), [&](BezierShape& w) {
                           splineops::chamfer(w, session->selKnots(), static_cast<float>(chamferSpin->value()));
                           return true;
                         });
                         session->clearSelection();
                         session->changed();
                       }),
                8, 0);
  vg->addWidget(chamferSpin, 8, 1);
  f->addRow(vertexBox);

  // Segment tools.
  auto* segmentBox = new QWidget(geo->body());
  auto* sg = new QGridLayout(segmentBox);
  sg->setContentsMargins(0, 0, 0, 0);
  sg->setSpacing(2);
  auto* divideSpin = new QSpinBox(segmentBox);
  divideSpin->setRange(1, 1000);
  divideSpin->setValue(1);
  sg->addWidget(simple(tr("Divide"), segmentBox,
                       [session, divideSpin] {
                         if (session->segs.empty()) return;
                         session->edit(QCoreApplication::translate("Shapes", "Divide"), [&](BezierShape& w) {
                           splineops::divideSegments(w, session->selSegs(), divideSpin->value());
                           return true;
                         });
                         session->clearSelection();
                         session->changed();
                       }),
                0, 0);
  sg->addWidget(divideSpin, 0, 1);
  auto* detachCopy = new QCheckBox(tr("Copy"), geo->body());
  SceneDocument* doc = ctx.doc;
  auto detach = [session, doc, detachCopy] {
    Node* src = session->node();
    if (!src) return;
    BezierShape cur = session->currentShape();
    BezierShape part = session->level() == 2 ? splineops::extractSegments(cur, session->selSegs())
                                             : splineops::extractSplines(cur, session->selSplines());
    if (part.empty()) return;
    bool copy = detachCopy->isChecked();
    int lvl = session->level();
    session->edit(QCoreApplication::translate("Shapes", "Detach"), [&](BezierShape& w) {
      auto node = std::make_unique<Node>();
      auto obj = std::make_shared<EditableSplineObject>();
      obj->shape = part;
      node->object = obj;
      node->name = doc->scene().uniqueNodeName("Shape");
      node->setWorldMatrix(src->objectWorldMatrix());
      node->wireColor = src->wireColor;
      doc->scene().addNode(std::move(node));
      doc->notifyStructureChanged();
      if (!copy) {
        if (lvl == 2) splineops::deleteSegments(w, session->selSegs());
        else splineops::deleteSplines(w, session->selSplines());
      }
      return true;
    });
    session->clearSelection();
    session->changed();
  };
  sg->addWidget(simple(tr("Detach"), segmentBox, detach), 1, 0);
  sg->addWidget(simple(tr("Delete"), segmentBox,
                       [session] {
                         if (session->segs.empty()) return;
                         session->edit(QCoreApplication::translate("Shapes", "Delete Segment"), [&](BezierShape& w) {
                           splineops::deleteSegments(w, session->selSegs());
                           return true;
                         });
                         session->clearSelection();
                         session->changed();
                       }),
                1, 1);
  sg->addWidget(simple(tr("Hide"), segmentBox,
                       [session] {
                         for (const auto& g : session->segs) session->hidden.insert(g);
                         session->segs.clear();
                         session->changed();
                       }),
                2, 0);
  sg->addWidget(simple(tr("Unhide All"), segmentBox,
                       [session] {
                         session->hidden.clear();
                         session->changed();
                       }),
                2, 1);
  f->addRow(segmentBox);

  // Spline tools.
  auto* splineBox = new QWidget(geo->body());
  auto* pg = new QGridLayout(splineBox);
  pg->setContentsMargins(0, 0, 0, 0);
  pg->setSpacing(2);
  auto* outlineSpin = new QDoubleSpinBox(splineBox);
  outlineSpin->setRange(-1e6, 1e6);
  outlineSpin->setValue(10);
  installSpinnerScrub(outlineSpin);
  auto* center = new QCheckBox(tr("Center"), splineBox);
  pg->addWidget(simple(tr("Outline"), splineBox,
                       [session, outlineSpin, center] {
                         if (session->splines.empty() || outlineSpin->value() == 0) return;
                         session->edit(QCoreApplication::translate("Shapes", "Outline"), [&](BezierShape& w) {
                           splineops::outline(w, session->selSplines(), static_cast<float>(outlineSpin->value()),
                                              center->isChecked());
                           return true;
                         });
                         session->changed();
                       }),
                0, 0);
  pg->addWidget(outlineSpin, 0, 1);
  pg->addWidget(center, 0, 2);
  auto* boolGroup = new QButtonGroup(splineBox);
  const char* boolNames[3] = {"Union", "Subtraction", "Intersection"};
  for (int b = 0; b < 3; ++b) {
    auto* rb = new QRadioButton(tr(boolNames[b]), splineBox);
    rb->setChecked(b == session->booleanOp);
    boolGroup->addButton(rb, b);
    pg->addWidget(rb, 1, b);
  }
  QObject::connect(boolGroup, &QButtonGroup::idClicked, root, [session](int id) { session->booleanOp = id; });
  modeButtons.push_back(modeButton(tr("Boolean"), Mode::Boolean, splineBox));
  pg->addWidget(modeButtons.back().first, 2, 0);
  auto* mirrorGroup = new QButtonGroup(splineBox);
  const char* mirrorNames[3] = {"Horizontal", "Vertical", "Both"};
  for (int b = 0; b < 3; ++b) {
    auto* rb = new QRadioButton(tr(mirrorNames[b]), splineBox);
    rb->setChecked(b == 0);
    mirrorGroup->addButton(rb, b);
    pg->addWidget(rb, 3, b);
  }
  auto* mirrorCopy = new QCheckBox(tr("Copy"), splineBox);
  pg->addWidget(simple(tr("Mirror"), splineBox,
                       [session, mirrorGroup, mirrorCopy] {
                         if (session->splines.empty()) return;
                         session->edit(QCoreApplication::translate("Shapes", "Mirror"), [&](BezierShape& w) {
                           splineops::mirror(w, session->selSplines(), mirrorGroup->checkedId(), mirrorCopy->isChecked());
                           return true;
                         });
                         session->changed();
                       }),
                4, 0);
  pg->addWidget(mirrorCopy, 4, 1);
  auto splineOp = [&](const QString& text, std::function<void(BezierShape&, const std::vector<int>&)> op, bool clear) {
    return simple(text, splineBox, [session, op, text, clear] {
      if (session->splines.empty()) return;
      session->edit(text, [&](BezierShape& w) {
        op(w, session->selSplines());
        return true;
      });
      if (clear) session->clearSelection();
      session->changed();
    });
  };
  pg->addWidget(splineOp(tr("Reverse"), [](BezierShape& w, const auto& s) { splineops::reverse(w, s); }, false), 5, 0);
  pg->addWidget(splineOp(tr("Close"), [](BezierShape& w, const auto& s) { splineops::close(w, s); }, false), 5, 1);
  pg->addWidget(splineOp(tr("Explode"),
                         [](BezierShape& w, const auto& sel) {
                           // Every segment of the selected splines becomes its own spline.
                           BezierShape pieces;
                           for (int si : sel) {
                             std::vector<SegmentRef> all;
                             for (int g = 0; g < w.splines[si].segmentCount(); ++g) all.push_back({si, g});
                             for (int g = 0; g < static_cast<int>(all.size()); ++g) {
                               BezierShape one = splineops::extractSegments(w, {all[g]});
                               for (auto& sp : one.splines) pieces.splines.push_back(sp);
                             }
                           }
                           splineops::deleteSplines(w, sel);
                           for (auto& sp : pieces.splines) w.splines.push_back(sp);
                         },
                         true),
                5, 2);
  pg->addWidget(simple(tr("Detach"), splineBox, detach), 6, 0);
  pg->addWidget(detachCopy, 6, 1);
  pg->addWidget(splineOp(tr("Delete"), [](BezierShape& w, const auto& s) { splineops::deleteSplines(w, s); }, true), 6, 2);
  modeButtons.push_back(modeButton(tr("Trim"), Mode::Trim, splineBox));
  pg->addWidget(modeButtons.back().first, 7, 0);
  modeButtons.push_back(modeButton(tr("Extend"), Mode::Extend, splineBox));
  pg->addWidget(modeButtons.back().first, 7, 1);
  f->addRow(splineBox);
  v->addWidget(geo);
  modeButtons.emplace_back(attach, Mode::Attach);
  QObject::connect(attach, &QPushButton::clicked, root, [session] { session->setMode(Mode::Attach); });

  QObject::connect(attachMult, &QPushButton::clicked, root, [session, doc, root] {
    Node* self = session->node();
    if (!self) return;
    QDialog dlg(root);
    dlg.setWindowTitle(QCoreApplication::translate("Shapes", "Attach Multiple"));
    auto* lay = new QVBoxLayout(&dlg);
    auto* list = new QListWidget(&dlg);
    for (const auto& up : doc->scene().nodes)
      if (up->id != self->id && up->kind() == ObjectKind::Shape && up->isShapeResult()) {
        auto* it = new QListWidgetItem(QString::fromStdString(up->name), list);
        it->setData(Qt::UserRole, up->id);
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(Qt::Unchecked);
      }
    lay->addWidget(list);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(bb);
    if (dlg.exec() != QDialog::Accepted) return;
    std::vector<uint32_t> ids;
    for (int i = 0; i < list->count(); ++i)
      if (list->item(i)->checkState() == Qt::Checked) ids.push_back(list->item(i)->data(Qt::UserRole).toUInt());
    if (ids.empty()) return;
    Mat4 inv = self->objectWorldMatrix().inverse();
    session->edit(QCoreApplication::translate("Shapes", "Attach Multiple"), [&](BezierShape& w) {
      for (uint32_t id : ids) {
        Node* o = doc->scene().findNode(id);
        if (!o) continue;
        BezierShape add = evaluatedShape(*o);
        add.transform(inv * o->objectWorldMatrix());
        for (auto& sp : add.splines) w.splines.push_back(sp);
        doc->scene().removeNode(id);
      }
      doc->notifyStructureChanged();
      return true;
    });
  });

  // Keep the UI in step with the session.
  QPointer<QWidget> vb = vertexBox, sb = segmentBox, pb = splineBox;
  QPushButton* lb0 = levelBtn[0];
  QPushButton* lb1 = levelBtn[1];
  QPushButton* lb2 = levelBtn[2];
  QPointer<QWidget> rootP = root;
  auto sync = [session, lb0, lb1, lb2, vb, sb, pb, modeButtons, infoP, rootP] {
    if (!rootP) return;
    int l = session->level();
    QPushButton* btns[3] = {lb0, lb1, lb2};
    for (int i = 0; i < 3; ++i) {
      QSignalBlocker b(btns[i]);
      btns[i]->setChecked(l == i + 1);
    }
    if (vb) vb->setVisible(l == 1);
    if (sb) sb->setVisible(l == 2);
    if (pb) pb->setVisible(l == 3);
    for (const auto& [b, m] : modeButtons) {
      QSignalBlocker blk(b);
      b->setChecked(session->mode() == m);
    }
    if (infoP) {
      BezierShape s = session->currentShape();
      int knots = 0;
      for (const auto& sp : s.splines) knots += static_cast<int>(sp.knots.size());
      QString what = l == 1 ? QCoreApplication::translate("Shapes", "%1 vertices selected").arg(session->knots.size())
                     : l == 2 ? QCoreApplication::translate("Shapes", "%1 segments selected").arg(session->segs.size())
                     : l == 3 ? QCoreApplication::translate("Shapes", "%1 splines selected").arg(session->splines.size())
                              : QCoreApplication::translate("Shapes", "%1 splines, %2 vertices")
                                    .arg(s.splines.size())
                                    .arg(knots);
      infoP->setText(what);
    }
  };
  int listener = session->addListener(sync);
  sync();
  EditorState* st = ctx.state;
  QObject::connect(root, &QObject::destroyed, [session, listener, st] {
    session->removeListener(listener);
    --session->liveRollouts;
    // Leave sub-object mode once no rollout shows this session (another object or stack entry).
    QTimer::singleShot(0, [session, st] {
      if (session->liveRollouts > 0) return;
      if (st->viewportTool() == session.get()) st->setViewportTool(nullptr);
      if (g_sessionHold == session) g_sessionHold.reset();
    });
  });
  v->addStretch();
  return root;
}

}  // namespace shapesapp
}  // namespace veraa
