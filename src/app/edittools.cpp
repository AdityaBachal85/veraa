#include "edittools.h"

#include <algorithm>
#include <QMouseEvent>
#include <QPainter>
#include <QSettings>

#include <cmath>
#include <vector>

#include "extension.h"
#include "theme.h"
#include "units.h"
#include "viewport.h"

namespace veraa {

namespace {

// Ray hit against visible geometry (excluding some nodes). Returns the nearest t.
bool surfaceHit(const Scene& scene, const Ray& ray, const std::vector<uint32_t>& exclude, Vec3& point, uint32_t* hitId) {
  float best = kInfinity;
  uint32_t bestId = 0;
  for (const auto& up : scene.nodes) {
    const Node& n = *up;
    if (n.kind() != ObjectKind::Geometry || !scene.isNodeVisible(n)) continue;
    if (std::find(exclude.begin(), exclude.end(), n.id) != exclude.end()) continue;
    if (n.triMesh().empty()) continue;
    Mat4 m = n.objectWorldMatrix();
    Mat4 inv = m.inverse();
    Ray local{inv.transformPoint(ray.o), inv.transformDir(ray.d)};
    float t = intersectTriMesh(n.triMesh(), local);
    if (t > 0) {
      Vec3 wp = m.transformPoint(local.o + local.d * t);
      float wt = dot(wp - ray.o, ray.d);
      if (wt > 0 && wt < best) {
        best = wt;
        bestId = n.id;
        point = wp;
      }
    }
  }
  if (hitId) *hitId = bestId;
  return bestId != 0;
}

// Surface point and normal under a pixel (normal from two neighbouring rays).
bool surfaceFrame(const Viewport& vp, float px, float py, const std::vector<uint32_t>& exclude, Vec3& p, Vec3& n) {
  const Scene& scene = vp.document()->scene();
  const ViewCamera& cam = vp.camera();
  if (!surfaceHit(scene, cam.rayAt(px, py), exclude, p, nullptr)) return false;
  Vec3 px1, py1;
  bool a = surfaceHit(scene, cam.rayAt(px + 1.5f, py), exclude, px1, nullptr);
  bool b = surfaceHit(scene, cam.rayAt(px, py + 1.5f), exclude, py1, nullptr);
  n = a && b ? cross(px1 - p, py1 - p) : Vec3(0, 0, 1);
  if (length(n) < 1e-8f) n = Vec3(0, 0, 1);
  n = normalize(n);
  if (dot(n, cam.forward()) > 0) n = -n;
  return true;
}

}  // namespace

bool surfaceFrameAt(const Viewport& vp, float px, float py, Vec3& point, Vec3& normal) {
  return surfaceFrame(vp, px, py, {}, point, normal);
}

namespace {

void collectDescendants(const Scene& s, uint32_t id, std::vector<uint32_t>& out) {
  out.push_back(id);
  for (Node* c : s.children(id)) collectDescendants(s, c->id, out);
}

// ---- Select and Place ----

class PlacementTool : public ViewportTool {
 public:
  PlacementTool(SceneDocument* doc, EditorState* state) : doc_(doc), state_(state) {}

  bool mousePress(Viewport& vp, QMouseEvent* e) override {
    if (e->button() == Qt::RightButton) {
      state_->setViewportTool(nullptr);
      return true;
    }
    if (e->button() != Qt::LeftButton) return false;
    float px = static_cast<float>(e->position().x()), py = static_cast<float>(e->position().y());
    uint32_t hit = vp.pickNode(px, py);
    if (!hit) return false;  // empty space: normal region selection
    if (!doc_->isSelected(hit)) doc_->select(hit, false);
    Node* n = doc_->scene().findNode(hit);
    if (!n) return false;
    node_ = hit;
    start_ = n->transform;
    exclude_.clear();
    collectDescendants(doc_->scene(), hit, exclude_);
    // Height of the pivot above the object's lowest point (along its local Z).
    Aabb lb = n->triMesh().bounds;
    Mat4 ow = n->objectWorldMatrix();
    Vec3 piv = n->worldPosition();
    Vec3 up = normalize(n->worldMatrix().column(2));
    base_ = 0.0f;
    if (lb.valid()) {
      float lowest = kInfinity;
      for (int i = 0; i < 8; ++i) {
        Vec3 c((i & 1) ? lb.hi.x : lb.lo.x, (i & 2) ? lb.hi.y : lb.lo.y, (i & 4) ? lb.hi.z : lb.lo.z);
        lowest = std::min(lowest, dot(ow.transformPoint(c) - piv, up));
      }
      base_ = -lowest;
    }
    doc_->beginEdit(QObject::tr("Select and Place"));
    dragging_ = true;
    return true;
  }

