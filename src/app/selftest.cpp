#include "selftest.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QPainter>
#include <QPixmap>
#include <QFileInfo>
#include <QThread>
#include <QUndoStack>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "commandpanel.h"
#include "framebuffer.h"
#include "mainwindow.h"
#include "materialeditor.h"
#include "rendersetup.h"
#include "viewport.h"
#include "viewportpanel.h"

namespace veraa {

namespace testing {

void pump(int ms) {
  QElapsedTimer t;
  t.start();
  do {
    QApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(5);
  } while (t.elapsed() < ms);
}

QString sibling(const QString& path, const QString& suffix) {
  QFileInfo fi(path);
  return fi.dir().filePath(fi.completeBaseName() + suffix + "." + (fi.suffix().isEmpty() ? "png" : fi.suffix()));
}

}  // namespace testing

namespace {

using testing::pump;
using testing::sibling;

}  // namespace

namespace testing {

bool saveGrab(QWidget* w, const QString& path) {
  w->repaint();
  pump(150);
  QPixmap pm = w->grab();
  // A window grab misses what viewports draw with QPainter on top of OpenGL, so paste each
  // viewport's own framebuffer (which includes those overlays) over it.
  QPainter painter(&pm);
  for (Viewport* vp : w->findChildren<Viewport*>())
    if (vp->isVisible()) painter.drawImage(vp->mapTo(w, QPoint(0, 0)), vp->grabFramebuffer());
  painter.end();
  bool ok = pm.save(path);
  std::printf("%s screenshot %s\n", ok ? "Saved" : "FAILED to save", qPrintable(path));
  return ok;
}

}  // namespace testing

namespace {
using testing::saveGrab;

Material* addMaterial(Scene& s, const char* name, const Vec3& diffuse) {
  Material* m = s.createMaterial(name);
  m->params.setColor("diffuse", diffuse);
  return m;
}

}  // namespace

void buildDemoScene(MainWindow& w) {
  SceneDocument* doc = w.document();
  Scene& s = doc->scene();
  doc->beginEdit("Demo Scene");

  Material* floor = addMaterial(s, "Floor", Vec3(0.42f, 0.3f, 0.2f));
  floor->params.setColor("reflect", Vec3(0.5f));
  floor->params.setNumber("reflectGlossiness", 0.72);
  Material* red = addMaterial(s, "Red Plastic", Vec3(0.6f, 0.06f, 0.05f));
  red->params.setColor("reflect", Vec3(1.0f));
  red->params.setNumber("reflectGlossiness", 0.88);
  Material* chrome = addMaterial(s, "Chrome", Vec3(0.95f));
  chrome->params.setColor("reflect", Vec3(1.0f));
  chrome->params.setNumber("metalness", 1.0);
  chrome->params.setNumber("reflectGlossiness", 0.97);
  Material* gold = addMaterial(s, "Gold", Vec3(1.0f, 0.77f, 0.34f));
  gold->params.setColor("reflect", Vec3(1.0f));
  gold->params.setNumber("metalness", 1.0);
  gold->params.setNumber("reflectGlossiness", 0.88);
  Material* glass = addMaterial(s, "Glass", Vec3(0.0f));
  glass->params.setColor("reflect", Vec3(1.0f));
  glass->params.setColor("refract", Vec3(1.0f));
  glass->params.setNumber("ior", 1.52);
  Material* white = addMaterial(s, "White Matte", Vec3(0.8f));

  Node* plane = s.createNode("Plane");
  plane->object->params.setNumber("length", 700);
  plane->object->params.setNumber("width", 900);
  plane->materialId = floor->id;

  Node* box = s.createNode("ChamferBox");
  box->object->params.setNumber("length", 90);
  box->object->params.setNumber("width", 90);
  box->object->params.setNumber("height", 90);
  box->object->params.setNumber("fillet", 8);
  box->transform.position = Vec3(-170, 40, 0);
  box->transform.rotation = Vec3(0, 0, 20);
  box->materialId = red->id;

  Node* sphere = s.createNode("Sphere");
  sphere->object->params.setNumber("radius", 50);
  sphere->transform.position = Vec3(-30, -60, 50);
  sphere->materialId = chrome->id;

  Node* cyl = s.createNode("Cylinder");
  cyl->object->params.setNumber("radius", 28);
  cyl->object->params.setNumber("height", 170);
  cyl->object->params.setNumber("heightSegs", 16);
  cyl->object->params.setNumber("sides", 32);
  cyl->transform.position = Vec3(130, 50, 0);
  auto bend = createModifier("Bend");
  bend->params.setNumber("angle", 60);
  cyl->modifiers.push_back(std::move(bend));
  cyl->materialId = gold->id;

  Node* torus = s.createNode("Torus");
  torus->object->params.setNumber("radius1", 45);
  torus->object->params.setNumber("radius2", 14);
  torus->object->params.setNumber("segments", 48);
  torus->object->params.setNumber("sides", 18);
  torus->transform.position = Vec3(60, -120, 14);
  torus->materialId = glass->id;

  Node* teapotish = s.createNode("GeoSphere");
  teapotish->object->params.setNumber("radius", 30);
  teapotish->transform.position = Vec3(-150, -130, 30);
  auto smooth = createModifier("Noise");
  smooth->params.setNumber("strengthZ", 0);
  smooth->params.setNumber("strengthX", 6);
  smooth->params.setNumber("strengthY", 6);
  smooth->params.setNumber("scale", 30);
  teapotish->modifiers.push_back(std::move(smooth));
  teapotish->materialId = white->id;

  Node* light = s.createNode("RectLight");
  light->object->params.setNumber("width", 220);
  light->object->params.setNumber("length", 160);
  light->object->params.setNumber("intensity", 6);
  light->transform.position = Vec3(-60, -80, 330);
  light->transform.rotation = Vec3(-15, 0, 0);

  Node* sun = s.createNode("SunLight");
  sun->transform.position = Vec3(420, -380, 520);
  sun->transform.rotation = rotationLookingAlong(normalize(Vec3(-420, 380, -520)));

  Node* cam = s.createNode("Camera");
  Vec3 eye(330, -470, 230), target(-10, 0, 45);
  cam->transform.position = eye;
  cam->transform.rotation = rotationLookingAlong(target - eye);
  cam->object->params.setNumber("fov", 48);

  s.renderSettings.setNumber("width", 960);
  s.renderSettings.setNumber("height", 540);
  doc->notifyStructureChanged();
  doc->notifyMaterialsChanged();
  doc->setSelection({cyl->id});
  doc->endEdit();
  w.viewports()->zoomExtentsAll(false);
}

namespace {

int g_failures = 0;

struct ModuleTestEntry {
  std::string name;
  testing::ModuleTest fn;
};

std::vector<ModuleTestEntry>& moduleTests() {
  static std::vector<ModuleTestEntry> list;
  return list;
}

}  // namespace

namespace testing {

void check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
  std::fflush(stdout);
  if (!ok) ++g_failures;
}

int failures() { return g_failures; }

QPointF projectPoint(Viewport* vp, const Vec3& p) {
  vp->updateCamera();
  float x = 0, y = 0;
  vp->camera().project(p, x, y);
  return QPointF(x, y);
}

SelfTestRegistrar::SelfTestRegistrar(const char* name, ModuleTest fn) { moduleTests().push_back({name, fn}); }

}  // namespace testing

