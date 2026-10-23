// Interactive creation for the Teapot and the Extended Primitives: drag out the footprint
// (radius or rectangle), then move the mouse to set height, fillet, widths and so on, clicking
// after each step (like the standard primitives).
#include <QMouseEvent>
#include <QPainter>

#include <cmath>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "document.h"
#include "editorstate.h"
#include "mainwindow.h"
#include "selftest.h"
#include "viewport.h"
#include "viewportpanel.h"

namespace veraa {

namespace {

struct Stage {
  std::vector<std::string> keys;  // parameters set by this step
  bool positive = true;           // clamp at zero
  float factor = 1.0f;            // value = factor * mouse distance
  QString prompt;
};

struct CreateSpec {
  bool rect = false;  // rectangle footprint (else radius from the press point)
  std::function<void(ParamBlock&, float dx, float dy, float r)> foot;
  std::vector<Stage> stages;
};

const std::map<QString, CreateSpec>& specs() {
  static std::map<QString, CreateSpec> m;
  if (!m.empty()) return m;
  auto radius = [](const char* key, float k = 1.0f) {
    return [key, k](ParamBlock& p, float, float, float r) { p.setNumber(key, r * k); };
  };
  Stage height{{"height"}, false, 1.0f, QObject::tr("Move to set the height, click to continue")};
  Stage heightPos{{"height"}, true, 1.0f, QObject::tr("Move to set the height, click to continue")};
  m["Teapot"] = {false, radius("radius"), {}};
  m["Hedra"] = {false, radius("radius"), {}};
  m["TorusKnot"] = {false, radius("radius"), {{{"radius2"}, true, 0.5f, QObject::tr("Move to set the tube radius")}}};
  m["ChamferCyl"] = {false, radius("radius"), {height, {{"fillet"}, true, 0.5f, QObject::tr("Move to set the fillet")}}};
  m["OilTank"] = {false, radius("radius"), {heightPos, {{"capHeight"}, true, 0.5f, QObject::tr("Move to set the cap height")}}};
  m["Capsule"] = {false, radius("radius"), {heightPos}};
  m["Spindle"] = {false, radius("radius"), {heightPos, {{"capHeight"}, true, 0.5f, QObject::tr("Move to set the cap height")}}};
  m["Gengon"] = {false, radius("radius"), {height, {{"fillet"}, true, 0.5f, QObject::tr("Move to set the fillet")}}};
  m["RingWave"] = {false, radius("radius"), {{{"ringWidth"}, true, 0.5f, QObject::tr("Move to set the ring width")}}};
  m["Hose"] = {false, radius("diameter", 2.0f), {heightPos}};
  m["LExt"] = {true,
               [](ParamBlock& p, float dx, float dy, float) {
                 p.setNumber("frontLength", std::fabs(dx));
                 p.setNumber("sideLength", std::fabs(dy));
               },
               {height, {{"sideWidth", "frontWidth"}, true, 0.5f, QObject::tr("Move to set the leg widths")}}};
  m["CExt"] = {true,
               [](ParamBlock& p, float dx, float dy, float) {
                 p.setNumber("backLength", std::fabs(dx));
                 p.setNumber("frontLength", std::fabs(dx));
                 p.setNumber("sideLength", std::fabs(dy));
               },
               {height, {{"backWidth", "sideWidth", "frontWidth"}, true, 0.5f, QObject::tr("Move to set the widths")}}};
  m["Prism"] = {true,
                [](ParamBlock& p, float dx, float dy, float) {
                  float a = std::fabs(dx), h = std::fabs(dy);
                  float side = std::sqrt(a * a * 0.25f + h * h);
                  p.setNumber("side1", a);
                  p.setNumber("side2", side);
                  p.setNumber("side3", side);
                },
                {height}};
  return m;
}

class PrimCreateTool : public ViewportTool {
 public:
  explicit PrimCreateTool(const QString& type) : type_(type), spec_(specs().at(type)) {}

