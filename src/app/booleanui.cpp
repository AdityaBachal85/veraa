// Create > Geometry > Compound Objects > Boolean: turns the selected object into a Boolean
// compound and lets the user pick more operands in the viewports, each with its own operation.
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QUndoStack>
#include <QVBoxLayout>

#include <cmath>

#include "commandpanel.h"
#include "csg.h"
#include "document.h"
#include "editorstate.h"
#include "mainwindow.h"
#include "paramform.h"
#include "polyops.h"
#include "selftest.h"
#include "viewport.h"
#include "viewportpanel.h"

namespace veraa {

namespace {

const char* const kOpNames[] = {"Union", "Intersection", "Subtraction (A-B)", "Subtraction (B-A)", "Merge"};

BooleanObject* booleanOf(Node* n) { return n ? dynamic_cast<BooleanObject*>(n->object.get()) : nullptr; }

// Converts the node into a Boolean whose operand A is its current (evaluated) mesh.
bool makeBoolean(SceneDocument* doc, uint32_t id) {
  Node* n = doc->scene().findNode(id);
  if (!n || n->kind() != ObjectKind::Geometry) return false;
  if (booleanOf(n)) return true;
  doc->beginEdit(QObject::tr("Create Boolean"));
  auto b = std::make_shared<BooleanObject>();
  b->meshA = n->mesh();
  b->nameA = n->name;
  n->object = b;
  n->modifiers.clear();
  doc->notifyStructureChanged();  // first: panels drop references to the replaced object
  doc->notifyNodesChanged({id}, ChangeGeometry);
  doc->endEdit();
  return true;
}

// Adds `operandId` to the Boolean on `id` (moving or copying the operand).
bool addOperand(SceneDocument* doc, uint32_t id, uint32_t operandId, CsgOp op, bool keepOriginal) {
  Node* self = doc->scene().findNode(id);
  Node* other = doc->scene().findNode(operandId);
  BooleanObject* b = booleanOf(self);
  if (!b || !other || other == self || other->kind() != ObjectKind::Geometry) return false;
  doc->beginEdit(QObject::tr("Pick Boolean Operand"));
  BooleanObject::Operand o;
  o.name = other->name;
  o.mesh = other->mesh();
  o.transform = self->objectWorldMatrix().inverse() * other->objectWorldMatrix();
  o.op = op;
  o.materialId = other->materialId;
  b->operands.push_back(std::move(o));
  b->touch();
  if (!keepOriginal) doc->scene().removeNode(operandId);
  doc->notifyNodesChanged({id}, ChangeGeometry);
  doc->notifyStructureChanged();
  doc->endEdit();
  return true;
}

class BooleanPickTool : public ViewportTool {
 public:
  BooleanPickTool(SceneDocument* doc, uint32_t id, std::function<void()> done) : doc_(doc), id_(id), done_(std::move(done)) {}
  bool mousePress(Viewport& vp, QMouseEvent* e) override {
    if (e->button() == Qt::RightButton) {
      finish(vp);
      return true;
    }
    if (e->button() != Qt::LeftButton) return false;
    uint32_t hit = vp.pickNode(static_cast<float>(e->position().x()), static_cast<float>(e->position().y()));
    Node* self = doc_->scene().findNode(id_);
    BooleanObject* b = booleanOf(self);
    if (!b || !hit || hit == id_) return true;
    CsgOp op = static_cast<CsgOp>(b->params.getInt("operation"));
    if (addOperand(doc_, id_, hit, op, keepOriginal)) vp.editorState()->setPrompt(QObject::tr("Operand added"));
    return true;
  }
  bool mouseRelease(Viewport&, QMouseEvent* e) override { return e->button() == Qt::LeftButton || e->button() == Qt::RightButton; }
  bool cancel() override {
    if (done_) done_();
    return false;
  }
  QString prompt() const override { return QObject::tr("Pick operand B (right-click to finish)"); }
  void drawOverlay(Viewport&, QPainter& p) override {
    p.setPen(QColor(120, 220, 255));
    p.drawText(QRectF(8, 24, 300, 18), Qt::AlignLeft | Qt::AlignVCenter, QObject::tr("Pick Operand"));
  }
  bool keepOriginal = false;
  uint32_t nodeId() const { return id_; }
  void setDone(std::function<void()> done) { done_ = std::move(done); }

