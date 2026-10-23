// Unwrap UVW: Modify panel rollout and the UV editor window (checker background, UV vertex /
// edge / face selection, move / rotate / scale, flatten mapping, planar / box / cylindrical /
// spherical projection of the selection, relax, stitch and pack).
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QToolBar>
#include <QUndoStack>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <set>

#include "commandpanel.h"
#include "document.h"
#include "editorstate.h"
#include "mainwindow.h"
#include "meshmods.h"
#include "paramform.h"
#include "selftest.h"

namespace veraa {

namespace {

struct UnwrapTarget {
  SceneDocument* doc = nullptr;
  uint32_t nodeId = 0;
  int stackIndex = -1;
  Node* node() const { return doc ? doc->scene().findNode(nodeId) : nullptr; }
  UnwrapUvwModifier* mod() const {
    Node* n = node();
    if (!n || stackIndex < 0 || stackIndex >= static_cast<int>(n->modifiers.size())) return nullptr;
    return dynamic_cast<UnwrapUvwModifier*>(n->modifiers[stackIndex].get());
  }
  // Input mesh at the modifier (evaluates the stack).
  const PolyMesh* input() const {
    Node* n = node();
    UnwrapUvwModifier* m = mod();
    if (!n || !m) return nullptr;
    n->mesh();
    return m->lastInputValid ? &m->lastInput : nullptr;
  }
  // Makes sure editable UVs exist for the current input.
  bool ensure() const {
    UnwrapUvwModifier* m = mod();
    const PolyMesh* in = input();
    if (!m || !in) return false;
    if (!m->matches(*in)) m->initFrom(*in);
    return true;
  }
  void notify() const {
    if (UnwrapUvwModifier* m = mod()) m->touch();
    if (doc) doc->notifyNodesChanged({nodeId}, ChangeGeometry);
  }
  // Runs fn on the UV data as one undo step.
  void edit(const QString& label, const std::function<void(UnwrapUvwModifier&, const PolyMesh&)>& fn) const {
    if (!ensure()) return;
    doc->beginEdit(label);
    UnwrapUvwModifier* m = mod();
    PolyMesh in = *input();
    fn(*m, in);
    notify();
    doc->endEdit();
  }
};

enum class UvMode { Vertex, Edge, Face };
enum class UvTool { Move, Rotate, Scale };

class UvCanvas : public QWidget {
 public:
  explicit UvCanvas(const UnwrapTarget& t, QWidget* parent) : QWidget(parent), t_(t) {
    setObjectName("uv_canvas");
    setMinimumSize(420, 420);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
  }

  UvMode mode = UvMode::Face;
  UvTool tool = UvTool::Move;
  std::set<int> selVerts;  // selected UV vertices (derived for edges/faces)
  std::set<int> selFaces;
  std::set<std::pair<int, int>> selEdges;
  std::function<void()> selectionChanged;

  QPointF toPx(const Vec2& uv) const {
    float s = scale();
    return QPointF(width() * 0.5 + (uv.x - center_.x) * s, height() * 0.5 - (uv.y - center_.y) * s);
  }
  Vec2 toUv(const QPointF& p) const {
    float s = scale();
    return Vec2(center_.x + static_cast<float>((p.x() - width() * 0.5) / s), center_.y - static_cast<float>((p.y() - height() * 0.5) / s));
  }
  float scale() const { return std::min(width(), height()) * 0.85f * zoom_; }

  // UV vertices affected by the selection at the current mode.
  std::set<int> activeVerts() const {
    UnwrapUvwModifier* m = t_.mod();
    std::set<int> out;
    if (!m) return out;
    if (mode == UvMode::Vertex) return selVerts;
    if (mode == UvMode::Edge) {
      for (const auto& e : selEdges) {
        out.insert(e.first);
        out.insert(e.second);
      }
      return out;
    }
    for (int f : selFaces)
      if (f >= 0 && f < static_cast<int>(m->faceUvs.size()))
        for (int t : m->faceUvs[f]) out.insert(t);
    return out;
  }

