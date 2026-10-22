// UI self tests for poly modeling (run with "Veraa --script selftest" or "--script polyModeling").
// Screenshots go to $VERAA_SCREENSHOT_DIR when it is set.
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QSpinBox>
#include <QToolButton>
#include <QUndoStack>

#include "commandpanel.h"
#include "document.h"
#include "editorstate.h"
#include "mainwindow.h"
#include "object.h"
#include "polyedit.h"
#include "polymods.h"
#include "selftest.h"
#include "viewport.h"
#include "viewportpanel.h"

namespace veraa {

namespace {

using testing::check;
using testing::projectPoint;
using testing::pump;

void shot(MainWindow& w, const QString& name) {
  QString dir = qEnvironmentVariable("VERAA_SCREENSHOT_DIR");
  if (dir.isEmpty()) return;
  QDir().mkpath(dir);
  testing::saveGrab(&w, QDir(dir).filePath(name + ".png"));
}

void click(Viewport* vp, QPointF p, Qt::KeyboardModifiers mods = Qt::NoModifier) {
  vp->simulateMouse(QEvent::MouseMove, p, Qt::NoButton, Qt::NoButton, mods);
  vp->simulateMouse(QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton, mods);
  vp->simulateMouse(QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton, mods);
  pump(30);
}

void drag(Viewport* vp, QPointF a, QPointF b, Qt::KeyboardModifiers mods = Qt::NoModifier) {
  vp->simulateMouse(QEvent::MouseMove, a, Qt::NoButton, Qt::NoButton, mods);
  vp->simulateMouse(QEvent::MouseButtonPress, a, Qt::LeftButton, Qt::LeftButton, mods);
  for (int i = 1; i <= 10; ++i) vp->simulateMouse(QEvent::MouseMove, a + (b - a) * (i / 10.0), Qt::NoButton, Qt::LeftButton, mods);
  vp->simulateMouse(QEvent::MouseButtonRelease, b, Qt::LeftButton, Qt::NoButton, mods);
  pump(30);
}

QAction* actionNamed(MainWindow& w, const QString& name) { return w.findChild<QAction*>(name); }

QAction* actionWithText(MainWindow& w, const QString& text) {
  for (QAction* a : w.findChildren<QAction*>())
    if (a->text() == text) return a;
  return nullptr;
}

template <class T>
T* widget(const QString& name) {
  // Panels being replaced are hidden first and deleted later; only visible widgets count.
  for (QWidget* top : QApplication::topLevelWidgets())
    for (T* t : top->findChildren<T*>(name))
      if (t->isVisible()) return t;
  for (QWidget* top : QApplication::topLevelWidgets())
    if (top->objectName() == name) return qobject_cast<T*>(top);
  return nullptr;
}

EditablePolyObject* polyOf(Node* n) { return n ? dynamic_cast<EditablePolyObject*>(n->object.get()) : nullptr; }

int faceToward(const PolyMesh& m, const Vec3& dir) {
  int best = -1;
  float bestDot = -2;
  for (size_t f = 0; f < m.faces.size(); ++f) {
    float d = dot(normalize(m.faceNormal(m.faces[f])), dir);
    if (d > bestDot) {
      bestDot = d;
      best = static_cast<int>(f);
    }
  }
  return best;
}

Vec3 faceCenter(const PolyMesh& m, int f) {
  Vec3 c;
  for (int v : m.faces[f].v) c += m.positions[v];
  return c / static_cast<float>(m.faces[f].v.size());
}

void testPolyModeling(MainWindow& w) {
  SceneDocument* doc = w.document();
  EditorState* st = w.editorState();
  ViewportPanel* vps = w.viewports();
  Viewport* vp = vps->viewport(3);
  vps->setActive(vp);
  vp->setViewType(ViewType::Perspective);
  vp->setShading(ShadingMode::Shaded);

  // 1. Create a box and convert it.
  doc->beginEdit("Create Box");
  Node* box = doc->createNode("Box");
  doc->endEdit();
  uint32_t id = box->id;
  doc->setSelection({id});
  vp->zoomExtents(false);
  pump(50);
  QAction* convert = actionWithText(w, QObject::tr("Convert to Editable Poly"));
  check(convert != nullptr, "Convert to Editable Poly command exists");
  if (convert) convert->trigger();
  pump(50);
  box = doc->scene().findNode(id);
  check(polyOf(box) != nullptr, "box converted to Editable Poly");
  if (!polyOf(box)) return;
  w.commandPanel()->showModify();
  pump(50);
  check(widget<QWidget>("PolyPanel") != nullptr, "Editable Poly rollouts shown in the Modify panel");

  // 2. Polygon level (hotkey 4) and pick the top polygon.
  QAction* level4 = actionNamed(w, "poly.level4");
  check(level4 != nullptr, "sub-object level hotkeys installed");
  if (level4) level4->trigger();
  pump(30);
  box = doc->scene().findNode(id);
  check(polyOf(box)->sel.level == PolyFace, "hotkey 4 switches to Polygon level");
  check(activePolyTool(st) != nullptr, "sub-object tool active");
  int top = faceToward(polyOf(box)->mesh, Vec3(0, 0, 1));
  click(vp, projectPoint(vp, box->objectWorldMatrix().transformPoint(faceCenter(polyOf(box)->mesh, top))));
  box = doc->scene().findNode(id);
  check(polyOf(box)->sel.faces.size() == 1 && polyOf(box)->sel.faces[0] == top, "clicking selects the top polygon");
  check(doc->selection().size() == 1 && doc->selection()[0] == id, "object stays selected in sub-object mode");
  shot(w, "poly_01_polygon_selected");

  // 3. Extrude through the settings caddy.
  auto* extrudeSettings = widget<QToolButton>("poly_extrudeFaces_settings");
  check(extrudeSettings != nullptr, "Extrude settings button");
  if (extrudeSettings) extrudeSettings->click();
  pump(50);
  auto* height = widget<QDoubleSpinBox>("caddy_extrudeFaces_height");
  check(height != nullptr, "Extrude caddy opens");
  if (height) height->setValue(30.0);
  pump(30);
  box = doc->scene().findNode(id);
  check(polyOf(box)->mesh.faces.size() == 10, "caddy previews the extrusion");
  shot(w, "poly_02_extrude_caddy");
  if (auto* ok = widget<QPushButton>("caddy_ok")) ok->click();
  pump(50);
  box = doc->scene().findNode(id);
  const PolyMesh& m1 = polyOf(box)->mesh;
  check(m1.faces.size() == 10, "extrude adds four side polygons");
  check(std::fabs(m1.bounds().hi.z - 130.0f) < 1e-3f, "extrude height 30");
  check(isClosedManifold(m1), "extruded mesh is closed");
  int undoAfterExtrude = doc->undoStack()->index();

  // 4. Edge level: click an edge of the extruded cap and chamfer it.
  if (auto* a = actionNamed(w, "poly.level2")) a->trigger();
  pump(30);
  box = doc->scene().findNode(id);
  const PolyMesh& m2 = polyOf(box)->mesh;
  int cap = faceToward(m2, Vec3(0, 0, 1));
  int ea = m2.faces[cap].v[0], eb = m2.faces[cap].v[1];
  Vec3 mid = (m2.positions[ea] + m2.positions[eb]) * 0.5f;
  click(vp, projectPoint(vp, box->objectWorldMatrix().transformPoint(mid)));
  box = doc->scene().findNode(id);
  check(polyOf(box)->sel.edges.size() == 1 && polyOf(box)->sel.edges[0] == edgeKey(ea, eb), "clicking selects the edge");
  if (auto* s = widget<QToolButton>("poly_chamferEdges_settings")) s->click();
  pump(30);
  if (auto* amount = widget<QDoubleSpinBox>("caddy_chamferEdges_amount")) amount->setValue(6.0);
  pump(30);
  shot(w, "poly_03_chamfer_caddy");
  if (auto* ok = widget<QPushButton>("caddy_ok")) ok->click();
  pump(50);
  box = doc->scene().findNode(id);
  check(polyOf(box)->mesh.faces.size() == 11, "chamfer adds a polygon");
  check(isClosedManifold(polyOf(box)->mesh), "chamfered mesh is closed");
  double chamfered = meshVolume(polyOf(box)->mesh);
  check(chamfered < 1.3e6 - 1.0, "chamfer removes volume");

  // 5. Undo the chamfer.
  doc->undoStack()->setIndex(undoAfterExtrude);
  pump(50);
  box = doc->scene().findNode(id);
  check(polyOf(box) && polyOf(box)->mesh.faces.size() == 10, "undo restores the extruded mesh");
  check(polyOf(box) && polyOf(box)->sel.level == PolyEdge, "sub-object level survives undo");
  check(activePolyTool(st) != nullptr, "sub-object tool is back after undo");

  // 6. Vertex level: region-select the cap corners and move them up with the gizmo.
  if (auto* a = actionNamed(w, "poly.level1")) a->trigger();
  pump(30);
  vp->setViewType(ViewType::Front);
  vp->zoomExtents(false);
  vp->camera().zoom(1.8f, vp->width() * 0.5f, vp->height() * 0.5f);
  pump(50);
  box = doc->scene().findNode(id);
  Mat4 ow = box->objectWorldMatrix();
  QPointF a = projectPoint(vp, ow.transformPoint(Vec3(-80, 0, 135)));
  QPointF b = projectPoint(vp, ow.transformPoint(Vec3(80, 0, 125)));
  drag(vp, a, b);
  box = doc->scene().findNode(id);
  check(polyOf(box)->sel.verts.size() == 4, "region select picks the four cap vertices (front and back)");
  st->setTool(TransformTool::Move);
  pump(30);
  auto tool = activePolyTool(st);
  Vec3 center = tool ? tool->selectionCenterWorld() : Vec3();
  float wpp = vp->camera().worldPerPixel(center);
  // Drag the Z axis handle up by 20 units.
  QPointF zHandle = projectPoint(vp, center + Vec3(0, 0, wpp * 60.0f));
  QPointF zTarget = zHandle + QPointF(0, -20.0f / wpp);
  drag(vp, zHandle, zTarget);
  box = doc->scene().findNode(id);
  check(std::fabs(polyOf(box)->mesh.bounds().hi.z - 150.0f) < 1.0f, "gizmo moves the selected vertices up by 20");
  shot(w, "poly_04_vertices_moved");

  // 7. Delete at polygon level removes polygons, not the object.
  if (auto* l4 = actionNamed(w, "poly.level4")) l4->trigger();
  pump(30);
  box = doc->scene().findNode(id);
  PolySelection s = polyOf(box)->sel;
  s.faces = {faceToward(polyOf(box)->mesh, Vec3(0, 0, 1))};
  PolyTarget(doc, st, id, -1).setSelection(s, "Select");
  QAction* del = actionWithText(w, QObject::tr("&Delete"));
  if (del) del->trigger();
  pump(30);
  box = doc->scene().findNode(id);
  check(box != nullptr && polyOf(box)->mesh.faces.size() == 9, "Delete removes the selected polygon only");
  // Border level + Cap closes it again.
  if (auto* l3 = actionNamed(w, "poly.level3")) l3->trigger();
  pump(30);
  box = doc->scene().findNode(id);
  auto loops = openBorderLoops(polyOf(box)->mesh);
  check(loops.size() == 1, "an open border after deleting");
  if (!loops.empty()) {
    Vec3 bm = (polyOf(box)->mesh.positions[loops[0][0]] + polyOf(box)->mesh.positions[loops[0][1]]) * 0.5f;
    click(vp, projectPoint(vp, box->objectWorldMatrix().transformPoint(bm)));
  }
  box = doc->scene().findNode(id);
  check(polyOf(box)->sel.edges.size() == 4, "clicking a border selects the whole loop");
  if (auto* cap2 = widget<QPushButton>("poly_capBorders")) cap2->click();
  pump(30);
  box = doc->scene().findNode(id);
  check(isClosedManifold(polyOf(box)->mesh), "Cap closes the border");

  // 8. Exit sub-object mode with hotkey 6.
  if (auto* l0 = actionNamed(w, "poly.level0")) l0->trigger();
  pump(30);
  box = doc->scene().findNode(id);
  check(polyOf(box)->sel.level == PolyObject, "hotkey 6 returns to the object level");
  check(activePolyTool(st) == nullptr, "sub-object tool removed at the object level");
  st->setTool(TransformTool::Select);

  // 9. Edit Poly modifier on a primitive records operations.
  doc->beginEdit("Create Box");
  Node* b2 = doc->createNode("Box");
  b2->transform.position = Vec3(250, 0, 0);
  doc->endEdit();
  uint32_t id2 = b2->id;
  doc->setSelection({id2});
  pump(30);
  w.commandPanel()->addModifier("EditPoly");
  pump(50);
  b2 = doc->scene().findNode(id2);
  check(b2 && b2->modifiers.size() == 1, "Edit Poly modifier added");
  if (auto* l4 = actionNamed(w, "poly.level4")) l4->trigger();
  pump(30);
  vp->setViewType(ViewType::Perspective);
  vp->zoomExtents(true);
  pump(50);
  b2 = doc->scene().findNode(id2);
  int top2 = faceToward(b2->mesh(), Vec3(0, 0, 1));
  click(vp, projectPoint(vp, b2->objectWorldMatrix().transformPoint(faceCenter(b2->mesh(), top2))));
  auto* epm = dynamic_cast<EditPolyModifier*>(doc->scene().findNode(id2)->modifiers[0].get());
  check(epm && epm->selection().faces.size() == 1, "Edit Poly: click selects a polygon");
  if (auto* inset = widget<QPushButton>("poly_insetFaces")) inset->click();
  pump(30);
  b2 = doc->scene().findNode(id2);
  check(b2->mesh().faces.size() == 10, "Edit Poly: Inset recorded and applied");
  shot(w, "poly_05_edit_poly_modifier");
  // Repeat Last runs the inset again.
  if (auto* rep = widget<QPushButton>("poly_repeatLast")) rep->click();
  pump(30);
  check(doc->scene().findNode(id2)->mesh().faces.size() == 14, "Repeat Last repeats the inset");
  // A sub-object move on the Edit Poly modifier is recorded as an operation.
  {
    PolyTarget t(doc, st, id2, 0);
    size_t opsBefore = epm ? dynamic_cast<EditPolyModifier*>(doc->scene().findNode(id2)->modifiers[0].get())->ops.size() : 0;
    t.beginLive("Move");
    json m = json::array();
    Mat4 up = Mat4::translate(Vec3(0, 0, 15));
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c) m.push_back(up.m[r][c]);
    t.updateLive({{"op", "transform"}, {"matrix", m}});
    t.endLive(true);
    pump(20);
    auto* mod = dynamic_cast<EditPolyModifier*>(doc->scene().findNode(id2)->modifiers[0].get());
    check(mod && mod->ops.size() == opsBefore + 1, "Edit Poly records the move");
    check(std::fabs(doc->scene().findNode(id2)->mesh().bounds().hi.z - 115.0f) < 1e-3f, "Edit Poly move raises the polygon");
  }
  if (auto* l0 = actionNamed(w, "poly.level0")) l0->trigger();
  pump(30);

