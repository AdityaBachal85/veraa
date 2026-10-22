// Viewport navigation modes from the navigation controls (bottom right of the window): Zoom,
// Zoom All, Field-of-View, Zoom Region, Pan, Orbit, Orbit Selected and Orbit SubObject. Each is a
// ViewportTool: drag with the left button, right-click or Esc to stop.
#pragma once

#include <QString>

#include <memory>

namespace veraa {

class ViewportPanel;
class ViewportTool;

enum class NavMode { Zoom, ZoomAll, Fov, ZoomRegion, Pan, Orbit, OrbitSelected, OrbitSubObject };

std::shared_ptr<ViewportTool> makeNavTool(NavMode mode, ViewportPanel* panel);
bool isNavTool(const ViewportTool* tool);
QString navToolPrompt(NavMode mode);

}  // namespace veraa