 private:
  void finish(Viewport& vp) {
    vp.editorState()->setViewportTool(nullptr);
    if (done_) done_();
  }
  SceneDocument* doc_;
  uint32_t id_;
  std::function<void()> done_;
};

class BooleanPanel : public QWidget {
 public:
  BooleanPanel(const ModifyPanelContext& ctx, QWidget* parent) : QWidget(parent), ctx_(ctx) {
    setObjectName("BooleanPanel");
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    auto* r = new Rollout(tr("Boolean Parameters"), this);
    v->addWidget(r);
    QFormLayout* f = r->form();
    pick_ = new QPushButton(tr("Pick Operand"), r->body());
    pick_->setObjectName("bool_pick");
    pick_->setCheckable(true);
    f->addRow(pick_);
    move_ = new QRadioButton(tr("Move"), r->body());
    copy_ = new QRadioButton(tr("Copy"), r->body());
    move_->setChecked(true);
    auto* row = new QWidget(r->body());
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->addWidget(move_);
    h->addWidget(copy_);
    f->addRow(tr("Operand:"), row);
    list_ = new QListWidget(r->body());
    list_->setObjectName("bool_operands");
    list_->setMaximumHeight(120);
    f->addRow(list_);
    opCombo_ = new QComboBox(r->body());
    opCombo_->setObjectName("bool_operandOp");
    for (const char* n : kOpNames) opCombo_->addItem(tr(n));
    f->addRow(tr("Operation:"), opCombo_);
    auto* extract = new QPushButton(tr("Extract Operand"), r->body());
    extract->setObjectName("bool_extract");
    auto* remove = new QPushButton(tr("Remove Operand"), r->body());
    remove->setObjectName("bool_remove");
    f->addRow(extract);
    f->addRow(remove);

    // A panel rebuilt while picking (the scene changed) keeps the pick mode going.
    if (auto tool = std::dynamic_pointer_cast<BooleanPickTool>(ctx_.state->viewportToolShared());
        tool && tool->nodeId() == ctx_.nodeId) {
      pick_->setChecked(true);
      QPointer<BooleanPanel> self(this);
      tool->setDone([self] {
        if (!self) return;
        QSignalBlocker b(self->pick_);
        self->pick_->setChecked(false);
      });
    }
    connect(pick_, &QPushButton::toggled, this, [this](bool on) {
      if (on) {
        QPointer<BooleanPanel> self(this);
        auto tool = std::make_shared<BooleanPickTool>(ctx_.doc, ctx_.nodeId, [self] {
          if (!self) return;
          QSignalBlocker b(self->pick_);
          self->pick_->setChecked(false);
        });
        tool->keepOriginal = copy_->isChecked();
        ctx_.state->setViewportTool(tool);
        ctx_.state->setPrompt(tool->prompt());
      } else if (std::dynamic_pointer_cast<BooleanPickTool>(ctx_.state->viewportToolShared())) {
        ctx_.state->setViewportTool(nullptr);
      }
    });
    connect(opCombo_, &QComboBox::activated, this, [this](int op) {
      int row = list_->currentRow() - 1;
      BooleanObject* b = booleanOf(ctx_.doc->scene().findNode(ctx_.nodeId));
      if (!b || row < 0 || row >= static_cast<int>(b->operands.size())) return;
      ctx_.doc->beginEdit(tr("Boolean Operation"));
      b->operands[row].op = static_cast<CsgOp>(op);
      b->touch();
      ctx_.doc->notifyNodesChanged({ctx_.nodeId}, ChangeGeometry);
      ctx_.doc->endEdit();
    });
    connect(list_, &QListWidget::currentRowChanged, this, [this](int row) {
      BooleanObject* b = booleanOf(ctx_.doc->scene().findNode(ctx_.nodeId));
      QSignalBlocker blk(opCombo_);
      opCombo_->setEnabled(b && row >= 1);
      if (b && row >= 1 && row - 1 < static_cast<int>(b->operands.size()))
        opCombo_->setCurrentIndex(static_cast<int>(b->operands[row - 1].op));
    });
    connect(extract, &QPushButton::clicked, this, [this] { takeOperand(true); });
    connect(remove, &QPushButton::clicked, this, [this] { takeOperand(false); });
    connect(ctx_.doc, &SceneDocument::nodesChanged, this, [this](const QVector<uint32_t>& ids, int) {
      if (ids.contains(ctx_.nodeId)) refresh();
    });
    refresh();
  }

  ~BooleanPanel() override {
    // Keep picking when the panel is only being rebuilt for the same object.
    Node* sel = ctx_.doc->firstSelected();
    if (sel && sel->id == ctx_.nodeId && booleanOf(sel)) return;
    if (std::dynamic_pointer_cast<BooleanPickTool>(ctx_.state->viewportToolShared())) ctx_.state->setViewportTool(nullptr);
  }

  void refresh() {
    BooleanObject* b = booleanOf(ctx_.doc->scene().findNode(ctx_.nodeId));
    if (!b) return;
    int row = list_->currentRow();
    QSignalBlocker blk(list_);
    list_->clear();
    list_->addItem(QString("A: %1").arg(QString::fromStdString(b->nameA)));
    for (size_t i = 0; i < b->operands.size(); ++i)
      list_->addItem(QString("B%1: %2  [%3]")
                         .arg(i + 1)
                         .arg(QString::fromStdString(b->operands[i].name), tr(kOpNames[static_cast<int>(b->operands[i].op)])));
    list_->setCurrentRow(std::min(std::max(row, 0), list_->count() - 1));
    opCombo_->setEnabled(list_->currentRow() >= 1);
  }

