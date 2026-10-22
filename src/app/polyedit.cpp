#include "polyedit.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "document.h"
#include "editorstate.h"
#include "glscene.h"
#include "object.h"
#include "polymods.h"
#include "theme.h"
#include "viewport.h"

namespace veraa {

PolyToolSettings& PolyToolSettings::get() {
  static PolyToolSettings s;
  return s;
}

SlicePlaneState& SlicePlaneState::get() {
  static SlicePlaneState s;
  return s;
}

RepeatLast& RepeatLast::get() {
  static RepeatLast r;
  return r;
}

namespace {

// One interactive operation at a time (caddy preview or transform drag).
struct LiveState {
  bool active = false;
  uint32_t node = 0;
  int stack = -1;
  PolyMesh mesh;      // Editable Poly state at begin
  PolySelection sel;  // selection at begin
  json op;
  QString label;
};

LiveState& liveState() {
  static LiveState s;
  return s;
}

json matrixToJson(const Mat4& m) {
  json a = json::array();
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) a.push_back(m.m[r][c]);
  return a;
}

QColor softColor(float w) {
  // Soft selection gradient: blue (0) -> green -> yellow -> orange -> red (1).
  w = clampf(w, 0.0f, 1.0f);
  struct Stop {
    float t;
    int r, g, b;
  };
  static const Stop stops[] = {{0.0f, 40, 60, 255}, {0.25f, 40, 220, 80}, {0.5f, 240, 240, 40}, {0.75f, 255, 150, 30},
                               {1.0f, 255, 30, 30}};
  for (int i = 1; i < 5; ++i)
    if (w <= stops[i].t) {
      float f = (w - stops[i - 1].t) / (stops[i].t - stops[i - 1].t);
      return QColor(static_cast<int>(stops[i - 1].r + (stops[i].r - stops[i - 1].r) * f),
                    static_cast<int>(stops[i - 1].g + (stops[i].g - stops[i - 1].g) * f),
                    static_cast<int>(stops[i - 1].b + (stops[i].b - stops[i - 1].b) * f));
    }
  return QColor(255, 30, 30);
}

bool rayTriangle(const Ray& r, const Vec3& a, const Vec3& b, const Vec3& c, float& t) {
  Vec3 e1 = b - a, e2 = c - a;
  Vec3 p = cross(r.d, e2);
  float det = dot(e1, p);
  if (std::fabs(det) < 1e-12f) return false;
  float inv = 1.0f / det;
  Vec3 s = r.o - a;
  float u = dot(s, p) * inv;
  if (u < 0.0f || u > 1.0f) return false;
  Vec3 q = cross(s, e1);
  float v = dot(r.d, q) * inv;
  if (v < 0.0f || u + v > 1.0f) return false;
  t = dot(e2, q) * inv;
  return t > 1e-6f;
}

float segmentDistance(float px, float py, float ax, float ay, float bx, float by, float& t) {
  float dx = bx - ax, dy = by - ay;
  float len2 = dx * dx + dy * dy;
  t = len2 > 1e-9f ? clampf(((px - ax) * dx + (py - ay) * dy) / len2, 0.0f, 1.0f) : 0.0f;
  return std::hypot(ax + dx * t - px, ay + dy * t - py);
}

bool segmentCrossesRect(const QRectF& r, const QPointF& a, const QPointF& b) {
  if (r.contains(a) || r.contains(b)) return true;
  auto cross = [](const QPointF& p, const QPointF& q, const QPointF& s, const QPointF& e) {
    auto orient = [](const QPointF& o, const QPointF& x, const QPointF& y) {
      return (x.x() - o.x()) * (y.y() - o.y()) - (x.y() - o.y()) * (y.x() - o.x());
    };
    double d1 = orient(s, e, p), d2 = orient(s, e, q), d3 = orient(p, q, s), d4 = orient(p, q, e);
    return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0));
  };
  QPointF c[4] = {r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft()};
  for (int i = 0; i < 4; ++i)
    if (cross(a, b, c[i], c[(i + 1) % 4])) return true;
  return false;
}

// Per-call cache of world/screen positions.
struct Projected {
  std::vector<Vec3> world;
  std::vector<QPointF> screen;
  std::vector<char> visible;  // in front of the camera
  std::vector<float> depth;
};

Projected project(Viewport& vp, const PolyMesh& m, const Mat4& ow) {
  Projected p;
  size_t n = m.positions.size();
  p.world.resize(n);
  p.screen.resize(n);
  p.visible.assign(n, 0);
  p.depth.assign(n, 0.0f);
  const ViewCamera& cam = vp.camera();
  for (size_t i = 0; i < n; ++i) {
    p.world[i] = ow.transformPoint(m.positions[i]);
    float x, y, d;
    if (cam.project(p.world[i], x, y, &d)) {
      p.screen[i] = QPointF(x, y);
      p.visible[i] = 1;
      p.depth[i] = d;
    }
  }
  return p;
}

Vec3 worldFaceNormal(const Projected& pr, const PolyMesh::Face& f) {
  Vec3 n;
  size_t k = f.v.size();
  for (size_t i = 0; i < k; ++i) {
    const Vec3& a = pr.world[f.v[i]];
    const Vec3& b = pr.world[f.v[(i + 1) % k]];
    n.x += (a.y - b.y) * (a.z + b.z);
    n.y += (a.z - b.z) * (a.x + b.x);
    n.z += (a.x - b.x) * (a.y + b.y);
  }
  return n;
}

