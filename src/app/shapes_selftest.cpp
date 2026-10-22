// UI self test for shapes and construction: Line clicks and closing, Bezier drags, Rectangle
// drag, Extrude/Lathe/Sweep through the modifier stack, Text, Loft (Get Shape pick), Wall, Door,
// spline sub-object editing with undo, Edit Spline, Convert to Editable Spline and a showcase
// screenshot (saved when VERAA_SCREENSHOT_DIR is set).
#include <QAction>
#include <QDir>
#include <QPushButton>

#include <map>

#include "aec.h"
#include "commandpanel.h"
#include "compound.h"
#include "mainwindow.h"
#include "patch_nurbs.h"
#include "selftest.h"
#include "shape_modifiers.h"
#include "shapes.h"
#include "shapes_app.h"
#include "viewport.h"
#include "viewportpanel.h"

namespace veraa {
namespace {

using testing::check;
using testing::projectPoint;
using testing::pump;

double volumeOf(const PolyMesh& m) {
  TriMesh t = triangulate(m);
  double v = 0;
  for (size_t i = 0; i + 2 < t.indices.size(); i += 3)
    v += dot(t.positions[t.indices[i]], cross(t.positions[t.indices[i + 1]], t.positions[t.indices[i + 2]])) / 6.0;
  return v;
}

bool closedManifold(const PolyMesh& m) {
  std::map<std::pair<int, int>, int> directed;
  for (const auto& f : m.faces)
    for (size_t k = 0; k < f.v.size(); ++k) directed[{f.v[k], f.v[(k + 1) % f.v.size()]}]++;
  for (const auto& [e, n] : directed) {
    if (n != 1) return false;
    auto it = directed.find({e.second, e.first});
    if (it == directed.end() || it->second != 1) return false;
  }
  return !m.faces.empty();
}

void click(Viewport* vp, QPointF p, Qt::MouseButton b = Qt::LeftButton) {
  vp->simulateMouse(QEvent::MouseMove, p, Qt::NoButton, Qt::NoButton);
  vp->simulateMouse(QEvent::MouseButtonPress, p, b, b);
  vp->simulateMouse(QEvent::MouseButtonRelease, p, b, Qt::NoButton);
}

void drag(Viewport* vp, QPointF a, QPointF b, int steps = 8) {
  vp->simulateMouse(QEvent::MouseMove, a, Qt::NoButton, Qt::NoButton);
  vp->simulateMouse(QEvent::MouseButtonPress, a, Qt::LeftButton, Qt::LeftButton);
  for (int i = 1; i <= steps; ++i) vp->simulateMouse(QEvent::MouseMove, a + (b - a) * (double(i) / steps), Qt::NoButton, Qt::LeftButton);
  vp->simulateMouse(QEvent::MouseButtonRelease, b, Qt::LeftButton, Qt::NoButton);
}

void moveTo(Viewport* vp, QPointF a, QPointF b, int steps = 6) {
  for (int i = 1; i <= steps; ++i) vp->simulateMouse(QEvent::MouseMove, a + (b - a) * (double(i) / steps), Qt::NoButton, Qt::NoButton);
}

Node* lastNodeOfType(Scene& s, const std::string& type) {
  for (auto it = s.nodes.rbegin(); it != s.nodes.rend(); ++it)
    if ((*it)->object && (*it)->object->type() == type) return it->get();
  return nullptr;
}

QPushButton* findButton(QWidget* root, const QString& text) {
  for (QPushButton* b : root->findChildren<QPushButton*>())
    if (b->text() == text && b->isVisible()) return b;
  for (QPushButton* b : root->findChildren<QPushButton*>())
    if (b->text() == text) return b;
  return nullptr;
}

int knotCount(const BezierShape& s) {
  int n = 0;
  for (const auto& sp : s.splines) n += static_cast<int>(sp.knots.size());
  return n;
}

void testShapes(MainWindow& w) {
  SceneDocument* doc = w.document();
  EditorState* st = w.editorState();
  Scene& s = doc->scene();
  Viewport* top = w.viewports()->viewport(0);
  Viewport* persp = w.viewports()->viewport(3);
  shapesapp::askYesNo = [](QWidget*, const QString&) { return true; };
  // Earlier tests may leave a viewport maximized or zoomed: start from the standard layout.
  if (w.viewports()->isMaximized()) w.viewports()->toggleMaximize();
  top->setViewType(ViewType::Top);
  top->camera().center = Vec3();
  top->camera().orthoHeight = 400.0f;
  top->updateCamera();
  w.viewports()->setActive(top);
  pump(100);
  QPointF c(top->width() * 0.5, top->height() * 0.5);

  // 1. Line: three clicks, then a click on the first knot closes it.
  st->setCreateType("Line");
  click(top, c + QPointF(-120, -60));
  moveTo(top, c + QPointF(-120, -60), c + QPointF(-20, -60));
  click(top, c + QPointF(-20, -60));
  moveTo(top, c + QPointF(-20, -60), c + QPointF(-20, 20));
  click(top, c + QPointF(-20, 20));
  moveTo(top, c + QPointF(-20, 20), c + QPointF(-120, -60));
  click(top, c + QPointF(-120, -60));
  Node* line = lastNodeOfType(s, "Line");
  auto* lineObj = line ? dynamic_cast<EditableSplineObject*>(line->object.get()) : nullptr;
  check(lineObj && lineObj->shape.splines.size() == 1, "Line created by clicks");
  check(lineObj && lineObj->shape.splines[0].knots.size() == 3, "Line has three corner knots");
  check(lineObj && lineObj->shape.splines[0].closed, "clicking the first knot closes the Line");
  uint32_t lineId = line ? line->id : 0;

  // 2. Line with a click-drag (Bezier knot), finished with a right-click.
  drag(top, c + QPointF(60, -80), c + QPointF(100, -100));
  moveTo(top, c + QPointF(100, -100), c + QPointF(160, -20));
  click(top, c + QPointF(160, -20));
  click(top, c + QPointF(160, -20), Qt::RightButton);
  Node* bez = lastNodeOfType(s, "Line");
  auto* bezObj = bez ? dynamic_cast<EditableSplineObject*>(bez->object.get()) : nullptr;
  check(bez && bez->id != lineId && bezObj && bezObj->shape.splines[0].knots.size() == 2, "second Line has two knots");
  check(bezObj && bezObj->shape.splines[0].knots[0].type == KnotType::Bezier, "dragging creates a Bezier knot");
  check(bezObj && !bezObj->shape.splines[0].closed, "right-click leaves the Line open");
  uint32_t bezId = bez ? bez->id : 0;
  st->setCreateType({});

  // 3. Rectangle by dragging.
  st->setCreateType("Rectangle");
  drag(top, c + QPointF(40, 40), c + QPointF(140, 100));
  st->setCreateType({});
  Node* rect = lastNodeOfType(s, "Rectangle");
  check(rect && rect->object->params.getFloat("width") > 10 && rect->object->params.getFloat("length") > 10,
        "Rectangle sized by dragging");

  // 4. Extrude from the Modifier List: a closed box of width x length x amount.
  if (rect) {
    doc->setSelection({rect->id});
    w.commandPanel()->addModifier("Extrude");
    pump();
    check(rect->modifiers.size() == 1 && rect->modifiers[0]->type() == "Extrude", "Extrude added to the Rectangle");
    doc->beginEdit("amount");
    rect->modifiers[0]->params.setNumber("amount", 50);
    doc->notifyNodeChanged(rect, ChangeGeometry);
    doc->endEdit();
    double expected = rect->object->params.getFloat("width") * rect->object->params.getFloat("length") * 50.0;
    check(!rect->isShapeResult() && closedManifold(rect->mesh()), "Extrude gives a closed mesh");
    check(std::fabs(volumeOf(rect->mesh()) - expected) < expected * 1e-3, "Extrude volume = width x length x amount");
  }

  // 5. Lathe of a profile line around its left edge.
  doc->beginEdit("lathe test");
  Node* prof = s.createNode("Line");
  auto* profObj = static_cast<EditableSplineObject*>(prof->object.get());
  profObj->shape.splines = {makePolylineSpline({{0, 0}, {40, 0}, {40, 60}, {0, 60}}, true)};
  prof->transform.position = Vec3(-300, 200, 0);
  prof->modifiers.push_back(createModifier("Lathe"));
  doc->notifyStructureChanged();
  doc->endEdit();
  check(closedManifold(prof->mesh()) && volumeOf(prof->mesh()) > 0.9 * kPi * 1600 * 60 * 0.95,
        "Lathe of a rectangle profile is a closed cylinder");

  // 6. Sweep along a Line.
  if (bez) {
    doc->setSelection({bez->id});
    w.commandPanel()->addModifier("Sweep");
    pump();
    check(!bez->isShapeResult() && closedManifold(bez->mesh()), "Sweep along a Line gives a closed solid");
  }

  // 7. Text by a click.
  st->setCreateType("Text");
  click(top, c + QPointF(-150, 120));
  st->setCreateType({});
  Node* text = lastNodeOfType(s, "Text");
  check(text && !shapesapp::evaluatedShape(*text).empty(), "Text node created with outlines");

  // 8. Loft: select the closed Line as path, choose Loft, then Get Shape picks a circle.
  doc->beginEdit("circle");
  Node* circle = s.createNode("Circle");
  circle->object->params.setNumber("radius", 15);
  circle->transform.position = Vec3(60, -150, 0);
  uint32_t circleId = circle->id;
  doc->notifyStructureChanged();
  doc->endEdit();
  if (bezId) {
    doc->setSelection({bezId});
    Node* path = s.findNode(bezId);
    // Use the plain Line (without Sweep) as the path.
    doc->beginEdit("strip");
    path->modifiers.clear();
    doc->notifyNodeChanged(path, ChangeGeometry);
    doc->endEdit();
  }
  st->setCreateType("Loft");
  pump(100);
  Node* loft = lastNodeOfType(s, "Loft");
  check(loft != nullptr, "Loft created from the selected path");
  if (loft) {
    w.commandPanel()->showModify();
    pump(100);
    QPushButton* getShape = findButton(w.commandPanel(), "Get Shape");
    check(getShape != nullptr, "Loft has a Get Shape button");
    if (getShape) {
      getShape->click();
      pump();
      top->updateCamera();
      Vec3 onCircle = circle->worldMatrix().transformPoint(Vec3(15, 0, 0));
      click(top, projectPoint(top, onCircle));
      click(top, projectPoint(top, onCircle), Qt::RightButton);
    }
    auto* lo = dynamic_cast<CompoundObject*>(loft->object.get());
    check(lo && lo->operandsWithRole("shape").size() == 1, "Get Shape picked the circle");
    check(!loft->mesh().empty(), "Loft builds a mesh");
  }

  // 9. Wall: three clicks and a right-click.
  st->setCreateType("Wall");
  click(top, c + QPointF(-200, 150));
  moveTo(top, c + QPointF(-200, 150), c + QPointF(-60, 150));
  click(top, c + QPointF(-60, 150));
  moveTo(top, c + QPointF(-60, 150), c + QPointF(-60, 220));
  click(top, c + QPointF(-60, 220));
  click(top, c + QPointF(-60, 220), Qt::RightButton);
  st->setCreateType({});
  Node* wall = lastNodeOfType(s, "Wall");
  auto* wallObj = wall ? dynamic_cast<WallObject*>(wall->object.get()) : nullptr;
  check(wallObj && wallObj->points.size() == 3, "Wall created with three points");
  check(wall && closedManifold(wall->mesh()) && volumeOf(wall->mesh()) > 0, "Wall is a closed solid");

  // 10. Door: drag the width, move for depth, click, move for height, click.
  st->setCreateType("PivotDoor");
  QPointF d0 = c + QPointF(100, 160);
  drag(top, d0, d0 + QPointF(90, 0));
  moveTo(top, d0 + QPointF(90, 0), d0 + QPointF(90, -20));
  click(top, d0 + QPointF(90, -20));
  moveTo(top, d0 + QPointF(90, -20), d0 + QPointF(90, -140));
  click(top, d0 + QPointF(90, -140));
  st->setCreateType({});
  Node* door = lastNodeOfType(s, "PivotDoor");
  check(door && door->object->params.getFloat("width") > 10 && door->object->params.getFloat("depth") > 1 &&
            door->object->params.getFloat("height") > 10,
        "Door sized by drag, depth and height");
  check(door && !door->mesh().empty(), "Door builds its frame and leaf");

  // 11. Sub-object editing of the closed Line: move a vertex, fillet it, undo.
  Node* ln = s.findNode(lineId);
  if (ln) {
    doc->setSelection({ln->id});
    w.commandPanel()->showModify();
    pump(100);
    check(shapesapp::setSplineSubObjectLevel(st, 1), "Vertex level available on the Line");
    auto* es = static_cast<EditableSplineObject*>(ln->object.get());
    Vec3 k0 = es->shape.splines[0].knots[0].p;
    QPointF kp = projectPoint(top, ln->objectWorldMatrix().transformPoint(k0));
    float dragWorld = 25.0f * top->camera().worldPerPixel(ln->objectWorldMatrix().transformPoint(k0));
    drag(top, kp, kp + QPointF(25, 0));
    es = static_cast<EditableSplineObject*>(ln->object.get());
    check(es->shape.splines[0].knots[0].p.x > k0.x + dragWorld * 0.5f, "dragging a vertex moves it");
    int before = knotCount(es->shape);
    if (QPushButton* fillet = findButton(w.commandPanel(), "Fillet")) fillet->click();
    pump();
    es = static_cast<EditableSplineObject*>(ln->object.get());
    check(knotCount(es->shape) == before + 1, "Fillet replaces the vertex with two");
    doc->undoStack()->undo();
    pump();
    es = static_cast<EditableSplineObject*>(s.findNode(lineId)->object.get());
    check(knotCount(es->shape) == before, "undo restores the vertex");
    // Segment level: Divide adds vertices.
    shapesapp::setSplineSubObjectLevel(st, 2);
    const Spline& sp = es->shape.splines[0];
    QPointF mid = projectPoint(top, s.findNode(lineId)->objectWorldMatrix().transformPoint(sp.evaluate(0, 0.5f)));
    click(top, mid);
    QPushButton* divide = findButton(w.commandPanel(), "Divide");
    check(divide != nullptr, "Segment level shows Divide");
    if (divide) divide->click();
    pump();
    es = static_cast<EditableSplineObject*>(s.findNode(lineId)->object.get());
    check(knotCount(es->shape) == before + 1, "Divide splits the selected segment");
    shapesapp::setSplineSubObjectLevel(st, 0);
  }

  // 12. Edit Spline on a parametric shape: moving a vertex stores the edit in the modifier.
  doc->beginEdit("star");
  Node* star = s.createNode("Star");
  star->transform.position = Vec3(-250, -200, 0);
  doc->notifyStructureChanged();
  doc->endEdit();
  doc->setSelection({star->id});
  w.commandPanel()->addModifier("EditSpline");
  pump(100);
  if (shapesapp::setSplineSubObjectLevel(st, 1)) {
    BezierShape before = shapesapp::evaluatedShape(*star);
    Vec3 k = before.splines[0].knots[0].p;
    QPointF kp = projectPoint(top, star->objectWorldMatrix().transformPoint(k));
    drag(top, kp, kp + QPointF(0, -30));
    auto* es = dynamic_cast<EditSplineModifier*>(star->modifiers[0].get());
    check(es && es->hasEdits, "Edit Spline stores the edited shape");
    check(es && std::fabs(es->shape.splines[0].knots[0].p.y - k.y) > 5, "Edit Spline vertex moved");
    shapesapp::setSplineSubObjectLevel(st, 0);
  } else {
    check(false, "Edit Spline sub-object level");
  }

  // 13. Convert to Editable Spline.
  // (Undo above rebuilt the scene's nodes, so look the circle up again by id.)
  doc->setSelection({circleId});
  if (QAction* a = w.command("convertToEditableSpline")) a->trigger();
  Node* conv = s.findNode(circleId);
  check(conv && conv->object->type() == "EditableSpline", "Convert to Editable Spline");
  check(conv && knotCount(static_cast<EditableSplineObject*>(conv->object.get())->shape) == 4,
        "converted circle keeps its four knots");
  w.commandPanel()->showModify();
  pump(50);
  doc->clearSelection();
  pump(50);
  QPushButton* stale = findButton(w.commandPanel(), "Attach Mult.");
  check(!stale || !stale->isVisible(), "spline rollout leaves with the selection");

  // 14. Scatter from the selected box onto a picked plane.
  doc->beginEdit("scatter operands");
  Node* src = s.createNode("Box");
  for (const char* k : {"length", "width", "height"}) src->object->params.setNumber(k, 6);
  src->transform.position = Vec3(-150, 60, 0);
  Node* ground = s.createNode("Plane");
  ground->object->params.setNumber("length", 80);
  ground->object->params.setNumber("width", 80);
  ground->transform.position = Vec3(-150, 0, 0);
  uint32_t groundId = ground->id;
  doc->notifyStructureChanged();
  doc->endEdit();
  doc->setSelection({src->id});
  st->setCreateType("Scatter");
  pump(100);
  Node* scatter = lastNodeOfType(s, "Scatter");
  check(scatter != nullptr && s.findNode(src->id) == nullptr, "Scatter replaces the selected source object");
  if (scatter) {
    w.commandPanel()->showModify();
    pump(100);
    if (QPushButton* pick = findButton(w.commandPanel(), "Pick Distribution Object")) {
      pick->click();
      pump();
      Node* g = s.findNode(groundId);
      click(top, projectPoint(top, g->worldMatrix().transformPoint(Vec3(10, 10, 0))));
    }
    auto* sc = dynamic_cast<CompoundObject*>(scatter->object.get());
    check(sc && sc->firstOperand("distribution") != nullptr, "Scatter picked its distribution object");
    check(scatter->mesh().faces.size() >= 10 * 6, "Scatter places the duplicates");
  }

  // 15. CV Curve by clicks (right-click ends) and a Quad Patch by dragging.
  st->setCreateType("CVCurve");
  // Offsets stay well inside the viewport (and clear of its label menu) at any layout size.
  click(top, c + QPointF(-160, -40));
  moveTo(top, c + QPointF(-160, -40), c + QPointF(-110, -80));
  click(top, c + QPointF(-110, -80));
  moveTo(top, c + QPointF(-110, -80), c + QPointF(-60, -40));
  click(top, c + QPointF(-60, -40));
  click(top, c + QPointF(-60, -40), Qt::RightButton);
  st->setCreateType({});
  Node* cvn = lastNodeOfType(s, "CVCurve");
  auto* cvo = cvn ? dynamic_cast<NurbsCurveObject*>(cvn->object.get()) : nullptr;
  check(cvo && cvo->points.size() == 3, "CV Curve created with three CVs");
  check(cvn && cvn->isShapeResult() && !cvn->shapeLines().empty(), "CV Curve draws as a spline");
  st->setCreateType("QuadPatch");
  drag(top, c + QPointF(120, -80), c + QPointF(200, -30));
  st->setCreateType({});
  Node* qp = lastNodeOfType(s, "QuadPatch");
  check(qp && qp->object->params.getFloat("width") > 10 && !qp->mesh().empty(), "Quad Patch sized by dragging");

  // Showcase screenshot: construction objects in a shaded perspective view.
  QString dir = qEnvironmentVariable("VERAA_SCREENSHOT_DIR");
  if (!dir.isEmpty()) {
    doc->newScene();
    pump(50);
    doc->beginEdit("showcase");
    auto add = [&](const char* type, Vec3 pos, float rotZ = 0) {
      Node* n = s.createNode(type);
      n->transform.position = pos;
      n->transform.rotation = Vec3(0, 0, rotZ);
      return n;
    };
    Node* room = add("Wall", Vec3(-200, -150, 0));
    static_cast<WallObject*>(room->object.get())->points = {{0, 0}, {500, 0}, {500, 350}, {0, 350}};
    static_cast<WallObject*>(room->object.get())->closed = true;
    room->object->params.setNumber("height", 180);
    add("PivotDoor", Vec3(-100, -160, 0))->object->params.setNumber("open", 35);
    add("CasementWindow", Vec3(120, -160, 80))->object->params.setNumber("open", 40);
    Node* stair = add("StraightStair", Vec3(170, 20, 0));
    stair->object->params.setNumber("height", 170);
    stair->object->params.setNumber("railRight", 1);
    stair->object->params.setNumber("stringers", 1);
    Node* tree = add("Foliage", Vec3(-420, 120, 0));
    tree->object->params.setNumber("height", 330);
    Node* tp = add("TextPlus", Vec3(-180, -330, 0));
    tp->object->params.setString("text", "Veraa");
    tp->object->params.setNumber("size", 90);
    tp->object->params.setNumber("extrude", 25);
    tp->object->params.setNumber("applyBevel", 1);
    tp->object->params.setNumber("bevelProfile", 1);
    tp->transform.rotation = Vec3(90, 0, 0);
    Node* vase = add("Line", Vec3(-20, 60, 0));
    static_cast<EditableSplineObject*>(vase->object.get())->shape.splines = {
        makePolylineSpline({{0, 0}, {30, 0}, {38, 20}, {25, 60}, {32, 90}, {0, 90}}, false)};
    vase->transform.rotation = Vec3(90, 0, 0);
    vase->modifiers.push_back(createModifier("Lathe"));
    vase->modifiers.back()->params.setNumber("segments", 32);
    Node* rail = add("Railing", Vec3(-200, -220, 0));
    rail->object->params.setNumber("length", 300);
    Node* spiral = add("SpiralStair", Vec3(420, 300, 0));
    spiral->object->params.setNumber("height", 220);
    spiral->object->params.setNumber("radius", 70);
    spiral->object->params.setNumber("railRight", 1);
    Node* knot = add("Star", Vec3(420, -320, 30));
    knot->object->params.setNumber("radius1", 60);
    knot->object->params.setNumber("radius2", 35);
    knot->object->params.setNumber("fillet1", 6);
    knot->modifiers.push_back(createModifier("Bevel"));
    knot->modifiers.back()->params.setNumber("level1Height", 15);
    knot->modifiers.back()->params.setNumber("level2", 1);
    knot->modifiers.back()->params.setNumber("level2Height", 6);
    knot->modifiers.back()->params.setNumber("level2Outline", -6);
    for (auto& n : s.nodes) n->invalidate();
    doc->notifyStructureChanged();
    doc->endEdit();
    persp->setShading(ShadingMode::Shaded);
    persp->setEdgedFaces(true);
    w.viewports()->setActive(persp);
    if (!w.viewports()->isMaximized()) w.viewports()->toggleMaximize();
    w.viewports()->zoomExtentsAll(false);
    pump(200);
    QDir().mkpath(dir);
    testing::saveGrab(&w, dir + "/shapes_showcase.png");
    w.viewports()->toggleMaximize();
    // Spline editing view: a Line in vertex level with a Bezier knot selected.
    doc->newScene();
    pump(50);
    st->setCreateType("Line");
    QPointF c2(top->width() * 0.5, top->height() * 0.5);
    click(top, c2 + QPointF(-140, 40));
    moveTo(top, c2 + QPointF(-140, 40), c2 + QPointF(-60, -60));
    drag(top, c2 + QPointF(-60, -60), c2 + QPointF(-10, -80));
    moveTo(top, c2 + QPointF(-10, -80), c2 + QPointF(60, 40));
    click(top, c2 + QPointF(60, 40));
    moveTo(top, c2 + QPointF(60, 40), c2 + QPointF(140, -40));
    click(top, c2 + QPointF(140, -40));
    click(top, c2 + QPointF(140, -40), Qt::RightButton);
    st->setCreateType({});
    Node* l2 = lastNodeOfType(s, "Line");
    if (l2) {
      doc->setSelection({l2->id});
      w.commandPanel()->showModify();
      pump(100);
      shapesapp::setSplineSubObjectLevel(st, 1);
      auto* es = static_cast<EditableSplineObject*>(l2->object.get());
      QPointF kp = projectPoint(top, l2->objectWorldMatrix().transformPoint(es->shape.splines[0].knots[1].p));
      click(top, kp);
      pump(100);
      w.viewports()->setActive(top);
      testing::saveGrab(&w, dir + "/shapes_spline_edit.png");
      shapesapp::setSplineSubObjectLevel(st, 0);
    }
  }
}

}  // namespace

VERAA_SELF_TEST(shapes, testShapes);

}  // namespace veraa
