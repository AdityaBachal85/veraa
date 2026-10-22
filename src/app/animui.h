// Shared helpers for the animation UI (Motion panel, menus, graph editors, rigging tools).
#pragma once

#include <QString>

#include <cstdint>
#include <functional>
#include <string>

#include "animation.h"

class QWidget;

namespace veraa {

class AnimController;
class MainWindow;
class Node;

// Starts picking a node in the viewports. filter may be empty (any node).
void startPickNode(MainWindow& w, const QString& prompt, std::function<bool(const Node&)> filter,
                   std::function<void(uint32_t)> done);
// Modal list of scene nodes (by name). Returns 0 when cancelled.
uint32_t pickNodeByName(QWidget* parent, const Scene& scene, const QString& title, std::function<bool(const Node&)> filter);

// Names shown in the controller tree, e.g. "Bezier Float", "Path Constraint".
QString controllerName(const Track* t);
QString constraintTitle(ConstraintType t);

// Adds a constraint of the given type to `nodeId` targeting `targetId` (undoable).
void addConstraintTo(AnimController* c, uint32_t nodeId, ConstraintType type, uint32_t targetId, bool keepOffset = false);
// Starts the pick-a-target workflow for the selection (Animation > Constraints).
void beginConstraintPick(MainWindow& w, ConstraintType type);

// Assigns a float controller type to tracks (undoable). Existing keys are kept where possible.
void assignFloatController(AnimController* c, const std::vector<std::string>& paths, ControllerType type);
// Switches a node's rotation between Euler XYZ (0) and TCB rotation (1), converting keys.
void setRotationController(AnimController* c, uint32_t nodeId, int controller);

// Creates an IK chain from startBone to endBone (solver 0 HI, 1 Limb). Returns the chain node id.
uint32_t createIkChain(AnimController* c, uint32_t startBone, uint32_t endBone, int solver);

// Trajectories for the selection (Motion panel > Trajectories).
void setShowSelectionTrajectories(bool on);
bool showSelectionTrajectories();

// Graph editors (mode 0 Curve Editor, 1 Dope Sheet).
void openTrackView(MainWindow& w, int mode);
void openSchematicView(MainWindow& w);

// Dialog editing a Noise / Expression / List controller of a track (undoable).
void editControllerProperties(AnimController* c, const std::string& path, QWidget* parent);

}  // namespace veraa
