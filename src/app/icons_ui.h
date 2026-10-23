// Icons for the main toolbar, axis toolbar, navigation controls, status bar and command panel
// tabs (drawn in code like icons.h).
#pragma once

#include <QIcon>

namespace veraa::icons {

QIcon selectByName();
QIcon place();
QIcon scaleNonUniform();
QIcon squash();
QIcon region(int shape);  // RegionShape order: rectangle, circle, fence, lasso, paint
QIcon windowCrossing(bool window);
QIcon lock();
QIcon axisCycle();
QIcon frameWindow();
QIcon zoomExtentsAll();
QIcon sceneExplorer();
QIcon layerExplorer();
QIcon mirror();
QIcon array();
QIcon align();
QIcon snap(int dims);  // 2, 25 (2.5D) or 3
QIcon angleSnap();
QIcon percentSnap();
QIcon spinnerSnap();
QIcon curveEditor();
QIcon schematic();
QIcon hierarchy();
QIcon display();
QIcon utilities();
QIcon link();
QIcon unlink();
QIcon bindSpaceWarp();
QIcon manipulate();
QIcon keyboardOverride();
QIcon namedSets();
QIcon pivotCenter(int mode);  // PivotCenter order: pivot point, selection, coordinate
QIcon isolate();
QIcon zoom();
QIcon zoomAll();
QIcon zoomRegion();
QIcon fieldOfView();
QIcon pan();
QIcon orbit(int mode);  // 0 orbit, 1 orbit selected, 2 orbit sub-object
QIcon home();
QIcon script();
QIcon absoluteMode(bool absolute);
QIcon adaptive();
// Layout additions (uilayout.cpp, status bar, track bar).
QIcon snapAxisConstraint();
QIcon slateEditor();
QIcon activeShade();
QIcon renderIterative();
QIcon ribbon();
QIcon safeScript();
QIcon timeTag();
QIcon miniCurveEditor();
QIcon tangent(int kind);  // 0 auto, 1 linear, 2 step, 3 smooth

}  // namespace veraa::icons
