// UI self test for the animation module ("Veraa --script animation", also part of "selftest").
// Screenshots go to $VERAA_SHOT_DIR (or a temp folder).
#include <QAction>
#include <QMenu>
#include <QMenuBar>
#include <QTimer>
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QSpinBox>
#include <QTreeWidget>

#include <cmath>

#include "animbar.h"
#include "animcontroller.h"
#include "animui.h"
#include "commandpanel.h"
#include "mainwindow.h"
#include "particles.h"
#include "renderanim.h"
#include "rigging.h"
#include "selftest.h"
#include "viewport.h"
#include "viewportpanel.h"

namespace veraa {
namespace {

using testing::check;
using testing::pump;

QString shotDir() {
  QString d = qEnvironmentVariable("VERAA_SHOT_DIR");
  if (d.isEmpty()) d = QDir(QDir::tempPath()).filePath("veraa-anim-shots");
  QDir().mkpath(d);
  return d;
}

void shot(QWidget* w, const QString& name) { testing::saveGrab(w, QDir(shotDir()).filePath(name)); }

Node* create(MainWindow& w, const char* type) {
  SceneDocument* doc = w.document();
  doc->beginEdit("Create");
  Node* n = doc->createNode(type);
  doc->endEdit();
  return n;
}

void moveNode(MainWindow& w, Node* n, const Vec3& pos) {
  SceneDocument* doc = w.document();
  doc->beginEdit("Move");
  n->transform.position = pos;
  doc->notifyNodeChanged(n, ChangeTransform);
  doc->endEdit();
}

void testAnimation(MainWindow& w) {
  AnimController* c = AnimController::instance();
  w.viewports()->zoomExtentsAll(false);
  check(c != nullptr, "animation controller installed");
  if (!c) return;
  SceneDocument* doc = w.document();
  check(w.command("anim.autoKey") && w.command("anim.setKey") && w.command("anim.play") && w.command("anim.timeConfig"),
        "animation commands registered");
  check(w.command("graph.curveEditor") && w.command("graph.dopeSheet") && w.command("graph.schematic"),
        "graph editor commands registered");
  check(w.findChild<TimeSlider*>() && w.findChild<TrackBar*>(), "time slider and track bar below the viewports");

  // ---- Auto Key ----
  Node* box = create(w, "Box");
  uint32_t boxId0 = box->id;
  doc->setSelection({boxId0});
  Vec3 start = box->transform.position;
  c->setFrame(50);
  w.command("anim.autoKey")->trigger();
  check(c->autoKey(), "N / Auto Key toggles on");
  moveNode(w, box, start + Vec3(100, 0, 0));
  std::string px = Animation::nodePath(box->id, "pos.x");
  Animation* anim = c->animation();
  check(anim->isAnimated(px), "Auto Key: moving at frame 50 creates a position track");
  const Track* tx = anim->findTrack(px);
  check(tx && tx->keys.size() == 2, "Auto Key: keys at frame 0 and frame 50");
  c->setFrame(25);
  check(std::fabs(box->transform.position.x - (start.x + 50.0f)) < 0.5f, "Auto Key: interpolated halfway at frame 25");
  c->setFrame(50);
  check(std::fabs(box->transform.position.x - (start.x + 100.0f)) < 1e-3f, "Auto Key: value at frame 50");
  c->setFrame(25);
  w.viewports()->zoomExtentsAll(false);
  pump(100);
  shot(&w, "anim_autokey.png");
  // Undo removes the keys.
  doc->undoStack()->undo();
  check(!anim->isAnimated(px), "undo removes the Auto Key keys");
  doc->undoStack()->redo();
  anim = c->animation();
  box = c->scene().findNode(boxId0);  // undo/redo rebuilt the nodes
  check(box != nullptr, "box survives undo/redo");
  if (!box) return;
  check(anim->isAnimated(px), "redo restores the keys");
  w.command("anim.autoKey")->trigger();
  check(!c->autoKey(), "Auto Key toggles off");

  // Without Auto Key, moving an animated object offsets its animation.
  c->setFrame(50);
  double before0 = anim->valueAt(c->scene(), px, 0);
  moveNode(w, box, box->transform.position + Vec3(10, 0, 0));
  check(std::fabs(anim->valueAt(c->scene(), px, 0) - (before0 + 10.0)) < 1e-3, "edit without Auto Key offsets the animation");

  // ---- Set Key ----
  w.command("anim.setKeyMode")->trigger();
  check(c->setKeyMode(), "Set Key mode on");
  c->setFrame(80);
  moveNode(w, box, box->transform.position + Vec3(0, 40, 0));
  std::string py = Animation::nodePath(box->id, "pos.y");
  check(!c->animation()->isAnimated(py), "Set Key mode: edits are not keyed until Set Keys");
  w.command("anim.setKey")->trigger();
  const Track* ty = c->animation()->findTrack(py);
  check(ty && ty->keyIndexAt(80) >= 0, "Set Keys (K) keys the position at the current frame");
  w.command("anim.setKeyMode")->trigger();

  // Track bar shows the keys of the selection.
  auto* bar = w.findChild<TrackBar*>();
  check(bar && bar->keyMarks().size() >= 3, "track bar shows the selection's keys");

  // Trajectory and ghosting display.
  if (QAction* t = w.command("views.trajectories")) t->setChecked(true);
  if (QAction* g = w.command("views.ghosting")) g->setChecked(true);
  c->setFrame(40);
  w.viewports()->zoomExtentsAll(false);
  pump(100);
  shot(&w, "anim_trajectory_ghosting.png");
  if (QAction* t = w.command("views.trajectories")) t->setChecked(false);
  if (QAction* g = w.command("views.ghosting")) g->setChecked(false);

  // ---- Playback ----
  c->setFrame(0);
  c->play();
  pump(500);
  check(c->frame() > 0, "playback advances the time");
  c->stop();
  check(!c->playing(), "playback stops");
  c->setFrame(0);
  w.command("anim.nextFrame")->trigger();
  check(std::fabs(c->frame() - 1.0) < 1e-9, ". steps to the next frame");
  w.command("anim.goEnd")->trigger();
  check(std::fabs(c->frame() - c->endFrame()) < 1e-9, "End goes to the last frame");
  w.command("anim.goStart")->trigger();
  check(std::fabs(c->frame() - c->startFrame()) < 1e-9, "Home goes to the first frame");
  c->setKeyStepMode(true);
  c->stepFrame(1);
  check(std::fabs(c->frame() - 50.0) < 1e-9, "key mode steps to the next key");
  c->setKeyStepMode(false);

  // ---- Time configuration ----
  {
    TimeConfigDialog d(c, &w);
    if (auto* e = d.findChild<QSpinBox*>("timeConfigEnd")) e->setValue(120);
    d.apply();
    check(c->endFrame() == 120, "Time Configuration changes the range");
    c->setRange(0, 100);
  }

  // ---- Graph editors ----
  doc->setSelection({box->id});
  w.command("graph.curveEditor")->trigger();
  pump(200);
  QWidget* tv = nullptr;
  for (QWidget* top : QApplication::topLevelWidgets())
    if (top->objectName() == "trackView" && top->isVisible()) tv = top;
  check(tv != nullptr, "Curve Editor opens");
  if (tv) {
    if (auto* tree = tv->findChild<QTreeWidget*>("trackViewTree")) {
      check(tree->topLevelItemCount() >= 1, "Curve Editor lists the selected object");
      // Show the X position curve.
      QTreeWidgetItemIterator it(tree);
      while (*it) {
        if ((*it)->text(0).startsWith("X Position")) (*it)->setSelected(true);
        ++it;
      }
    }
    pump(100);
    shot(tv, "anim_curve_editor.png");
    tv->close();
  }
  w.command("graph.dopeSheet")->trigger();
  pump(200);
  for (QWidget* top : QApplication::topLevelWidgets())
    if (top->objectName() == "trackView" && top->isVisible()) {
      shot(top, "anim_dope_sheet.png");
      top->close();
    }
  w.command("graph.schematic")->trigger();
  pump(200);
  bool schematic = false;
  for (QWidget* top : QApplication::topLevelWidgets())
    if (top->objectName() == "schematicView" && top->isVisible()) {
      schematic = true;
      shot(top, "anim_schematic.png");
      top->close();
    }
  check(schematic, "Schematic View opens");

  // ---- Path constraint ----
  Node* line = create(w, "EditableSpline");
  {
    auto* sp = static_cast<EditableSplineObject*>(line->object.get());
    Spline s;
    s.knots = {cornerKnot(Vec3(-100, 100, 0)), cornerKnot(Vec3(100, 100, 0))};
    sp->shape.splines = {s};
    sp->params.setNumber("steps", 0);
    line->invalidate();
  }
  Node* ball = create(w, "Sphere");
  addConstraintTo(c, ball->id, ConstraintType::Path, line->id);
  c->editAnimation("Path Percent", [&] {
    Track& t = c->animation()->tracks[Animation::nodePath(ball->id, "c:0/percent")];
    t.setKey(0, 0);
    t.setKey(100, 100);
    for (auto& k : t.keys) k.inType = k.outType = TangentType::Linear;
  });
  c->setFrame(50);
  check(std::fabs(ball->worldPosition().x) < 0.5f && std::fabs(ball->worldPosition().y - 100.0f) < 0.5f,
        "path constraint: halfway along the line at frame 50");
  c->setFrame(100);
  check(std::fabs(ball->worldPosition().x - 100.0f) < 0.5f, "path constraint: end of the line at frame 100");
  doc->setSelection({ball->id});
  if (QAction* m = w.command("anim.motionPanel")) m->trigger();
  pump(200);
  c->setFrame(30);
  w.viewports()->zoomExtentsAll(false);
  pump(100);
  shot(&w, "anim_motion_panel.png");

  // ---- Skin ----
  Node* body = create(w, "Box");
  body->object->params.setNumber("heightSegs", 4);
  body->transform.position = Vec3(300, 0, 0);
  body->invalidate();
  Node* bone = create(w, "Bone");
  bone->transform.position = Vec3(300, 0, 50);
  doc->setSelection({body->id});
  w.commandPanel()->addModifier("Skin");
  auto* skin = dynamic_cast<SkinModifier*>(body->modifiers.empty() ? nullptr : body->modifiers.back().get());
  check(skin != nullptr, "Skin modifier added from the Modifier List");
  if (skin) {
    skin->addBone(c->scene(), bone->id, *body);
    body->invalidate();
    PolyMesh rest = body->mesh();
    moveNode(w, bone, Vec3(300, 0, 80));
    c->refresh();
    body->invalidate();
    float moved = 0;
    const PolyMesh& now = body->mesh();
    for (size_t i = 0; i < now.positions.size() && i < rest.positions.size(); ++i)
      moved = std::max(moved, length(now.positions[i] - rest.positions[i]));
    check(moved > 20.0f, "skin: moving the bone moves the vertices");
    w.commandPanel()->showModify();
    doc->setSelection({});
    doc->setSelection({body->id});
    w.viewports()->zoomExtentsAll(true);
    pump(150);
    shot(&w, "anim_skin.png");
  }

  // ---- Bones creation and IK ----
  w.editorState()->setCreateType("Bone");
  Viewport* top = w.viewports()->viewport(0);
  size_t bonesBefore = 0;
  for (const auto& up : c->scene().nodes) bonesBefore += up->object && up->object->type() == "Bone";
  std::vector<Vec3> joints = {Vec3(-200, -200, 0), Vec3(-150, -200, 0), Vec3(-100, -200, 0)};
  for (const Vec3& p : joints) {
    QPointF sp = testing::projectPoint(top, p);
    top->simulateMouse(QEvent::MouseMove, sp, Qt::NoButton, Qt::NoButton);
    top->simulateMouse(QEvent::MouseButtonPress, sp, Qt::LeftButton, Qt::LeftButton);
    top->simulateMouse(QEvent::MouseButtonRelease, sp, Qt::LeftButton, Qt::NoButton);
  }
  QPointF last = testing::projectPoint(top, Vec3(-80, -200, 0));
  top->simulateMouse(QEvent::MouseMove, last, Qt::NoButton, Qt::NoButton);
  top->simulateMouse(QEvent::MouseButtonPress, last, Qt::RightButton, Qt::RightButton);
  top->simulateMouse(QEvent::MouseButtonRelease, last, Qt::RightButton, Qt::NoButton);
  w.editorState()->setCreateType({});
  std::vector<Node*> bones;
  for (const auto& up : c->scene().nodes)
    if (up->object && up->object->type() == "Bone") bones.push_back(up.get());
  check(bones.size() == bonesBefore + 3, "bone chain created with clicks (two bones and an end bone)");
  if (bones.size() >= bonesBefore + 3) {
    Node* b0 = bones[bonesBefore];
    Node* b2 = bones[bonesBefore + 2];
    uint32_t ik = createIkChain(c, b0->id, b2->id, 0);
    check(ik != 0, "HI Solver creates an IK chain");
    if (Node* goal = c->scene().findNode(ik)) {
      moveNode(w, goal, Vec3(-130, -170, 0));
      c->refresh();
      check(length(b2->worldPosition() - Vec3(-130, -170, 0)) < 1.0f, "IK chain reaches the goal");
    }
  }

  // ---- Particles ----
  Node* spray = create(w, "Spray");
  spray->transform.position = Vec3(0, -300, 200);
  Node* grav = create(w, "Gravity");
  grav->transform.position = Vec3(0, -300, 300);
  check(bindToSpaceWarp(*spray, *grav), "spray bound to gravity");
  c->setFrame(5);
  size_t f5 = spray->mesh().faces.size();
  c->setFrame(25);
  size_t f25 = spray->mesh().faces.size();
  check(f5 > 0 && f25 > f5, "particle count grows over time");
  doc->setSelection({spray->id});
  w.viewports()->zoomExtentsAll(false);
  pump(100);
  shot(&w, "anim_particles.png");

  // ---- Render animation ----
  QString out = QDir(shotDir()).filePath("render_seq/frame.png");
  QDir(QFileInfo(out).absolutePath()).removeRecursively();
  RenderAnimOptions o;
  o.frames = {0, 10, 20};
  o.width = 80;
  o.height = 60;
  o.samples = 2;
  o.path = out;
  int written = renderAnimation(w, o, {});
  bool files = written == 3;
  for (double f : o.frames) files &= QFileInfo::exists(sequenceFileName(out, static_cast<int>(f)));
  check(files, "Render Animation writes one PNG per frame");
  o.format = 2;
  o.frames = {0, 1, 2, 3};
  o.path = QDir(shotDir()).filePath("render_seq/clip.avi");
  check(renderAnimation(w, o, {}) == 4 && QFileInfo(o.path).size() > 1000, "Render Animation writes an AVI");
  QString preview = QDir(shotDir()).filePath("render_seq/preview.avi");
  check(createViewportPreview(w, {0, 5, 10}, preview, 50, {}) == 3 && QFileInfo(preview).size() > 1000,
        "Preview - Grab Viewport writes an AVI");

  // Dialogs and the Animation menu (screenshots; modal dialogs are grabbed and closed).
  auto grabModal = [](const QString& name) {
    QTimer::singleShot(400, [name] {
      if (QWidget* d = QApplication::activeModalWidget()) {
        shot(d, name);
        d->close();
      }
    });
  };
  grabModal("anim_render_dialog.png");
  w.command("render.animation")->trigger();
  grabModal("anim_time_config.png");
  w.command("anim.timeConfig")->trigger();
  if (QMenu* m = w.menuAt("Animation")) {
    m->popup(w.mapToGlobal(QPoint(430, 22)));
    pump(200);
    shot(m, "anim_menu.png");
    m->hide();
  }

  // Animation is part of the saved file.
  QString file = QDir(shotDir()).filePath("anim_test.veraa");
  QString err;
  check(doc->save(file, &err), "save animated scene");
  check(doc->load(file, &err), "reload animated scene");
  check(c->animation()->isAnimated(px), "animation survives save and load");
  c->setFrame(0);
}

}  // namespace

VERAA_SELF_TEST(animation, testAnimation);

}  // namespace veraa