  bool mousePress(Viewport& vp, QMouseEvent* e) override {
    EditorState* st = vp.editorState();
    if (st->createType() != type_) {
      st->setViewportTool(nullptr);
      return false;
    }
    if (e->button() == Qt::RightButton) {
      if (stage_ > 0) cancel();
      else {
        st->setCreateType({});
        st->setViewportTool(nullptr);
      }
      return true;
    }
    if (e->button() != Qt::LeftButton) return false;
    float px = static_cast<float>(e->position().x()), py = static_cast<float>(e->position().y());
    if (stage_ == 0) {
      Vec3 hit;
      vp.updateCamera();
      if (!vp.hitConstructionPlane(px, py, hit)) return true;
      SceneDocument* doc = vp.document();
      doc_ = doc;
      doc->beginEdit(QObject::tr("Create %1").arg(QString::fromStdString(objectDisplayName(type_.toStdString()))));
      Node* n = doc->createNode(type_);
      if (!n) {
        doc->cancelEdit();
        return true;
      }
      node_ = n->id;
      p0_ = hit;
      Vec3 rot = vp.camera().creationRotation();
      rot_ = Mat4::fromEulerDeg(rot);
      n->transform.rotation = rot;
      n->transform.position = hit;
      spec_.foot(n->object->params, 0, 0, 0);
      for (const auto& s : spec_.stages)
        for (const auto& k : s.keys) n->object->params.setNumber(k, 0.0);
      doc->select(n->id, false);
      doc->notifyNodeChanged(n, ChangeAll);
      stage_ = 1;
      press_ = e->position().toPoint();
      return true;
    }
    if (stage_ >= 2) {
      if (stage_ - 2 + 1 < static_cast<int>(spec_.stages.size())) {
        ++stage_;
        startY_ = py;
        Node* n = node();
        base_ = n ? n->object->params.getFloat(spec_.stages[stage_ - 2].keys[0]) : 0.0f;
        st->setPrompt(spec_.stages[stage_ - 2].prompt);
      } else {
        finish(st);
      }
    }
    return true;
  }

  bool mouseMove(Viewport& vp, QMouseEvent* e) override {
    if (stage_ == 0) return false;
    Node* n = node();
    if (!n) return true;
    float px = static_cast<float>(e->position().x()), py = static_cast<float>(e->position().y());
    ParamBlock& p = n->object->params;
    if (stage_ == 1 && (e->buttons() & Qt::LeftButton)) {
      Vec3 hit;
      if (!vp.hitConstructionPlane(px, py, hit)) return true;
      Vec3 d = rot_.transposed().transformDir(hit - p0_);
      float r = std::sqrt(d.x * d.x + d.y * d.y);
      spec_.foot(p, d.x, d.y, r);
      if (spec_.rect) n->transform.position = p0_ + rot_.transformDir(Vec3(d.x * 0.5f, d.y * 0.5f, 0));
      doc_->notifyNodeChanged(n, ChangeGeometry | ChangeTransform);
    } else if (stage_ >= 2) {
      const Stage& s = spec_.stages[stage_ - 2];
      float v = base_ + (startY_ - py) * vp.camera().worldPerPixel(n->transform.position) * s.factor;
      if (s.positive) v = std::max(v, 0.0f);
      for (const auto& k : s.keys) p.setNumber(k, v);
      doc_->notifyNodeChanged(n, ChangeGeometry);
    }
    return true;
  }

  bool mouseRelease(Viewport& vp, QMouseEvent* e) override {
    if (stage_ != 1 || e->button() != Qt::LeftButton) return stage_ > 0;
    if ((e->position().toPoint() - press_).manhattanLength() < 3) {
      cancel();  // a click without dragging creates nothing
      return true;
    }
    if (spec_.stages.empty()) {
      finish(vp.editorState());
      return true;
    }
    stage_ = 2;
    startY_ = static_cast<float>(e->position().y());
    Node* n = node();
    base_ = n ? n->object->params.getFloat(spec_.stages[0].keys[0]) : 0.0f;
    vp.editorState()->setPrompt(spec_.stages[0].prompt);
    return true;
  }

  bool cancel() override {
    if (stage_ == 0) return false;
    stage_ = 0;
    node_ = 0;
    if (doc_) doc_->cancelEdit();
    return true;
  }

  QString prompt() const override { return QObject::tr("Click and drag to create a %1").arg(type_); }

 private:
  Node* node() const { return doc_ ? doc_->scene().findNode(node_) : nullptr; }
  void finish(EditorState* st) {
    stage_ = 0;
    node_ = 0;
    doc_->endEdit();
    st->setPrompt(QObject::tr("Click and drag to create another %1, right-click to stop")
                      .arg(QString::fromStdString(objectDisplayName(type_.toStdString()))));
  }