bool faceFrontWorld(const ViewCamera& cam, const Projected& pr, const PolyMesh::Face& f) {
  Vec3 n = worldFaceNormal(pr, f);
  Vec3 c;
  for (int v : f.v) c += pr.world[v];
  c /= static_cast<float>(std::max<size_t>(1, f.v.size()));
  Vec3 toEye = cam.orthographic() ? -cam.forward() : cam.eye() - c;
  return dot(n, toEye) > 0.0f;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// PolyTarget

Node* PolyTarget::node() const { return doc_ ? doc_->scene().findNode(nodeId_) : nullptr; }

EditablePolyObject* PolyTarget::editablePoly() const {
  if (stackIndex_ >= 0) return nullptr;
  Node* n = node();
  return n ? dynamic_cast<EditablePolyObject*>(n->object.get()) : nullptr;
}

SubObjectModifier* PolyTarget::modifier() const {
  if (stackIndex_ < 0) return nullptr;
  Node* n = node();
  if (!n || stackIndex_ >= static_cast<int>(n->modifiers.size())) return nullptr;
  return dynamic_cast<SubObjectModifier*>(n->modifiers[stackIndex_].get());
}

bool PolyTarget::canEdit() const {
  if (editablePoly()) return true;
  SubObjectModifier* m = modifier();
  return m && m->canEdit();
}

const PolyMesh* PolyTarget::mesh() const {
  if (EditablePolyObject* ep = editablePoly()) return &ep->mesh;
  if (SubObjectModifier* m = modifier()) {
    node()->mesh();  // evaluates the stack (fills the modifier's cache)
    return m->cacheValid ? &m->cachedMesh : nullptr;
  }
  return nullptr;
}

PolySelection PolyTarget::selection() const {
  if (EditablePolyObject* ep = editablePoly()) return ep->sel;
  if (SubObjectModifier* m = modifier()) {
    node()->mesh();
    return m->selection();
  }
  return PolySelection();
}

Mat4 PolyTarget::objectToWorld() const {
  Node* n = node();
  return n ? n->objectWorldMatrix() : Mat4();
}

void PolyTarget::writeSelection(const PolySelection& s) const {
  if (EditablePolyObject* ep = editablePoly()) ep->sel = s;
  else if (SubObjectModifier* m = modifier()) m->setSelection(s);
}

void PolyTarget::notify() const {
  if (!doc_) return;
  doc_->notifyNodesChanged({nodeId_}, ChangeGeometry);
  if (state_) state_->requestRedraw();
}

void PolyTarget::showMessage(const QString& text) const {
  if (state_) state_->setPrompt(text);
}

void PolyTarget::setSelection(const PolySelection& s, const QString& label) {
  if (!valid()) return;
  LiveState& live = liveState();
  if (live.active && live.node == nodeId_ && live.stack == stackIndex_) {
    // Selection changed while a caddy is open: re-run the operation on the new selection.
    live.sel = s;
    if (EditablePolyObject* ep = editablePoly()) {
      ep->mesh = live.mesh;
      ep->sel = s;
    } else {
      writeSelection(s);
    }
    if (!live.op.is_null()) updateLive(live.op);
    else notify();
    return;
  }
  doc_->beginEdit(label);
  writeSelection(s);
  notify();
  doc_->endEdit();
}

void PolyTarget::setLevel(int level) {
  if (!valid()) return;
  PolySelection s = selection();
  if (s.level == level) return;
  s.level = level;
  writeSelection(s);
  notify();
}

void PolyTarget::setSoftSelection(const SoftSelection& soft) {
  if (!valid()) return;
  PolySelection s = selection();
  if (s.soft == soft) return;
  s.soft = soft;
  doc_->softEdit(QObject::tr("Soft Selection"), QString("polysoft/%1/%2").arg(nodeId_).arg(stackIndex_));
  writeSelection(s);
  notify();
}

bool PolyTarget::run(const json& op, const QString& label) {
  if (!valid()) return false;
  if (!canEdit()) {
    showMessage(QObject::tr("This modifier only selects; add an Edit Poly modifier to edit"));
    return false;
  }
  std::string msg;
  if (EditablePolyObject* ep = editablePoly()) {
    PolyMesh m = ep->mesh;
    PolySelection s = ep->sel;
    if (!runPolyOp(m, s, op, &msg)) {
      showMessage(QString::fromStdString(msg));
      return false;
    }
    doc_->beginEdit(label);
    ep->mesh = std::move(m);
    ep->sel = s;
    notify();
    doc_->endEdit();
  } else if (auto* mod = dynamic_cast<EditPolyModifier*>(modifier())) {
    const PolyMesh* cur = mesh();
    if (cur) {
      PolyMesh m = *cur;
      PolySelection s = mod->selection();
      if (!runPolyOp(m, s, op, &msg)) {
        showMessage(QString::fromStdString(msg));
        return false;
      }
    }
    doc_->beginEdit(label);
    mod->record(op);
    notify();
    doc_->endEdit();
  } else {
    return false;
  }
  RepeatLast::get().op = op;
  RepeatLast::get().label = label;
  showMessage(label);
  return true;
}

// Live previews change the scene without an open undo step; the result is committed as one
// step at the end (so an undo/redo in between simply drops the preview).
void PolyTarget::beginLive(const QString& label) {
  LiveState& live = liveState();
  if (live.active) {
    PolyTarget other(doc_, state_, live.node, live.stack);
    other.endLive(true);
  }
  if (!valid()) return;
  live = LiveState();
  live.active = true;
  live.node = nodeId_;
  live.stack = stackIndex_;
  live.label = label;
  if (EditablePolyObject* ep = editablePoly()) {
    live.mesh = ep->mesh;
    live.sel = ep->sel;
  } else {
    live.sel = selection();
  }
}

bool PolyTarget::live() const {
  const LiveState& live = liveState();
  return live.active && live.node == nodeId_ && live.stack == stackIndex_;
}

bool PolyTarget::updateLive(const json& op) {
  LiveState& live = liveState();
  if (!this->live() || !valid()) return false;
  live.op = op;
  bool ok = true;
  std::string msg;
  if (EditablePolyObject* ep = editablePoly()) {
    PolyMesh m = live.mesh;
    PolySelection s = live.sel;
    ok = runPolyOp(m, s, op, &msg);
    ep->mesh = ok ? std::move(m) : live.mesh;
    ep->sel = ok ? s : live.sel;
  } else if (auto* mod = dynamic_cast<EditPolyModifier*>(modifier())) {
    mod->setPreview(op);
  }
  if (!ok) showMessage(QString::fromStdString(msg));
  notify();
  return ok;
}

void PolyTarget::endLive(bool commit) {
  LiveState& live = liveState();
  if (!this->live()) return;
  live.active = false;
  json op = live.op;
  live.op = json();
  bool hasOp = !op.is_null();
  if (EditablePolyObject* ep = editablePoly()) {
    PolyMesh result = ep->mesh;
    PolySelection resultSel = ep->sel;
    ep->mesh = std::move(live.mesh);
    ep->sel = live.sel;
    if (commit && hasOp) {
      doc_->beginEdit(live.label);
      ep->mesh = std::move(result);
      ep->sel = resultSel;
      notify();
      doc_->endEdit();
    } else {
      notify();
    }
  } else if (auto* mod = dynamic_cast<EditPolyModifier*>(modifier())) {
    mod->clearPreview();
    if (commit && hasOp) {
      doc_->beginEdit(live.label);
      mod->record(op);
      notify();
      doc_->endEdit();
    } else {
      notify();
    }
  }
  live.mesh = PolyMesh();
  if (commit && hasOp && op.value("op", std::string()) != "transform") {
    RepeatLast::get().op = op;
    RepeatLast::get().label = live.label;
  }
}

void abandonPolyLive() {
  LiveState& live = liveState();
  live.active = false;
  live.op = json();
  live.mesh = PolyMesh();
}

void drawGizmoOverlay(QPainter& p, const ViewCamera& cam, const Gizmo& gizmo, GizmoHandle hot) {
  LineBatch g;
  gizmo.draw(g, cam, hot);
  auto toPx = [&](const LineVertex& lv, QPointF& out) {
    float x, y;
    if (!cam.project(Vec3(lv.x, lv.y, lv.z), x, y)) return false;
    out = QPointF(x, y);
    return true;
  };
  p.save();
  p.setRenderHint(QPainter::Antialiasing, true);
  for (size_t i = 0; i + 2 < g.tris.size(); i += 3) {
    QPointF a, b, c;
    if (!toPx(g.tris[i], a) || !toPx(g.tris[i + 1], b) || !toPx(g.tris[i + 2], c)) continue;
    const LineVertex& lv = g.tris[i];
    QPainterPath path;
    path.moveTo(a);
    path.lineTo(b);
    path.lineTo(c);
    path.closeSubpath();
    p.fillPath(path, QColor::fromRgbF(lv.r, lv.g, lv.b, lv.a));
  }
  for (size_t i = 0; i + 1 < g.lines.size(); i += 2) {
    QPointF a, b;
    if (!toPx(g.lines[i], a) || !toPx(g.lines[i + 1], b)) continue;
    const LineVertex& lv = g.lines[i];
    p.setPen(QPen(QColor::fromRgbF(lv.r, lv.g, lv.b, lv.a), 1.6));
    p.drawLine(a, b);
  }
  p.restore();
}

std::shared_ptr<PolyEditTool> activePolyTool(EditorState* state) {
  return state ? std::dynamic_pointer_cast<PolyEditTool>(state->viewportToolShared()) : nullptr;
}

// ---------------------------------------------------------------------------------------------
// Picking

bool PolyEditTool::facesFront(Viewport& vp, int face) const {
  const PolyMesh* m = target_.mesh();
  if (!m || face < 0 || face >= static_cast<int>(m->faces.size())) return false;
  Projected pr;
  Mat4 ow = target_.objectToWorld();
  pr.world.resize(m->positions.size());
  for (int v : m->faces[face].v) pr.world[v] = ow.transformPoint(m->positions[v]);
  return faceFrontWorld(vp.camera(), pr, m->faces[face]);
}

bool PolyEditTool::vertFront(Viewport& vp, int v) const {
  const PolyMesh* m = target_.mesh();
  if (!m) return false;
  for (size_t f = 0; f < m->faces.size(); ++f)
    if (std::find(m->faces[f].v.begin(), m->faces[f].v.end(), v) != m->faces[f].v.end() &&
        facesFront(vp, static_cast<int>(f)))
      return true;
  return false;
}

int PolyEditTool::pickVertex(Viewport& vp, float px, float py, float radius) const {
  const PolyMesh* m = target_.mesh();
  if (!m) return -1;
  PolySelection sel = target_.selection();
  Projected pr = project(vp, *m, target_.objectToWorld());
  // Vertices of visible polygons, and which of those face the viewer.
  std::vector<char> used(m->positions.size(), 0), front(m->positions.size(), 0);
  bool ignoreBack = PolyToolSettings::get().ignoreBackfacing;
  for (size_t f = 0; f < m->faces.size(); ++f) {
    if (sel.isFaceHidden(static_cast<int>(f))) continue;
    bool fr = !ignoreBack || faceFrontWorld(vp.camera(), pr, m->faces[f]);
    for (int v : m->faces[f].v) {
      used[v] = 1;
      if (fr) front[v] = 1;
    }
  }
  int best = -1;
  float bestD = radius, bestDepth = kInfinity;
  for (size_t v = 0; v < m->positions.size(); ++v) {
    if (!used[v] || !front[v] || !pr.visible[v] || sel.isVertHidden(static_cast<int>(v))) continue;
    float d = static_cast<float>(std::hypot(pr.screen[v].x() - px, pr.screen[v].y() - py));
    if (d < bestD - 0.5f || (d <= bestD + 0.5f && pr.depth[v] < bestDepth)) {
      if (d > radius) continue;
      best = static_cast<int>(v);
      bestD = std::min(bestD, d);
      bestDepth = pr.depth[v];
    }
  }
  return best;
}

bool PolyEditTool::pickEdge(Viewport& vp, float px, float py, uint64_t& edge, float& t, float radius) const {
  const PolyMesh* m = target_.mesh();
  if (!m) return false;
  PolySelection sel = target_.selection();
  Projected pr = project(vp, *m, target_.objectToWorld());
  bool ignoreBack = PolyToolSettings::get().ignoreBackfacing;
  bool borderOnly = sel.level == PolyBorder;
  std::map<uint64_t, int> uses;  // edge -> visible faces using it
  std::map<uint64_t, bool> anyFront;
  for (size_t f = 0; f < m->faces.size(); ++f) {
    const auto& face = m->faces[f];
    bool hidden = sel.isFaceHidden(static_cast<int>(f));
    bool fr = !ignoreBack || faceFrontWorld(vp.camera(), pr, face);
    for (size_t k = 0; k < face.v.size(); ++k) {
      uint64_t key = edgeKey(face.v[k], face.v[(k + 1) % face.v.size()]);
      uses[key] += hidden ? 0 : 1;
      if (!hidden && fr) anyFront[key] = true;
      if (borderOnly) uses[key] += 0;
    }
  }
  // Border level only picks open edges (used by exactly one polygon).
  std::map<uint64_t, int> faceCount;
  if (borderOnly) {
    for (const auto& face : m->faces)
      for (size_t k = 0; k < face.v.size(); ++k) faceCount[edgeKey(face.v[k], face.v[(k + 1) % face.v.size()])]++;
  }
  float best = radius;
  bool found = false;
  for (const auto& [key, n] : uses) {
    if (n == 0 || !anyFront.count(key)) continue;
    if (borderOnly && faceCount[key] != 1) continue;
    int a = edgeKeyA(key), b = edgeKeyB(key);
    if (!pr.visible[a] || !pr.visible[b]) continue;
    float tt;
    float d = segmentDistance(px, py, static_cast<float>(pr.screen[a].x()), static_cast<float>(pr.screen[a].y()),
                              static_cast<float>(pr.screen[b].x()), static_cast<float>(pr.screen[b].y()), tt);
    if (d < best) {
      best = d;
      edge = key;
      t = tt;
      found = true;
    }
  }
  return found;
}

int PolyEditTool::pickFace(Viewport& vp, float px, float py, Vec3* worldHit) const {
  const PolyMesh* m = target_.mesh();
  if (!m) return -1;
  PolySelection sel = target_.selection();
  Mat4 ow = target_.objectToWorld();
  Mat4 inv = ow.inverse();
  Ray wr = vp.camera().rayAt(px, py);
  Ray r{inv.transformPoint(wr.o), inv.transformDir(wr.d)};
  bool ignoreBack = PolyToolSettings::get().ignoreBackfacing;
  float bestT = kInfinity;
  int best = -1;
  for (size_t f = 0; f < m->faces.size(); ++f) {
    if (sel.isFaceHidden(static_cast<int>(f))) continue;
    const auto& face = m->faces[f];
    if (ignoreBack && dot(m->faceNormal(face), r.d) > 0.0f) continue;
    std::vector<int> tris = faceTriangles(*m, face);
    for (size_t i = 0; i + 2 < tris.size(); i += 3) {
      float t;
      if (rayTriangle(r, m->positions[face.v[tris[i]]], m->positions[face.v[tris[i + 1]]],
                      m->positions[face.v[tris[i + 2]]], t) &&
          t < bestT) {
        bestT = t;
        best = static_cast<int>(f);
      }
    }
  }
  if (best >= 0 && worldHit) *worldHit = ow.transformPoint(r.o + r.d * bestT);
  return best;
}

PolyEditTool::Hit PolyEditTool::pick(Viewport& vp, float px, float py, int level) const {
  Hit h;
  const PolyMesh* m = target_.mesh();
  if (!m) return h;
  Mat4 ow = target_.objectToWorld();
  if (level == PolyVertex) {
    h.vert = pickVertex(vp, px, py);
    if (h.vert >= 0) h.world = ow.transformPoint(m->positions[h.vert]);
  } else if (level == PolyEdge || level == PolyBorder) {
    float t;
    uint64_t e;
    if (pickEdge(vp, px, py, e, t)) {
      h.edge = e;
      h.hasEdge = true;
      h.edgeT = t;
      h.world = ow.transformPoint(lerp(m->positions[edgeKeyA(e)], m->positions[edgeKeyB(e)], t));
    }
  } else if (level == PolyFace || level == PolyElement) {
    h.face = pickFace(vp, px, py, &h.world);
  }
  return h;
}

namespace {

template <class T>
void combine(std::vector<T>& list, std::vector<T> items, bool ctrl, bool alt, bool toggle) {
  sortUnique(items);
  if (alt) {
    std::vector<T> out;
    std::set_difference(list.begin(), list.end(), items.begin(), items.end(), std::back_inserter(out));
    list = out;
  } else if (ctrl) {
    bool allIn = !items.empty() && std::includes(list.begin(), list.end(), items.begin(), items.end());
    if (toggle && allIn) {
      std::vector<T> out;
      std::set_difference(list.begin(), list.end(), items.begin(), items.end(), std::back_inserter(out));
      list = out;
    } else {
      list.insert(list.end(), items.begin(), items.end());
      sortUnique(list);
    }
  } else {
    list = items;
  }
}

}  // namespace

void PolyEditTool::clickSelect(Viewport& vp, float px, float py, Qt::KeyboardModifiers mods, bool doubleClick) {
  const PolyMesh* m = target_.mesh();
  if (!m) return;
  PolySelection s = target_.selection();
  const PolySelection before = s;
  bool ctrl = mods & Qt::ControlModifier, alt = mods & Qt::AltModifier;
  const PolyToolSettings& opt = PolyToolSettings::get();
  int level = s.level;
  auto edgesAtVertex = [&](int v) {
    std::vector<uint64_t> out;
    for (const auto& f : m->faces)
      for (size_t k = 0; k < f.v.size(); ++k) {
        int a = f.v[k], b = f.v[(k + 1) % f.v.size()];
        if (a == v || b == v) out.push_back(edgeKey(a, b));
      }
    return out;
  };
  auto facesAtVertex = [&](int v) {
    std::vector<int> out;
    for (size_t f = 0; f < m->faces.size(); ++f)
      if (std::find(m->faces[f].v.begin(), m->faces[f].v.end(), v) != m->faces[f].v.end() &&
          !s.isFaceHidden(static_cast<int>(f)))
        out.push_back(static_cast<int>(f));
    return out;
  };
  bool picked = false;
  if (level == PolyVertex) {
    int v = pickVertex(vp, px, py);
    if (v >= 0) {
      combine(s.verts, std::vector<int>{v}, ctrl, alt, true);
      picked = true;
    }
  } else if (level == PolyEdge || level == PolyBorder) {
    std::vector<uint64_t> items;
    if (opt.byVertex && level == PolyEdge) {
      int v = pickVertex(vp, px, py);
      if (v >= 0) items = edgesAtVertex(v);
    } else {
      uint64_t e;
      float t;
      if (pickEdge(vp, px, py, e, t)) {
        if (level == PolyBorder) {
          items = borderLoopOf(*m, edgeKeyA(e), edgeKeyB(e));
        } else if (doubleClick) {
          PolySelection loop;
          loop.level = PolyEdge;
          loop.edges = {e};
          selectLoop(*m, loop);
          items = loop.edges;
        } else {
          items = {e};
        }
      }
    }
    if (!items.empty()) {
      combine(s.edges, items, ctrl, alt, true);
      picked = true;
    }
  } else if (level == PolyFace || level == PolyElement) {
    std::vector<int> items;
    if (opt.byVertex) {
      int v = pickVertex(vp, px, py);
      if (v >= 0) items = facesAtVertex(v);
    } else {
      int f = pickFace(vp, px, py);
      if (f >= 0) {
        if (level == PolyElement || doubleClick) items = elementOf(*m, f);
        else if (opt.byAngle) items = facesByAngle(*m, f, opt.angle);
        else items = {f};
      }
    }
    if (level == PolyElement && opt.byVertex && !items.empty()) items = elementOf(*m, items[0]);
    // Hidden polygons cannot be selected.
    items.erase(std::remove_if(items.begin(), items.end(), [&](int f) { return s.isFaceHidden(f); }), items.end());
    if (!items.empty()) {
      combine(s.faces, items, ctrl, alt, true);
      picked = true;
    }
  }
  if (!picked && !ctrl && !alt) s.clearLevel(level);
  s.normalize();
  if (s.verts != before.verts || s.edges != before.edges || s.faces != before.faces) target_.setSelection(s, QObject::tr("Select"));
}

void PolyEditTool::regionSelect(Viewport& vp, const QRect& rect, Qt::KeyboardModifiers mods) {
  const PolyMesh* m = target_.mesh();
  if (!m) return;
  PolySelection s = target_.selection();
  const PolySelection before = s;
  bool ctrl = mods & Qt::ControlModifier, alt = mods & Qt::AltModifier;
  QRectF r(rect);
  Projected pr = project(vp, *m, target_.objectToWorld());
  bool ignoreBack = PolyToolSettings::get().ignoreBackfacing;
  std::vector<char> front(m->faces.size(), 1);
  if (ignoreBack)
    for (size_t f = 0; f < m->faces.size(); ++f) front[f] = faceFrontWorld(vp.camera(), pr, m->faces[f]) ? 1 : 0;
  auto inside = [&](int v) { return pr.visible[v] && r.contains(pr.screen[v]); };
  int level = s.level;
  if (level == PolyVertex) {
    std::vector<char> ok(m->positions.size(), 0);
    for (size_t f = 0; f < m->faces.size(); ++f)
      if (!s.isFaceHidden(static_cast<int>(f)) && front[f])
        for (int v : m->faces[f].v) ok[v] = 1;
    std::vector<int> items;
    for (size_t v = 0; v < m->positions.size(); ++v)
      if (ok[v] && !s.isVertHidden(static_cast<int>(v)) && inside(static_cast<int>(v))) items.push_back(static_cast<int>(v));
    combine(s.verts, items, ctrl, alt, false);
  } else if (level == PolyEdge || level == PolyBorder) {
    std::map<uint64_t, int> count;
    std::set<uint64_t> frontEdges;
    for (size_t f = 0; f < m->faces.size(); ++f) {
      const auto& face = m->faces[f];
      for (size_t k = 0; k < face.v.size(); ++k) {
        uint64_t key = edgeKey(face.v[k], face.v[(k + 1) % face.v.size()]);
        count[key]++;
        if (front[f] && !s.isFaceHidden(static_cast<int>(f))) frontEdges.insert(key);
      }
    }
    std::vector<uint64_t> items;
    for (uint64_t key : frontEdges) {
      int a = edgeKeyA(key), b = edgeKeyB(key);
      if (level == PolyBorder && count[key] != 1) continue;
      if (!pr.visible[a] || !pr.visible[b]) continue;
      if (segmentCrossesRect(r, pr.screen[a], pr.screen[b])) items.push_back(key);
    }
    if (level == PolyBorder) {
      std::vector<uint64_t> loops;
      for (uint64_t key : items) {
        auto l = borderLoopOf(*m, edgeKeyA(key), edgeKeyB(key));
        loops.insert(loops.end(), l.begin(), l.end());
      }
      items = loops;
    }
    combine(s.edges, items, ctrl, alt, false);
  } else if (level == PolyFace || level == PolyElement) {
    std::vector<int> items;
    for (size_t f = 0; f < m->faces.size(); ++f) {
      if (s.isFaceHidden(static_cast<int>(f)) || !front[f]) continue;
      const auto& face = m->faces[f];
      bool hit = false;
      QPointF c;
      int vis = 0;
      for (int v : face.v) {
        if (inside(v)) hit = true;
        if (pr.visible[v]) {
          c += pr.screen[v];
          ++vis;
        }
      }
      if (!hit && vis > 0 && r.contains(c / vis)) hit = true;
      if (hit) items.push_back(static_cast<int>(f));
    }
    if (level == PolyElement) {
      std::vector<int> all;
      std::set<int> done;
      for (int f : items)
        if (!done.count(f)) {
          auto el = elementOf(*m, f);
          done.insert(el.begin(), el.end());
          all.insert(all.end(), el.begin(), el.end());
        }
      items = all;
    }
    combine(s.faces, items, ctrl, alt, false);
  }
  s.normalize();
  if (s.verts != before.verts || s.edges != before.edges || s.faces != before.faces) target_.setSelection(s, QObject::tr("Region Select"));
}

Vec3 PolyEditTool::selectionCenterWorld() const {
  const PolyMesh* m = target_.mesh();
  if (!m) return Vec3();
  PolySelection s = target_.selection();
  Mat4 ow = target_.objectToWorld();
  Aabb b;
  for (int v : selectedVertsAtLevel(*m, s))
    if (v >= 0 && v < static_cast<int>(m->positions.size())) b.expand(ow.transformPoint(m->positions[v]));
  return b.valid() ? b.center() : ow.translation();
}

// ---------------------------------------------------------------------------------------------
// Transforms

bool PolyEditTool::gizmoShown(Viewport& vp) const {
  EditorState* st = vp.editorState();
  if (st->creating() || st->tool() == TransformTool::Select || mode_ != PolyPickMode::None) return false;
  if (!target_.canEdit()) return false;
  const PolyMesh* m = target_.mesh();
  return m && !selectedVertsAtLevel(*m, target_.selection()).empty();
}

void PolyEditTool::setupGizmo(Viewport& vp) {
  Vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  TransformTool tool = vp.editorState()->tool();
  if (tool != TransformTool::Move) {
    Mat4 m = target_.node() ? target_.node()->worldMatrix() : Mat4();
    for (int i = 0; i < 3; ++i) axes[i] = normalize(m.column(i));
  }
  gizmo_.setup(vp.camera(), tool, transforming_ ? dragCenter_ : selectionCenterWorld(), axes);
}

void PolyEditTool::beginTransform(Viewport& vp, GizmoHandle h, float px, float py) {
  TransformTool tool = vp.editorState()->tool();
  setupGizmo(vp);
  dragCenter_ = gizmo_.origin;
  dragValue_ = Vec3();
  QString label = tool == TransformTool::Move     ? QObject::tr("Move")
                  : tool == TransformTool::Rotate ? QObject::tr("Rotate")
                                                  : QObject::tr("Scale");
  target_.beginLive(label);
  gizmoDrag_.begin(vp.camera(), gizmo_, h, px, py);
  if (tool != TransformTool::Rotate) vp.editorState()->setConstraint(h);
  transforming_ = true;
}

void PolyEditTool::updateTransform(Viewport& vp, float px, float py) {
  EditorState* st = vp.editorState();
  TransformTool tool = st->tool();
  const ViewCamera& cam = vp.camera();
  Mat4 w;
  if (tool == TransformTool::Move) {
    Vec3 d = gizmoDrag_.translation(cam, px, py);
    Vec3 snapped;
    if (st->snapPoint(vp, px, py, dragCenter_ + d, snapped)) d = snapped - dragCenter_;
    dragValue_ = d;
    w = Mat4::translate(d);
  } else if (tool == TransformTool::Rotate) {
    Vec3 axis;
    float angle = st->snapAngle(gizmoDrag_.rotation(cam, px, py, axis));
    dragValue_ = Vec3(degrees(angle), 0, 0);
    w = Mat4::translate(dragCenter_) * Mat4::rotateAxis(axis, angle) * Mat4::translate(-dragCenter_);
  } else {
    Vec3 f = st->snapPercent(gizmoDrag_.scaling(cam, px, py));
    dragValue_ = f * 100.0f;
    Mat4 basis;
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 3; ++c) basis.m[r][c] = gizmo_.axes[c][r];
    w = Mat4::translate(dragCenter_) * basis * Mat4::scale(f) * basis.transposed() * Mat4::translate(-dragCenter_);
  }
  Mat4 ow = target_.objectToWorld();
  Mat4 local = ow.inverse() * w * ow;
  target_.updateLive({{"op", "transform"}, {"matrix", matrixToJson(local)}});
}