  void selectAll() {
    UnwrapUvwModifier* m = t_.mod();
    if (!m) return;
    selFaces.clear();
    for (size_t f = 0; f < m->faceUvs.size(); ++f) selFaces.insert(static_cast<int>(f));
    selVerts.clear();
    for (size_t i = 0; i < m->uvs.size(); ++i) selVerts.insert(static_cast<int>(i));
    selEdges.clear();
    for (const auto& f : m->faceUvs)
      for (size_t k = 0; k < f.size(); ++k) selEdges.insert(edgeOf(f[k], f[(k + 1) % f.size()]));
    if (selectionChanged) selectionChanged();
    update();
  }
  static std::pair<int, int> edgeOf(int a, int b) { return {std::min(a, b), std::max(a, b)}; }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.fillRect(rect(), QColor(40, 40, 40));
    // Checker in the 0..1 tile.
    QPointF a = toPx(Vec2(0, 1)), b = toPx(Vec2(1, 0));
    QRectF tile(a, b);
    int n = 8;
    for (int i = 0; i < n; ++i)
      for (int j = 0; j < n; ++j) {
        QRectF c(tile.left() + tile.width() * i / n, tile.top() + tile.height() * j / n, tile.width() / n, tile.height() / n);
        p.fillRect(c, (i + j) % 2 ? QColor(150, 150, 150) : QColor(95, 95, 95));
      }
    p.setPen(QPen(QColor(60, 140, 255), 1.5));
    p.drawRect(tile);
    UnwrapUvwModifier* m = t_.mod();
    if (!m) return;
    p.setRenderHint(QPainter::Antialiasing);
    // Selected faces tinted.
    for (int f : selFaces) {
      if (f < 0 || f >= static_cast<int>(m->faceUvs.size()) || mode != UvMode::Face) continue;
      QPolygonF poly;
      for (int t : m->faceUvs[f])
        if (t >= 0 && t < static_cast<int>(m->uvs.size())) poly << toPx(m->uvs[t]);
      p.setPen(Qt::NoPen);
      p.setBrush(QColor(255, 40, 40, 70));
      p.drawPolygon(poly);
    }
    p.setBrush(Qt::NoBrush);
    for (const auto& f : m->faceUvs)
      for (size_t k = 0; k < f.size(); ++k) {
        int x = f[k], y = f[(k + 1) % f.size()];
        if (x < 0 || y < 0 || x >= static_cast<int>(m->uvs.size()) || y >= static_cast<int>(m->uvs.size())) continue;
        bool sel = mode == UvMode::Edge && selEdges.count(edgeOf(x, y));
        p.setPen(QPen(sel ? QColor(255, 50, 50) : QColor(235, 235, 235), sel ? 2.0 : 1.0));
        p.drawLine(toPx(m->uvs[x]), toPx(m->uvs[y]));
      }
    if (mode == UvMode::Vertex)
      for (size_t i = 0; i < m->uvs.size(); ++i) {
        QPointF c = toPx(m->uvs[i]);
        bool sel = selVerts.count(static_cast<int>(i));
        p.fillRect(QRectF(c.x() - 2.5, c.y() - 2.5, 5, 5), sel ? QColor(255, 40, 40) : QColor(40, 100, 255));
      }
    if (region_) {
      p.setPen(QPen(Qt::white, 1, Qt::DashLine));
      p.drawRect(QRect(press_, cur_).normalized());
    }
  }

  int pickVertex(const QPointF& pos) const {
    UnwrapUvwModifier* m = t_.mod();
    int best = -1;
    double bd = 7.0;
    for (size_t i = 0; m && i < m->uvs.size(); ++i) {
      QPointF c = toPx(m->uvs[i]);
      double d = std::hypot(c.x() - pos.x(), c.y() - pos.y());
      if (d < bd) {
        bd = d;
        best = static_cast<int>(i);
      }
    }
    return best;
  }
  bool pickEdge(const QPointF& pos, std::pair<int, int>& out) const {
    UnwrapUvwModifier* m = t_.mod();
    double bd = 6.0;
    bool found = false;
    if (!m) return false;
    for (const auto& f : m->faceUvs)
      for (size_t k = 0; k < f.size(); ++k) {
        QPointF a = toPx(m->uvs[f[k]]), b = toPx(m->uvs[f[(k + 1) % f.size()]]);
        QPointF d = b - a;
        double len2 = d.x() * d.x() + d.y() * d.y();
        double t = len2 > 0 ? std::clamp(((pos - a).x() * d.x() + (pos - a).y() * d.y()) / len2, 0.0, 1.0) : 0.0;
        QPointF q = a + d * t;
        double dist = std::hypot(q.x() - pos.x(), q.y() - pos.y());
        if (dist < bd) {
          bd = dist;
          out = edgeOf(f[k], f[(k + 1) % f.size()]);
          found = true;
        }
      }
    return found;
  }
  int pickFace(const QPointF& pos) const {
    UnwrapUvwModifier* m = t_.mod();
    for (size_t f = 0; m && f < m->faceUvs.size(); ++f) {
      QPolygonF poly;
      for (int t : m->faceUvs[f]) poly << toPx(m->uvs[t]);
      if (poly.containsPoint(pos, Qt::OddEvenFill)) return static_cast<int>(f);
    }
    return -1;
  }

