// Scripted runs used for automated checks and screenshots:
//   Veraa --script demo --screenshot out.png   (builds a demo scene and saves screenshots)
//   Veraa --script selftest                     (drives creation, gizmos, undo, files; exit code = failures)
#pragma once

#include <QPointF>
#include <QString>

#include "vmath.h"

class QWidget;

namespace veraa {

class MainWindow;
class Viewport;

// Builds a small showcase scene (floor, primitives, materials, lights, camera).
void buildDemoScene(MainWindow& w);

int runScript(MainWindow& w, const QString& script, const QString& screenshotPath);
// Times import, drawing, selection, transforms, undo and files on VERAA_BENCH_FILE (bench.cpp).
int runBench(MainWindow& w);

// Helpers for feature module UI tests. A module registers a test with
//   static void testMyFeature(MainWindow& w) { testing::check(..., "what"); }
//   VERAA_SELF_TEST(myFeature, testMyFeature);
// "Veraa --script selftest" runs every module test (each on a fresh scene);
// "Veraa --script myFeature" runs just one.
namespace testing {
void check(bool ok, const char* what);
int failures();
void pump(int ms = 50);
QPointF projectPoint(Viewport* vp, const Vec3& p);
QString sibling(const QString& path, const QString& suffix);
bool saveGrab(QWidget* w, const QString& path);
using ModuleTest = void (*)(MainWindow& w);
struct SelfTestRegistrar {
  SelfTestRegistrar(const char* name, ModuleTest fn);
};
}  // namespace testing

}  // namespace veraa

#define VERAA_SELF_TEST(name, fn) static ::veraa::testing::SelfTestRegistrar veraaSelfTest_##name(#name, fn)