// ---------------------------------------------------------------------------------------------
// Events

void PolyEditTool::setPickMode(PolyPickMode m) {
  if (mode_ == m) return;
  mode_ = m;
  pickVert_ = -1;
  pickEdgeKey_ = 0;
  createVerts_.clear();
  cutActive_ = false;
  sliceStarted_ = false;
  hoverValid_ = false;
  if (target_.state()) {
    target_.state()->setPrompt(prompt());
    target_.state()->requestRedraw();
  }
}

void PolyEditTool::finishMode() {
  bool was = mode_ != PolyPickMode::None;
  setPickMode(PolyPickMode::None);
  if (was && onPickModeEnded) onPickModeEnded();
}

QString PolyEditTool::prompt() const {
  switch (mode_) {
    case PolyPickMode::TargetWeld:
      return pickVert_ >= 0 || pickEdgeKey_ ? QObject::tr("Target Weld: click the target") : QObject::tr("Target Weld: click the source");
    case PolyPickMode::InsertVertex: return QObject::tr("Insert Vertex: click on edges (right-click to end)");
    case PolyPickMode::Create: return QObject::tr("Create: click vertices in order; click the first one again to close");
    case PolyPickMode::Cut: return QObject::tr("Cut: click points on edges or vertices (right-click to end)");
    case PolyPickMode::QuickSlice: return QObject::tr("QuickSlice: click two points to slice across the view");
    case PolyPickMode::SwiftLoop: return QObject::tr("Swift Loop: click an edge to insert a loop");
    case PolyPickMode::HingePick: return QObject::tr("Pick the hinge edge");
    case PolyPickMode::Attach: return QObject::tr("Attach: click objects to attach (right-click to end)");
    default: break;
  }
  static const char* names[] = {"", "Vertex", "Edge", "Border", "Polygon", "Element"};
  int l = target_.valid() ? target_.level() : 0;
  return QObject::tr("%1 sub-object level: click or drag to select, Ctrl adds, Alt removes").arg(names[std::clamp(l, 0, 5)]);
}