  void applyClick(const QPointF& pos, Qt::KeyboardModifiers mods) {
    bool ctrl = mods & Qt::ControlModifier, alt = mods & Qt::AltModifier;
    auto apply = [&](auto& set, const auto& item, bool hit) {
      if (!hit) {
        if (!ctrl && !alt) set.clear();
        return;
      }
      if (alt) set.erase(item);
      else if (ctrl) {
        if (set.count(item)) set.erase(item);
        else set.insert(item);
      } else {
        set.clear();
        set.insert(item);
      }
    };
    if (mode == UvMode::Vertex) {
      int v = pickVertex(pos);
      apply(selVerts, v, v >= 0);
    } else if (mode == UvMode::Edge) {
      std::pair<int, int> e;
      bool hit = pickEdge(pos, e);
      apply(selEdges, e, hit);
    } else {
      int f = pickFace(pos);
      apply(selFaces, f, f >= 0);
    }
    if (selectionChanged) selectionChanged();
  }

  void applyRegion(const QRect& r, Qt::KeyboardModifiers mods) {
    UnwrapUvwModifier* m = t_.mod();
    if (!m) return;
    bool alt = mods & Qt::AltModifier, ctrl = mods & Qt::ControlModifier;
    QRectF rf(r);
    if (!ctrl && !alt) {
      selVerts.clear();
      selEdges.clear();
      selFaces.clear();
    }
    auto add = [&](auto& set, const auto& item) {
      if (alt) set.erase(item);
      else set.insert(item);
    };
    for (size_t f = 0; f < m->faceUvs.size(); ++f) {
      const auto& fc = m->faceUvs[f];
      bool all = true;
      for (size_t k = 0; k < fc.size(); ++k) {
        bool in = rf.contains(toPx(m->uvs[fc[k]]));
        all = all && in;
        if (in && mode == UvMode::Vertex) add(selVerts, fc[k]);
        if (mode == UvMode::Edge && in && rf.contains(toPx(m->uvs[fc[(k + 1) % fc.size()]])))
          add(selEdges, edgeOf(fc[k], fc[(k + 1) % fc.size()]));
      }
      if (all && mode == UvMode::Face) add(selFaces, static_cast<int>(f));
    }
    if (selectionChanged) selectionChanged();
  }

  void mousePressEvent(QMouseEvent* e) override {
    press_ = cur_ = e->position().toPoint();
    if (e->button() == Qt::MiddleButton) {
      panning_ = true;
      return;
    }
    if (e->button() != Qt::LeftButton || !t_.ensure()) return;
    mods_ = e->modifiers();
    // Dragging the current selection transforms it.
    std::set<int> verts = activeVerts();
    bool onSel = false;
    if (!(mods_ & (Qt::ControlModifier | Qt::AltModifier)) && !verts.empty()) {
      QPointF pos = e->position();
      if (mode == UvMode::Vertex) onSel = selVerts.count(pickVertex(pos)) > 0;
      else if (mode == UvMode::Edge) {
        std::pair<int, int> ed;
        onSel = pickEdge(pos, ed) && selEdges.count(ed);
      } else {
        onSel = selFaces.count(pickFace(pos)) > 0;
      }
      if (tool != UvTool::Move) onSel = true;  // rotate / scale act anywhere
    }
    if (onSel) {
      dragging_ = true;
      UnwrapUvwModifier* m = t_.mod();
      startUvs_ = m->uvs;
      dragVerts_ = verts;
      Vec2 c;
      for (int v : verts) c += m->uvs[v];
      pivot_ = c / static_cast<float>(std::max<size_t>(1, verts.size()));
      return;
    }
    pressed_ = true;
  }

