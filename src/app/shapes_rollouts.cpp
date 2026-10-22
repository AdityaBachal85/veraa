// Modify panel rollouts for shapes and construction objects: Text/TextPlus (font and multi-line
// text), Section (update / create shape), Sweep custom section, Bevel Profile, Trim/Extend
// picks, Railing path, Wall from spline and Compound Object operands. Also creates compound
// objects from the selection and adds Convert to Editable Spline.
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QTimer>
#include <QVBoxLayout>

#include "aec.h"
#include "commandpanel.h"
#include "glscene.h"
#include "mainwindow.h"
#include "paramform.h"
#include "shape_modifiers.h"
#include "shapes.h"
#include "shapes_app.h"
#include "viewport.h"

namespace veraa {
namespace shapesapp {

void updateSection(SceneDocument* doc, Node* section) {
  auto* so = section ? dynamic_cast<SectionShape*>(section->object.get()) : nullptr;
  if (!so) return;
  std::vector<std::pair<const TriMesh*, Mat4>> meshes;
  const Scene& scene = doc->scene();
  for (const auto& up : scene.nodes) {
    const Node& n = *up;
    if (n.id == section->id || !scene.isNodeVisible(n)) continue;
    if (n.kind() != ObjectKind::Geometry && n.kind() != ObjectKind::Shape) continue;
    if (n.triMesh().empty()) continue;
    meshes.emplace_back(&n.triMesh(), n.objectWorldMatrix());
  }
  bool bounded = so->params.getInt("extents") == 1;
  so->shape = sliceMeshes(meshes, section->objectWorldMatrix(), bounded ? so->params.getFloat("length") * 0.5f : 0.0f,
                          bounded ? so->params.getFloat("width") * 0.5f : 0.0f);
  so->touch();
  doc->notifyNodeChanged(section, ChangeGeometry);
}

namespace {

QString tr(const char* s) { return QCoreApplication::translate("Shapes", s); }

Node* nodeOf(const ModifyPanelContext& ctx) { return ctx.doc->scene().findNode(ctx.nodeId); }

template <class T>
T* modifierOf(const ModifyPanelContext& ctx) {
  Node* n = nodeOf(ctx);
  if (!n || ctx.stackIndex < 0 || ctx.stackIndex >= static_cast<int>(n->modifiers.size())) return nullptr;
  return dynamic_cast<T*>(n->modifiers[ctx.stackIndex].get());
}

bool isShapeNode(const Node& n) { return n.kind() == ObjectKind::Shape && n.isShapeResult(); }
bool isMeshNode(const Node& n) {
  return (n.kind() == ObjectKind::Geometry || n.kind() == ObjectKind::Shape) && !n.mesh().empty();
}

// A pick button: checked while its pick tool is active.
QPushButton* pickButton(const QString& text, const ModifyPanelContext& ctx, QWidget* parent,
                        std::function<std::shared_ptr<ViewportTool>()> makeTool) {
  auto* b = new QPushButton(text, parent);
  b->setCheckable(true);
  auto tool = std::make_shared<std::weak_ptr<ViewportTool>>();
  EditorState* st = ctx.state;
  QObject::connect(b, &QPushButton::clicked, b, [b, st, tool, makeTool](bool on) {
    if (on) {
      auto t = makeTool();
      *tool = t;
      st->setViewportTool(t);
    } else if (auto t = tool->lock(); t && st->viewportTool() == t.get()) {
      st->setViewportTool(nullptr);
    }
  });
  QObject::connect(st, &EditorState::viewportToolChanged, b, [b, st, tool] {
    auto t = tool->lock();
    QSignalBlocker blk(b);
    b->setChecked(t && st->viewportTool() == t.get());
  });
  return b;
}

// ---------------------------------------------------------------------------------------------
// Text / TextPlus

QWidget* textRollout(const ModifyPanelContext& ctx, QWidget* parent) {
  Node* n = nodeOf(ctx);
  if (!n || ctx.stackIndex >= 0) return nullptr;
  auto* roll = new Rollout(tr("Text"), parent);
  auto* font = new QComboBox(roll->body());
  for (const auto& f : availableFonts()) font->addItem(QString::fromStdString(f));
  QString current = QString::fromStdString(n->object->params.getString("font"));
  if (font->findText(current) < 0) font->addItem(current);
  font->setCurrentText(current);
  roll->form()->addRow(tr("Font"), font);
  auto* text = new QPlainTextEdit(roll->body());
  text->setPlainText(QString::fromStdString(n->object->params.getString("text")));
  text->setMinimumHeight(70);
  text->setMaximumHeight(110);
  roll->form()->addRow(text);
  SceneDocument* doc = ctx.doc;
  uint32_t id = ctx.nodeId;
  auto set = [doc, id](const char* key, const QString& v) {
    Node* node = doc->scene().findNode(id);
    if (!node) return;
    doc->softEdit(tr("Change %1").arg(key), QString("%1/text/%2").arg(id).arg(key));
    if (node->object->params.setString(key, v.toStdString())) {
      node->object->touch();
      doc->notifyNodeChanged(node, ChangeGeometry);
    }
  };
  QObject::connect(font, &QComboBox::currentTextChanged, roll, [set](const QString& f) { set("font", f); });
  QObject::connect(text, &QPlainTextEdit::textChanged, roll, [set, text] { set("text", text->toPlainText()); });
  return roll;
}

// ---------------------------------------------------------------------------------------------
// Section

QWidget* sectionRollout(const ModifyPanelContext& ctx, QWidget* parent) {
  if (ctx.stackIndex >= 0) return nullptr;
  auto* roll = new Rollout(tr("Section Parameters"), parent);
  auto* update = new QPushButton(tr("Update Section"), roll->body());
  auto* create = new QPushButton(tr("Create Shape"), roll->body());
  roll->form()->addRow(update);
  roll->form()->addRow(create);
  SceneDocument* doc = ctx.doc;
  uint32_t id = ctx.nodeId;
  QObject::connect(update, &QPushButton::clicked, roll, [doc, id] {
    Node* n = doc->scene().findNode(id);
    if (!n) return;
    doc->beginEdit(tr("Update Section"));
    updateSection(doc, n);
    doc->endEdit();
  });
  QObject::connect(create, &QPushButton::clicked, roll, [doc, id] {
    Node* n = doc->scene().findNode(id);
    auto* so = n ? dynamic_cast<SectionShape*>(n->object.get()) : nullptr;
    if (!so || so->shape.empty()) return;
    doc->beginEdit(tr("Create Shape"));
    auto node = std::make_unique<Node>();
    auto obj = std::make_shared<EditableSplineObject>();
    obj->shape = so->shape;
    node->object = obj;
    node->name = doc->scene().uniqueNodeName("SectionShape");
    node->transform = n->transform;
    node->parentId = n->parentId;
    node->objectOffset = n->objectOffset;
    node->wireColor = Vec3(1.0f, 0.8f, 0.1f);
    Node* added = doc->scene().addNode(std::move(node));
    doc->notifyStructureChanged();
    doc->setSelection({added->id});
    doc->endEdit();
  });
  return roll;
}

// Draws Section planes (their length x width rectangle).
class SectionDecorator : public ViewportDecorator {
 public:
  void drawLines(Viewport& vp, LineBatch& lines) override {
    const Scene& scene = vp.document()->scene();
    for (const auto& up : scene.nodes) {
      const Node& n = *up;
      if (!scene.isNodeVisible(n) || !n.object || n.object->type() != "Section") continue;
      Mat4 m = n.objectWorldMatrix();
      float hl = n.object->params.getFloat("length") * 0.5f, hw = n.object->params.getFloat("width") * 0.5f;
      Vec3 c[4] = {m.transformPoint(Vec3(-hw, -hl, 0)), m.transformPoint(Vec3(hw, -hl, 0)),
                   m.transformPoint(Vec3(hw, hl, 0)), m.transformPoint(Vec3(-hw, hl, 0))};
      QColor col = vp.document()->isSelected(n.id) ? QColor(255, 255, 255) : QColor(240, 200, 40);
      for (int i = 0; i < 4; ++i) lines.line(c[i], c[(i + 1) % 4], col);
      lines.line(c[0], c[2], col.darker(150));
      lines.line(c[1], c[3], col.darker(150));
    }
  }
};

// ---------------------------------------------------------------------------------------------
// Sweep, Bevel Profile, Trim/Extend

QWidget* sweepRollout(const ModifyPanelContext& ctx, QWidget* parent) {
  auto* mod = modifierOf<SweepModifier>(ctx);
  if (!mod) return nullptr;
  auto* roll = new Rollout(tr("Custom Section"), parent);
  auto* name = new QLabel(mod->customSectionName.empty() ? tr("(none)") : QString::fromStdString(mod->customSectionName),
                          roll->body());
  roll->form()->addRow(tr("Section:"), name);
  SceneDocument* doc = ctx.doc;
  ModifyPanelContext c = ctx;
  QPointer<QLabel> label = name;
  auto* pick = pickButton(tr("Pick"), ctx, roll->body(), [doc, c, label] {
    return makePickTool(
        tr("Pick a shape to use as the sweep section"),
        [c](const Node& n) { return n.id != c.nodeId && isShapeNode(n); },
        [doc, c, label](Node& picked) {
          auto* m = modifierOf<SweepModifier>(c);
          Node* n = nodeOf(c);
          if (!m || !n) return;
          doc->beginEdit(tr("Pick Sweep Section"));
          m->customSection = evaluatedShape(picked);
          m->customSectionName = picked.name;
          m->params.setNumber("section", SweepModifier::customSectionIndex());
          m->touch();
          doc->notifyNodeChanged(n, ChangeGeometry);
          doc->endEdit();
          if (label) label->setText(QString::fromStdString(picked.name));
        },
        false);
  });
  roll->form()->addRow(pick);
  return roll;
}

QWidget* bevelProfileRollout(const ModifyPanelContext& ctx, QWidget* parent) {
  auto* mod = modifierOf<BevelProfileModifier>(ctx);
  if (!mod) return nullptr;
  auto* roll = new Rollout(tr("Bevel Profile"), parent);
  auto* name =
      new QLabel(mod->profileName.empty() ? tr("(none)") : QString::fromStdString(mod->profileName), roll->body());
  roll->form()->addRow(tr("Profile:"), name);
  SceneDocument* doc = ctx.doc;
  ModifyPanelContext c = ctx;
  QPointer<QLabel> label = name;
  auto* pick = pickButton(tr("Pick Profile"), ctx, roll->body(), [doc, c, label] {
    return makePickTool(
        tr("Pick the profile shape"), [c](const Node& n) { return n.id != c.nodeId && isShapeNode(n); },
        [doc, c, label](Node& picked) {
          auto* m = modifierOf<BevelProfileModifier>(c);
          Node* n = nodeOf(c);
          if (!m || !n) return;
          doc->beginEdit(tr("Pick Profile"));
          m->profile = evaluatedShape(picked);
          m->profileName = picked.name;
          m->touch();
          doc->notifyNodeChanged(n, ChangeGeometry);
          doc->endEdit();
          if (label) label->setText(QString::fromStdString(picked.name));
        },
        false);
  });
  roll->form()->addRow(pick);
  return roll;
}

class TrimPickTool : public ViewportTool {
 public:
  TrimPickTool(ModifyPanelContext ctx, std::function<bool()> extend, std::function<void()> changed)
      : ctx_(ctx), extend_(std::move(extend)), changed_(std::move(changed)) {}
  bool mousePress(Viewport& vp, QMouseEvent* e) override {
    if (e->button() == Qt::RightButton) {
      ending_ = true;  // ends on release, so the release doesn't open the quad menu
      return true;
    }
    if (e->button() != Qt::LeftButton) return false;
    Node* n = nodeOf(ctx_);
    auto* m = modifierOf<TrimExtendModifier>(ctx_);
    Vec3 local;
    if (!n || !m || !hitNodePlane(vp, *n, static_cast<float>(e->position().x()), static_cast<float>(e->position().y()), 0,
                                 local))
      return true;
    ctx_.doc->beginEdit(extend_() ? tr("Extend") : tr("Trim"));
    m->ops.push_back({extend_(), local});
    m->touch();
    ctx_.doc->notifyNodeChanged(n, ChangeGeometry);
    ctx_.doc->endEdit();
    changed_();
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
  QString prompt() const override {
    return tr("Click spline pieces to trim them (or open ends to extend them); right-click to stop");
  }

 private:
  bool ending_ = false;
  ModifyPanelContext ctx_;
  std::function<bool()> extend_;
  std::function<void()> changed_;
};

QWidget* trimExtendRollout(const ModifyPanelContext& ctx, QWidget* parent) {
  auto* mod = modifierOf<TrimExtendModifier>(ctx);
  if (!mod) return nullptr;
  auto* roll = new Rollout(tr("Pick Locations"), parent);
  auto* trim = new QRadioButton(tr("Trim"), roll->body());
  auto* extend = new QRadioButton(tr("Extend"), roll->body());
  trim->setChecked(true);
  auto* row = new QHBoxLayout;
  row->addWidget(trim);
  row->addWidget(extend);
  roll->form()->addRow(row);
  auto* count = new QLabel(tr("%1 operation(s)").arg(mod->ops.size()), roll->body());
  QPointer<QLabel> countP = count;
  QPointer<QRadioButton> extP = extend;
  ModifyPanelContext c = ctx;
  auto refresh = [c, countP] {
    if (auto* m = modifierOf<TrimExtendModifier>(c); m && countP) countP->setText(tr("%1 operation(s)").arg(m->ops.size()));
  };
  auto* pick = pickButton(tr("Pick Locations"), ctx, roll->body(), [c, extP, refresh] {
    return std::make_shared<TrimPickTool>(c, [extP] { return extP && extP->isChecked(); }, refresh);
  });
  auto* clear = new QPushButton(tr("Clear Operations"), roll->body());
  QObject::connect(clear, &QPushButton::clicked, roll, [c, refresh] {
    auto* m = modifierOf<TrimExtendModifier>(c);
    Node* n = nodeOf(c);
    if (!m || !n || m->ops.empty()) return;
    c.doc->beginEdit(tr("Clear Trim/Extend"));
    m->ops.clear();
    m->touch();
    c.doc->notifyNodeChanged(n, ChangeGeometry);
    c.doc->endEdit();
    refresh();
  });
  roll->form()->addRow(pick);
  roll->form()->addRow(clear);
  roll->form()->addRow(count);
  return roll;
}

// ---------------------------------------------------------------------------------------------
// Railing path and Wall from spline

QWidget* railingRollout(const ModifyPanelContext& ctx, QWidget* parent) {
  Node* n = nodeOf(ctx);
  auto* rail = n ? dynamic_cast<RailingObject*>(n->object.get()) : nullptr;
  if (!rail || ctx.stackIndex >= 0) return nullptr;
  auto* roll = new Rollout(tr("Railing Path"), parent);
  auto* name = new QLabel(rail->pathName.empty() ? tr("(straight)") : QString::fromStdString(rail->pathName), roll->body());
  roll->form()->addRow(tr("Path:"), name);
  QPointer<QLabel> label = name;
  ModifyPanelContext c = ctx;
  auto* pick = pickButton(tr("Pick Railing Path"), ctx, roll->body(), [c, label] {
    return makePickTool(
        tr("Pick a shape for the railing to follow"), [c](const Node& p) { return p.id != c.nodeId && isShapeNode(p); },
        [c, label](Node& picked) {
          Node* node = nodeOf(c);
          auto* r = node ? dynamic_cast<RailingObject*>(node->object.get()) : nullptr;
          if (!r) return;
          c.doc->beginEdit(tr("Pick Railing Path"));
          r->path = evaluatedShape(picked);
          r->path.transform(node->objectWorldMatrix().inverse() * picked.objectWorldMatrix());
          r->pathName = picked.name;
          r->touch();
          c.doc->notifyNodeChanged(node, ChangeGeometry);
          c.doc->endEdit();
          if (label) label->setText(QString::fromStdString(picked.name));
        },
        false);
  });
  auto* clear = new QPushButton(tr("Clear Path"), roll->body());
  QObject::connect(clear, &QPushButton::clicked, roll, [c, label] {
    Node* node = nodeOf(c);
    auto* r = node ? dynamic_cast<RailingObject*>(node->object.get()) : nullptr;
    if (!r || r->path.empty()) return;
    c.doc->beginEdit(tr("Clear Railing Path"));
    r->path = BezierShape();
    r->pathName.clear();
    r->touch();
    c.doc->notifyNodeChanged(node, ChangeGeometry);
    c.doc->endEdit();
    if (label) label->setText(tr("(straight)"));
  });
  roll->form()->addRow(pick);
  roll->form()->addRow(clear);
  return roll;
}

QWidget* wallRollout(const ModifyPanelContext& ctx, QWidget* parent) {
  if (ctx.stackIndex >= 0) return nullptr;
  auto* roll = new Rollout(tr("Wall Path"), parent);
  ModifyPanelContext c = ctx;
  auto* pick = pickButton(tr("Pick Spline"), ctx, roll->body(), [c] {
    return makePickTool(
        tr("Pick a spline to build the wall along"), [c](const Node& p) { return p.id != c.nodeId && isShapeNode(p); },
        [c](Node& picked) {
          Node* node = nodeOf(c);
          auto* w = node ? dynamic_cast<WallObject*>(node->object.get()) : nullptr;
          BezierShape s = evaluatedShape(picked);
          if (!w || s.empty()) return;
          s.transform(node->objectWorldMatrix().inverse() * picked.objectWorldMatrix());
          const Spline& sp = s.splines.front();
          std::vector<Vec2> pts;
          for (const auto& p : sp.polyline(4, true))
            if (pts.empty() || std::hypot(p.x - pts.back().x, p.y - pts.back().y) > 1e-3f) pts.emplace_back(p.x, p.y);
          if (pts.size() < 2) return;
          c.doc->beginEdit(tr("Wall from Spline"));
          w->points = pts;
          w->closed = sp.closed;
          w->touch();
          c.doc->notifyNodeChanged(node, ChangeGeometry);
          c.doc->endEdit();
        },
        false);
  });
  roll->form()->addRow(pick);
  return roll;
}

// ---------------------------------------------------------------------------------------------
// Compound objects

QWidget* compoundRollout(const ModifyPanelContext& ctx, QWidget* parent) {
  Node* n = nodeOf(ctx);
  auto* comp = n ? dynamic_cast<CompoundObject*>(n->object.get()) : nullptr;
  if (!comp || ctx.stackIndex >= 0) return nullptr;
  bool loft = comp->type() == "Loft";
  auto* roll = new Rollout(loft ? tr("Creation Method") : tr("Pick Operand"), parent);
  auto* list = new QListWidget(roll->body());
  list->setMaximumHeight(110);
  ModifyPanelContext c = ctx;
  QPointer<QListWidget> listP = list;
  auto refresh = [c, listP] {
    Node* node = nodeOf(c);
    auto* cp = node ? dynamic_cast<CompoundObject*>(node->object.get()) : nullptr;
    if (!cp || !listP) return;
    int row = listP->currentRow();
    listP->clear();
    for (const auto& op : cp->operands) {
      QString text = QString("%1: %2").arg(QString::fromStdString(op.role), QString::fromStdString(op.name));
      if (cp->type() == "Loft" && op.role == "shape") text += QString(" @ %1%").arg(op.percent, 0, 'f', 1);
      listP->addItem(text);
    }
    if (row >= 0 && row < listP->count()) listP->setCurrentRow(row);
  };
  refresh();
  // Copy or Move the picked object into the compound.
  auto* copy = new QRadioButton(tr("Copy"), roll->body());
  auto* move = new QRadioButton(tr("Move"), roll->body());
  bool defaultMove = comp->type() == "Connect" || comp->type() == "ShapeMerge" || comp->type() == "Conform";
  (defaultMove ? move : copy)->setChecked(true);
  auto* mode = new QHBoxLayout;
  mode->addWidget(copy);
  mode->addWidget(move);
  roll->form()->addRow(mode);
  QPointer<QRadioButton> moveP = move;
  QDoubleSpinBox* percent = nullptr;
  if (loft) {
    percent = new QDoubleSpinBox(roll->body());
    percent->setRange(0, 100);
    percent->setDecimals(1);
    percent->setSuffix("%");
    roll->form()->addRow(tr("Path:"), percent);
    installSpinnerScrub(percent);
  }
  QPointer<QDoubleSpinBox> percentP = percent;
  for (const CompoundRole& role : comp->roles()) {
    CompoundRole r = role;
    auto* b = pickButton(QString::fromStdString(r.label), ctx, roll->body(), [c, r, refresh, moveP, percentP] {
      return makePickTool(
          tr("Pick the operand: %1").arg(QString::fromStdString(r.label)),
          [c, r](const Node& p) { return p.id != c.nodeId && (r.shape ? isShapeNode(p) : isMeshNode(p)); },
          [c, r, refresh, moveP, percentP](Node& picked) {
            Node* node = nodeOf(c);
            auto* cp = node ? dynamic_cast<CompoundObject*>(node->object.get()) : nullptr;
            if (!cp) return;
            c.doc->beginEdit(tr("Pick Operand"));
            CompoundOperand op = captureOperand(picked, *node, r);
            if (percentP) op.percent = static_cast<float>(percentP->value());
            cp->setOperand(op);
            cp->touch();
            if (moveP && moveP->isChecked()) {
              uint32_t gone = picked.id;
              c.doc->scene().removeNode(gone);
              QVector<uint32_t> sel = c.doc->selection();
              sel.removeAll(gone);
              c.doc->notifyStructureChanged();
              c.doc->setSelection(sel);
            }
            c.doc->notifyNodeChanged(node, ChangeGeometry);
            c.doc->endEdit();
            refresh();
          },
          r.multiple);
    });
    roll->form()->addRow(b);
  }
  roll->form()->addRow(list);
  auto* buttons = new QHBoxLayout;
  auto* remove = new QPushButton(tr("Remove"), roll->body());
  auto* refreshOps = new QPushButton(tr("Refresh"), roll->body());
  refreshOps->setToolTip(tr("Re-read every operand from the object it was picked from"));
  buttons->addWidget(remove);
  buttons->addWidget(refreshOps);
  roll->form()->addRow(buttons);
  QObject::connect(remove, &QPushButton::clicked, roll, [c, listP, refresh] {
    Node* node = nodeOf(c);
    auto* cp = node ? dynamic_cast<CompoundObject*>(node->object.get()) : nullptr;
    int row = listP ? listP->currentRow() : -1;
    if (!cp || row < 0 || row >= static_cast<int>(cp->operands.size())) return;
    c.doc->beginEdit(tr("Remove Operand"));
    cp->operands.erase(cp->operands.begin() + row);
    cp->touch();
    c.doc->notifyNodeChanged(node, ChangeGeometry);
    c.doc->endEdit();
    refresh();
  });
  QObject::connect(refreshOps, &QPushButton::clicked, roll, [c, refresh] {
    Node* node = nodeOf(c);
    auto* cp = node ? dynamic_cast<CompoundObject*>(node->object.get()) : nullptr;
    if (!cp) return;
    c.doc->beginEdit(tr("Refresh Operands"));
    for (auto& op : cp->operands) {
      Node* src = op.sourceId ? c.doc->scene().findNode(op.sourceId) : nullptr;
      const CompoundRole* r = cp->findRole(op.role);
      if (!src || !r || src->id == node->id) continue;
      float pct = op.percent;
      op = captureOperand(*src, *node, *r);
      op.percent = pct;
    }
    cp->touch();
    c.doc->notifyNodeChanged(node, ChangeGeometry);
    c.doc->endEdit();
    refresh();
  });
  if (percent) {
    // Editing the spinner moves the selected Loft shape along the path.
    QObject::connect(list, &QListWidget::currentRowChanged, roll, [c, percentP](int row) {
      Node* node = nodeOf(c);
      auto* cp = node ? dynamic_cast<CompoundObject*>(node->object.get()) : nullptr;
      if (!cp || !percentP || row < 0 || row >= static_cast<int>(cp->operands.size())) return;
      if (cp->operands[row].role != "shape") return;
      QSignalBlocker b(percentP);
      percentP->setValue(cp->operands[row].percent);
    });
    QObject::connect(percent, &QDoubleSpinBox::valueChanged, roll, [c, listP, refresh](double v) {
      Node* node = nodeOf(c);
      auto* cp = node ? dynamic_cast<CompoundObject*>(node->object.get()) : nullptr;
      int row = listP ? listP->currentRow() : -1;
      if (!cp || row < 0 || row >= static_cast<int>(cp->operands.size()) || cp->operands[row].role != "shape") return;
      c.doc->softEdit(tr("Shape Path Level"), QString("%1/loftpct").arg(c.nodeId));
      cp->operands[row].percent = static_cast<float>(v);
      cp->touch();
      c.doc->notifyNodeChanged(node, ChangeGeometry);
      refresh();
    });
  }
  return roll;
}

// Creates a compound object from the selection when its type is chosen (like the Compound
// Objects buttons, which need a selected object).
void createCompoundFromSelection(MainWindow& w, const QString& type) {
  SceneDocument* doc = w.document();
  EditorState* st = w.editorState();
  auto obj = createObject(type.toStdString());
  auto* proto = dynamic_cast<CompoundObject*>(obj.get());
  if (!proto) return;
  CompoundRole first = proto->roles().front();
  std::vector<Node*> sources;
  for (Node* n : doc->selectedNodes())
    if (first.shape ? isShapeNode(*n) : isMeshNode(*n)) sources.push_back(n);
  st->setCreateType({});
  if (sources.empty()) {
    st->setPrompt(first.shape ? tr("Select a shape first, then choose %1").arg(type)
                              : tr("Select an object first, then choose %1").arg(type));
    return;
  }
  if (!first.multiple) sources.resize(1);
  doc->beginEdit(tr("Create %1").arg(type));
  Node* c = doc->createNode(type);
  if (!c) {
    doc->cancelEdit();
    return;
  }
  c->setWorldMatrix(sources[0]->objectWorldMatrix());
  auto* cp = dynamic_cast<CompoundObject*>(c->object.get());
  for (Node* s : sources) cp->setOperand(captureOperand(*s, *c, first));
  // The selected object becomes part of the compound, except Loft paths and Terrain contours.
  bool keep = type == "Loft" || type == "Terrain";
  if (!keep)
    for (Node* s : sources) {
      c->wireColor = s->wireColor;
      c->materialId = s->materialId;
      doc->scene().removeNode(s->id);
    }
  cp->touch();
  doc->notifyStructureChanged();
  doc->setSelection({c->id});
  doc->notifyNodeChanged(c, ChangeAll);
  doc->endEdit();
  w.commandPanel()->showModify();
  QString next = type == "Loft" ? tr("Use Get Shape to add cross sections") : tr("Pick the other operand in the Modify panel");
  st->setPrompt(next);
}

void convertToEditableSpline(SceneDocument* doc) {
  QVector<uint32_t> ids;
  for (Node* n : doc->selectedNodes())
    if (isShapeNode(*n)) ids.push_back(n->id);
  if (ids.isEmpty()) return;
  doc->beginEdit(tr("Convert to Editable Spline"));
  for (uint32_t id : ids) {
    Node* n = doc->scene().findNode(id);
    auto es = std::make_shared<EditableSplineObject>();
    es->shape = evaluatedShape(*n);
    for (const ParamDef& d : es->params.defs())
      if (n->object->params.has(d.key)) {
        const ParamDef* od = n->object->params.def(d.key);
        if (od && od->type == d.type) {
          if (d.type == ParamType::Bool) es->params.setNumber(d.key, n->object->params.getBool(d.key) ? 1 : 0);
          else if (d.type == ParamType::Int || d.type == ParamType::Enum) es->params.setNumber(d.key, n->object->params.getInt(d.key));
          else if (d.type == ParamType::Float) es->params.setNumber(d.key, n->object->params.getFloat(d.key));
        }
      }
    n->object = es;
    n->modifiers.clear();
  }
  doc->notifyNodesChanged(ids, ChangeGeometry);
  doc->notifyStructureChanged();
  doc->endEdit();
}

void installRollouts(MainWindow& w) {
  registerModifyRollouts("Text", textRollout);
  registerModifyRollouts("TextPlus", textRollout);
  registerModifyRollouts("Section", sectionRollout);
  registerModifyRollouts("Sweep", sweepRollout);
  registerModifyRollouts("BevelProfile", bevelProfileRollout);
  registerModifyRollouts("TrimExtend", trimExtendRollout);
  registerModifyRollouts("Railing", railingRollout);
  registerModifyRollouts("Wall", wallRollout);
  for (const char* t : {"Loft", "Scatter", "Connect", "ShapeMerge", "Terrain", "Conform", "BlobMesh"})
    registerModifyRollouts(t, compoundRollout);
  for (const char* t : {"EditableSpline", "Line", "Freehand", "EditSpline"})
    registerModifyRollouts(t, buildSplineEditRollout);
  w.editorState()->addDecorator(std::make_shared<SectionDecorator>());

  MainWindow* win = &w;
  QObject::connect(w.editorState(), &EditorState::createTypeChanged, &w, [win](const QString& type) {
    static const QStringList fromSelection = {"Loft", "Scatter", "Connect", "ShapeMerge", "Terrain", "Conform"};
    if (!fromSelection.contains(type)) return;
    QTimer::singleShot(0, win, [win, type] {
      if (win->editorState()->createType() == type) createCompoundFromSelection(*win, type);
    });
  });

  QMenu* tools = w.menuAt("Tools");
  QAction* convert = tools->addAction(tr("Convert to Editable Spline"));
  SceneDocument* doc = w.document();
  QObject::connect(convert, &QAction::triggered, &w, [doc] { convertToEditableSpline(doc); });
  w.registerCommand("convertToEditableSpline", convert);
}

}  // namespace
}  // namespace shapesapp

VERAA_APP_EXTENSION(shapesRollouts, 210, shapesapp::installRollouts);

}  // namespace veraa