bool PolyEditTool::cancel() {
  if (cancelPolyCaddy()) return true;
  if (transforming_) {
    transforming_ = false;
    target_.endLive(false);
    return true;
  }
  if (mode_ != PolyPickMode::None) {
    if (mode_ == PolyPickMode::Create && createVerts_.size() >= 3) finishCreate();
    finishMode();
    return true;
  }
  if (pressed_) {
    pressed_ = false;
    region_ = false;
    return true;
  }
  return false;
}

bool PolyEditTool::deleteSelection() {
  if (!target_.valid()) return false;
  const PolyMesh* m = target_.mesh();
  PolySelection s = target_.selection();
  if (!m || s.level == PolyObject) return false;
  if (selectedVertsAtLevel(*m, s).empty()) {
    target_.showMessage(QObject::tr("Nothing selected at this sub-object level"));
    return true;
  }
  target_.run({{"op", "delete"}}, QObject::tr("Delete Sub-objects"));
  return true;
}

bool PolyEditTool::mousePress(Viewport& vp, QMouseEvent* e) {
  if (vp.editorState()->creating() || !target_.valid()) return false;
  if (e->button() == Qt::RightButton) {
    if (transforming_) {
      cancel();
      return true;
    }
    if (mode_ != PolyPickMode::None) {
      if (mode_ == PolyPickMode::Create && createVerts_.size() >= 3) finishCreate();
      finishMode();
      return true;
    }
    return false;
  }
  if (e->button() != Qt::LeftButton) return false;
  vp.updateCamera();
  float px = static_cast<float>(e->position().x()), py = static_cast<float>(e->position().y());
  pressPos_ = curPos_ = e->position().toPoint();
  pressMods_ = e->modifiers();
  if (mode_ != PolyPickMode::None) return pickModePress(vp, e);
  bool ctrl = e->modifiers() & Qt::ControlModifier, alt = e->modifiers() & Qt::AltModifier;
  EditorState* st = vp.editorState();
  bool transformTool = st->tool() != TransformTool::Select && target_.canEdit();
  if (transformTool && !ctrl && !alt) {
    if (gizmoShown(vp)) {
      setupGizmo(vp);
      GizmoHandle h = gizmo_.hitTest(vp.camera(), px, py);
      if (h != GizmoHandle::None) {
        beginTransform(vp, h, px, py);
        return true;
      }
    }
    // Clicking a sub-object selects it and drags it right away.
    PolySelection s = target_.selection();
    Hit hit = pick(vp, px, py, s.level);
    bool onSelected = (hit.vert >= 0 && s.hasVert(hit.vert)) || (hit.hasEdge && s.hasEdge(hit.edge)) ||
                      (hit.face >= 0 && s.hasFace(hit.face));
    bool onAny = hit.vert >= 0 || hit.hasEdge || hit.face >= 0;
    if (onAny) {
      if (!onSelected) clickSelect(vp, px, py, Qt::NoModifier, false);
      GizmoHandle h = st->constraint();
      if (st->tool() == TransformTool::Rotate)
        h = handleAxisMask(h) == 1 ? GizmoHandle::X : handleAxisMask(h) == 2 ? GizmoHandle::Y : GizmoHandle::Z;
      beginTransform(vp, h, px, py);
      return true;
    }
  }
  pressed_ = true;
  region_ = false;
  return true;
}