  void mouseMoveEvent(QMouseEvent* e) override {
    QPoint pos = e->position().toPoint();
    if (panning_) {
      QPointF d = pos - cur_;
      center_ = center_ - Vec2(static_cast<float>(d.x()) / scale(), -static_cast<float>(d.y()) / scale());
      cur_ = pos;
      update();
      return;
    }
    cur_ = pos;
    if (dragging_) {
      UnwrapUvwModifier* m = t_.mod();
      if (!m) return;
      m->uvs = startUvs_;
      QPointF dp = QPointF(pos - press_);
      for (int v : dragVerts_) {
        if (v < 0 || v >= static_cast<int>(m->uvs.size())) continue;
        Vec2 p = m->uvs[v];
        if (tool == UvTool::Move) {
          p = p + Vec2(static_cast<float>(dp.x()) / scale(), -static_cast<float>(dp.y()) / scale());
        } else if (tool == UvTool::Rotate) {
          float a = static_cast<float>(dp.x()) * 0.01f;
          Vec2 d = p - pivot_;
          p = pivot_ + Vec2(d.x * std::cos(a) - d.y * std::sin(a), d.x * std::sin(a) + d.y * std::cos(a));
        } else {
          float f = std::max(0.01f, 1.0f + static_cast<float>(dp.x() - dp.y()) * 0.005f);
          p = pivot_ + (p - pivot_) * f;
        }
        m->uvs[v] = p;
      }
      t_.notify();
      update();
      return;
    }
    if (pressed_ && (pos - press_).manhattanLength() > 3) region_ = true;
    if (pressed_) update();
  }

  void mouseReleaseEvent(QMouseEvent* e) override {
    if (e->button() == Qt::MiddleButton) {
      panning_ = false;
      return;
    }
    if (dragging_) {
      dragging_ = false;
      UnwrapUvwModifier* m = t_.mod();
      if (!m) return;
      std::vector<Vec2> result = m->uvs;
      m->uvs = startUvs_;
      t_.edit(tr("Move UVs"), [result](UnwrapUvwModifier& mod, const PolyMesh&) { mod.uvs = result; });
      update();
      return;
    }
    if (!pressed_) return;
    pressed_ = false;
    if (region_) applyRegion(QRect(press_, e->position().toPoint()).normalized(), mods_);
    else applyClick(e->position(), mods_);
    region_ = false;
    update();
  }

  void wheelEvent(QWheelEvent* e) override {
    Vec2 before = toUv(e->position());
    zoom_ *= std::pow(1.15f, static_cast<float>(e->angleDelta().y()) / 120.0f);
    zoom_ = std::clamp(zoom_, 0.05f, 200.0f);
    Vec2 after = toUv(e->position());
    center_ = center_ + (before - after);
    update();
  }