  // 10. Poly Select passes a vertex selection to a Push modifier above it.
  doc->beginEdit("Create Box");
  Node* b3 = doc->createNode("Box");
  b3->transform.position = Vec3(-250, 0, 0);
  doc->endEdit();
  uint32_t id3 = b3->id;
  doc->setSelection({id3});
  w.commandPanel()->addModifier("PolySelect");
  pump(30);
  if (auto* l1 = actionNamed(w, "poly.level1")) l1->trigger();
  pump(30);
  check(widget<QWidget>("poly_extrudeVerts") == nullptr, "Poly Select shows only selection rollouts");
  vp->zoomExtents(true);
  pump(30);
  b3 = doc->scene().findNode(id3);
  int corner = -1;
  float bestZ = -1e9f;
  const PolyMesh& bm = b3->mesh();
  for (size_t v = 0; v < bm.positions.size(); ++v) {
    // A top vertex facing the perspective camera.
    Vec3 p = bm.positions[v];
    float score = p.z * 10.0f - p.y + p.x * 0.1f;
    if (score > bestZ) {
      bestZ = score;
      corner = static_cast<int>(v);
    }
  }
  click(vp, projectPoint(vp, b3->objectWorldMatrix().transformPoint(bm.positions[corner])));
  auto* psel = dynamic_cast<PolySelectModifier*>(doc->scene().findNode(id3)->modifiers[0].get());
  check(psel && psel->sel.verts.size() == 1, "Poly Select: clicking selects a vertex");
  if (auto* l0 = actionNamed(w, "poly.level0")) l0->trigger();
  pump(20);
  // Level 0 at the modifier would pass everything up; go back to vertex level for the stack.
  if (psel) {
    PolySelection s = psel->sel;
    s.level = PolyVertex;
    psel->setSelection(s);
  }
  w.commandPanel()->addModifier("Push");
  pump(30);
  b3 = doc->scene().findNode(id3);
  int moved = 0;
  PolyMesh raw = static_cast<GeometryObject*>(b3->object.get())->build();
  for (size_t v = 0; v < raw.positions.size(); ++v)
    if (length(b3->mesh().positions[v] - raw.positions[v]) > 1e-3f) ++moved;
  check(moved == 1, "Push above Poly Select moves only the selected vertex");

  // 11. The viewport context menu offers sub-object levels and Collapse Stack.
  QAction* collapse = actionNamed(w, "tools.collapseStack");
  check(collapse && collapse->property("quadMenu").toBool(), "Collapse Stack is in the context menu");
  check(actionNamed(w, "poly.level1") && actionNamed(w, "poly.level1")->isVisible(), "sub-object levels are in the context menu");
  collapse->trigger();
  pump(30);
  b3 = doc->scene().findNode(id3);
  check(polyOf(b3) && b3->modifiers.empty() && b3->mesh().faces.size() == 6, "Collapse Stack bakes the modifiers");
}

}  // namespace

VERAA_SELF_TEST(polyModeling, testPolyModeling);

}  // namespace veraa
