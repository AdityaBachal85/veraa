// "Veraa --script bench": times the operations that matter on big imported scenes (see
// docs/PERFORMANCE.md). The file to import comes from the VERAA_BENCH_FILE environment variable;
// results are printed as "BENCH <step> <ms> ms  rss <MB>".
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QUndoStack>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "glscene.h"
#include "io_formats.h"
#include "mainwindow.h"
#include "sceneexplorer.h"
#include "selftest.h"
#include "viewport.h"
#include "viewportpanel.h"

namespace veraa {

namespace {

long rssMb() {
#ifdef __linux__
  std::ifstream f("/proc/self/status");
  std::string line;
  while (std::getline(f, line))
    if (line.rfind("VmRSS:", 0) == 0) return std::atol(line.c_str() + 6) / 1024;
#endif
  return 0;
}

// Delivers every pending event (queued rebuilds, repaints) so a step's cost includes the UI work it
// triggers.
void settle() {
  for (int i = 0; i < 4; ++i) QApplication::processEvents(QEventLoop::AllEvents);
}

void report(const char* step, qint64 ms) {
  std::printf("BENCH %-30s %8lld ms  rss %6ld MB\n", step, static_cast<long long>(ms), rssMb());
  std::fflush(stdout);
}

// Runs `op`, then delivers the events it caused; prints both times. The event part includes
// viewport repaints, which are slow with software OpenGL.
template <typename F>
void timed(const char* step, F op) {
  QElapsedTimer t;
  t.start();
  op();
  qint64 a = t.elapsed();
  settle();
  qint64 b = t.elapsed();
  std::printf("BENCH %-30s %8lld ms  (op %lld ms + events %lld ms)  rss %6ld MB\n", step, static_cast<long long>(b),
              static_cast<long long>(a), static_cast<long long>(b - a), rssMb());
  std::fflush(stdout);
}

// Draws every visible viewport once (synchronously) and returns the CPU time spent inside
// paintGL, summed over the viewports.
double drawAll(MainWindow& w) {
  double cpu = 0;
  ViewportPanel* vps = w.viewports();
  for (int i = 0; i < vps->count(); ++i) {
    Viewport* vp = vps->viewport(i);
    if (!vp->isVisible()) continue;
    vp->grabFramebuffer();
    cpu += vp->lastPaintCpuMs();
  }
  return cpu;
}

}  // namespace

int runBench(MainWindow& w) {
  QString file = QString::fromLocal8Bit(qgetenv("VERAA_BENCH_FILE"));
  if (file.isEmpty() || !QFileInfo::exists(file)) {
    std::printf("Set VERAA_BENCH_FILE to the scene to import\n");
    return 1;
  }
  SceneDocument* doc = w.document();
  Scene& scene = doc->scene();
  QElapsedTimer t;
  // With software OpenGL, rasterizing millions of triangles dominates every redraw; skipping the
  // mesh draw calls measures the application's own CPU work (uploads, culling, state changes).
  bool noDraw = qEnvironmentVariableIntValue("VERAA_BENCH_NODRAW") != 0;
  GlScene::setSkipMeshDraws(noDraw);
  std::printf("BENCH   mesh draw calls %s\n", noDraw ? "skipped (VERAA_BENCH_NODRAW)" : "on");
  settle();
  report("start", 0);

  // Import (parse + scene build + undo step + UI refresh, including the first redraw).
  QString err;
  QVector<uint32_t> ids;
  bool imported = false;
  GlScene::resetStats();
  timed("import", [&] { imported = w.importPath(file, &ids, &err); });
  if (!imported) {
    std::printf("Import failed: %s\n", qPrintable(err));
    return 1;
  }
  size_t tris = 0;
  for (const auto& n : scene.nodes) tris += n->triMesh().triangleCount();
  std::printf("BENCH   objects %zu, triangles %zu, uploaded %.1f MB\n", scene.nodes.size(), tris,
              GlScene::uploadedBytes() / 1048576.0);

  w.viewports()->zoomExtentsAll(false);
  GlScene::resetStats();
  t.start();
  double cpu = drawAll(w);
  report("first draw (all views)", t.elapsed());
  std::printf("BENCH   paintGL CPU %.1f ms, uploaded %.1f MB\n", cpu, GlScene::uploadedBytes() / 1048576.0);
  GlScene::resetStats();
  t.start();
  cpu = drawAll(w);
  report("second draw (all views)", t.elapsed());
  std::printf("BENCH   paintGL CPU %.1f ms, uploaded %.1f MB\n", cpu, GlScene::uploadedBytes() / 1048576.0);

  // Navigation frame in the perspective view: orbit a little and redraw.
  Viewport* persp = w.viewports()->viewport(3);
  persp->setProperty("navigating", true);
  GlScene::resetStats();
  t.start();
  double navCpu = 0;
  // VERAA_BENCH_ORBIT_FRAMES repeats the orbit (for profiling the navigation frame).
  int orbitFrames = std::max(5, qEnvironmentVariableIntValue("VERAA_BENCH_ORBIT_FRAMES"));
  for (int i = 0; i < orbitFrames; ++i) {
    persp->camera().yawDeg += 2.0f;
    persp->grabFramebuffer();
    navCpu += persp->lastPaintCpuMs();
  }
  persp->setProperty("navigating", false);
  report("orbit frame (persp, avg)", t.elapsed() / orbitFrames);
  std::printf("BENCH   paintGL CPU %.1f ms/frame, uploaded %.1f MB; objects drawn %zu, culled %zu, as boxes %zu\n",
              navCpu / orbitFrames, GlScene::uploadedBytes() / 1048576.0, persp->lastDrawnObjects(),
              persp->lastCulledObjects(), persp->lastBoxedObjects());

  // Select one object in the middle of the list.
  uint32_t target = 0;
  for (size_t i = scene.nodes.size() / 2; i < scene.nodes.size() && !target; ++i)
    if (scene.nodes[i]->kind() == ObjectKind::Geometry && !scene.nodes[i]->triMesh().empty()) target = scene.nodes[i]->id;
  timed("select one object", [&] { doc->setSelection({target}); });

  // Pick by clicking (ray cast through the scene) at the selected object's center.
  if (Node* n = scene.findNode(target)) {
    QPointF p = testing::projectPoint(persp, n->worldBounds().center());
    t.start();
    uint32_t hit = persp->pickNode(static_cast<float>(p.x()), static_cast<float>(p.y()));
    report("click pick (first)", t.elapsed());
    t.start();
    for (int i = 0; i < 5; ++i) hit = persp->pickNode(static_cast<float>(p.x()), static_cast<float>(p.y()));
    std::printf("BENCH click pick (next, avg)          %.2f ms\n", t.nsecsElapsed() / 5e6);
    std::printf("BENCH   picked %u (selected %u)\n", hit, target);
  }

  // Move it like a gizmo drag: one undo step with 10 intermediate updates, redrawing each time.
  GlScene::resetStats();
  t.start();
  double dragPaint = 0;
  doc->beginEdit("Move");
  for (int i = 0; i < 10; ++i) {
    if (Node* n = scene.findNode(target)) n->transform.position.x += 1.0f;
    doc->notifyNodesChanged({target}, ChangeTransform);
    persp->grabFramebuffer();
    dragPaint += persp->lastPaintCpuMs();
  }
  report("move: 10 drag steps", t.elapsed());
  std::printf("BENCH   paintGL CPU %.1f ms/step\n", dragPaint / 10);
  timed("move: commit (endEdit)", [&] { doc->endEdit(); });
  std::printf("BENCH   uploaded %.1f MB during the drag\n", GlScene::uploadedBytes() / 1048576.0);

  QUndoStack* undo = doc->undoStack();
  timed("undo move", [&] { undo->undo(); });
  timed("redo move", [&] { undo->redo(); });
  GlScene::resetStats();
  drawAll(w);
  std::printf("BENCH   redraw after undo/redo uploaded %.1f MB\n", GlScene::uploadedBytes() / 1048576.0);

  // Scene Explorer: full repopulation.
  timed("structure change (explorers)", [&] { doc->notifyStructureChanged(); });

  // Save and load.
  QString out = QDir::temp().filePath("veraa_bench.veraa");
  if (const char* d = std::getenv("VERAA_BENCH_OUT")) out = QString::fromLocal8Bit(d);
  t.start();
  if (!doc->save(out, &err)) std::printf("Save failed: %s\n", qPrintable(err));
  report("save .veraa", t.elapsed());
  std::printf("BENCH   file size %.1f MB\n", QFileInfo(out).size() / 1048576.0);
  doc->newScene();
  settle();
  timed("load .veraa", [&] {
    if (!w.openFile(out)) std::printf("Load failed\n");
  });
  t.start();
  drawAll(w);
  report("draw after load", t.elapsed());
  QFile::remove(out);
  return 0;
}

}  // namespace veraa