namespace {
using testing::check;
using testing::projectPoint;

// Runs one module test on a fresh scene.
void runModuleTest(MainWindow& w, const ModuleTestEntry& t) {
  std::printf("---- %s ----\n", t.name.c_str());
  w.document()->newScene();
  pump(50);
  t.fn(w);
  w.document()->newScene();
  pump(50);
}

int selfTest(MainWindow& w) {
  SceneDocument* doc = w.document();
  EditorState* st = w.editorState();
  Scene& s = doc->scene();
  ViewportPanel* vps = w.viewports();
  Viewport* top = vps->viewport(0);
  Viewport* persp = vps->viewport(3);
  pump(200);

  // 1. Interactive Box creation in the Top view: drag footprint, release, move up for height, click.
  st->setCreateType("Box");
  QPointF c(top->width() * 0.5, top->height() * 0.5);
  top->simulateMouse(QEvent::MouseButtonPress, c, Qt::LeftButton, Qt::LeftButton);
  for (int i = 1; i <= 10; ++i)
    top->simulateMouse(QEvent::MouseMove, c + QPointF(8 * i, -6 * i), Qt::NoButton, Qt::LeftButton);
  top->simulateMouse(QEvent::MouseButtonRelease, c + QPointF(80, -60), Qt::LeftButton, Qt::NoButton);
  for (int i = 1; i <= 10; ++i)
    top->simulateMouse(QEvent::MouseMove, c + QPointF(80, -60 - 5 * i), Qt::NoButton, Qt::NoButton);
  top->simulateMouse(QEvent::MouseButtonPress, c + QPointF(80, -110), Qt::LeftButton, Qt::LeftButton);
  top->simulateMouse(QEvent::MouseButtonRelease, c + QPointF(80, -110), Qt::LeftButton, Qt::NoButton);
  st->setCreateType({});
  check(s.nodes.size() == 1, "interactive box creation adds one node");
  Node* box = s.nodes.empty() ? nullptr : s.nodes[0].get();
  check(box && box->object->params.getFloat("width") > 1 && box->object->params.getFloat("length") > 1,
        "box footprint from drag");
  check(box && box->object->params.getFloat("height") > 1, "box height from mouse move");
  check(box && doc->isSelected(box->id), "created box is selected");

  // 2. Undo / redo creation.
  doc->undoStack()->undo();
  check(s.nodes.empty(), "undo removes the box");
  doc->undoStack()->redo();
  check(s.nodes.size() == 1, "redo restores the box");
  box = s.nodes.empty() ? nullptr : s.nodes[0].get();
  if (!box) return g_failures + 1;
  uint32_t boxId = box->id;

  // 3. Move gizmo drag along X in the Top view.
  st->setTool(TransformTool::Move);
  doc->setSelection({boxId});
  pump(50);
  Vec3 p0 = s.findNode(boxId)->transform.position;
  QPointF o = projectPoint(top, p0);
  QPointF onX = o + QPointF(45, 0);
  top->simulateMouse(QEvent::MouseMove, onX, Qt::NoButton, Qt::NoButton);
  top->simulateMouse(QEvent::MouseButtonPress, onX, Qt::LeftButton, Qt::LeftButton);
  for (int i = 1; i <= 10; ++i)
    top->simulateMouse(QEvent::MouseMove, onX + QPointF(10 * i, 7 * i), Qt::NoButton, Qt::LeftButton);
  top->simulateMouse(QEvent::MouseButtonRelease, onX + QPointF(100, 70), Qt::LeftButton, Qt::NoButton);
  Vec3 p1 = s.findNode(boxId)->transform.position;
  check(p1.x - p0.x > 10.0f && std::fabs(p1.y - p0.y) < 1e-3f, "move gizmo X axis drag moves only along X");
  doc->undoStack()->undo();
  check(length(s.findNode(boxId)->transform.position - p0) < 1e-3f, "undo restores position");

  // 4. Rotate gizmo: drag a quarter turn on the Z ring in the Top view.
  st->setTool(TransformTool::Rotate);
  doc->setSelection({boxId});
  pump(30);
  o = projectPoint(top, s.findNode(boxId)->transform.position);
  float ringR = 85.0f;
  auto ringPoint = [&](double deg) {
    double a = deg * 3.14159265 / 180.0;
    return o + QPointF(ringR * std::cos(a), -ringR * std::sin(a));
  };
  top->simulateMouse(QEvent::MouseMove, ringPoint(30), Qt::NoButton, Qt::NoButton);
  top->simulateMouse(QEvent::MouseButtonPress, ringPoint(30), Qt::LeftButton, Qt::LeftButton);
  for (int i = 1; i <= 18; ++i) top->simulateMouse(QEvent::MouseMove, ringPoint(30 + 5 * i), Qt::NoButton, Qt::LeftButton);
  top->simulateMouse(QEvent::MouseButtonRelease, ringPoint(120), Qt::LeftButton, Qt::NoButton);
  float rz = s.findNode(boxId)->transform.rotation.z;
  std::printf("      rotation z = %.2f\n", rz);
  check(std::fabs(rz - 90.0f) < 3.0f, "rotate gizmo Z ring quarter turn");

  // 5. Scale gizmo: drag the center handle up for uniform scale.
  st->setTool(TransformTool::Scale);
  pump(30);
  top->simulateMouse(QEvent::MouseMove, o, Qt::NoButton, Qt::NoButton);
  top->simulateMouse(QEvent::MouseButtonPress, o, Qt::LeftButton, Qt::LeftButton);
  for (int i = 1; i <= 6; ++i)
    top->simulateMouse(QEvent::MouseMove, o + QPointF(0, -10 * i), Qt::NoButton, Qt::LeftButton);
  top->simulateMouse(QEvent::MouseButtonRelease, o + QPointF(0, -60), Qt::LeftButton, Qt::NoButton);
  Vec3 sc = s.findNode(boxId)->transform.scale;
  std::printf("      scale = %.3f %.3f %.3f\n", sc.x, sc.y, sc.z);
  check(std::fabs(sc.x - 1.5f) < 0.05f && std::fabs(sc.z - 1.5f) < 0.05f, "uniform scale from center handle");
  doc->undoStack()->undo();
  doc->undoStack()->undo();
  if (!s.findNode(boxId)) {
    check(false, "box still exists after undoing rotate and scale");
    return g_failures;
  }
  check(length(s.findNode(boxId)->transform.rotation) < 1e-3f &&
            length(s.findNode(boxId)->transform.scale - Vec3(1.0f)) < 1e-4f,
        "undo rotate and scale");
  st->setTool(TransformTool::Move);

  // Region select: a rectangle over the whole Top view selects the box.
  doc->clearSelection();
  st->setTool(TransformTool::Select);
  top->simulateMouse(QEvent::MouseButtonPress, QPointF(5, 30), Qt::LeftButton, Qt::LeftButton);
  top->simulateMouse(QEvent::MouseMove, QPointF(200, 200), Qt::NoButton, Qt::LeftButton);
  top->simulateMouse(QEvent::MouseMove, QPointF(top->width() - 5, top->height() - 5), Qt::NoButton, Qt::LeftButton);
  top->simulateMouse(QEvent::MouseButtonRelease, QPointF(top->width() - 5, top->height() - 5), Qt::LeftButton,
                     Qt::NoButton);
  check(doc->isSelected(boxId), "region select picks the box");
  // Click on empty space clears the selection.
  top->simulateMouse(QEvent::MouseButtonPress, QPointF(8, top->height() - 8), Qt::LeftButton, Qt::LeftButton);
  top->simulateMouse(QEvent::MouseButtonRelease, QPointF(8, top->height() - 8), Qt::LeftButton, Qt::NoButton);
  check(doc->selection().isEmpty(), "click on empty space deselects");
  st->setTool(TransformTool::Move);

  // 5. Modifiers: add, change parameter (soft edit), undo.
  doc->setSelection({boxId});
  w.commandPanel()->addModifier("Bend");
  box = s.findNode(boxId);
  check(box && box->modifiers.size() == 1, "add Bend modifier");
  if (!box || box->modifiers.empty()) return g_failures;
  box->modifiers[0]->params.setNumber("angle", 45);
  doc->softEdit("Change angle", "test/angle");
  box->modifiers[0]->params.setNumber("angle", 30);
  doc->notifyNodeChanged(box, ChangeGeometry);
  doc->commitSoftEdit();
  doc->undoStack()->undo();
  box = s.findNode(boxId);
  check(box && box->modifiers.size() == 1 && std::fabs(box->modifiers[0]->params.getFloat("angle") - 45) < 1e-3f,
        "undo parameter change");
  doc->undoStack()->undo();
  box = s.findNode(boxId);
  check(box && box->modifiers.empty(), "undo add modifier");

  // 6. Clone, material, delete.
  doc->setSelection({boxId});
  w.cloneSelection(false);
  check(s.nodes.size() == 2, "clone creates a copy");
  Material* m = nullptr;
  doc->beginEdit("mat");
  m = s.createMaterial("Test");
  doc->notifyMaterialsChanged();
  doc->endEdit();
  doc->setSelection({boxId});
  doc->assignMaterialToSelection(m->id);
  check(s.findNode(boxId)->materialId == m->id, "assign material");
  doc->setSelection({boxId});
  doc->deleteSelected();
  check(s.nodes.size() == 1, "delete selection");
  doc->undoStack()->undo();
  check(s.nodes.size() == 2 && s.findNode(boxId), "undo delete");

  // 7. Light and camera creation by clicking.
  st->setCreateType("OmniLight");
  top->simulateMouse(QEvent::MouseButtonPress, c + QPointF(-60, 40), Qt::LeftButton, Qt::LeftButton);
  top->simulateMouse(QEvent::MouseButtonRelease, c + QPointF(-60, 40), Qt::LeftButton, Qt::NoButton);
  st->setCreateType({});
  check(s.nodes.size() == 3 && s.nodes.back()->light(), "omni light created by click");
  uint32_t camId = persp->createCameraFromView();
  check(camId && s.findNode(camId) && s.findNode(camId)->camera(), "create camera from view");
  check(persp->camera().type == ViewType::Camera, "perspective switched to camera view");
  persp->setViewType(ViewType::Perspective);

  // 8. Material editor and render setup open without errors.
  w.materialEditor()->show();
  w.renderSetup()->show();
  pump(100);
  w.materialEditor()->hide();
  w.renderSetup()->hide();

  // 9. Save, new, open.
  QString path = QDir::temp().filePath("veraa_selftest.veraa");
  QString err;
  size_t count = s.nodes.size();
  check(doc->save(path, &err), "save scene");
  doc->newScene();
  check(s.nodes.empty(), "new scene is empty");
  check(w.openFile(path), "open saved scene");
  check(s.nodes.size() == count, "reopened scene has the same objects");
  QFile::remove(path);

  // 10. Render a small image through the frame buffer.
  s.renderSettings.setNumber("width", 160);
  s.renderSettings.setNumber("height", 90);
  s.renderSettings.setNumber("samples", 4);
  w.render();
  bool finished = w.frameBuffer()->waitForFinish(60000);
  check(finished, "render finishes");
  check(!w.frameBuffer()->image().isNull(), "render produced an image");
  w.frameBuffer()->hide();

  // 11. Undo everything back to an empty scene does not crash.
  while (doc->undoStack()->canUndo()) doc->undoStack()->undo();
  pump(50);
  check(true, "undo history fully unwound");

  // Feature module tests (VERAA_SELF_TEST).
  for (const auto& t : moduleTests()) runModuleTest(w, t);

  std::printf("%d failure(s)\n", g_failures);
  return g_failures;
}

}  // namespace

