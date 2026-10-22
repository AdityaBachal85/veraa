// Rigging and dynamics tools: bone chain creation, IK chain / Skin / Morpher / Path Deform /
// deflector rollouts, skin weight painting and weight table, and Bind to Space Warp.
#include <QAction>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QTableWidget>
#include <QToolBar>
#include <QVBoxLayout>

#include <cmath>

#include "animcontroller.h"
#include "animicons.h"
#include "animui.h"
#include "extension.h"
#include "glscene.h"
#include "mainwindow.h"
#include "particles.h"
#include "picktool.h"
#include "rigging.h"
#include "viewport.h"

namespace veraa {

// Mesh entering a node's modifier `index` (the object and the modifiers below it).
PolyMesh meshBelowModifier(const Node& n, int index) {
  GeomState st;
  st.scene = n.scene();
  st.nodeId = n.id;
  st.time = n.scene() ? n.scene()->time.currentFrame : 0.0;
  if (auto* g = n.geometry()) st.mesh = g->buildAt(st);
  else if (auto* s = n.shapeObject()) {
    st.shape = s->buildShape();
    st.isShape = true;
  }
  for (int i = 0; i < index && i < static_cast<int>(n.modifiers.size()); ++i)
    if (n.modifiers[static_cast<size_t>(i)] && n.modifiers[static_cast<size_t>(i)]->enabled) n.modifiers[static_cast<size_t>(i)]->applyState(st);
  return st.mesh;
}

namespace {

// ---------------------------------------------------------------------------------------------
// Bones: click to place joints, right-click to finish (adds an end bone).

Mat4 boneFrame(const Vec3& from, const Vec3& to, const Vec3& viewDir) {
  Vec3 x = normalize(to - from);
  Vec3 z = viewDir - x * dot(viewDir, x);
  if (length2(z) < 1e-8f) z = std::fabs(x.z) < 0.9f ? Vec3(0, 0, 1) : Vec3(0, 1, 0);
  z = normalize(z);
  z = z - x * dot(z, x);
  z = normalize(z);
  Vec3 y = cross(z, x);
  Mat4 m;
  for (int r = 0; r < 3; ++r) {
    m.m[r][0] = x[r];
    m.m[r][1] = y[r];
    m.m[r][2] = z[r];
    m.m[r][3] = from[r];
  }
  return m;
}

class BoneTool : public ViewportTool {
 public:
  bool mousePress(Viewport& vp, QMouseEvent* e) override {
    SceneDocument* doc = vp.document();
    if (e->button() == Qt::RightButton) {
      finish(vp, true);
      return true;
    }
    if (e->button() != Qt::LeftButton) return false;
    Vec3 hit;
    if (!vp.hitConstructionPlane(static_cast<float>(e->position().x()), static_cast<float>(e->position().y()), hit)) return true;
    if (!active_) {
      doc->beginEdit(QObject::tr("Create Bones"));
      active_ = true;
      start_ = hit;
      current_ = addBone(vp, 0, hit, hit + Vec3(1, 0, 0));
      return true;
    }
    // Fix the current bone and start the next one at its end.
    Node* cur = doc->scene().findNode(current_);
    if (!cur) return true;
    if (length(hit - start_) < 1e-3f) return true;
    place(vp, *cur, start_, hit);
    start_ = hit;
    current_ = addBone(vp, current_, hit, hit + (hit - cur->worldPosition()));
    return true;
  }

  bool mouseMove(Viewport& vp, QMouseEvent* e) override {
    if (!active_) return false;
    Vec3 hit;
    if (!vp.hitConstructionPlane(static_cast<float>(e->position().x()), static_cast<float>(e->position().y()), hit)) return true;
    Node* cur = vp.document()->scene().findNode(current_);
    if (cur && length(hit - start_) > 1e-3f) {
      place(vp, *cur, start_, hit);
      vp.document()->notifyNodeChanged(cur, ChangeTransform | ChangeGeometry);
    }
    return true;
  }

  bool keyPress(Viewport& vp, QKeyEvent* e) override {
    if (e->key() == Qt::Key_Escape) {
      finish(vp, false);
      return true;
    }
    return false;
  }

  bool cancel() override {
    if (!active_ || !doc_) return false;
    finishWith(nullptr, true);
    return true;
  }
  QString prompt() const override { return QObject::tr("Click to place bone joints; right-click to end the chain"); }

