// Interface self-tests ("Veraa --script interface", also part of "--script selftest"), and the
// interface screenshot run ("Veraa --script interfaceScreens" with VERAA_UI_SCREENS=<folder>).
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QMenu>
#include <QMenuBar>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>

#include <cmath>

#include "commandpanel.h"
#include "commandtabs.h"
#include "customize.h"
#include "edittools.h"
#include "extension.h"
#include "interfacedata.h"
#include "io_formats.h"
#include "layerexplorer.h"
#include "mainwindow.h"
#include "navtools.h"
#include "sceneexplorer.h"
#include "scripting.h"
#include "scriptui.h"
#include "selftest.h"
#include "tooldialogs.h"
#include "uistate.h"
#include "units.h"
#include "viewportpanel.h"

namespace veraa {

namespace {

using testing::check;
using testing::projectPoint;
using testing::pump;

void click(Viewport* vp, QPointF p, Qt::KeyboardModifiers m = Qt::NoModifier) {
  vp->simulateMouse(QEvent::MouseMove, p, Qt::NoButton, Qt::NoButton, m);
  vp->simulateMouse(QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton, m);
  vp->simulateMouse(QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton, m);
}

void drag(Viewport* vp, const QVector<QPointF>& path) {
  vp->simulateMouse(QEvent::MouseMove, path.front(), Qt::NoButton, Qt::NoButton);
  vp->simulateMouse(QEvent::MouseButtonPress, path.front(), Qt::LeftButton, Qt::LeftButton);
  for (int i = 1; i < path.size(); ++i) vp->simulateMouse(QEvent::MouseMove, path[i], Qt::NoButton, Qt::LeftButton);
  vp->simulateMouse(QEvent::MouseButtonRelease, path.back(), Qt::LeftButton, Qt::NoButton);
}

Node* makeBox(SceneDocument* doc, const char* name, const Vec3& pos, float size = 40.0f) {
  doc->beginEdit("Create test box");
  Node* n = doc->createNode("Box");
  n->name = name;
  n->object->params.setNumber("width", size);
  n->object->params.setNumber("length", size);
  n->object->params.setNumber("height", size);
  n->transform.position = pos;
  n->invalidate();
  doc->notifyStructureChanged();
  doc->endEdit();
  return n;
}

Aabb tightBounds(const Node& n) {
  Aabb b;
  Mat4 m = n.objectWorldMatrix();
  for (const Vec3& p : n.triMesh().positions) b.expand(m.transformPoint(p));
  return b;
}

// Screen point on the ViewCube whose click looks from `want`.
bool cubePoint(Viewport* vp, const Vec3& want, QPointF& out) {
  vp->updateCamera();
  QRectF r = vp->viewCube().rect();
  for (double y = r.top(); y < r.bottom(); y += 1.0)
    for (double x = r.left(); x < r.right(); x += 1.0) {
      Vec3 dir;
      if (vp->viewCube().hitTest(vp->camera(), QPointF(x, y), &dir) == ViewCube::Hit::Region &&
          length(dir - normalize(want)) < 1e-3f) {
        out = QPointF(x, y);
        // Prefer the middle of the cell: keep scanning a few pixels further.
        QPointF p2(x + 2, y + 2);
        Vec3 d2;
        if (vp->viewCube().hitTest(vp->camera(), p2, &d2) == ViewCube::Hit::Region && length(d2 - normalize(want)) < 1e-3f)
          out = p2;
        return true;
      }
    }
  return false;
}

void testInterface(MainWindow& w) {
  SceneDocument* doc = w.document();
  EditorState* st = w.editorState();
  ViewportPanel* vps = w.viewports();
  UiState& ui = UiState::instance();
  Scene& s = doc->scene();
  Viewport* top = vps->viewport(0);
  Viewport* persp = vps->viewport(3);
  st->setTool(TransformTool::Select);

  Node* a = makeBox(doc, "BoxA", Vec3(0, 0, 0));
  Node* b = makeBox(doc, "BoxB", Vec3(220, 0, 0));
  uint32_t aId = a->id, bId = b->id;
  vps->zoomExtentsAll(false);
  pump(100);
  QPointF ca = projectPoint(top, Vec3(0, 0, 20));

  // ---- Selection filter ----
  auto* filter = w.findChild<QComboBox*>();
  for (QComboBox* c : w.findChildren<QComboBox*>())
    if (c->toolTip() == QObject::tr("Selection Filter")) filter = c;
  int lightsIdx = filter->findText(QObject::tr("Lights"));
  filter->setCurrentIndex(lightsIdx);
  Q_EMIT filter->activated(lightsIdx);
  doc->clearSelection();
  click(top, ca);
  check(doc->selection().isEmpty(), "selection filter Lights ignores a box click");
  filter->setCurrentIndex(0);
  Q_EMIT filter->activated(0);
  click(top, ca);
  check(doc->isSelected(aId), "selection filter All picks the box");

  // ---- Region shapes ----
  doc->clearSelection();
  ui.setRegionShape(RegionShape::Circle);
  drag(top, {ca, ca + QPointF(20, 0), ca + QPointF(45, 0)});
  check(doc->isSelected(aId) && !doc->isSelected(bId), "circular region selects the box under it only");

  doc->clearSelection();
  ui.setRegionShape(RegionShape::Fence);
  QPointF f0 = ca + QPointF(-60, -60), f1 = ca + QPointF(60, -60), f2 = ca + QPointF(60, 60), f3 = ca + QPointF(-60, 60);
  drag(top, {f0, f0 + QPointF(30, 0), f1});
  click(top, f2);
  top->simulateMouse(QEvent::MouseButtonPress, f3, Qt::LeftButton, Qt::LeftButton);
  top->simulateMouse(QEvent::MouseButtonRelease, f3, Qt::LeftButton, Qt::NoButton);
  top->simulateDoubleClick(f3);
  check(doc->isSelected(aId) && !doc->isSelected(bId), "fence region selects the enclosed box");

  doc->clearSelection();
  ui.setRegionShape(RegionShape::Lasso);
  QVector<QPointF> lasso;
  for (int i = 0; i <= 24; ++i) {
    double t = 2 * 3.14159265 * i / 24;
    lasso << ca + QPointF(std::cos(t) * 55, std::sin(t) * 55);
  }
  drag(top, lasso);
  check(doc->isSelected(aId) && !doc->isSelected(bId), "lasso region selects the enclosed box");

  doc->clearSelection();
  ui.setRegionShape(RegionShape::Paint);
  drag(top, {ca + QPointF(-30, 0), ca, ca + QPointF(30, 0)});
  check(doc->isSelected(aId) && !doc->isSelected(bId), "paint selection selects what the brush touches");

  doc->clearSelection();
  ui.setRegionShape(RegionShape::Rectangle);
  // A drag that starts on an object selects that object, so these regions start outside it.
  ui.setWindowSelection(true);
  drag(top, {ca + QPointF(-70, -70), ca + QPointF(-20, -20), ca});
  check(!doc->isSelected(aId), "window selection skips a partly covered box");
  drag(top, {ca + QPointF(-70, -70), ca + QPointF(70, 70)});
  check(doc->isSelected(aId), "window selection takes a fully enclosed box");
  doc->clearSelection();
  ui.setWindowSelection(false);
  drag(top, {ca + QPointF(-70, -70), ca + QPointF(-20, -20), ca});
  check(doc->isSelected(aId), "crossing selection takes a partly covered box");

  // ---- Selection lock ----
  doc->setSelection({aId});
  ui.setSelectionLocked(true);
  click(top, QPointF(10, top->height() - 10));
  check(doc->isSelected(aId), "selection lock keeps the selection on an empty click");
  ui.setSelectionLocked(false);

  // ---- ViewCube ----
  vps->setActive(persp);
  persp->update();
  pump(150);
  QPointF cp;
  bool found = cubePoint(persp, Vec3(0, 0, 1), cp);
  check(found, "ViewCube top face found");
  if (found) {
    click(persp, cp);
    pump(500);
    check(persp->camera().type == ViewType::Perspective && persp->camera().pitchDeg > 85.0f,
          "ViewCube top face click turns the perspective view to look down");
  }
  top->update();
  pump(100);
  found = cubePoint(top, Vec3(1, -1, 1), cp);
  check(found, "ViewCube corner found in the Top view");
  if (found) {
    click(top, cp);
    pump(500);
    check(top->camera().type == ViewType::User && std::fabs(top->camera().pitchDeg - 35.26f) < 1.0f,
          "ViewCube corner click makes the Top view an orthographic corner view");
  }
  top->setViewType(ViewType::Top);
  vps->zoomExtentsAll(false);
  pump(50);
  ca = projectPoint(top, Vec3(0, 0, 20));

  // ---- Freeze blocks selection ----
  doc->beginEdit("Freeze");
  a->frozen = true;
  doc->notifyNodeChanged(a, ChangeDisplay);
  doc->endEdit();
  doc->clearSelection();
  click(top, ca);
  check(!doc->isSelected(aId), "frozen objects cannot be selected");
  doc->beginEdit("Unfreeze");
  a->frozen = false;
  doc->notifyNodeChanged(a, ChangeDisplay);
  doc->endEdit();

  // ---- Layers ----
  doc->setSelection({aId});
  uint32_t layer = w.layerExplorer()->createLayer("TestLayer", true);
  check(s.findNode(aId)->layerId == layer, "new layer receives the selected object");
  w.layerExplorer()->setLayerFlag(layer, 0, true);
  check(!s.isNodeVisible(*s.findNode(aId)), "hiding a layer hides its objects");
  doc->clearSelection();
  click(top, ca);
  check(!doc->isSelected(aId), "objects on a hidden layer cannot be picked");
  w.layerExplorer()->setLayerFlag(layer, 0, false);
  check(s.isNodeVisible(*s.findNode(aId)), "unhiding the layer shows its objects again");

  // ---- Hide by category ----
  ui.setHiddenCategories(1 << static_cast<int>(ObjectKind::Geometry));
  doc->clearSelection();
  click(top, ca);
  check(doc->selection().isEmpty(), "Display panel hide geometry stops picking");
  ui.setHiddenCategories(0);

  // ---- Affect Pivot Only ----
  auto* hp = w.findChild<HierarchyPanel*>();
  check(hp != nullptr, "Hierarchy panel exists");
  doc->setSelection({aId});
  st->setTool(TransformTool::Move);
  pump(50);
  a = s.findNode(aId);
  Aabb before = tightBounds(*a);
  Vec3 pivotBefore = a->worldPosition();
  if (hp) hp->setPivotMode(PivotEditMode::AffectPivotOnly);
  QPointF o = projectPoint(top, a->worldPosition());
  QPointF onX = o + QPointF(45, 0);
  QVector<QPointF> xDrag = {onX};
  for (int i = 1; i <= 8; ++i) xDrag << onX + QPointF(10 * i, 0);
  drag(top, xDrag);
  a = s.findNode(aId);
  Aabb after = tightBounds(*a);
  check(a->worldPosition().x - pivotBefore.x > 10.0f, "Affect Pivot Only moves the pivot");
  check(length(after.lo - before.lo) < 1e-3f && length(after.hi - before.hi) < 1e-3f,
        "Affect Pivot Only leaves the geometry in place");
  if (hp) {
    hp->setPivotMode(PivotEditMode::None);
    hp->resetPivot();
  }
  a = s.findNode(aId);
  check(length(a->worldPosition() - Vec3(0, 0, 0)) < 1e-3f, "Reset Pivot returns the pivot to the object origin");

  // ---- Transform type-in ----
  doc->setSelection({aId});
  st->setTool(TransformTool::Move);
  w.updateTypeIn();
  auto* tx = w.findChild<UnitSpinBox*>("TypeInX");
  auto* ty = w.findChild<UnitSpinBox*>("TypeInY");
  check(tx && ty, "status bar type-in fields exist");
  if (tx && ty) {
    tx->setValue(123.0);
    check(std::fabs(s.findNode(aId)->worldPosition().x - 123.0f) < 1e-3f, "absolute type-in moves the object");
    ui.setAbsoluteTypeIn(false);
    w.updateTypeIn();
    ty->setValue(10.0);
    check(std::fabs(s.findNode(aId)->worldPosition().y - 10.0f) < 1e-3f, "offset type-in moves by the offset");
    check(std::fabs(ty->value()) < 1e-6, "offset type-in returns to zero");
    ui.setAbsoluteTypeIn(true);
  }
  w.openTransformTypeIn();
  check(w.findChild<TransformTypeInDialog*>() && w.findChild<TransformTypeInDialog*>()->isVisible(),
        "Transform Type-In dialog opens (F12)");
  w.findChild<TransformTypeInDialog*>()->hide();

  // ---- Hold / Fetch ----
  w.hold();
  doc->setSelection({aId});
  doc->deleteSelected();
  check(!s.findNodeByName("BoxA"), "object deleted after Hold");
  check(w.fetch(), "Fetch succeeds");
  check(s.findNodeByName("BoxA") != nullptr, "Fetch restores the held scene");
  a = s.findNodeByName("BoxA");
  aId = a ? a->id : 0;

  // ---- Units ----
  Units& units = Units::instance();
  units.setDisplay(DisplayUnits::Millimeters);
  UnitSpinBox sb;
  sb.setRange(-1e6, 1e6);
  sb.setValue(2.5);
  check(sb.text() == "25.0mm", "2.5 cm shows as 25.0mm");
  double v = 0;
  check(units.parse("10mm", &v) && std::fabs(v - 1.0) < 1e-9, "10mm parses as 1 cm");
  check(units.parse("2", &v) && std::fabs(v - 0.2) < 1e-9, "a plain number uses the display unit (mm)");
  units.setDisplay(DisplayUnits::Centimeters);
  check(sb.text() == "2.5cm", "switching back to cm updates unit fields");
  check(units.parse("1m", &v) && std::fabs(v - 100.0) < 1e-9, "1m parses as 100 cm");
  units.setDisplay(DisplayUnits::FeetInches);
  check(units.format(30.48) == "1'0\"", "30.48 cm shows as 1'0\"");
  units.setDisplay(DisplayUnits::Centimeters);

  // ---- Selection sets and scene states ----
  doc->setSelection({aId, bId});
  setSelectionSet(doc, "Pair", doc->selection());
  doc->clearSelection();
  check(selectionSetMembers(s, "Pair").size() == 2, "named selection set stores its objects");
  saveSceneState(doc, "Start", StateAllParts);
  Vec3 posB = s.findNode(bId)->transform.position;
  doc->beginEdit("move");
  s.findNode(bId)->transform.position = posB + Vec3(0, 300, 0);
  s.findNode(bId)->hidden = true;
  doc->endEdit();
  check(restoreSceneState(doc, "Start"), "scene state restores");
  check(length(s.findNode(bId)->transform.position - posB) < 1e-4f && !s.findNode(bId)->hidden,
        "scene state brings back transforms and visibility");

  // ---- Isolate ----
  doc->setSelection({aId});
  w.isolateSelection(true);
  check(s.findNode(bId)->hidden && !s.findNode(aId)->hidden, "Isolate Selection hides the other objects");
  w.isolateSelection(false);
  check(!s.findNode(bId)->hidden, "End Isolate shows them again");

  // ---- Mirror ----
  doc->setSelection({bId});
  Vec3 bp = s.findNode(bId)->worldPosition();
  MirrorDialog::mirror(doc, 0, 0.0f, 1);
  Node* mirrored = doc->firstSelected();
  check(mirrored && mirrored->id != bId, "Mirror with Copy creates a mirrored copy");
  if (mirrored) check(mirrored->worldMatrix().column(0).x < 0 && length(mirrored->worldPosition() - bp) < 1e-3f,
                      "mirrored copy is flipped on X around the selection center");
  if (mirrored) {
    doc->setSelection({mirrored->id});
    doc->deleteSelected();
  }

  // ---- Reference coordinate system ----
  doc->beginEdit("rotate");
  s.findNode(aId)->transform.rotation = Vec3(0, 0, 30);
  doc->endEdit();
  doc->setSelection({aId});
  st->setTool(TransformTool::Move);
  ui.setRefCoordSys(RefCoordSys::Local);
  Vec3 axes[3], origin;
  top->transformFrame(axes, origin);
  check(std::fabs(axes[0].x - std::cos(radians(30.0f))) < 1e-3f, "Local coordinate system follows the object's axes");
  ui.setRefCoordSys(RefCoordSys::World);
  top->transformFrame(axes, origin);
  check(length(axes[0] - Vec3(1, 0, 0)) < 1e-5f, "World coordinate system uses world axes");
  ui.setRefCoordSys(RefCoordSys::View);
  doc->beginEdit("unrotate");
  s.findNode(aId)->transform.rotation = Vec3();
  doc->endEdit();

  // ---- Viewport layouts ----
  vps->setViewLayout(0);
  check(vps->visibleViewports().size() == 1, "single viewport layout");
  vps->setViewLayout(11);
  check(vps->visibleViewports().size() == 4 && vps->viewLayout() == 11, "1 + 3 viewport layout");
  vps->setViewLayout(9);
  pump(100);
  check(vps->visibleViewports().size() == 4, "back to the 2 x 2 layout");

  // ---- Navigation tool: Pan ----
  vps->setActive(top);
  st->setViewportTool(makeNavTool(NavMode::Pan, vps));
  Vec3 centerBefore = top->camera().center;
  drag(top, {QPointF(200, 200), QPointF(220, 200), QPointF(260, 200)});
  check(length(top->camera().center - centerBefore) > 1e-3f, "Pan View tool pans the view");
  top->undoViewChange();
  check(length(top->camera().center - centerBefore) < 1e-3f, "Undo View Change restores the view");
  st->setViewportTool(nullptr);

  // ---- Measure tool ----
  st->setViewportTool(makeMeasureTool(doc, st));
  QString prompt;
  QMetaObject::Connection promptConn =
      QObject::connect(st, &EditorState::promptChanged, &w, [&prompt](const QString& t) { prompt = t; });
  QPointF m0 = projectPoint(top, Vec3(-300, -300, 0)), m1 = projectPoint(top, Vec3(-200, -300, 0));
  click(top, m0);
  click(top, m1);
  QObject::disconnect(promptConn);
  check(prompt.contains(QObject::tr("Distance")) && prompt.contains("100"), "Measure Distance reports 100 cm");
  st->setViewportTool(nullptr);

  // ---- Explorer linking ----
  w.sceneExplorer()->linkNodes({bId}, aId);
  check(s.findNode(bId)->parentId == aId, "Scene Explorer drag-to-link links objects");
  w.sceneExplorer()->linkNodes({bId}, 0);
  check(s.findNode(bId)->parentId == 0, "Scene Explorer unlink");

  // ---- File formats through the window ----
  QString fbx = QDir::temp().filePath("veraa_ui_test.fbx");
  QString err;
  doc->setSelection({aId});
  check(w.exportPath(fbx, true, &err), "Export Selected to FBX");
  QVector<uint32_t> imported;
  check(w.importPath(fbx, &imported, &err) && imported.size() == 1, "Import the FBX back");
  if (imported.size() == 1) {
    Node* im = s.findNode(imported[0]);
    check(im && im->mesh().positions.size() == s.findNode(aId)->mesh().positions.size(),
          "FBX round trip keeps the vertex count");
    check(im && length(im->worldPosition() - s.findNode(aId)->worldPosition()) < 1e-3f, "FBX round trip keeps the position");
    doc->setSelection(imported);
    doc->deleteSelected();
  }
  QFile::remove(fbx);
  QString glb = QDir::temp().filePath("veraa_ui_test.glb");
  doc->setSelection({aId});
  check(w.exportPath(glb, true, &err), "Export Selected to GLB");
  check(w.importPath(glb, &imported, &err) && imported.size() == 1, "Import the GLB back");
  if (imported.size() == 1) {
    Node* im = s.findNode(imported[0]);
    check(im && im->mesh().positions.size() == s.findNode(aId)->mesh().positions.size(),
          "glTF round trip keeps the (welded) vertex count");
    doc->setSelection(imported);
    doc->deleteSelected();
  }
  QFile::remove(glb);

  // ---- Python ----
  ScriptEngine& py = ScriptEngine::instance();
  if (py.available()) {
    QString out;
    size_t count = s.nodes.size();
    bool ok = py.run("import veraa\nb = veraa.create('Box', name='PyBox', width=40, position=(10, 20, 0))\n"
                     "b['height'] = 15\nprint(b.name, b['width'], len(veraa.nodes()))\n",
                     &out, false);
    std::printf("      python: %s\n", qPrintable(out.trimmed()));
    Node* pb = s.findNodeByName("PyBox");
    check(ok && s.nodes.size() == count + 1 && pb, "Python creates a box");
    check(pb && std::fabs(pb->object->params.getFloat("width") - 40.0f) < 1e-4f &&
              std::fabs(pb->object->params.getFloat("height") - 15.0f) < 1e-4f &&
              length(pb->worldPosition() - Vec3(10, 20, 0)) < 1e-4f,
          "Python sets parameters and position");
    QString out2;
    py.run("1 + 2", &out2, true);
    check(out2.trimmed() == "3", "listener echoes expression values");
    QString out3;
    check(!py.run("raise ValueError('boom')", &out3, false) && out3.contains("ValueError"), "Python errors are reported");
    auto* listener = w.findChild<ScriptListener*>();
    check(listener && listener->execute("veraa.node('PyBox').name").contains("PyBox"), "mini listener runs Python");
    // Macro recorder.
    py.clearRecorded();
    py.setRecording(true);
    Node* rec = makeBox(doc, "RecBox", Vec3(0, 400, 0));
    doc->beginEdit("move");
    rec->transform.position = Vec3(5, 400, 0);
    doc->notifyNodeChanged(rec, ChangeTransform);
    doc->endEdit();
    py.setRecording(false);
    QString macro = py.recorded().join('\n');
    check(macro.contains("veraa.create(\"Box\", name=\"RecBox\"") && macro.contains(".position = (5, 400, 0)"),
          "macro recorder records creation and moves as Python");
    // Replaying the macro in a clean state recreates the object.
    doc->setSelection({rec->id});
    doc->deleteSelected();
    py.run(macro, nullptr, false);
    Node* again = s.findNodeByName("RecBox");
    check(again && length(again->worldPosition() - Vec3(5, 400, 0)) < 1e-3f, "recorded macro replays");
  } else {
    std::printf("      python unavailable: %s\n", qPrintable(py.unavailableReason()));
    check(true, "Python scripting skipped (not built)");
  }

  // ---- Hotkeys ----
  QHash<QString, QString> keys = w.hotkeys();
  keys["edit.selectByName"] = "Ctrl+Shift+H";
  w.applyHotkeys(keys, false);
  check(w.command("edit.selectByName")->shortcut() == QKeySequence("Ctrl+Shift+H"), "hotkey reassignment applies");
  {
    HotkeyEditorDialog dlg(&w);
    QString previous = dlg.assign("edit.move", "Q");
    check(previous == "edit.select" && dlg.keys().value("edit.select").isEmpty(),
          "Hotkey Editor moves a conflicting key off the other command");
  }
  w.applyHotkeys(w.defaultHotkeys(), false);
  check(w.command("edit.selectByName")->shortcut() == QKeySequence("H"), "default hotkeys restore");

  // ---- Workspaces ----
  w.saveWorkspace("UiTest");
  check(w.workspaceNames().contains("UiTest"), "workspace saved");
  check(w.loadWorkspace("UiTest"), "workspace loads");
  w.deleteWorkspace("UiTest");
  check(!w.workspaceNames().contains("UiTest"), "workspace deleted");

  // ---- Proxied and fallback commands exist ----
  for (const char* id : {"tools.unlink", "tools.selectAndLink", "snaps.angle", "snaps.percent", "snaps.toggle",
                         "snaps.spinner", "edit.mirror"})
    check(w.command(id) != nullptr, qPrintable(QString("command %1 is available").arg(id)));

  st->setTool(TransformTool::Select);
  doc->clearSelection();
}

// Screenshots of the interface for review (only with VERAA_UI_SCREENS set).
void interfaceScreens(MainWindow& w) {
  QString dir = qEnvironmentVariable("VERAA_UI_SCREENS");
  if (dir.isEmpty()) {
    std::printf("      VERAA_UI_SCREENS not set; skipping interface screenshots\n");
    return;
  }
  QDir().mkpath(dir);
  buildDemoScene(w);
  w.editorState()->setTool(TransformTool::Move);
  w.commandPanel()->showModify();
  pump(300);
  check(testing::saveGrab(&w, QDir(dir).filePath("01_main_window.png")), "main window screenshot");

  auto grabMenu = [&](const QString& title, const QString& file) {
    QMenu* m = w.menuAt(title);
    Q_EMIT m->aboutToShow();
    m->popup(w.mapToGlobal(QPoint(100, 30)));
    pump(250);
    bool ok = m->grab().save(QDir(dir).filePath(file));
    m->hide();
    check(ok, qPrintable("menu screenshot " + file));
  };
  grabMenu("File", "02_file_menu.png");
  grabMenu("Edit", "05_edit_menu.png");
  grabMenu("Tools", "06_tools_menu.png");
  grabMenu("Views", "07_views_menu.png");
  grabMenu("Customize", "12_customize_menu.png");
  grabMenu("Scripting", "13_scripting_menu.png");

  auto grabDialog = [&](QWidget* d, const QString& file) {
    d->show();
    d->raise();
    pump(250);
    bool ok = d->grab().save(QDir(dir).filePath(file));
    d->hide();
    check(ok, qPrintable("dialog screenshot " + file));
  };
  {
    ViewportConfigDialog dlg(w.viewports(), &w, 0);
    grabDialog(&dlg, "20_viewport_configuration.png");
  }
  {
    HotkeyEditorDialog dlg(&w);
    grabDialog(&dlg, "21_hotkey_editor.png");
  }
  {
    UnitsSetupDialog dlg(&w);
    grabDialog(&dlg, "22_units_setup.png");
  }
  {
    PreferencesDialog dlg(&w);
    grabDialog(&dlg, "23_preferences.png");
  }
  {
    LightListerDialog dlg(w.document(), &w);
    grabDialog(&dlg, "24_light_lister.png");
  }
  showListener(w);
  pump(200);
  if (auto* l = w.findChild<QDialog*>("ScriptListenerWindow")) {
    if (auto* sl = l->findChild<ScriptListener*>()) sl->execute("[n.name for n in veraa.nodes()][:4]");
    grabDialog(l, "25_script_listener.png");
  }
  // Command panel tabs.
  QTabWidget* cp = w.commandPanel();
  for (int i = 0; i < cp->count(); ++i) {
    cp->setCurrentIndex(i);
    pump(150);
    QString name = cp->tabToolTip(i);
    check(cp->grab().save(QDir(dir).filePath(QString("3%1_panel_%2.png").arg(i).arg(name.toLower()))),
          qPrintable("command panel screenshot " + name));
  }
  cp->setCurrentIndex(1);
  // Different viewport looks.
  ViewportPanel* vps = w.viewports();
  vps->viewport(3)->setShading(ShadingMode::Clay);
  vps->viewport(1)->setShading(ShadingMode::Facets);
  vps->viewport(2)->setSafeFrame(true);
  vps->viewport(0)->setShading(ShadingMode::BoundingBox);
  pump(200);
  check(testing::saveGrab(&w, QDir(dir).filePath("40_viewport_styles.png")), "viewport styles screenshot");
  vps->setViewLayout(11);
  pump(300);
  check(testing::saveGrab(&w, QDir(dir).filePath("41_layout_1_plus_3.png")), "layout screenshot");
  vps->setViewLayout(9);
  for (int i = 0; i < 4; ++i) {
    vps->viewport(i)->setShading(i == 3 ? ShadingMode::Shaded : ShadingMode::Wireframe);
    vps->viewport(i)->setSafeFrame(false);
  }
  w.layerExplorer()->parentWidget()->show();
  w.layerExplorer()->parentWidget()->raise();
  pump(200);
  check(testing::saveGrab(&w, QDir(dir).filePath("42_layer_explorer.png")), "layer explorer screenshot");
}

}  // namespace

VERAA_SELF_TEST(interface, testInterface);
VERAA_SELF_TEST(interfaceScreens, interfaceScreens);

}  // namespace veraa
