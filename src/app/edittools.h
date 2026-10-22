// Viewport tools used by the main toolbar and menus: Select and Place, Measure Distance,
// Select and Manipulate, object picking (Pick coordinate system, Align target, ...), Select and
// Link, and a home-grid point snapper.
#pragma once

#include <QString>

#include <cstdint>
#include <functional>
#include <memory>

namespace veraa {

class EditorState;
class Node;
class SceneDocument;
class ViewportTool;

std::shared_ptr<ViewportTool> makePlacementTool(SceneDocument* doc, EditorState* state);
std::shared_ptr<ViewportTool> makeMeasureTool(SceneDocument* doc, EditorState* state);
std::shared_ptr<ViewportTool> makeManipulateTool(SceneDocument* doc, EditorState* state);
std::shared_ptr<ViewportTool> makeLinkTool(SceneDocument* doc, EditorState* state);
// Calls `picked` with the clicked object's id. Right-click or Esc ends the tool.
std::shared_ptr<ViewportTool> makePickObjectTool(SceneDocument* doc, const QString& prompt,
                                                 std::function<void(uint32_t)> picked,
                                                 std::function<bool(const Node&)> filter = {});
// Snaps points to the home grid (spacing from Customize > Preferences / grid settings).
void installGridSnapper(EditorState* state, bool on);
double homeGridSpacing();

}  // namespace veraa