 private:
  uint32_t addBone(Viewport& vp, uint32_t parent, const Vec3& from, const Vec3& to) {
    SceneDocument* doc = vp.document();
    doc_ = doc;
    view_ = vp.camera().forward();
    Node* b = doc->createNode("Bone");
    if (!b) return 0;
    b->renderable = false;
    b->name = doc->scene().uniqueNodeName("Bone");
    if (Node* p = doc->scene().findNode(parent)) b->wireColor = p->wireColor;
    else b->wireColor = Vec3(0.85f, 0.75f, 0.25f);
    if (parent) doc->scene().setParent(b->id, parent);
    place(vp, *b, from, to);
    doc->notifyStructureChanged();
    chain_.push_back(b->id);
    return b->id;
  }

  void place(Viewport& vp, Node& b, const Vec3& from, const Vec3& to) {
    Vec3 up = -vp.camera().forward();
    b.setWorldMatrix(boneFrame(from, to, up));
    float len = std::max(0.01f, length(to - from));
    b.object->params.setNumber("length", len);
    float w = std::clamp(len * 0.15f, 0.5f, 20.0f);
    b.object->params.setNumber("width", w);
    b.object->params.setNumber("height", w);
    b.object->touch();
    b.invalidate();
  }

  void finish(Viewport& vp, bool keep) {
    if (!active_) {
      vp.editorState()->setViewportTool(nullptr);
      vp.editorState()->setCreateType({});
      return;
    }
    finishWith(&vp, keep);
  }

  void finishWith(Viewport* vp, bool keep) {
    SceneDocument* doc = doc_;
    active_ = false;
    if (!doc) return;
    // The bone following the cursor becomes the small end bone ("nub").
    Node* cur = doc->scene().findNode(current_);
    if (keep && cur && chain_.size() >= 2) {
      float w = cur->object->params.getFloat("width");
      cur->object->params.setNumber("length", std::max(w, 0.5f));
      Node* parent = cur->parent();
      if (parent) {
        Mat4 pw = parent->worldMatrix();
        cur->setWorldMatrix(Mat4::translate(start_) * Mat4::translate(-pw.translation()) * pw);
      }
      cur->object->touch();
      cur->invalidate();
      doc->notifyStructureChanged();
      doc->setSelection({chain_.front()});
      doc->endEdit();
    } else {
      doc->cancelEdit();
    }
    chain_.clear();
    current_ = 0;
    if (vp) vp->editorState()->setViewportTool(nullptr);
  }

  bool active_ = false;
  SceneDocument* doc_ = nullptr;
  Vec3 start_, view_;
  uint32_t current_ = 0;
  std::vector<uint32_t> chain_;
};

// ---------------------------------------------------------------------------------------------
// Skin

QPointer<QWidget> g_skinRollout;
uint32_t g_skinNode = 0;
int g_skinIndex = -1;
int g_skinBone = -1;
bool g_showWeights = true;

SkinModifier* skinOf(SceneDocument* doc, uint32_t node, int index) {
  Node* n = doc->scene().findNode(node);
  if (!n || index < 0 || index >= static_cast<int>(n->modifiers.size())) return nullptr;
  return dynamic_cast<SkinModifier*>(n->modifiers[static_cast<size_t>(index)].get());
}

class PaintWeightsTool : public ViewportTool {
 public:
  PaintWeightsTool(uint32_t node, int index) : node_(node), index_(index) {}
  float radius = 30.0f;    // pixels
  float strength = 0.15f;  // per dab

  bool mousePress(Viewport& vp, QMouseEvent* e) override {
    if (e->button() == Qt::RightButton) {
      vp.editorState()->setViewportTool(nullptr);
      return true;
    }
    if (e->button() != Qt::LeftButton) return false;
    painting_ = true;
    vp.document()->beginEdit(QObject::tr("Paint Weights"));
    dab(vp, e);
    return true;
  }
  bool mouseMove(Viewport& vp, QMouseEvent* e) override {
    cursor_ = e->position();
    if (painting_) dab(vp, e);
    vp.update();
    return true;
  }
  bool mouseRelease(Viewport& vp, QMouseEvent*) override {
    if (!painting_) return false;
    painting_ = false;
    vp.document()->endEdit();
    return true;
  }
  void drawOverlay(Viewport& vp, QPainter& p) override {
    p.setPen(QPen(QColor(0xf0, 0x60, 0x60), 1.5));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(cursor_, radius, radius);
    p.setPen(QColor(0xf0, 0xd0, 0x40));
    p.drawText(QRectF(8, vp.height() - 26, vp.width() - 16, 20), Qt::AlignLeft | Qt::AlignVCenter,
               QObject::tr("Paint Weights: drag to add weight to the selected bone, Alt+drag to remove, right-click to stop"));
  }
  bool cancel() override {
    if (painting_) painting_ = false;
    return true;
  }
  bool allowsGizmo() const override { return false; }
  QString prompt() const override { return QObject::tr("Paint Weights"); }