  bool mouseMove(Viewport& vp, QMouseEvent* e) override {
    if (!dragging_) return false;
    Node* n = doc_->scene().findNode(node_);
    if (!n) return false;
    float px = static_cast<float>(e->position().x()), py = static_cast<float>(e->position().y());
    Vec3 p, normal;
    if (!surfaceFrame(vp, px, py, exclude_, p, normal)) {
      if (!vp.hitConstructionPlane(px, py, p)) return true;
      normal = Vec3(0, 0, 1);
    }
    n->transform = start_;
    // Shift: also rotate the object so its local Z follows the surface normal.
    if (e->modifiers() & Qt::ShiftModifier) {
      Vec3 z = normal;
      Vec3 x = std::fabs(z.z) > 0.9f ? Vec3(1, 0, 0) : normalize(cross(Vec3(0, 0, 1), z));
      Vec3 y = cross(z, x);
      Mat4 world = n->worldMatrix();
      Vec3 s(length(world.column(0)), length(world.column(1)), length(world.column(2)));
      Mat4 m = Mat4::identity();
      for (int r = 0; r < 3; ++r) {
        m.m[r][0] = x[r] * s.x;
        m.m[r][1] = y[r] * s.y;
        m.m[r][2] = z[r] * s.z;
      }
      n->setWorldMatrix(m);
    }
    n->setWorldPosition(p + normal * base_);
    doc_->notifyNodeChanged(n, ChangeTransform);
    return true;
  }

  bool mouseRelease(Viewport&, QMouseEvent* e) override {
    if (!dragging_ || e->button() != Qt::LeftButton) return false;
    dragging_ = false;
    doc_->endEdit();
    return true;
  }

  bool cancel() override {
    if (!dragging_) return false;
    dragging_ = false;
    doc_->cancelEdit();
    return true;
  }
  QString prompt() const override {
    return QObject::tr("Select and Place: drag an object over other surfaces (Shift also rotates it to the surface)");
  }

 private:
  SceneDocument* doc_;
  EditorState* state_;
  uint32_t node_ = 0;
  Transform start_;
  std::vector<uint32_t> exclude_;
  float base_ = 0;
  bool dragging_ = false;
};

// ---- Measure Distance ----

class MeasureTool : public ViewportTool {
 public:
  MeasureTool(SceneDocument* doc, EditorState* state) : doc_(doc), state_(state) {}

  bool mousePress(Viewport& vp, QMouseEvent* e) override {
    if (e->button() == Qt::RightButton) {
      state_->setViewportTool(nullptr);
      return true;
    }
    if (e->button() != Qt::LeftButton) return false;
    Vec3 p;
    if (!pointAt(vp, e->position(), p)) return true;
    if (count_ == 0 || count_ == 2) {
      a_ = p;
      b_ = p;
      count_ = 1;
    } else {
      b_ = p;
      count_ = 2;
      report();
    }
    return true;
  }

  bool mouseMove(Viewport& vp, QMouseEvent* e) override {
    if (count_ != 1) return false;
    Vec3 p;
    if (pointAt(vp, e->position(), p)) b_ = p;
    report();
    return true;
  }

  void drawLines(Viewport&, LineBatch& lines) override {
    if (count_ == 0) return;
    lines.line(a_, b_, theme::gizmoHot());
  }

  void drawOverlay(Viewport& vp, QPainter& p) override {
    if (count_ == 0) return;
    float ax, ay, bx, by;
    if (!vp.camera().project(a_, ax, ay) || !vp.camera().project(b_, bx, by)) return;
    p.setPen(theme::gizmoHot());
    p.setBrush(theme::gizmoHot());
    p.drawEllipse(QPointF(ax, ay), 3, 3);
    p.drawEllipse(QPointF(bx, by), 3, 3);
    p.setPen(Qt::white);
    p.drawText(QPointF((ax + bx) * 0.5 + 8, (ay + by) * 0.5 - 8), Units::instance().format(length(b_ - a_)));
  }