 private:
  UnwrapTarget t_;
  Vec2 center_{0.5f, 0.5f};
  float zoom_ = 1.0f;
  QPoint press_, cur_;
  Qt::KeyboardModifiers mods_;
  bool pressed_ = false, region_ = false, dragging_ = false, panning_ = false;
  std::vector<Vec2> startUvs_;
  std::set<int> dragVerts_;
  Vec2 pivot_;
};

class UvEditor : public QDialog {
 public:
  UvEditor(const UnwrapTarget& t, QWidget* parent) : QDialog(parent), t_(t) {
    setObjectName("UvEditor");
    setWindowTitle(tr("Edit UVWs"));
    resize(720, 640);
    auto* v = new QVBoxLayout(this);
    auto* tb = new QToolBar(this);
    v->addWidget(tb);
    canvas_ = new UvCanvas(t, this);
    v->addWidget(canvas_, 1);
    info_ = new QLabel(this);
    v->addWidget(info_);
    auto* modes = new QActionGroup(this);
    auto addMode = [&](const QString& text, UvMode m, const QString& name) {
      QAction* a = tb->addAction(text);
      a->setObjectName(name);
      a->setCheckable(true);
      modes->addAction(a);
      connect(a, &QAction::triggered, this, [this, m] {
        canvas_->mode = m;
        canvas_->update();
        updateInfo();
      });
      return a;
    };
    addMode(tr("Vertex"), UvMode::Vertex, "uv_modeVertex");
    addMode(tr("Edge"), UvMode::Edge, "uv_modeEdge");
    addMode(tr("Face"), UvMode::Face, "uv_modeFace")->setChecked(true);
    tb->addSeparator();
    auto* tools = new QActionGroup(this);
    auto addTool = [&](const QString& text, UvTool tl) {
      QAction* a = tb->addAction(text);
      a->setCheckable(true);
      tools->addAction(a);
      connect(a, &QAction::triggered, this, [this, tl] { canvas_->tool = tl; });
      return a;
    };
    addTool(tr("Move"), UvTool::Move)->setChecked(true);
    addTool(tr("Rotate"), UvTool::Rotate);
    addTool(tr("Scale"), UvTool::Scale);
    tb->addSeparator();
    tb->addAction(tr("Select All"), this, [this] {
      canvas_->selectAll();
      updateInfo();
    });
    auto* ops = new QToolBar(this);
    v->insertWidget(1, ops);
    auto op = [&](const QString& text, const QString& name, std::function<void()> fn) {
      QAction* a = ops->addAction(text);
      a->setObjectName(name);
      connect(a, &QAction::triggered, this, [this, fn] {
        fn();
        canvas_->update();
        updateInfo();
      });
    };
    op(tr("Flatten"), "uv_flatten", [this] {
      t_.edit(tr("Flatten Mapping"), [](UnwrapUvwModifier& m, const PolyMesh& in) { flattenUvs(in, m.uvs, m.faceUvs, 60.0f, 0.02f); });
    });
    const char* names[4] = {"Planar", "Box", "Cylindrical", "Spherical"};
    for (int k = 0; k < 4; ++k)
      op(tr(names[k]), QString("uv_project%1").arg(k), [this, k] {
        std::vector<int> faces = selectedFacesOrAll();
        t_.edit(tr("Project UVs"), [faces, k](UnwrapUvwModifier& m, const PolyMesh& in) {
          projectFaceUvs(in, m.uvs, m.faceUvs, faces, k);
          compactUvList(m.uvs, m.faceUvs);
        });
        canvas_->selFaces = std::set<int>(faces.begin(), faces.end());
      });
    op(tr("Relax"), "uv_relax", [this] {
      std::set<int> vs = canvas_->activeVerts();
      std::vector<int> sel(vs.begin(), vs.end());
      t_.edit(tr("Relax UVs"), [sel](UnwrapUvwModifier& m, const PolyMesh&) { relaxUvs(m.uvs, m.faceUvs, sel, 0.5f, 10); });
    });
    op(tr("Stitch"), "uv_stitch", [this] {
      std::set<int> vs = canvas_->activeVerts();
      std::vector<int> sel(vs.begin(), vs.end());
      t_.edit(tr("Stitch UVs"), [sel](UnwrapUvwModifier& m, const PolyMesh& in) {
        stitchUvs(in, m.uvs, m.faceUvs, sel);
        compactUvList(m.uvs, m.faceUvs);
      });
      canvas_->selVerts.clear();
      canvas_->selEdges.clear();
    });
    op(tr("Pack"), "uv_pack", [this] {
      t_.edit(tr("Pack UVs"), [](UnwrapUvwModifier& m, const PolyMesh&) { packUvCharts(m.uvs, m.faceUvs, 0.02f); });
    });
    canvas_->selectionChanged = [this] { updateInfo(); };
    connect(t.doc, &SceneDocument::structureChanged, this, [this] {
      if (!t_.mod()) close();
      canvas_->update();
    });
    connect(t.doc, &SceneDocument::nodesChanged, this, [this](const QVector<uint32_t>& ids, int) {
      if (ids.contains(t_.nodeId)) canvas_->update();
    });
    t_.ensure();
    updateInfo();
  }

