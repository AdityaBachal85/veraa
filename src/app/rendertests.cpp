// Self tests of the rendering UI ("Veraa --script rendering"): Render Setup tabs, Environment
// and Effects, material editor navigation, Slate editor, light/camera creation tools, frame
// buffer elements / Light Mix / history / saving, Batch Render, Print Size Assistant, proxy
// export and fur. Set VERAA_SHOT_DIR to also save screenshots of the windows.
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTabWidget>
#include <QTemporaryDir>

#include <cmath>

#include "commandpanel.h"
#include "framebuffer.h"
#include "mainwindow.h"
#include "materialeditor.h"
#include "materialwidgets.h"
#include "renderdialogs.h"
#include "renderobjects.h"
#include "rendersetup.h"
#include "rendertools.h"
#include "selftest.h"
#include "slateeditor.h"
#include "viewcamera.h"
#include "viewport.h"
#include "viewportpanel.h"

namespace veraa {

namespace {

using testing::check;
using testing::pump;

QString shot(const QString& name) {
  QString dir = qEnvironmentVariable("VERAA_SHOT_DIR");
  return dir.isEmpty() ? QString() : QDir(dir).filePath(name + ".png");
}

void grab(QWidget* w, const QString& name) {
  QString path = shot(name);
  if (!path.isEmpty()) testing::saveGrab(w, path);
}

Material* makeMaterial(Scene& s, const char* name, const char* type) {
  Material* m = s.createMaterial(name);
  m->setMaterialType(type);
  return m;
}

Node* addNode(SceneDocument* doc, const char* type, const Vec3& pos, uint32_t material = 0) {
  Node* n = doc->createNode(type);
  n->transform.position = pos;
  n->materialId = material;
  return n;
}

// A small product-shot scene: floor, car paint sphere, wax box, two lights, a physical camera.
void buildRenderScene(SceneDocument* doc) {
  Scene& s = doc->scene();
  doc->beginEdit("Render test scene");
  Material* floor = makeMaterial(s, "Floor", "VeraaMtl");
  floor->params.setColor("diffuse", Vec3(0.55f, 0.55f, 0.58f));
  floor->params.setColor("reflect", Vec3(0.25f));
  floor->params.setNumber("reflectGlossiness", 0.7);
  Material* paint = makeMaterial(s, "Car paint", "VeraaCarPaintMtl");
  paint->params.setColor("baseColor", Vec3(0.55f, 0.02f, 0.03f));
  Material* wax = makeMaterial(s, "Wax", "VeraaSSSMtl");
  wax->params.setColor("sssColor", Vec3(0.9f, 0.75f, 0.45f));
  Material* gold = makeMaterial(s, "Gold", "VeraaMtl");
  gold->params.setColor("diffuse", Vec3(1.0f, 0.78f, 0.34f));
  gold->params.setColor("reflect", Vec3(1.0f));
  gold->params.setNumber("metalness", 1.0);
  gold->params.setNumber("reflectGlossiness", 0.85);

  Node* plane = addNode(doc, "Plane", Vec3(0, 0, 0), floor->id);
  plane->object->params.setNumber("length", 800);
  plane->object->params.setNumber("width", 800);
  Node* sphere = addNode(doc, "Sphere", Vec3(-70, 0, 40), paint->id);
  sphere->object->params.setNumber("radius", 40);
  sphere->object->params.setNumber("segments", 48);
  sphere->name = "PaintSphere";
  Node* box = addNode(doc, "Box", Vec3(55, 10, 0), wax->id);
  box->object->params.setNumber("length", 50);
  box->object->params.setNumber("width", 50);
  box->object->params.setNumber("height", 60);
  box->name = "WaxBox";
  Node* ball = addNode(doc, "Sphere", Vec3(5, -70, 18), gold->id);
  ball->object->params.setNumber("radius", 18);
  ball->object->params.setNumber("segments", 32);
  ball->name = "GoldBall";

  Node* key = addNode(doc, "VeraaLight", Vec3(-60, -120, 220));
  key->name = "KeyLight";
  key->object->params.setNumber("width", 150);
  key->object->params.setNumber("length", 150);
  key->object->params.setNumber("intensity", 6.0);
  key->transform.rotation = rotationLookingAlong(Vec3(0, 0, 30) - key->transform.position);
  Node* rim = addNode(doc, "VeraaLight", Vec3(170, 120, 110));
  rim->name = "RimLight";
  rim->object->params.setNumber("lightType", 2);  // sphere
  rim->object->params.setNumber("lightRadius", 12);
  rim->object->params.setNumber("colorMode", 1);
  rim->object->params.setNumber("temperature", 3200);
  rim->object->params.setNumber("intensity", 40.0);
  rim->object->params.setString("lightGroup", "Accent");

  Node* cam = addNode(doc, "VeraaPhysicalCamera", Vec3(0, -330, 110));
  cam->name = "ProductCam";
  cam->transform.rotation = rotationLookingAlong(Vec3(0, 0, 30) - cam->transform.position);
  cam->object->params.setNumber("exposure", 0);
  cam->object->params.setNumber("focalLength", 50);

  ParamBlock& rs = s.renderSettings;
  rs.setString("camera", "ProductCam");
  rs.setNumber("width", 480);
  rs.setNumber("height", 270);
  rs.setNumber("samples", 64);
  rs.setNumber("envIntensity", 0.6);
  for (const char* e : {"elemLighting", "elemGI", "elemSpecular", "elemReflection", "elemSSS", "elemNormals", "elemZDepth"})
    rs.setNumber(e, 1);
  rs.setNumber("lightMix", 1);
  doc->notifyStructureChanged();
  doc->notifyMaterialsChanged();
  doc->notifyRenderSettingsChanged();
  doc->endEdit();
}

void testRenderSetup(MainWindow& w) {
  RenderSetupDialog* rs = w.renderSetup();
  rs->show();
  QStringList tabs = rs->tabTitles();
  check(tabs.size() == 7 && tabs.contains("Sampler") && tabs.contains("Render Elements"), "render setup has its tabs");
  check(rs->showTab("Sampler"), "render setup shows the Sampler tab");
  grab(rs, "rs_sampler");
  rs->showTab("Common");
  grab(rs, "rs_common");
  rs->showTab("Render Elements");
  grab(rs, "rs_elements");
  rs->hide();

  EnvironmentDialog* env = environmentDialog(w);
  env->show();
  pump(50);
  grab(env, "environment");
  // Add Sun and Sky through the dialog's code path.
  SceneDocument* doc = w.document();
  doc->beginEdit("Sun and Sky");
  Node* sun = addSunAndSky(doc);
  doc->endEdit();
  check(sun && sun->object->type() == "SunPositioner", "Add Sun and Sky creates a sun positioner");
  check(doc->scene().renderSettings.getInt("envMode") == 3 && doc->scene().renderSettings.getInt("exposureControl") == 1,
        "Add Sun and Sky switches to the physical sky and exposure");
  doc->undoStack()->undo();
  check(doc->scene().nodes.empty() && doc->scene().renderSettings.getInt("envMode") != 3, "undo Add Sun and Sky");
  env->hide();

  PrintSizeDialog* ps = printSizeDialog(w);
  ps->setPaper("A4", false, 300);
  check(ps->pixelWidth() == 2480 && ps->pixelHeight() == 3508, "print size A4 at 300 dpi is 2480 x 3508");
  ps->setPaper("A3", true, 150);
  check(ps->pixelWidth() == 2480 && ps->pixelHeight() == 1754, "print size A3 landscape at 150 dpi");
  ps->apply();
  check(doc->scene().renderSettings.getInt("width") == 2480, "print size applies the render size");
  doc->undoStack()->undo();
  check(doc->scene().renderSettings.getInt("width") != 2480, "undo print size");
}

void testCreationTools(MainWindow& w) {
  SceneDocument* doc = w.document();
  EditorState* st = w.editorState();
  Scene& s = doc->scene();
  Viewport* top = w.viewports()->viewport(0);
  top->zoomExtents(false);
  auto drag = [&](const QString& type, const Vec3& a, const Vec3& b) {
    st->setCreateType(type);
    QPointF pa = testing::projectPoint(top, a), pb = testing::projectPoint(top, b);
    top->simulateMouse(QEvent::MouseButtonPress, pa, Qt::LeftButton, Qt::LeftButton);
    top->simulateMouse(QEvent::MouseMove, (pa + pb) * 0.5, Qt::NoButton, Qt::LeftButton);
    top->simulateMouse(QEvent::MouseMove, pb, Qt::NoButton, Qt::LeftButton);
    top->simulateMouse(QEvent::MouseButtonRelease, pb, Qt::LeftButton, Qt::NoButton);
    st->setCreateType({});
    return s.nodes.empty() ? nullptr : s.nodes.back().get();
  };

  Node* light = drag("VeraaLight", Vec3(-100, -50, 0), Vec3(100, 50, 0));
  check(light && light->object->type() == "VeraaLight", "VeraaLight created by dragging");
  if (light) {
    float wv = light->object->params.getFloat("width"), lv = light->object->params.getFloat("length");
    std::printf("      light %.1f x %.1f at z %.1f\n", wv, lv, light->transform.position.z);
    check(std::fabs(wv - 200.0f) < 15.0f && std::fabs(lv - 100.0f) < 15.0f, "plane light size follows the drag");
    check(std::fabs(light->transform.position.z - 250.0f) < 1.0f, "plane light is lifted to the ceiling");
  }
  Node* ies = drag("VeraaIES", Vec3(150, 0, 0), Vec3(150, 0, 0));
  check(ies && ies->object->type() == "VeraaIES", "VeraaIES created by clicking");
  check(ies && !ies->object->params.getString("iesFile").empty(), "VeraaIES gets a bundled IES file");
  Node* sun = drag("SunPositioner", Vec3(0, 0, 0), Vec3(0, 200, 0));
  check(sun && std::fabs(sun->object->params.getFloat("compassRadius") - 200.0f) < 15.0f,
        "Sun Positioner compass radius follows the drag");
  check(s.renderSettings.getInt("envMode") == 3, "creating a sun turns on the physical sky");
  Node* cam = drag("VeraaPhysicalCamera", Vec3(0, -300, 0), Vec3(0, 0, 0));
  check(cam && cam->camera() && cam->object->params.getFloat("focusDistance") > 250.0f,
        "physical camera aims and focuses on the drag target");
  Node* plane = drag("VeraaInfinitePlane", Vec3(30, 30, 0), Vec3(30, 30, 0));
  check(plane && plane->object->type() == "VeraaInfinitePlane", "infinite plane created by clicking");
  w.viewports()->zoomExtentsAll(false);
  pump(100);
  grab(&w, "gizmos");

  // Fur: select a box, pick VeraaFur.
  doc->beginEdit("box");
  Node* box = doc->createNode("Box");
  box->object->params.setNumber("length", 60);
  box->object->params.setNumber("width", 60);
  box->object->params.setNumber("height", 30);
  doc->notifyNodeChanged(box, ChangeAll);
  doc->endEdit();
  const uint32_t boxId = box->id;
  const std::string boxName = box->name;
  doc->setSelection({boxId});
  size_t before = s.nodes.size();
  st->setCreateType("VeraaFur");
  pump(50);
  check(s.nodes.size() == before + 1 && s.nodes.back()->object->type() == "VeraaFur" &&
            s.nodes.back()->object->params.getString("sourceNode") == boxName,
        "VeraaFur grows on the selected object");
  check(st->createType().isEmpty(), "fur creation leaves creation mode");

  // Proxy export replaces the box.
  QTemporaryDir tmp;
  QString vxp = tmp.filePath("box.vxp");
  QString err;
  uint32_t proxyId = exportProxy(doc, {boxId}, vxp, true, &err);
  Node* proxy = s.findNode(proxyId);
  check(proxyId && QFileInfo::exists(vxp), "export proxy writes a .vxp file");
  check(proxy && proxy->triMesh().triangleCount() == 12 && !s.findNode(boxId), "proxy replaces the exported object");
  doc->undoStack()->undo();
  check(s.findNode(boxId) && !s.findNode(proxyId), "undo export proxy");

  // Displacement rollout builds for the modifier.
  doc->setSelection({boxId});
  w.commandPanel()->addModifier("VeraaDisplacementMod");
  check(s.findNode(boxId) && !s.findNode(boxId)->modifiers.empty(), "displacement modifier added");
  ModifyRolloutFactory f = modifyRolloutsFor("VeraaDisplacementMod");
  check(static_cast<bool>(f), "displacement modifier has a map rollout");
  if (f) {
    ModifyPanelContext ctx{doc, st, boxId, 0};
    QWidget* rollout = f(ctx, nullptr);
    check(rollout != nullptr, "displacement rollout widget created");
    delete rollout;
  }
}

void testMaterialEditors(MainWindow& w) {
  SceneDocument* doc = w.document();
  Scene& s = doc->scene();
  doc->beginEdit("material");
  Material* m = makeMaterial(s, "Blend test", "VeraaBlendMtl");
  doc->notifyMaterialsChanged();
  doc->endEdit();
  MaterialEditor* me = w.materialEditor();
  me->show();
  me->selectMaterial(m->id);
  pump(100);
  check(me->currentMaterial() == m->id, "material editor shows the material");
  // Go into the first sub-material (the base).
  if (!m->subs.empty()) {
    me->navigate({MtlPathStep{MtlPathStep::Kind::Sub, 0, {}}});
    pump(50);
    check(me->levelEditor()->target().material != nullptr && me->levelEditor()->target().material != m,
          "navigate into a sub-material");
  }
  me->navigate({});
  pump(300);
  grab(me, "material_editor");

  SlateEditor* slate = slateEditor(w);
  slate->showMaterial(m->id);
  slate->show();
  pump(100);
  int nodes = slate->nodeCount();
  check(nodes >= 1, "Slate editor shows the material as nodes");
  doc->beginEdit("veraamtl");
  Material* v = makeMaterial(s, "Slate test", "VeraaMtl");
  doc->notifyMaterialsChanged();
  doc->endEdit();
  slate->showMaterial(v->id);
  pump(50);
  int n0 = slate->nodeCount();
  check(slate->addMapToRoot("diffuse", "Checker"), "Slate connects a new map to the material");
  pump(50);
  check(slate->nodeCount() == n0 + 1 && v->maps.count("diffuse") && v->maps["diffuse"].map, "Slate graph grew by one node");
  slate->addMapToRoot("bump", "Noise");
  pump(300);
  grab(slate, "slate");
  slate->hide();
  me->hide();
}

void testFrameBuffer(MainWindow& w) {
  SceneDocument* doc = w.document();
  buildRenderScene(doc);
  w.render();
  FrameBufferWindow* fb = w.frameBuffer();
  bool finished = fb->waitForFinish(180000);
  check(finished, "product scene render finishes");
  RenderStats st = fb->renderer().stats();
  std::printf("      %d passes in %.2f s, %.1f M samples/s\n", st.passes, st.seconds, st.samplesPerSecond * 1e-6);
  QStringList elems = fb->elementList();
  check(elems.contains("Lighting") && elems.contains("SSS") && elems.contains("ZDepth"), "requested render elements exist");
  QStringList groups = fb->lightGroups();
  check(groups.contains("Environment") && groups.contains("KeyLight") && groups.contains("Accent"),
        "Light Mix has environment, light and named groups");
  grab(fb, "vfb_beauty");
  QImage beauty = fb->image().copy();
  fb->setLightMixIntensity("Accent", 4.0f);
  pump(100);
  QImage boosted = fb->image().copy();
  check(boosted.size() == beauty.size() && boosted != beauty, "Light Mix changes the image");
  grab(fb, "vfb_lightmix");
  fb->setLightMixIntensity("Accent", 1.0f);
  fb->saveToHistory();
  check(fb->historySize() == 1, "image saved to history");
  fb->showElement("SSS");
  pump(100);
  grab(fb, "vfb_sss");
  fb->showElement(QString());
  QTemporaryDir tmp;
  bool allSaved = true;
  for (const char* ext : {"png", "jpg", "tif", "exr", "hdr", "bmp"}) {
    QString path = tmp.filePath(QString("out.%1").arg(ext));
    bool ok = fb->saveImageTo(path, QString(ext) == "png" || QString(ext) == "exr") && QFileInfo(path).size() > 1000;
    if (!ok) std::printf("      could not save %s\n", ext);
    allSaved &= ok;
  }
  check(allSaved, "frame buffer saves PNG, JPG, TIFF, EXR, HDR and BMP");
  fb->hide();

  // Batch render two small jobs.
  BatchRenderDialog* batch = batchRenderDialog(w);
  QString out1 = tmp.filePath("batch/view1.png"), out2 = tmp.filePath("batch/view2.exr");
  BatchJob j1;
  j1.name = "Small";
  j1.width = 160;
  j1.height = 90;
  j1.samples = 8;
  j1.output = out1.toStdString();
  j1.saveElements = true;
  BatchJob j2 = j1;
  j2.name = "Tiny EXR";
  j2.width = 96;
  j2.height = 54;
  j2.output = out2.toStdString();
  j2.saveElements = false;
  batch->addJob(j1);
  batch->addJob(j2);
  check(batchJobsFromScene(doc->scene()).size() == 2, "batch jobs are stored in the scene");
  batch->show();
  batch->startBatch();
  check(batch->waitForFinish(120000), "batch render finishes");
  check(batch->jobStatus(0).startsWith("Done") && batch->jobStatus(1).startsWith("Done"), "batch jobs succeed");
  check(QFileInfo::exists(out1) && QFileInfo::exists(out2), "batch render writes the images");
  check(QFileInfo::exists(tmp.filePath("batch/view1_Lighting.png")), "batch render writes render elements");
  grab(batch, "batch");
  batch->hide();
}

void testRendering(MainWindow& w) {
  testRenderSetup(w);
  w.document()->newScene();
  testCreationTools(w);
  w.document()->newScene();
  testMaterialEditors(w);
  w.document()->newScene();
  testFrameBuffer(w);
}

}  // namespace

VERAA_SELF_TEST(rendering, testRendering);

}  // namespace veraa