int runScript(MainWindow& w, const QString& script, const QString& shotPath) {
  w.showNormal();
  w.resize(1600, 950);
  w.move(0, 0);
  pump(300);
  if (script == "selftest") return selfTest(w);
  for (const auto& t : moduleTests())
    if (script == QString::fromStdString(t.name)) {
      runModuleTest(w, t);
      std::printf("%d failure(s)\n", g_failures);
      return g_failures;
    }

  if (script == "demo" || script.isEmpty()) {
    if (w.document()->scene().nodes.empty()) buildDemoScene(w);
    w.editorState()->setTool(TransformTool::Move);
    w.commandPanel()->showModify();
    pump(300);
  }
  if (shotPath.isEmpty()) return 0;
  QDir().mkpath(QFileInfo(shotPath).absolutePath());
  bool ok = saveGrab(&w, shotPath);

  SceneDocument* doc = w.document();
  ViewportPanel* vps = w.viewports();
  Viewport* top = vps->viewport(0);

  // Rotate tool with the Z ring highlighted under the mouse.
  w.editorState()->setTool(TransformTool::Rotate);
  pump(50);
  if (Node* sel = doc->firstSelected()) {
    QPointF o = projectPoint(top, sel->transform.position);
    top->simulateMouse(QEvent::MouseMove, o + QPointF(85 * 0.866, -85 * 0.5), Qt::NoButton, Qt::NoButton);
  }
  ok &= saveGrab(&w, sibling(shotPath, "_rotate"));
  w.editorState()->setTool(TransformTool::Move);

  // Camera view, maximized, with a light selected in the Modify panel.
  Viewport* persp = vps->viewport(3);
  vps->setActive(persp);
  for (const auto& n : doc->scene().nodes)
    if (n->camera()) persp->setCameraView(n->id);
  for (const auto& n : doc->scene().nodes)
    if (n->light() && n->object->type() == "RectLight") doc->setSelection({n->id});
  vps->toggleMaximize();
  pump(200);
  ok &= saveGrab(&w, sibling(shotPath, "_camera"));
  vps->toggleMaximize();
  persp->setViewType(ViewType::Perspective);
  pump(300);

  // Interactive creation of a Torus in the Top view, captured while setting Radius 2.
  w.editorState()->setCreateType("Torus");
  QPointF c(top->width() * 0.25, top->height() * 0.72);
  top->simulateMouse(QEvent::MouseButtonPress, c, Qt::LeftButton, Qt::LeftButton);
  for (int i = 1; i <= 8; ++i) top->simulateMouse(QEvent::MouseMove, c + QPointF(5 * i, 2 * i), Qt::NoButton, Qt::LeftButton);
  top->simulateMouse(QEvent::MouseButtonRelease, c + QPointF(40, 16), Qt::LeftButton, Qt::NoButton);
  for (int i = 1; i <= 5; ++i) top->simulateMouse(QEvent::MouseMove, c + QPointF(40, 16 - 2 * i), Qt::NoButton, Qt::NoButton);
  pump(100);
  ok &= saveGrab(&w, sibling(shotPath, "_create"));
  top->simulateMouse(QEvent::MouseButtonPress, c + QPointF(40, 6), Qt::LeftButton, Qt::LeftButton);
  top->simulateMouse(QEvent::MouseButtonRelease, c + QPointF(40, 6), Qt::LeftButton, Qt::NoButton);
  w.editorState()->setCreateType({});

  MaterialEditor* me = w.materialEditor();
  me->show();
  if (Node* n = w.document()->firstSelected(); n && n->materialId) me->selectMaterial(n->materialId);
  pump(8000);
  ok &= saveGrab(me, sibling(shotPath, "_material"));
  me->hide();

  RenderSetupDialog* rs = w.renderSetup();
  rs->show();
  pump(200);
  ok &= saveGrab(rs, sibling(shotPath, "_rendersetup"));
  rs->hide();

  // Render from the camera.
  Scene& s = w.document()->scene();
  for (const auto& n : s.nodes)
    if (n->camera()) s.renderSettings.setString("camera", n->name);
  s.renderSettings.setNumber("samples", 64);
  w.render();
  FrameBufferWindow* fb = w.frameBuffer();
  fb->waitForFinish(180000);
  pump(300);
  ok &= saveGrab(fb, sibling(shotPath, "_render"));
  return ok ? 0 : 1;
}

}  // namespace veraa