 private:
  void takeOperand(bool extract) {
    int row = list_->currentRow() - 1;
    Node* self = ctx_.doc->scene().findNode(ctx_.nodeId);
    BooleanObject* b = booleanOf(self);
    if (!b || row < 0 || row >= static_cast<int>(b->operands.size())) {
      ctx_.state->setPrompt(tr("Select an operand B in the list"));
      return;
    }
    ctx_.doc->beginEdit(extract ? tr("Extract Operand") : tr("Remove Operand"));
    BooleanObject::Operand o = b->operands[row];
    b->operands.erase(b->operands.begin() + row);
    b->touch();
    if (extract) {
      auto poly = std::make_shared<EditablePolyObject>();
      poly->setMesh(o.mesh);
      auto node = std::make_unique<Node>();
      node->name = o.name.empty() ? "Operand" : o.name;
      node->object = poly;
      node->materialId = o.materialId;
      Node* added = ctx_.doc->scene().addNode(std::move(node));
      if (Node* s = ctx_.doc->scene().findNode(ctx_.nodeId)) added->setWorldMatrix(s->objectWorldMatrix() * o.transform);
    }
    ctx_.doc->notifyNodesChanged({ctx_.nodeId}, ChangeGeometry);
    ctx_.doc->notifyStructureChanged();
    ctx_.doc->endEdit();
  }

  ModifyPanelContext ctx_;
  QPushButton* pick_ = nullptr;
  QRadioButton *move_ = nullptr, *copy_ = nullptr;
  QListWidget* list_ = nullptr;
  QComboBox* opCombo_ = nullptr;
};

void installBoolean(MainWindow& w) {
  registerModifyRollouts("Boolean", [](const ModifyPanelContext& ctx, QWidget* parent) -> QWidget* {
    return new BooleanPanel(ctx, parent);
  });
  // Choosing Boolean in the Create panel converts the selected object (like 3ds Max).
  MainWindow* win = &w;
  QObject::connect(w.editorState(), &EditorState::createTypeChanged, &w, [win](const QString& type) {
    if (type != "Boolean") return;
    QTimer::singleShot(0, win, [win] {
      EditorState* st = win->editorState();
      if (st->createType() != "Boolean") return;
      st->setCreateType({});
      Node* n = win->document()->firstSelected();
      if (!n || n->kind() != ObjectKind::Geometry || win->document()->selection().size() != 1) {
        st->setPrompt(QObject::tr("Select one geometry object first, then click Boolean"));
        return;
      }
      makeBoolean(win->document(), n->id);
      win->commandPanel()->showModify();
      st->setPrompt(QObject::tr("Boolean created: click Pick Operand, then click objects to combine"));
    });
  });
}

// ---------------------------------------------------------------------------------------------
// Self test: Boolean subtract through the UI, with a volume check.

void testBoolean(MainWindow& w) {
  using testing::check;
  using testing::pump;
  SceneDocument* doc = w.document();
  EditorState* st = w.editorState();
  doc->beginEdit("Create");
  Node* a = doc->createNode("Box");
  Node* b = doc->createNode("Box");
  b->transform.position = Vec3(50, 50, 50);
  doc->endEdit();
  uint32_t ida = a->id, idb = b->id;
  doc->setSelection({ida});
  st->setCreateType("Boolean");
  pump(80);
  a = doc->scene().findNode(ida);
  check(booleanOf(a) != nullptr, "Boolean created from the selected box");
  check(!st->creating(), "Boolean leaves creation mode");
  QPushButton* pick = nullptr;
  for (QPushButton* p : w.findChildren<QPushButton*>("bool_pick"))
    if (p->isVisible()) pick = p;
  check(pick != nullptr, "Pick Operand button");
  if (!pick) return;
  pick->click();
  pump(30);
  Viewport* vp = w.viewports()->viewport(3);
  w.viewports()->setActive(vp);
  vp->zoomExtents(false);
  pump(30);
  // Click a point of box B that is outside box A.
  Node* nb = doc->scene().findNode(idb);
  QPointF at = testing::projectPoint(vp, nb->objectWorldMatrix().transformPoint(Vec3(0, 0, 100)));
  vp->simulateMouse(QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
  vp->simulateMouse(QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
  pump(50);
  a = doc->scene().findNode(ida);
  check(booleanOf(a) && booleanOf(a)->operands.size() == 1, "operand B picked");
  check(doc->scene().findNode(idb) == nullptr, "operand node moved into the Boolean");
  double vol = a ? meshVolume(a->mesh()) : 0.0;
  check(std::fabs(vol - (1e6 - 125000.0)) < 50.0, "A - B volume is 875000");
  if (QString dir = qEnvironmentVariable("VERAA_SCREENSHOT_DIR"); !dir.isEmpty()) {
    vp->setShading(ShadingMode::Shaded);
    testing::saveGrab(&w, dir + "/boolean_01_subtract.png");
  }
  st->setViewportTool(nullptr);
  doc->undoStack()->undo();
  pump(50);
  check(doc->scene().findNode(idb) != nullptr, "undo brings operand B back");
}

}  // namespace

VERAA_APP_EXTENSION(booleanCompound, 220, installBoolean);
VERAA_SELF_TEST(booleanCompound, testBoolean);

}  // namespace veraa
