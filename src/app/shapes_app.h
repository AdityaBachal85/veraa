// Internal helpers shared by the shapes & construction app files (creation tools, spline
// editing, compound/AEC rollouts and their self test).
#pragma once

#include <QString>

#include <functional>

#include "compound.h"
#include "extension.h"
#include "scene.h"

class QWidget;

namespace veraa {

class SceneDocument;
class EditorState;
class Viewport;

namespace shapesapp {

// The shape a node's stack produces (object plus modifiers), in object space. Empty when the
// result is not a shape. upTo limits the stack: only modifiers below this index are applied
// (-1 = the whole stack).
BezierShape evaluatedShape(const Node& n, int upTo = -1);

// Captures a picked node as a compound operand (role decides shape/mesh and space).
CompoundOperand captureOperand(const Node& picked, const Node& compoundNode, const CompoundRole& role);

// Asks a yes/no question such as "Close spline?" (a Line click on its first knot).
// Replaceable so the self test can answer without a modal dialog.
extern std::function<bool(QWidget* parent, const QString& question)> askYesNo;

// Recomputes a Section node's shape from the visible geometry it cuts.
void updateSection(SceneDocument* doc, Node* section);

// Picks a node in a viewport and calls back with it; Esc or right-click ends picking.
// accept filters candidates; the tool keeps picking when multiple is true.
std::shared_ptr<ViewportTool> makePickTool(const QString& prompt, std::function<bool(const Node&)> accept,
                                           std::function<void(Node&)> picked, bool multiple);

// Hit of a pixel ray with the XY plane of a node (its object space), returned in object space.
bool hitNodePlane(const Viewport& vp, const Node& n, float px, float py, float z, Vec3& local);

// Spline sub-object editing session (Editable Spline / Edit Spline).
QWidget* buildSplineEditRollout(const ModifyPanelContext& ctx, QWidget* parent);
// Sets the sub-object level of the active spline editing session (0 object, 1 vertex,
// 2 segment, 3 spline). Returns false when no session is active.
bool setSplineSubObjectLevel(EditorState* state, int level);

}  // namespace shapesapp
}  // namespace veraa