 private:
  void dab(Viewport& vp, QMouseEvent* e) {
    SceneDocument* doc = vp.document();
    SkinModifier* skin = skinOf(doc, node_, index_);
    Node* n = doc->scene().findNode(node_);
    if (!skin || !n || g_skinBone < 0 || g_skinBone >= static_cast<int>(skin->bones.size())) return;
    bool remove = e->modifiers() & Qt::AltModifier;
    PolyMesh bind = meshBelowModifier(*n, index_);
    const PolyMesh& shown = n->mesh();
    if (shown.positions.size() != bind.positions.size()) return;
    std::vector<VertexWeights> weights = skin->computeWeights(bind);
    Mat4 m = n->objectWorldMatrix();
    QPointF c = e->position();
    for (size_t v = 0; v < shown.positions.size(); ++v) {
      float sx, sy;
      if (!vp.camera().project(m.transformPoint(shown.positions[v]), sx, sy)) continue;
      float d = std::hypot(sx - static_cast<float>(c.x()), sy - static_cast<float>(c.y()));
      if (d > radius) continue;
      float falloff = 1.0f - d / radius;
      float cur = 0.0f;
      for (const auto& [b, w] : weights[v])
        if (b == g_skinBone) cur = w;
      float next = clampf(cur + (remove ? -1.0f : 1.0f) * strength * falloff, 0.0f, 1.0f);
      if (std::fabs(next - cur) > 1e-4f) skin->setVertexWeight(bind, static_cast<int>(v), g_skinBone, next);
    }
    doc->notifyNodeChanged(n, ChangeGeometry);
  }
  uint32_t node_;
  int index_;
  bool painting_ = false;
  QPointF cursor_;
};

void showWeightTable(QWidget* parent, SceneDocument* doc, uint32_t node, int index) {
  SkinModifier* skin = skinOf(doc, node, index);
  Node* n = doc->scene().findNode(node);
  if (!skin || !n) return;
  PolyMesh bind = meshBelowModifier(*n, index);
  std::vector<VertexWeights> weights = skin->computeWeights(bind);
  QDialog d(parent);
  d.setWindowTitle(QObject::tr("Weight Table"));
  d.resize(640, 480);
  auto* v = new QVBoxLayout(&d);
  int rows = std::min<int>(static_cast<int>(bind.positions.size()), 5000);
  auto* table = new QTableWidget(rows, static_cast<int>(skin->bones.size()), &d);
  QStringList headers;
  for (const auto& b : skin->bones) {
    const Node* bn = doc->scene().findNode(b.nodeId);
    headers << (bn ? QString::fromStdString(bn->name) : QObject::tr("(missing)"));
  }
  table->setHorizontalHeaderLabels(headers);
  for (int r = 0; r < rows; ++r) {
    table->setVerticalHeaderItem(r, new QTableWidgetItem(QObject::tr("#%1%2").arg(r).arg(skin->overrides.count(r) ? " *" : "")));
    for (int b = 0; b < static_cast<int>(skin->bones.size()); ++b) {
      float w = 0;
      for (const auto& [bi, wi] : weights[static_cast<size_t>(r)])
        if (bi == b) w = wi;
      table->setItem(r, b, new QTableWidgetItem(QString::number(w, 'f', 3)));
    }
  }
  v->addWidget(new QLabel(QObject::tr("Edit a cell to set that bone's weight (other bones are scaled to keep the total at 1)."), &d));
  v->addWidget(table);
  bool loading = false;
  QObject::connect(table, &QTableWidget::itemChanged, &d, [&](QTableWidgetItem* it) {
    if (loading) return;
    SkinModifier* sk = skinOf(doc, node, index);
    Node* nn = doc->scene().findNode(node);
    if (!sk || !nn) return;
    doc->beginEdit(QObject::tr("Edit Weight"));
    sk->setVertexWeight(bind, it->row(), it->column(), static_cast<float>(it->text().toDouble()));
    doc->notifyNodeChanged(nn, ChangeGeometry);
    doc->endEdit();
    // Show the normalized result of the row.
    loading = true;
    std::vector<VertexWeights> ws = sk->computeWeights(bind);
    for (int b = 0; b < table->columnCount(); ++b) {
      float w = 0;
      for (const auto& [bi, wi] : ws[static_cast<size_t>(it->row())])
        if (bi == b) w = wi;
      table->item(it->row(), b)->setText(QString::number(w, 'f', 3));
    }
    loading = false;
  });
  auto* bb = new QDialogButtonBox(QDialogButtonBox::Close, &d);
  QObject::connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
  v->addWidget(bb);
  d.exec();
}

QWidget* skinRollout(const ModifyPanelContext& ctx, QWidget* parent) {
  auto* w = new QWidget(parent);
  w->setObjectName("skinRollout");
  auto* v = new QVBoxLayout(w);
  v->setContentsMargins(0, 0, 0, 0);
  SceneDocument* doc = ctx.doc;
  EditorState* state = ctx.state;
  uint32_t node = ctx.nodeId;
  int index = ctx.stackIndex;
  g_skinRollout = w;
  g_skinNode = node;
  g_skinIndex = index;
  v->addWidget(new QLabel(QObject::tr("<b>Bones</b>"), w));
  auto* list = new QListWidget(w);
  list->setObjectName("skinBoneList");
  list->setMaximumHeight(130);
  v->addWidget(list);
  auto* row = new QHBoxLayout;
  auto* add = new QPushButton(QObject::tr("Add..."), w);
  auto* pick = new QPushButton(QObject::tr("Pick"), w);
  auto* remove = new QPushButton(QObject::tr("Remove"), w);
  row->addWidget(add);
  row->addWidget(pick);
  row->addWidget(remove);
  v->addLayout(row);
  auto* form = new QFormLayout;
  auto* inner = new QDoubleSpinBox(w);
  auto* outer = new QDoubleSpinBox(w);
  for (QDoubleSpinBox* s : {inner, outer}) {
    s->setRange(0, 1e6);
    s->setDecimals(2);
    s->setKeyboardTracking(false);
  }
  form->addRow(QObject::tr("Envelope Inner Radius:"), inner);
  form->addRow(QObject::tr("Envelope Outer Radius:"), outer);
  v->addLayout(form);
  auto* paint = new QPushButton(QObject::tr("Paint Weights"), w);
  paint->setCheckable(true);
  auto* table = new QPushButton(QObject::tr("Weight Table..."), w);
  auto* showW = new QCheckBox(QObject::tr("Show Weights in Viewport"), w);
  showW->setChecked(g_showWeights);
  auto* reset = new QPushButton(QObject::tr("Reset Skin Pose"), w);
  auto* clear = new QPushButton(QObject::tr("Clear Painted Weights"), w);
  v->addWidget(paint);
  v->addWidget(table);
  v->addWidget(showW);
  v->addWidget(reset);
  v->addWidget(clear);

  auto refresh = [doc, node, index, list, inner, outer] {
    SkinModifier* skin = skinOf(doc, node, index);
    QSignalBlocker b(list);
    list->clear();
    if (!skin) return;
    for (const auto& b2 : skin->bones) {
      const Node* bn = doc->scene().findNode(b2.nodeId);
      list->addItem(bn ? QString::fromStdString(bn->name) : QObject::tr("(missing)"));
    }
    if (g_skinBone >= list->count()) g_skinBone = list->count() - 1;
    if (g_skinBone < 0 && list->count()) g_skinBone = 0;
    list->setCurrentRow(g_skinBone);
    QSignalBlocker b3(inner), b4(outer);
    if (g_skinBone >= 0 && g_skinBone < static_cast<int>(skin->bones.size())) {
      inner->setValue(skin->bones[static_cast<size_t>(g_skinBone)].innerRadius);
      outer->setValue(skin->bones[static_cast<size_t>(g_skinBone)].outerRadius);
    }
  };
  auto edit = [doc, node, index](const QString& label, const std::function<void(SkinModifier&, Node&)>& fn) {
    SkinModifier* skin = skinOf(doc, node, index);
    Node* n = doc->scene().findNode(node);
    if (!skin || !n) return;
    doc->beginEdit(label);
    fn(*skin, *n);
    skin->touch();
    doc->notifyNodeChanged(n, ChangeGeometry);
    doc->endEdit();
  };
  auto addBones = [doc, node, edit, refresh](const std::vector<uint32_t>& ids) {
    edit(QObject::tr("Add Bones"), [&](SkinModifier& s, Node& n) {
      for (uint32_t id : ids) s.addBone(doc->scene(), id, n);
    });
    refresh();
    Q_UNUSED(node);
  };
  QObject::connect(add, &QPushButton::clicked, w, [w, doc, node, addBones] {
    QDialog d(w);
    d.setWindowTitle(QObject::tr("Select Bones"));
    auto* lv = new QVBoxLayout(&d);
    auto* l = new QListWidget(&d);
    l->setSelectionMode(QAbstractItemView::ExtendedSelection);
    for (const auto& up : doc->scene().nodes) {
      if (up->id == node) continue;
      auto* it = new QListWidgetItem(QString::fromStdString(up->name), l);
      it->setData(Qt::UserRole, up->id);
      if (up->object && up->object->type() == "Bone") it->setSelected(true);
    }
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
    QObject::connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    lv->addWidget(l);
    lv->addWidget(bb);
    if (d.exec() != QDialog::Accepted) return;
    std::vector<uint32_t> ids;
    for (QListWidgetItem* it : l->selectedItems()) ids.push_back(it->data(Qt::UserRole).toUInt());
    addBones(ids);
  });
  QObject::connect(pick, &QPushButton::clicked, w, [state, node, addBones] {
    state->setViewportTool(std::make_shared<PickNodeTool>(
        QObject::tr("Skin: pick bones to add (right-click to stop)"), [node](const Node& n) { return n.id != node; },
        [addBones](uint32_t id) { addBones({id}); }, true));
  });
  QObject::connect(remove, &QPushButton::clicked, w, [edit, refresh] {
    if (g_skinBone < 0) return;
    int b = g_skinBone;
    edit(QObject::tr("Remove Bone"), [b](SkinModifier& s, Node&) { s.removeBone(b); });
    refresh();
  });
  QObject::connect(list, &QListWidget::currentRowChanged, w, [refresh, state](int r) {
    g_skinBone = r;
    refresh();
    state->requestRedraw();
  });
  auto setRadius = [edit](bool isInner, double val) {
    if (g_skinBone < 0) return;
    int b = g_skinBone;
    edit(QObject::tr("Envelope"), [b, isInner, val](SkinModifier& s, Node&) {
      if (b >= static_cast<int>(s.bones.size())) return;
      SkinBone& sb = s.bones[static_cast<size_t>(b)];
      if (isInner) sb.innerRadius = static_cast<float>(val);
      else sb.outerRadius = static_cast<float>(val);
      sb.outerRadius = std::max(sb.outerRadius, sb.innerRadius + 1e-3f);
    });
  };
  QObject::connect(inner, &QDoubleSpinBox::valueChanged, w, [setRadius](double v2) { setRadius(true, v2); });
  QObject::connect(outer, &QDoubleSpinBox::valueChanged, w, [setRadius](double v2) { setRadius(false, v2); });
  QObject::connect(paint, &QPushButton::toggled, w, [state, node, index](bool on) {
    state->setViewportTool(on ? std::make_shared<PaintWeightsTool>(node, index) : nullptr);
  });
  QObject::connect(state, &EditorState::viewportToolChanged, paint, [paint, state] {
    QSignalBlocker b(paint);
    paint->setChecked(dynamic_cast<PaintWeightsTool*>(state->viewportTool()) != nullptr);
  });
  QObject::connect(table, &QPushButton::clicked, w, [w, doc, node, index] { showWeightTable(w, doc, node, index); });
  QObject::connect(showW, &QCheckBox::toggled, w, [state](bool on) {
    g_showWeights = on;
    state->requestRedraw();
  });
  QObject::connect(reset, &QPushButton::clicked, w, [edit] {
    edit(QObject::tr("Reset Skin Pose"), [](SkinModifier& s, Node& n) {
      if (n.scene()) s.resetSkinPose(*n.scene(), n);
    });
  });
  QObject::connect(clear, &QPushButton::clicked, w, [edit] {
    edit(QObject::tr("Clear Painted Weights"), [](SkinModifier& s, Node&) { s.clearOverrides(); });
  });
  QObject::connect(doc, &SceneDocument::nodesChanged, list, [refresh, list, node](const QVector<uint32_t>& ids, int) {
    if (ids.contains(node)) refresh();
  });
  refresh();
  return w;
}

// Envelopes and weights of the skin being edited.
class SkinDecorator : public ViewportDecorator {
 public:
  explicit SkinDecorator(SceneDocument* doc) : doc_(doc) {}
  void drawLines(Viewport& vp, LineBatch& b) override {
    if (!g_skinRollout || !g_skinRollout->isVisible()) return;
    SkinModifier* skin = skinOf(doc_, g_skinNode, g_skinIndex);
    Node* n = doc_->scene().findNode(g_skinNode);
    if (!skin || !n) return;
    for (int i = 0; i < static_cast<int>(skin->bones.size()); ++i) {
      const SkinBone& sb = skin->bones[static_cast<size_t>(i)];
      const Node* bn = doc_->scene().findNode(sb.nodeId);
      if (!bn) continue;
      Mat4 m = bn->worldMatrix();
      Vec3 a = m.transformPoint(sb.start), e = m.transformPoint(sb.end);
      bool sel = i == g_skinBone;
      QColor ci = sel ? QColor(0xf0, 0x40, 0x40) : QColor(0x90, 0x60, 0x60);
      QColor co = sel ? QColor(0xa0, 0x30, 0x30) : QColor(0x60, 0x40, 0x40);
      b.line(a, e, ci);
      Vec3 axis = normalize(e - a);
      Vec3 u, v;
      makeBasis(axis, u, v);
      float sc = length(m.column(1));
      for (const Vec3& c : {a, e})
        for (int k = 0; k < 24; ++k) {
          float t0 = kTwoPi * k / 24, t1 = kTwoPi * (k + 1) / 24;
          b.line(c + (u * std::cos(t0) + v * std::sin(t0)) * sb.innerRadius * sc, c + (u * std::cos(t1) + v * std::sin(t1)) * sb.innerRadius * sc, ci);
          b.line(c + (u * std::cos(t0) + v * std::sin(t0)) * sb.outerRadius * sc, c + (u * std::cos(t1) + v * std::sin(t1)) * sb.outerRadius * sc, co);
        }
    }
    if (!g_showWeights || g_skinBone < 0) return;
    PolyMesh bind = meshBelowModifier(*n, g_skinIndex);
    const PolyMesh& shown = n->mesh();
    if (bind.positions.size() != shown.positions.size() || shown.positions.size() > 20000) return;
    std::vector<VertexWeights> ws = skin->computeWeights(bind);
    Mat4 m = n->objectWorldMatrix();
    float px = vp.camera().worldPerPixel(m.translation()) * 3.0f;
    for (size_t i = 0; i < shown.positions.size(); ++i) {
      float w = 0;
      for (const auto& [bi, wi] : ws[i])
        if (bi == g_skinBone) w = wi;
      if (w <= 0.001f) continue;
      QColor col = QColor::fromRgbF(1.0f, 1.0f - w, 1.0f - w);
      if (w < 0.5f) col = QColor::fromRgbF(0.2f + w, 0.6f + w * 0.8f, 1.0f - w);
      Vec3 p = m.transformPoint(shown.positions[i]);
      b.line(p - Vec3(px, 0, 0), p + Vec3(px, 0, 0), col);
      b.line(p - Vec3(0, px, 0), p + Vec3(0, px, 0), col);
      b.line(p - Vec3(0, 0, px), p + Vec3(0, 0, px), col);
    }
  }