  std::vector<int> selectedFacesOrAll() const {
    std::vector<int> faces;
    UnwrapUvwModifier* m = t_.mod();
    if (!m) return faces;
    if (canvas_->mode == UvMode::Face && !canvas_->selFaces.empty()) return {canvas_->selFaces.begin(), canvas_->selFaces.end()};
    // Faces touching selected vertices/edges, else everything.
    std::set<int> vs = canvas_->activeVerts();
    for (size_t f = 0; f < m->faceUvs.size(); ++f) {
      bool hit = vs.empty();
      for (int t : m->faceUvs[f]) hit = hit || vs.count(t);
      if (hit) faces.push_back(static_cast<int>(f));
    }
    return faces;
  }

  void updateInfo() {
    UnwrapUvwModifier* m = t_.mod();
    if (!m) return;
    QString what = canvas_->mode == UvMode::Vertex ? tr("%1 vertices").arg(canvas_->selVerts.size())
                   : canvas_->mode == UvMode::Edge ? tr("%1 edges").arg(canvas_->selEdges.size())
                                                   : tr("%1 faces").arg(canvas_->selFaces.size());
    info_->setText(tr("%1 selected - %2 UV vertices, %3 faces. Middle-drag pans, wheel zooms.")
                       .arg(what)
                       .arg(m->uvs.size())
                       .arg(m->faceUvs.size()));
  }

  UvCanvas* canvas() const { return canvas_; }

 private:
  UnwrapTarget t_;
  UvCanvas* canvas_;
  QLabel* info_;
};

class UnwrapPanel : public QWidget {
 public:
  UnwrapPanel(const ModifyPanelContext& ctx, QWidget* parent) : QWidget(parent) {
    setObjectName("UnwrapPanel");
    t_ = UnwrapTarget{ctx.doc, ctx.nodeId, ctx.stackIndex};
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    auto* r = new Rollout(tr("Edit UVs"), this);
    v->addWidget(r);
    auto* open = new QPushButton(tr("Open UV Editor..."), r->body());
    open->setObjectName("unwrap_open");
    r->form()->addRow(open);
    auto* flatten = new QPushButton(tr("Flatten Mapping"), r->body());
    flatten->setObjectName("unwrap_flatten");
    auto* angle = new QDoubleSpinBox(r->body());
    angle->setRange(1, 180);
    angle->setValue(60);
    r->form()->addRow(flatten);
    r->form()->addRow(tr("Face Angle Threshold:"), angle);
    auto* reset = new QPushButton(tr("Reset UVWs"), r->body());
    reset->setObjectName("unwrap_reset");
    r->form()->addRow(reset);
    info_ = new QLabel(r->body());
    r->form()->addRow(info_);
    connect(open, &QPushButton::clicked, this, [this] {
      static QPointer<UvEditor> editor;
      if (editor) editor->close();
      editor = new UvEditor(t_, window());
      editor->setAttribute(Qt::WA_DeleteOnClose);
      editor->show();
    });
    connect(flatten, &QPushButton::clicked, this, [this, angle] {
      float a = static_cast<float>(angle->value());
      t_.edit(tr("Flatten Mapping"), [a](UnwrapUvwModifier& m, const PolyMesh& in) { flattenUvs(in, m.uvs, m.faceUvs, a, 0.02f); });
    });
    connect(reset, &QPushButton::clicked, this, [this] {
      t_.edit(tr("Reset UVWs"), [](UnwrapUvwModifier& m, const PolyMesh& in) {
        m.uvs.clear();
        m.faceUvs.clear();
        m.initFrom(in);
      });
    });
    connect(ctx.doc, &SceneDocument::nodesChanged, this, [this](const QVector<uint32_t>& ids, int) {
      if (ids.contains(t_.nodeId)) refresh();
    });
    refresh();
  }
  void refresh() {
    UnwrapUvwModifier* m = t_.mod();
    if (!m) return;
    const PolyMesh* in = t_.input();
    info_->setText(!m->hasEdits() ? tr("No edits yet (the input UVs pass through)")
                   : in && m->matches(*in) ? tr("%1 UV vertices on %2 faces").arg(m->uvs.size()).arg(m->faceUvs.size())
                                           : tr("Topology changed below: edits are not applied"));
  }

