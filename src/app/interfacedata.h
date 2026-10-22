// Interface data saved with the scene (Scene::extensions["interface"]): named selection sets,
// scene states, file properties, external references (XRef scenes) and the working pivot.
// Every setter that takes a SceneDocument is one undo step.
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>

#include "document.h"

namespace veraa {

// ---- Named selection sets ----
QStringList selectionSetNames(const Scene& s);
QVector<uint32_t> selectionSetMembers(const Scene& s, const QString& name);
void setSelectionSet(SceneDocument* doc, const QString& name, const QVector<uint32_t>& ids);
void removeSelectionSet(SceneDocument* doc, const QString& name);
void renameSelectionSet(SceneDocument* doc, const QString& from, const QString& to);

// ---- Scene states (Tools > Manage Scene States) ----
// Parts captured by a scene state.
enum SceneStatePart : int {
  StateHidden = 1,
  StateFrozen = 2,
  StateTransforms = 4,
  StateMaterials = 8,
  StateLayers = 16,
  StateDisplay = 32,  // wire color, see-through, display as box
  StateAllParts = 63,
};
QStringList sceneStateNames(const Scene& s);
int sceneStateParts(const Scene& s, const QString& name);
void saveSceneState(SceneDocument* doc, const QString& name, int parts);
bool restoreSceneState(SceneDocument* doc, const QString& name);
void deleteSceneState(SceneDocument* doc, const QString& name);

// ---- File properties (File > File Properties) ----
// Summary keys: title, subject, author, manager, company, category, keywords, comments;
// "custom" holds an object of user-defined name/value pairs.
json fileProperties(const Scene& s);
void setFileProperties(SceneDocument* doc, const json& props);

// ---- XRef scenes (File > Reference > XRef Scene) ----
struct XRefEntry {
  QString path;
  bool enabled = true;
  QVector<uint32_t> nodeIds;  // nodes merged from the file (read-only, frozen)
};
QVector<XRefEntry> xrefScenes(const Scene& s);
void setXRefScenes(Scene& s, const QVector<XRefEntry>& list);

// ---- Transform locks (Hierarchy > Link Info > Locks) ----
// Bits: 0-2 move X/Y/Z, 3-5 rotate X/Y/Z, 6-8 scale X/Y/Z (local axes).
int transformLocks(const Scene& s, uint32_t nodeId);
void setTransformLocks(SceneDocument* doc, uint32_t nodeId, int mask);
// Puts locked components of `t` back to their values in `before`.
void applyTransformLocks(const Scene& s, uint32_t nodeId, const Transform& before, Transform& t);

// ---- Working pivot (Hierarchy > Working Pivot) ----
bool hasWorkingPivot(const Scene& s);
Transform workingPivotTransform(const Scene& s);
void setWorkingPivot(SceneDocument* doc, const Transform* t);  // nullptr clears it

}  // namespace veraa