  bool cancel() override {
    bool had = count_ != 0;
    count_ = 0;
    return had;
  }
  QString prompt() const override { return QObject::tr("Measure Distance: click two points"); }

  float distance() const { return length(b_ - a_); }

 private:
  bool pointAt(Viewport& vp, QPointF pos, Vec3& p) const {
    Vec3 n;
    if (surfaceFrame(vp, static_cast<float>(pos.x()), static_cast<float>(pos.y()), {}, p, n)) {
      Vec3 snapped;
      if (state_->snapPoint(vp, static_cast<float>(pos.x()), static_cast<float>(pos.y()), p, snapped)) p = snapped;
      return true;
    }
    return vp.hitConstructionPlane(static_cast<float>(pos.x()), static_cast<float>(pos.y()), p);
  }
  void report() {
    const Units& u = Units::instance();
    Vec3 d = b_ - a_;
    state_->setPrompt(QObject::tr("Distance: %1   Delta X: %2  Y: %3  Z: %4")
                          .arg(u.format(length(d)), u.format(d.x), u.format(d.y), u.format(d.z)));
  }

  SceneDocument* doc_;
  EditorState* state_;
  Vec3 a_, b_;
  int count_ = 0;
};

// ---- Select and Manipulate ----

struct Handle {
  uint32_t node;
  std::string key;
  int axis;      // local axis the parameter runs along
  float factor;  // value = coordinate * factor (2 for full width/length)
  Vec3 world;    // handle position
};

class ManipulateTool : public ViewportTool {
 public:
  ManipulateTool(SceneDocument* doc, EditorState* state) : doc_(doc), state_(state) {}

  std::vector<Handle> handles() const {
    std::vector<Handle> out;
    for (Node* n : doc_->selectedNodes()) {
      if (!n->object) continue;
      const ParamBlock& p = n->object->params;
      Mat4 m = n->objectWorldMatrix();
      auto add = [&](const char* key, int axis, float factor) {
        if (!p.has(key)) return;
        Vec3 local;
        local[axis] = p.getFloat(key) / factor;
        out.push_back({n->id, key, axis, factor, m.transformPoint(local)});
      };
      add("radius", 0, 1.0f);
      add("radius1", 0, 1.0f);
      add("width", 0, 2.0f);
      add("length", 1, 2.0f);
      add("height", 2, 1.0f);
    }
    return out;
  }

  bool mousePress(Viewport& vp, QMouseEvent* e) override {
    if (e->button() == Qt::RightButton) {
      state_->setViewportTool(nullptr);
      return true;
    }
    if (e->button() != Qt::LeftButton) return false;
    float px = static_cast<float>(e->position().x()), py = static_cast<float>(e->position().y());
    for (const Handle& h : handles()) {
      float sx, sy;
      if (!vp.camera().project(h.world, sx, sy)) continue;
      if (std::hypot(sx - px, sy - py) < 7.0f) {
        active_ = h;
        dragging_ = true;
        doc_->beginEdit(QObject::tr("Manipulate"));
        return true;
      }
    }
    return false;  // selection works as usual
  }