 private:
  UnwrapTarget t_;
  QLabel* info_ = nullptr;
};

void installUnwrap(MainWindow&) {
  registerModifyRollouts("UnwrapUVW", [](const ModifyPanelContext& ctx, QWidget* parent) -> QWidget* {
    return new UnwrapPanel(ctx, parent);
  });
}

void testUnwrap(MainWindow& w) {
  using testing::check;
  using testing::pump;
  SceneDocument* doc = w.document();
  doc->beginEdit("Create");
  Node* box = doc->createNode("Box");
  doc->endEdit();
  uint32_t id = box->id;
  doc->setSelection({id});
  w.commandPanel()->addModifier("UnwrapUVW");
  pump(50);
  QPushButton* open = nullptr;
  for (QPushButton* b : w.findChildren<QPushButton*>("unwrap_open"))
    if (b->isVisible()) open = b;
  check(open != nullptr, "Unwrap UVW rollout shown");
  if (!open) return;
  open->click();
  pump(100);
  auto* editor = static_cast<UvEditor*>(w.findChild<QDialog*>("UvEditor"));
  check(editor != nullptr, "UV editor window opens");
  if (!editor) return;
  if (auto* a = editor->findChild<QAction*>("uv_flatten")) a->trigger();
  pump(30);
  Node* n = doc->scene().findNode(id);
  auto* uw = n ? dynamic_cast<UnwrapUvwModifier*>(n->modifiers[0].get()) : nullptr;
  check(uw && uw->hasEdits() && uw->faceUvs.size() == 6, "Flatten creates edited UVs for all faces");
  bool inside = true;
  for (const auto& t : uw->uvs) inside = inside && t.x >= -1e-4f && t.x <= 1.0001f && t.y >= -1e-4f && t.y <= 1.0001f;
  check(inside, "flattened UVs fit the 0..1 tile");
  check(n->mesh().faces[0].uv.size() == 4 && n->mesh().uvs.size() == uw->uvs.size(), "the mesh uses the edited UVs");
  // Select every face and drag them in the canvas.
  editor->canvas()->selectAll();
  Vec2 before = uw->uvs[0];
  UvCanvas* cv = editor->canvas();
  int f0 = 0;
  QPolygonF poly;
  for (int t : uw->faceUvs[f0]) poly << cv->toPx(uw->uvs[t]);
  QPointF c = poly.boundingRect().center();
  QMouseEvent press(QEvent::MouseButtonPress, c, cv->mapToGlobal(c.toPoint()), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(cv, &press);
  QPointF to = c + QPointF(40, 0);
  QMouseEvent move(QEvent::MouseMove, to, cv->mapToGlobal(to.toPoint()), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(cv, &move);
  QMouseEvent release(QEvent::MouseButtonRelease, to, cv->mapToGlobal(to.toPoint()), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(cv, &release);
  pump(30);
  n = doc->scene().findNode(id);
  uw = dynamic_cast<UnwrapUvwModifier*>(n->modifiers[0].get());
  check(uw && uw->uvs[0].x > before.x + 1e-3f, "dragging the selection moves UVs");
  if (QString dir = qEnvironmentVariable("VERAA_SCREENSHOT_DIR"); !dir.isEmpty()) testing::saveGrab(editor, dir + "/unwrap_01_editor.png");
  doc->undoStack()->undo();
  pump(30);
  n = doc->scene().findNode(id);
  uw = n ? dynamic_cast<UnwrapUvwModifier*>(n->modifiers[0].get()) : nullptr;
  check(uw && std::fabs(uw->uvs[0].x - before.x) < 1e-5f, "undo restores the UVs");
  // Cylindrical projection of all faces.
  editor = static_cast<UvEditor*>(w.findChild<QDialog*>("UvEditor"));
  if (editor) {
    editor->canvas()->selectAll();
    if (auto* a = editor->findChild<QAction*>("uv_project2")) a->trigger();
    pump(30);
    n = doc->scene().findNode(id);
    uw = dynamic_cast<UnwrapUvwModifier*>(n->modifiers[0].get());
    check(uw && uw->matches(n->modifiers.empty() ? PolyMesh() : uw->lastInput), "cylindrical projection keeps the UVs valid");
    editor->close();
  }
}

}  // namespace

VERAA_APP_EXTENSION(unwrapUvw, 250, installUnwrap);
VERAA_SELF_TEST(unwrapUvw, testUnwrap);

}  // namespace veraa