 private:
  SceneDocument* doc_;
};

// ---------------------------------------------------------------------------------------------
// Morpher

QWidget* morpherRollout(const ModifyPanelContext& ctx, QWidget* parent) {
  auto* w = new QWidget(parent);
  w->setObjectName("morpherRollout");
  auto* v = new QVBoxLayout(w);
  v->setContentsMargins(0, 0, 0, 0);
  SceneDocument* doc = ctx.doc;
  EditorState* state = ctx.state;
  uint32_t node = ctx.nodeId;
  int index = ctx.stackIndex;
  v->addWidget(new QLabel(QObject::tr("<b>Channel List</b>"), w));
  auto* list = new QListWidget(w);
  list->setMaximumHeight(180);
  v->addWidget(list);
  auto* pick = new QPushButton(QObject::tr("Pick Object from Scene"), w);
  auto* reload = new QPushButton(QObject::tr("Reload All Morph Targets"), w);
  auto* del = new QPushButton(QObject::tr("Delete Channel"), w);
  v->addWidget(pick);
  v->addWidget(reload);
  v->addWidget(del);
  auto morph = [doc, node, index]() -> MorpherModifier* {
    Node* n = doc->scene().findNode(node);
    if (!n || index < 0 || index >= static_cast<int>(n->modifiers.size())) return nullptr;
    return dynamic_cast<MorpherModifier*>(n->modifiers[static_cast<size_t>(index)].get());
  };
  auto refresh = [morph, list] {
    MorpherModifier* m = morph();
    int row = std::max(0, list->currentRow());
    list->clear();
    if (!m) return;
    for (int i = 0; i < kMorphChannels; ++i) {
      const MorphChannel& c = m->channels[static_cast<size_t>(i)];
      list->addItem(QStringLiteral("%1: %2  (%3%)")
                        .arg(i + 1)
                        .arg(c.positions.empty() ? QObject::tr("-empty-") : QString::fromStdString(c.name))
                        .arg(m->params.getFloat("ch" + std::to_string(i + 1)), 0, 'f', 1));
    }
    list->setCurrentRow(row);
  };
  QObject::connect(pick, &QPushButton::clicked, w, [state, doc, node, index, list, morph, refresh] {
    int ch = std::max(0, list->currentRow());
    Node* self = doc->scene().findNode(node);
    size_t count = self ? meshBelowModifier(*self, index).positions.size() : 0;
    state->setViewportTool(std::make_shared<PickNodeTool>(
        QObject::tr("Morpher: pick a target with the same vertex count"),
        [node, count](const Node& n) { return n.id != node && n.kind() == ObjectKind::Geometry && n.mesh().positions.size() == count; },
        [doc, node, ch, morph, refresh](uint32_t id) {
          MorpherModifier* m = morph();
          Node* t = doc->scene().findNode(id);
          if (!m || !t) return;
          doc->beginEdit(QObject::tr("Pick Morph Target"));
          m->captureTarget(ch, *t);
          if (m->params.getFloat("ch" + std::to_string(ch + 1)) == 0.0f) m->params.setNumber("ch" + std::to_string(ch + 1), 0);
          doc->notifyNodesChanged({node}, ChangeGeometry);
          doc->endEdit();
          refresh();
        }));
  });
  QObject::connect(reload, &QPushButton::clicked, w, [doc, node, morph, refresh] {
    MorpherModifier* m = morph();
    if (!m) return;
    doc->beginEdit(QObject::tr("Reload Morph Targets"));
    for (int i = 0; i < kMorphChannels; ++i)
      if (Node* t = doc->scene().findNode(m->channels[static_cast<size_t>(i)].sourceNode)) m->captureTarget(i, *t);
    doc->notifyNodesChanged({node}, ChangeGeometry);
    doc->endEdit();
    refresh();
  });
  QObject::connect(del, &QPushButton::clicked, w, [doc, node, list, morph, refresh] {
    MorpherModifier* m = morph();
    int ch = list->currentRow();
    if (!m || ch < 0) return;
    doc->beginEdit(QObject::tr("Delete Morph Channel"));
    m->channels[static_cast<size_t>(ch)] = MorphChannel();
    m->touch();
    doc->notifyNodesChanged({node}, ChangeGeometry);
    doc->endEdit();
    refresh();
  });
  QObject::connect(doc, &SceneDocument::nodesChanged, list, [refresh] { refresh(); });
  refresh();
  return w;
}

// ---------------------------------------------------------------------------------------------
// Pick-a-node rollouts (Path Deform path, UDeflector object, IK chain joints)

QWidget* pickParamRollout(const ModifyPanelContext& ctx, QWidget* parent, const QString& title, const char* key,
                          std::function<bool(const Node&)> filter) {
  auto* w = new QWidget(parent);
  auto* v = new QVBoxLayout(w);
  v->setContentsMargins(0, 0, 0, 0);
  SceneDocument* doc = ctx.doc;
  EditorState* state = ctx.state;
  uint32_t node = ctx.nodeId;
  int index = ctx.stackIndex;
  auto block = [doc, node, index]() -> ParamBlock* {
    Node* n = doc->scene().findNode(node);
    if (!n) return nullptr;
    if (index < 0) return n->object ? &n->object->params : nullptr;
    if (index >= static_cast<int>(n->modifiers.size()) || !n->modifiers[static_cast<size_t>(index)]) return nullptr;
    return &n->modifiers[static_cast<size_t>(index)]->params;
  };
  auto* label = new QLabel(w);
  auto* btn = new QPushButton(title, w);
  v->addWidget(label);
  v->addWidget(btn);
  std::string k = key;
  auto refresh = [doc, block, label, k] {
    ParamBlock* p = block();
    const Node* t = p ? doc->scene().findNode(static_cast<uint32_t>(p->getInt(k))) : nullptr;
    label->setText(QObject::tr("Current: %1").arg(t ? QString::fromStdString(t->name) : QObject::tr("none")));
  };
  QObject::connect(btn, &QPushButton::clicked, w, [state, doc, node, block, k, filter, refresh, title] {
    state->setViewportTool(std::make_shared<PickNodeTool>(
        title, [node, filter](const Node& n) { return n.id != node && (!filter || filter(n)); },
        [doc, node, block, k, refresh](uint32_t id) {
          ParamBlock* p = block();
          if (!p) return;
          doc->beginEdit(QObject::tr("Pick"));
          p->setNumber(k, id);
          doc->notifyNodesChanged({node}, ChangeGeometry);
          doc->endEdit();
          if (AnimController* c = AnimController::instance()) c->refresh();
          refresh();
        }));
  });
  refresh();
  return w;
}

// ---------------------------------------------------------------------------------------------
// Bind to Space Warp

void bindToSpaceWarpTool(MainWindow& w) {
  SceneDocument* doc = w.document();
  QVector<uint32_t> sel = doc->selection();
  if (sel.isEmpty()) {
    QMessageBox::information(&w, QObject::tr("Bind to Space Warp"),
                             QObject::tr("Select the particle systems or objects to bind, then pick a space warp."));
    return;
  }
  startPickNode(w, QObject::tr("Bind to Space Warp: pick a space warp"),
                [doc, sel](const Node& n) {
                  if (n.kind() != ObjectKind::SpaceWarp) return false;
                  for (uint32_t id : sel)
                    if (const Node* t = doc->scene().findNode(id); t && canBindToSpaceWarp(*t, n)) return true;
                  return false;
                },
                [doc, sel](uint32_t warp) {
                  const Node* wn = doc->scene().findNode(warp);
                  if (!wn) return;
                  doc->beginEdit(QObject::tr("Bind to Space Warp"));
                  QVector<uint32_t> changed;
                  for (uint32_t id : sel)
                    if (Node* t = doc->scene().findNode(id); t && bindToSpaceWarp(*t, *wn)) changed.push_back(id);
                  doc->notifyNodesChanged(changed, ChangeGeometry);
                  doc->endEdit();
                });
}

void installRigTools(MainWindow& w) {
  registerCreationTool("Bone", [] { return std::make_shared<BoneTool>(); });
  registerModifyRollouts("Skin", skinRollout);
  registerModifyRollouts("Morpher", morpherRollout);
  registerModifyRollouts("PathDeform", [](const ModifyPanelContext& ctx, QWidget* parent) {
    return pickParamRollout(ctx, parent, QObject::tr("Pick Path"), "path", [](const Node& n) { return n.kind() == ObjectKind::Shape; });
  });
  registerModifyRollouts("UDeflector", [](const ModifyPanelContext& ctx, QWidget* parent) {
    return pickParamRollout(ctx, parent, QObject::tr("Pick Object"), "object",
                            [](const Node& n) { return n.kind() == ObjectKind::Geometry; });
  });
  registerModifyRollouts("SpaceWarpBinding", [](const ModifyPanelContext& ctx, QWidget* parent) {
    return pickParamRollout(ctx, parent, QObject::tr("Pick Space Warp"), "warp",
                            [](const Node& n) { return n.kind() == ObjectKind::SpaceWarp; });
  });
  registerModifyRollouts("IKChain", [](const ModifyPanelContext& ctx, QWidget* parent) {
    auto* box = new QWidget(parent);
    auto* v = new QVBoxLayout(box);
    v->setContentsMargins(0, 0, 0, 0);
    v->addWidget(pickParamRollout(ctx, box, QObject::tr("Pick Start Joint"), "startBone", {}));
    v->addWidget(pickParamRollout(ctx, box, QObject::tr("Pick End Joint"), "endBone", {}));
    return box;
  });
  w.editorState()->addDecorator(std::make_shared<SkinDecorator>(w.document()));

  QMenu* tools = w.menuAt("Tools");
  QAction* bind = tools->addAction(animicons::bindSpaceWarp(), QObject::tr("Bind to Space Warp"));
  w.registerCommand("tools.bindSpaceWarp", bind);
  QObject::connect(bind, &QAction::triggered, &w, [&w] { bindToSpaceWarpTool(w); });
  if (QToolBar* tb = w.mainToolBar()) tb->addAction(bind);
}

}  // namespace

VERAA_APP_EXTENSION(rigTools, 307, installRigTools);

}  // namespace veraa