  bool mouseMove(Viewport& vp, QMouseEvent* e) override {
    float px = static_cast<float>(e->position().x()), py = static_cast<float>(e->position().y());
    if (!dragging_) {
      bool over = false;
      for (const Handle& h : handles()) {
        float sx, sy;
        if (vp.camera().project(h.world, sx, sy) && std::hypot(sx - px, sy - py) < 7.0f) over = true;
      }
      if (over != hover_) {
        hover_ = over;
        vp.update();
      }
      return false;
    }
    Node* n = doc_->scene().findNode(active_.node);
    if (!n) return true;
    Mat4 m = n->objectWorldMatrix();
    Vec3 origin = m.transformPoint(Vec3());
    Vec3 axisW = m.transformDir(Vec3(active_.axis == 0 ? 1.0f : 0.0f, active_.axis == 1 ? 1.0f : 0.0f,
                                     active_.axis == 2 ? 1.0f : 0.0f));
    float axisLen = length(axisW);
    if (axisLen < 1e-8f) return true;
    Vec3 dir = axisW / axisLen;
    // Closest point on the handle's axis line to the mouse ray.
    Ray r = vp.camera().rayAt(px, py);
    Vec3 w0 = origin - r.o;
    float a = dot(dir, dir), b = dot(dir, r.d), c = dot(r.d, r.d), d = dot(dir, w0), ee = dot(r.d, w0);
    float denom = a * c - b * b;
    if (std::fabs(denom) < 1e-8f) return true;
    float s = (b * ee - c * d) / denom;
    float value = std::max(0.0f, s / axisLen * active_.factor);
    n->object->params.setNumber(active_.key, value);
    n->object->touch();
    n->invalidate();
    doc_->notifyNodeChanged(n, ChangeGeometry);
    state_->setPrompt(QObject::tr("%1 = %2").arg(QString::fromStdString(active_.key), Units::instance().format(value)));
    return true;
  }

  bool mouseRelease(Viewport&, QMouseEvent* e) override {
    if (!dragging_ || e->button() != Qt::LeftButton) return false;
    dragging_ = false;
    doc_->endEdit();
    return true;
  }

  void drawLines(Viewport& vp, LineBatch& lines) override {
    for (const Handle& h : handles())
      if (Node* n = doc_->scene().findNode(h.node)) lines.line(n->objectWorldMatrix().transformPoint(Vec3()), h.world, QColor(80, 220, 80));
    Q_UNUSED(vp);
  }

  void drawOverlay(Viewport& vp, QPainter& p) override {
    for (const Handle& h : handles()) {
      float sx, sy;
      if (!vp.camera().project(h.world, sx, sy)) continue;
      bool hot = dragging_ && h.node == active_.node && h.key == active_.key;
      p.setPen(QPen(Qt::black, 1));
      p.setBrush(hot ? theme::gizmoHot() : QColor(80, 220, 80));
      p.drawRect(QRectF(sx - 4, sy - 4, 8, 8));
    }
  }

  bool allowsGizmo() const override { return false; }
  bool cancel() override {
    if (!dragging_) return false;
    dragging_ = false;
    doc_->cancelEdit();
    return true;
  }
  QString prompt() const override { return QObject::tr("Manipulate: drag the green handles to change parameters"); }

 private:
  SceneDocument* doc_;
  EditorState* state_;
  Handle active_{};
  bool dragging_ = false;
  bool hover_ = false;
};

// ---- Select and Link ----

class LinkTool : public ViewportTool {
 public:
  LinkTool(SceneDocument* doc, EditorState* state) : doc_(doc), state_(state) {}

  bool mousePress(Viewport& vp, QMouseEvent* e) override {
    if (e->button() == Qt::RightButton) {
      state_->setViewportTool(nullptr);
      return true;
    }
    if (e->button() != Qt::LeftButton) return false;
    uint32_t hit = vp.pickNode(static_cast<float>(e->position().x()), static_cast<float>(e->position().y()));
    if (!hit) return false;
    if (!doc_->isSelected(hit)) doc_->select(hit, false);
    dragging_ = true;
    from_ = to_ = e->position();
    return true;
  }
  bool mouseMove(Viewport& vp, QMouseEvent* e) override {
    if (!dragging_) return false;
    to_ = e->position();
    vp.update();
    return true;
  }
  bool mouseRelease(Viewport& vp, QMouseEvent* e) override {
    if (!dragging_ || e->button() != Qt::LeftButton) return false;
    dragging_ = false;
    uint32_t parent = vp.pickNode(static_cast<float>(e->position().x()), static_cast<float>(e->position().y()));
    if (!parent || doc_->isSelected(parent)) {
      vp.update();
      return true;
    }
    Scene& s = doc_->scene();
    doc_->beginEdit(QObject::tr("Link"));
    int linked = 0;
    for (uint32_t id : doc_->selection())
      if (s.setParent(id, parent)) ++linked;
    doc_->notifyNodesChanged(doc_->selection(), ChangeTransform);
    doc_->notifyStructureChanged();
    doc_->endEdit();
    if (Node* p = s.findNode(parent))
      state_->setPrompt(QObject::tr("Linked %1 object(s) to %2").arg(linked).arg(QString::fromStdString(p->name)));
    return true;
  }
  void drawOverlay(Viewport&, QPainter& p) override {
    if (!dragging_) return;
    p.setPen(QPen(theme::gizmoHot(), 1.5, Qt::DashLine));
    p.drawLine(from_, to_);
  }
  bool cancel() override {
    bool was = dragging_;
    dragging_ = false;
    return was;
  }
  QString prompt() const override {
    return QObject::tr("Select and Link: drag from the child object to its new parent");
  }

