// Scripted runs used for automated checks and screenshots:
//   Veraa --script demo --screenshot out.png   (builds a demo scene and saves screenshots)
//   Veraa --script selftest                     (drives creation, gizmos, undo, files; exit code = failures)
#pragma once

#include <QString>

namespace veraa {

class MainWindow;

// Builds a small showcase scene (floor, primitives, materials, lights, camera).
void buildDemoScene(MainWindow& w);

int runScript(MainWindow& w, const QString& script, const QString& screenshotPath);

}  // namespace veraa