bool PolyEditTool::mouseMove(Viewport& vp, QMouseEvent* e) {
  if (vp.editorState()->creating() || !target_.valid()) return false;
  float px = static_cast<float>(e->position().x()), py = static_cast<float>(e->position().y());
  QPoint pos = e->position().toPoint();
  if (transforming_) {
    updateTransform(vp, px, py);
    return true;
  }
  if (pressed_) {
    curPos_ = pos;
    if ((pos - pressPos_).manhattanLength() > 3) region_ = true;
    return true;
  }
  if (mode_ == PolyPickMode::SwiftLoop || mode_ == PolyPickMode::InsertVertex) {
    uint64_t edge;
    float t;
    bool had = hoverValid_;
    hoverValid_ = pickEdge(vp, px, py, edge, t, 10.0f);
    if (hoverValid_) {
      hoverEdge_ = edge;
      hoverT_ = t;
    }
    curPos_ = pos;
    return hoverValid_ || had;
  }
  if (mode_ == PolyPickMode::Create || mode_ == PolyPickMode::Cut || mode_ == PolyPickMode::QuickSlice ||
      mode_ == PolyPickMode::TargetWeld) {
    curPos_ = pos;
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

bool PolyEditTool::mouseRelease(Viewport& vp, QMouseEvent* e) {
  if (!target_.valid()) return false;
  if (e->button() == Qt::RightButton) return mode_ != PolyPickMode::None;
  if (e->button() != Qt::LeftButton) return false;
  if (transforming_) {
    transforming_ = false;
    target_.endLive(true);
    return true;
  }
  if (pressed_) {
    pressed_ = false;
    if (region_) {
      region_ = false;
      regionSelect(vp, QRect(pressPos_, e->position().toPoint()).normalized(), pressMods_);
    } else {
      clickSelect(vp, static_cast<float>(pressPos_.x()), static_cast<float>(pressPos_.y()), pressMods_, false);
    }
    return true;
  }
  return mode_ != PolyPickMode::None;
}

bool PolyEditTool::mouseDoubleClick(Viewport& vp, QMouseEvent* e) {
  if (vp.editorState()->creating() || !target_.valid() || e->button() != Qt::LeftButton) return false;
  if (mode_ == PolyPickMode::Create) {
    finishCreate();
    return true;
  }
  if (mode_ != PolyPickMode::None) return true;
  int level = target_.level();
  if (level == PolyEdge || level == PolyFace) {
    clickSelect(vp, static_cast<float>(e->position().x()), static_cast<float>(e->position().y()), e->modifiers(), true);
    return true;
  }
  return false;
}

bool PolyEditTool::keyPress(Viewport& vp, QKeyEvent* e) {
  if (!target_.valid() || vp.editorState()->creating()) return false;
  if (e->key() == Qt::Key_Backspace) {
    int level = target_.level();
    if (level == PolyVertex) target_.run({{"op", "removeVerts"}}, QObject::tr("Remove Vertices"));
    else if (level == PolyEdge || level == PolyBorder)
      target_.run({{"op", "removeEdges"}, {"removeVerts", bool(e->modifiers() & Qt::ControlModifier)}},
                  QObject::tr("Remove Edges"));
    return true;
  }
  if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && mode_ == PolyPickMode::Create) {
    finishCreate();
    finishMode();
    return true;
  }
  return false;
}

void PolyEditTool::finishCreate() {
  if (createVerts_.size() >= 3) target_.run({{"op", "createFace"}, {"verts", createVerts_}}, QObject::tr("Create Polygon"));
  createVerts_.clear();
}

bool PolyEditTool::pickModePress(Viewport& vp, QMouseEvent* e) {
  float px = static_cast<float>(e->position().x()), py = static_cast<float>(e->position().y());
  const PolyMesh* m = target_.mesh();
  if (!m) return true;
  int level = target_.level();
  Mat4 ow = target_.objectToWorld();
  switch (mode_) {
    case PolyPickMode::TargetWeld: {
      if (level == PolyVertex) {
        int v = pickVertex(vp, px, py);
        if (v < 0) return true;
        if (pickVert_ < 0) {
          pickVert_ = v;
        } else if (v != pickVert_) {
          target_.run({{"op", "weldPairs"}, {"pairs", json::array({json::array({pickVert_, v})})}}, QObject::tr("Target Weld"));
          pickVert_ = -1;
        }
      } else {
        uint64_t edge;
        float t;
        if (!pickEdge(vp, px, py, edge, t)) return true;
        if (!pickEdgeKey_) {
          pickEdgeKey_ = edge;
        } else if (edge != pickEdgeKey_) {
          int a0 = edgeKeyA(pickEdgeKey_), b0 = edgeKeyB(pickEdgeKey_), a1 = edgeKeyA(edge), b1 = edgeKeyB(edge);
          auto d = [&](int x, int y) { return length(m->positions[x] - m->positions[y]); };
          json pairs = d(a0, a1) + d(b0, b1) <= d(a0, b1) + d(b0, a1)
                           ? json::array({json::array({a0, a1}), json::array({b0, b1})})
                           : json::array({json::array({a0, b1}), json::array({b0, a1})});
          target_.run({{"op", "weldPairs"}, {"pairs", pairs}}, QObject::tr("Target Weld"));
          pickEdgeKey_ = 0;
        }
      }
      if (target_.state()) target_.state()->setPrompt(prompt());
      return true;
    }
    case PolyPickMode::InsertVertex: {
      if (level == PolyFace || level == PolyElement) {
        Vec3 hit;
        int f = pickFace(vp, px, py, &hit);
        if (f >= 0)
          target_.run({{"op", "insertVertexInFace"}, {"face", f}, {"point", vec3ToJson(ow.inverse().transformPoint(hit))}},
                      QObject::tr("Insert Vertex"));
      } else {
        uint64_t edge;
        float t;
        if (pickEdge(vp, px, py, edge, t, 10.0f))
          target_.run({{"op", "insertVertex"}, {"a", edgeKeyA(edge)}, {"b", edgeKeyB(edge)}, {"t", t}},
                      QObject::tr("Insert Vertex"));
      }
      return true;
    }
    case PolyPickMode::Create: {
      int v = pickVertex(vp, px, py, 12.0f);
      if (v < 0) {
        target_.showMessage(QObject::tr("Create: click existing vertices"));
        return true;
      }
      if (!createVerts_.empty() && v == createVerts_.front()) {
        finishCreate();
        return true;
      }
      if (std::find(createVerts_.begin(), createVerts_.end(), v) == createVerts_.end()) createVerts_.push_back(v);
      return true;
    }
    case PolyPickMode::Cut: {
      json point;
      int v = pickVertex(vp, px, py, 8.0f);
      uint64_t edge;
      float t;
      if (v >= 0) point = {{"v", v}};
      else if (pickEdge(vp, px, py, edge, t, 8.0f)) point = {{"a", edgeKeyA(edge)}, {"b", edgeKeyB(edge)}, {"t", t}};
      else return true;
      if (!cutActive_) {
        // The start point becomes a vertex right away so following cuts can continue from it.
        if (point.contains("v")) {
          cutFrom_.vert = v;
        } else {
          if (!target_.run({{"op", "insertVertex"}, {"a", point["a"]}, {"b", point["b"]}, {"t", point["t"]}}, QObject::tr("Cut")))
            return true;
          const PolyMesh* mm = target_.mesh();
          cutFrom_.vert = mm ? static_cast<int>(mm->positions.size()) - 1 : -1;
        }
        cutActive_ = cutFrom_.vert >= 0;
        return true;
      }
      if (target_.run({{"op", "cutSegment"}, {"from", {{"v", cutFrom_.vert}}}, {"to", point}}, QObject::tr("Cut"))) {
        const PolyMesh* mm = target_.mesh();
        cutFrom_.vert = point.contains("v") ? v : (mm ? static_cast<int>(mm->positions.size()) - 1 : -1);
      } else if (point.contains("v")) {
        cutFrom_.vert = v;  // start again from here
      }
      return true;
    }
    case PolyPickMode::QuickSlice: {
      if (!sliceStarted_) {
        sliceStarted_ = true;
        sliceAPx_ = e->position().toPoint();
        return true;
      }
      sliceStarted_ = false;
      const ViewCamera& cam = vp.camera();
      Ray ra = cam.rayAt(static_cast<float>(sliceAPx_.x()), static_cast<float>(sliceAPx_.y()));
      Ray rb = cam.rayAt(px, py);
      Vec3 n, p0;
      if (cam.orthographic()) {
        n = cross(rb.o - ra.o, cam.forward());
        p0 = ra.o;
      } else {
        n = cross(ra.d, rb.d);
        p0 = cam.eye();
      }
      if (length2(n) < 1e-12f) return true;
      n = normalize(n);
      Mat4 inv = ow.inverse();
      Vec3 lp = inv.transformPoint(p0);
      // Plane normals transform with the transpose of the object-to-world matrix.
      Vec3 ln = normalize(Vec3(ow.m[0][0] * n.x + ow.m[1][0] * n.y + ow.m[2][0] * n.z,
                               ow.m[0][1] * n.x + ow.m[1][1] * n.y + ow.m[2][1] * n.z,
                               ow.m[0][2] * n.x + ow.m[1][2] * n.y + ow.m[2][2] * n.z));
      PolySelection s = target_.selection();
      bool onlySel = (s.level == PolyFace || s.level == PolyElement) && !s.faces.empty();
      target_.run({{"op", "slice"}, {"point", vec3ToJson(lp)}, {"normal", vec3ToJson(ln)}, {"selectedOnly", onlySel}},
                  QObject::tr("QuickSlice"));
      return true;
    }
    case PolyPickMode::SwiftLoop: {
      uint64_t edge;
      float t;
      if (pickEdge(vp, px, py, edge, t, 10.0f))
        target_.run({{"op", "insertLoop"}, {"a", edgeKeyA(edge)}, {"b", edgeKeyB(edge)}, {"t", t}}, QObject::tr("Swift Loop"));
      return true;
    }
    case PolyPickMode::HingePick: {
      uint64_t edge;
      float t;
      if (pickEdge(vp, px, py, edge, t, 10.0f)) {
        if (onHingePicked) onHingePicked(edgeKeyA(edge), edgeKeyB(edge));
        finishMode();
      }
      return true;
    }
    case PolyPickMode::Attach: {
      uint32_t id = vp.pickNode(px, py);
      Node* self = target_.node();
      Node* other = target_.doc()->scene().findNode(id);
      if (!other || !self || other == self || other->kind() != ObjectKind::Geometry) return true;
      PolyMesh om = other->mesh();
      Mat4 rel = self->objectWorldMatrix().inverse() * other->objectWorldMatrix();
      om.transform(rel);
      if (rel.determinant3x3() < 0.0f) om.flipFaces();
      SceneDocument* doc = target_.doc();
      doc->beginEdit(QObject::tr("Attach"));
      if (target_.run({{"op", "attach"}, {"mesh", polyMeshToJson(om)}}, QObject::tr("Attach"))) {
        doc->scene().removeNode(id);
        doc->notifyStructureChanged();
      }
      doc->endEdit();
      return true;
    }
    case PolyPickMode::None: break;
  }
  return true;
}

// ---------------------------------------------------------------------------------------------
// Drawing

void PolyEditTool::drawLines(Viewport& vp, LineBatch& lines) {
  const PolyMesh* m = target_.mesh();
  if (!m || vp.editorState()->creating()) return;
  PolySelection s = target_.selection();
  Mat4 ow = target_.objectToWorld();
  QColor red(255, 40, 40);
  QColor faint(150, 150, 150, 120);
  bool shaded = vp.shading() == ShadingMode::Shaded;
  // Sub-object levels show the cage of the editable mesh in shaded views.
  if (shaded && !vp.edgedFaces())
    for (size_t f = 0; f < m->faces.size(); ++f) {
      if (s.isFaceHidden(static_cast<int>(f))) continue;
      const auto& face = m->faces[f];
      for (size_t k = 0; k < face.v.size(); ++k) {
        int a = face.v[k], b = face.v[(k + 1) % face.v.size()];
        if (a < b) lines.line(ow.transformPoint(m->positions[a]), ow.transformPoint(m->positions[b]), faint);
      }
    }
  if (s.level == PolyEdge || s.level == PolyBorder) {
    for (uint64_t k : s.edges) {
      int a = edgeKeyA(k), b = edgeKeyB(k);
      if (a >= static_cast<int>(m->positions.size()) || b >= static_cast<int>(m->positions.size())) continue;
      lines.line(ow.transformPoint(m->positions[a]), ow.transformPoint(m->positions[b]), red);
    }
  }
  if (s.level == PolyFace || s.level == PolyElement) {
    QColor tint(255, 30, 30, 90);
    for (int f : s.faces) {
      if (f < 0 || f >= static_cast<int>(m->faces.size()) || s.isFaceHidden(f)) continue;
      const auto& face = m->faces[f];
      std::vector<int> tris = faceTriangles(*m, face);
      for (size_t i = 0; i + 2 < tris.size(); i += 3)
        lines.tri(ow.transformPoint(m->positions[face.v[tris[i]]]), ow.transformPoint(m->positions[face.v[tris[i + 1]]]),
                  ow.transformPoint(m->positions[face.v[tris[i + 2]]]), tint);
      for (size_t k = 0; k < face.v.size(); ++k)
        lines.line(ow.transformPoint(m->positions[face.v[k]]),
                   ow.transformPoint(m->positions[face.v[(k + 1) % face.v.size()]]), red);
    }
  }
  // Slice plane.
  const SlicePlaneState& sp = SlicePlaneState::get();
  if (sp.active && sp.nodeId == target_.nodeId() && sp.stackIndex == target_.stackIndex()) {
    Mat4 r = Mat4::fromEulerDeg(sp.rotation);
    Vec3 u = r.transformDir(Vec3(1, 0, 0)) * sp.size, v = r.transformDir(Vec3(0, 1, 0)) * sp.size;
    Vec3 c[4] = {sp.center - u - v, sp.center + u - v, sp.center + u + v, sp.center - u + v};
    QColor yellow(255, 220, 40);
    for (int i = 0; i < 4; ++i) lines.line(ow.transformPoint(c[i]), ow.transformPoint(c[(i + 1) % 4]), yellow);
    for (int i = 1; i < 4; ++i) {
      float f = i / 4.0f;
      lines.line(ow.transformPoint(lerp(c[0], c[1], f)), ow.transformPoint(lerp(c[3], c[2], f)), QColor(255, 220, 40, 120));
      lines.line(ow.transformPoint(lerp(c[0], c[3], f)), ow.transformPoint(lerp(c[1], c[2], f)), QColor(255, 220, 40, 120));
    }
    lines.tri(ow.transformPoint(c[0]), ow.transformPoint(c[1]), ow.transformPoint(c[2]), QColor(255, 220, 40, 40));
    lines.tri(ow.transformPoint(c[0]), ow.transformPoint(c[2]), ow.transformPoint(c[3]), QColor(255, 220, 40, 40));
  }

  // Swift Loop / Insert Vertex hover preview.
  if (hoverValid_ && (mode_ == PolyPickMode::SwiftLoop || mode_ == PolyPickMode::InsertVertex)) {
    int a = edgeKeyA(hoverEdge_), b = edgeKeyB(hoverEdge_);
    if (a < static_cast<int>(m->positions.size()) && b < static_cast<int>(m->positions.size())) {
      QColor c(80, 220, 255);
      lines.line(ow.transformPoint(m->positions[a]), ow.transformPoint(m->positions[b]), c);
      if (mode_ == PolyPickMode::SwiftLoop) {
        // Preview: connect the corresponding points across the quad ring.
        PolyMesh copy = *m;
        PolySelection tmp;
        size_t before = copy.positions.size();
        if (runPolyOp(copy, tmp, {{"op", "insertLoop"}, {"a", a}, {"b", b}, {"t", hoverT_}}))
          for (uint64_t k : tmp.edges) {
            int x = edgeKeyA(k), y = edgeKeyB(k);
            if (x >= static_cast<int>(before) || y >= static_cast<int>(before))
              lines.line(ow.transformPoint(copy.positions[x]), ow.transformPoint(copy.positions[y]), c);
          }
      }
    }
  }
}

void PolyEditTool::drawOverlay(Viewport& vp, QPainter& p) {
  const PolyMesh* m = target_.mesh();
  if (!m || vp.editorState()->creating()) return;
  vp.updateCamera();
  PolySelection s = target_.selection();
  Projected pr = project(vp, *m, target_.objectToWorld());
  p.setRenderHint(QPainter::Antialiasing, false);
  bool shaded = vp.shading() == ShadingMode::Shaded;
  // Vertex ticks (and soft selection colors at any level).
  bool soft = s.soft.enabled && s.level != PolyObject;
  if (s.level == PolyVertex || soft) {
    std::vector<char> used(m->positions.size(), 0), front(m->positions.size(), 0);
    for (size_t f = 0; f < m->faces.size(); ++f) {
      if (s.isFaceHidden(static_cast<int>(f))) continue;
      bool fr = !shaded || faceFrontWorld(vp.camera(), pr, m->faces[f]);
      for (int v : m->faces[f].v) {
        used[v] = 1;
        if (fr) front[v] = 1;
      }
    }
    std::vector<float> w;
    if (soft) w = softSelectionWeights(*m, s, s.soft);
    for (size_t v = 0; v < m->positions.size(); ++v) {
      if (!used[v] || !front[v] || !pr.visible[v] || s.isVertHidden(static_cast<int>(v))) continue;
      QColor c;
      if (soft && w[v] > 0.0f) c = softColor(w[v]);
      else if (s.level != PolyVertex) continue;
      else c = s.hasVert(static_cast<int>(v)) ? QColor(255, 30, 30) : QColor(40, 80, 255);
      int size = s.level == PolyVertex ? 4 : 3;
      p.fillRect(QRectF(pr.screen[v].x() - size / 2.0, pr.screen[v].y() - size / 2.0, size, size), c);
    }
  }
  p.setRenderHint(QPainter::Antialiasing, true);

  // Gizmo (drawn on top so the mesh never hides it).
  if (gizmoShown(vp)) {
    setupGizmo(vp);
    drawGizmoOverlay(p, vp.camera(), gizmo_, transforming_ ? gizmoDrag_.handle() : hot_);
  }

  // Region rectangle.
  if (pressed_ && region_) {
    p.setPen(QPen(QColor(255, 255, 255), 1, Qt::DashLine));
    p.setBrush(QColor(255, 80, 80, 25));
    p.drawRect(QRect(pressPos_, curPos_).normalized());
    p.setBrush(Qt::NoBrush);
  }

  // Pick-mode feedback.
  p.setPen(QPen(QColor(80, 220, 255), 1.5));
  if (mode_ == PolyPickMode::Create && !createVerts_.empty()) {
    QPolygonF poly;
    for (int v : createVerts_)
      if (v < static_cast<int>(pr.screen.size())) poly << pr.screen[v];
    poly << QPointF(curPos_);
    p.drawPolyline(poly);
  }
  if (mode_ == PolyPickMode::Cut && cutActive_ && cutFrom_.vert >= 0 && cutFrom_.vert < static_cast<int>(pr.screen.size()))
    p.drawLine(pr.screen[cutFrom_.vert], QPointF(curPos_));
  if (mode_ == PolyPickMode::QuickSlice && sliceStarted_) p.drawLine(QPointF(sliceAPx_), QPointF(curPos_));
  if (mode_ == PolyPickMode::TargetWeld) {
    if (pickVert_ >= 0 && pickVert_ < static_cast<int>(pr.screen.size())) p.drawLine(pr.screen[pickVert_], QPointF(curPos_));
    if (pickEdgeKey_) {
      int a = edgeKeyA(pickEdgeKey_), b = edgeKeyB(pickEdgeKey_);
      if (a < static_cast<int>(pr.screen.size()) && b < static_cast<int>(pr.screen.size()))
        p.drawLine(pr.screen[a], pr.screen[b]);
    }
  }

  // Readout while transforming.
  if (transforming_) {
    TransformTool tool = vp.editorState()->tool();
    QString text = tool == TransformTool::Move
                       ? QString("%1, %2, %3").arg(dragValue_.x, 0, 'f', 2).arg(dragValue_.y, 0, 'f', 2).arg(dragValue_.z, 0, 'f', 2)
                   : tool == TransformTool::Rotate ? QString("%1°").arg(dragValue_.x, 0, 'f', 1)
                                                   : QString("%1%").arg((dragValue_.x + dragValue_.y + dragValue_.z) / 3.0f, 0, 'f', 1);
    p.setPen(QColor(255, 230, 120));
    p.drawText(QPointF(curPos_) + QPointF(14, -10), text);
  }

  // Sub-object level badge.
  static const char* names[] = {"", "Vertex", "Edge", "Border", "Polygon", "Element"};
  if (s.level > 0 && s.level <= 5) {
    p.setPen(QColor(255, 200, 80));
    p.drawText(QRectF(8, 24, 300, 18), Qt::AlignLeft | Qt::AlignVCenter,
               QObject::tr("Sub-object: %1").arg(names[s.level]));
  }
}

}  // namespace veraa