 private:
  SceneDocument* doc_;
  EditorState* state_;
  bool dragging_ = false;
  QPointF from_, to_;
};

// ---- Pick object ----

class PickObjectTool : public ViewportTool {
 public:
  PickObjectTool(SceneDocument* doc, QString prompt, std::function<void(uint32_t)> picked,
                 std::function<bool(const Node&)> filter)
      : doc_(doc), prompt_(std::move(prompt)), picked_(std::move(picked)), filter_(std::move(filter)) {}

  bool mousePress(Viewport& vp, QMouseEvent* e) override {
    if (e->button() == Qt::RightButton) {
      vp.editorState()->setViewportTool(nullptr);
      return true;
    }
    if (e->button() != Qt::LeftButton) return true;
    uint32_t hit = vp.pickNode(static_cast<float>(e->position().x()), static_cast<float>(e->position().y()));
    Node* n = doc_->scene().findNode(hit);
    if (n && (!filter_ || filter_(*n))) {
      auto cb = picked_;  // the callback may replace this tool
      cb(hit);
    }
    return true;
  }
  bool mouseMove(Viewport& vp, QMouseEvent* e) override {
    uint32_t hit = vp.pickNode(static_cast<float>(e->position().x()), static_cast<float>(e->position().y()));
    Node* n = doc_->scene().findNode(hit);
    vp.setCursor(n && (!filter_ || filter_(*n)) ? Qt::PointingHandCursor : Qt::ForbiddenCursor);
    return true;
  }
  bool mouseRelease(Viewport&, QMouseEvent*) override { return true; }
  QString prompt() const override { return prompt_; }

 private:
  SceneDocument* doc_;
  QString prompt_;
  std::function<void(uint32_t)> picked_;
  std::function<bool(const Node&)> filter_;
};

}  // namespace

std::shared_ptr<ViewportTool> makePlacementTool(SceneDocument* doc, EditorState* state) {
  return std::make_shared<PlacementTool>(doc, state);
}

std::shared_ptr<ViewportTool> makeMeasureTool(SceneDocument* doc, EditorState* state) {
  return std::make_shared<MeasureTool>(doc, state);
}

std::shared_ptr<ViewportTool> makeManipulateTool(SceneDocument* doc, EditorState* state) {
  return std::make_shared<ManipulateTool>(doc, state);
}

std::shared_ptr<ViewportTool> makeLinkTool(SceneDocument* doc, EditorState* state) {
  return std::make_shared<LinkTool>(doc, state);
}

std::shared_ptr<ViewportTool> makePickObjectTool(SceneDocument* doc, const QString& prompt,
                                                 std::function<void(uint32_t)> picked,
                                                 std::function<bool(const Node&)> filter) {
  return std::make_shared<PickObjectTool>(doc, prompt, std::move(picked), std::move(filter));
}

double homeGridSpacing() {
  double s = QSettings("Veraa", "Veraa").value("grid/spacing", 10.0).toDouble();
  return s > 0 ? s : 10.0;
}

void installGridSnapper(EditorState* state, bool on) {
  if (!on) {
    state->setPointSnapper(nullptr);
    return;
  }
  state->setPointSnapper([](const Viewport&, float, float, const Vec3& candidate, Vec3& out) {
    float s = static_cast<float>(homeGridSpacing());
    out = Vec3(std::round(candidate.x / s) * s, std::round(candidate.y / s) * s, std::round(candidate.z / s) * s);
    return true;
  });
}

}  // namespace veraa