  QString type_;
  CreateSpec spec_;
  SceneDocument* doc_ = nullptr;
  int stage_ = 0;
  uint32_t node_ = 0;
  Vec3 p0_;
  Mat4 rot_;
  QPoint press_;
  float startY_ = 0, base_ = 0;
};

void installPrimCreation(MainWindow&) {
  for (const auto& [type, spec] : specs()) {
    QString t = type;
    registerCreationTool(t, [t] { return std::make_shared<PrimCreateTool>(t); });
  }
}

// Self test: drag-create a teapot and a ChamferCyl in the Top view.
void testPrimCreation(MainWindow& w) {
  using testing::check;
  using testing::pump;
  SceneDocument* doc = w.document();
  EditorState* st = w.editorState();
  Viewport* top = w.viewports()->viewport(0);
  w.viewports()->setActive(top);
  top->setViewType(ViewType::Top);
  pump(30);
  auto drag = [&](QPointF a, QPointF b) {
    top->simulateMouse(QEvent::MouseButtonPress, a, Qt::LeftButton, Qt::LeftButton);
    for (int i = 1; i <= 6; ++i) top->simulateMouse(QEvent::MouseMove, a + (b - a) * (i / 6.0), Qt::NoButton, Qt::LeftButton);
    top->simulateMouse(QEvent::MouseButtonRelease, b, Qt::LeftButton, Qt::NoButton);
  };
  QPointF c(top->width() * 0.3, top->height() * 0.5);
  st->setCreateType("Teapot");
  drag(c, c + QPointF(60, 0));
  pump(30);
  Node* tp = doc->firstSelected();
  check(tp && tp->object->type() == "Teapot", "teapot created by dragging");
  float wpp = top->camera().worldPerPixel(Vec3());
  check(tp && std::fabs(tp->object->params.getFloat("radius") - 60 * wpp) < 3 * wpp, "teapot radius follows the drag");
  // ChamferCyl: radius drag, then height and fillet with clicks.
  QPointF c2(top->width() * 0.7, top->height() * 0.5);
  st->setCreateType("ChamferCyl");
  drag(c2, c2 + QPointF(50, 0));
  for (int i = 1; i <= 5; ++i) top->simulateMouse(QEvent::MouseMove, c2 + QPointF(50, -8.0 * i), Qt::NoButton, Qt::NoButton);
  top->simulateMouse(QEvent::MouseButtonPress, c2 + QPointF(50, -40), Qt::LeftButton, Qt::LeftButton);
  top->simulateMouse(QEvent::MouseButtonRelease, c2 + QPointF(50, -40), Qt::LeftButton, Qt::NoButton);
  for (int i = 1; i <= 3; ++i) top->simulateMouse(QEvent::MouseMove, c2 + QPointF(50, -40 - 3.0 * i), Qt::NoButton, Qt::NoButton);
  top->simulateMouse(QEvent::MouseButtonPress, c2 + QPointF(50, -49), Qt::LeftButton, Qt::LeftButton);
  top->simulateMouse(QEvent::MouseButtonRelease, c2 + QPointF(50, -49), Qt::LeftButton, Qt::NoButton);
  pump(30);
  Node* cc = doc->firstSelected();
  check(cc && cc->object->type() == "ChamferCyl", "ChamferCyl created");
  check(cc && cc->object->params.getFloat("height") > 10 * wpp, "ChamferCyl height set by the second step");
  check(cc && cc->object->params.getFloat("fillet") > 0.5f * wpp, "ChamferCyl fillet set by the third step");
  st->setCreateType({});
  st->setViewportTool(nullptr);
  // Every new primitive type shows in the viewport.
  doc->beginEdit("Showcase");
  float x = -600;
  for (const char* t : {"Teapot", "Hedra", "TorusKnot", "ChamferCyl", "OilTank", "Capsule", "Spindle", "LExt", "Gengon",
                        "CExt", "RingWave", "Hose", "Prism"}) {
    Node* n = doc->createNode(t);
    check(n != nullptr, t);
    if (n) n->transform.position = Vec3(x, 300, 0);
    x += 110;
  }
  doc->endEdit();
  Viewport* persp = w.viewports()->viewport(3);
  persp->setShading(ShadingMode::Shaded);
  w.viewports()->zoomExtentsAll(false);
  pump(50);
  if (QString dir = qEnvironmentVariable("VERAA_SCREENSHOT_DIR"); !dir.isEmpty()) {
    testing::saveGrab(&w, dir + "/primitives_01.png");
    // Close-up of the teapot and torus knot in the shaded perspective view.
    for (const auto& n : doc->scene().nodes)
      if (n->name == "Teapot002") doc->setSelection({n->id});
    w.viewports()->setActive(persp);
    w.viewports()->toggleMaximize();
    pump(100);
    persp->zoomExtents(true);
    pump(100);
    testing::saveGrab(&w, dir + "/primitives_02_teapot.png");
    w.viewports()->toggleMaximize();
  }
}

}  // namespace

VERAA_APP_EXTENSION(primCreation, 230, installPrimCreation);
VERAA_SELF_TEST(primitiveCreation, testPrimCreation);

}  // namespace veraa
