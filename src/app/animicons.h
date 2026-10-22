// Drawn icons for the animation controls and graph editors.
#pragma once

#include <QIcon>

namespace veraa::animicons {

QIcon goStart();
QIcon goEnd();
QIcon prevFrame();
QIcon nextFrame();
QIcon play();
QIcon stop();
QIcon key();         // Set Keys
QIcon keyFilters();
QIcon keyMode();
QIcon timeConfig();
QIcon motion();      // Motion command panel tab
QIcon curveEditor();
QIcon dopeSheet();
QIcon schematic();
QIcon frameAll();
QIcon addKey();
QIcon moveKeys();
QIcon tangent(int type);  // TangentType order: Auto, Smooth, Linear, Step, Fast, Slow, Custom
QIcon outOfRange(int type);

}  // namespace veraa::animicons
