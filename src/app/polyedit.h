// Sub-object editing for Editable Poly objects and the Edit Poly / Poly Select modifiers:
// the edit target (which mesh and selection), and the viewport tool that picks, region-selects
// and transforms vertices, edges, borders, polygons and elements.
#pragma once

#include <QObject>
#include <QPoint>
#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "extension.h"
#include "gizmo.h"
#include "polyops.h"

class QMouseEvent;
class QPainter;

namespace veraa {

class EditablePolyObject;
class EditorState;
class Node;
class SceneDocument;
class SubObjectModifier;
class Viewport;

// Selection options shared by every sub-object tool (Selection rollout check boxes).
struct PolyToolSettings {
  bool byVertex = false;
  bool ignoreBackfacing = false;
  bool byAngle = false;
  float angle = 45.0f;
  static PolyToolSettings& get();
};

// Which poly is being edited: the Editable Poly base object of a node (stackIndex -1) or a
// sub-object modifier in its stack. Holds ids only, so it stays valid across undo/redo.
class PolyTarget {
 public:
  PolyTarget() = default;
  PolyTarget(SceneDocument* doc, EditorState* state, uint32_t nodeId, int stackIndex)
      : doc_(doc), state_(state), nodeId_(nodeId), stackIndex_(stackIndex) {}

  SceneDocument* doc() const { return doc_; }
  EditorState* state() const { return state_; }
  uint32_t nodeId() const { return nodeId_; }
  int stackIndex() const { return stackIndex_; }
  bool sameAs(const PolyTarget& o) const { return nodeId_ == o.nodeId_ && stackIndex_ == o.stackIndex_; }

  Node* node() const;
  EditablePolyObject* editablePoly() const;
  SubObjectModifier* modifier() const;
  bool valid() const { return editablePoly() || modifier(); }
  bool canEdit() const;  // false for Poly Select / Mesh Select (selection only)

  // Mesh at this point of the stack, in object space (null when invalid).
  const PolyMesh* mesh() const;
  PolySelection selection() const;
  int level() const { return selection().level; }
  Mat4 objectToWorld() const;

  // Undoable selection change (merged into an open live operation).
  void setSelection(const PolySelection& s, const QString& label);
  // Sub-object level change (not an undo step of its own, like 3ds Max).
  void setLevel(int level);
  // Soft selection settings (spinner drags merge into one undo step).
  void setSoftSelection(const SoftSelection& soft);
  // Runs an editing operation as one undo step; failures are shown in the prompt line.
  bool run(const json& op, const QString& label);

  // Interactive operations (caddy settings, transform drags): begin opens an undo step, update
  // re-applies the operation from the state at begin, end commits or cancels.
  void beginLive(const QString& label);
  bool updateLive(const json& op);
  void endLive(bool commit);
  bool live() const;

  // Tells views the geometry changed (and repaints).
  void notify() const;
  void showMessage(const QString& text) const;

 private:
  void writeSelection(const PolySelection& s) const;

  SceneDocument* doc_ = nullptr;
  EditorState* state_ = nullptr;
  uint32_t nodeId_ = 0;
  int stackIndex_ = -1;
};

// Edit Geometry > Slice Plane: a plane in the edited object's space, shown in the viewports.
struct SlicePlaneState {
  bool active = false;
  uint32_t nodeId = 0;
  int stackIndex = -1;
  Vec3 center;    // object space
  Vec3 rotation;  // degrees (Euler XYZ); the plane normal is the rotated +Z axis
  float size = 100.0f;
  Vec3 normal() const { return normalize(Mat4::fromEulerDeg(rotation).transformDir(Vec3(0, 0, 1))); }
  static SlicePlaneState& get();
};

// Last operation, for Edit Geometry > Repeat Last.
struct RepeatLast {
  json op;
  QString label;
  static RepeatLast& get();
};

// Picks for the interactive modes of the sub-object tool.
enum class PolyPickMode {
  None,
  TargetWeld,    // vertex/edge level: click source, then target
  InsertVertex,  // click an edge (or inside a polygon at polygon level)
  Create,        // polygon level: click vertices to build a polygon
  Cut,           // click points on edges/vertices; consecutive points are connected
  QuickSlice,    // click two points to slice along the view
  SwiftLoop,     // click an edge to insert a loop through its ring
  HingePick,     // click the hinge edge for Hinge From Edge
  Attach,        // click another object to attach it
};

// Viewport tool active while an Editable Poly / Edit Poly / Poly Select is at a sub-object level.
class PolyEditTool : public ViewportTool {
 public:
  explicit PolyEditTool(const PolyTarget& target) : target_(target) {}

  const PolyTarget& target() const { return target_; }
  void setPickMode(PolyPickMode m);
  PolyPickMode pickMode() const { return mode_; }
  // Called when the hinge edge is picked (HingePick mode).
  std::function<void(int a, int b)> onHingePicked;
  // Called when a pick mode ends (so buttons can pop up).
  std::function<void()> onPickModeEnded;

  bool mousePress(Viewport& vp, QMouseEvent* e) override;
  bool mouseMove(Viewport& vp, QMouseEvent* e) override;
  bool mouseRelease(Viewport& vp, QMouseEvent* e) override;
  bool mouseDoubleClick(Viewport& vp, QMouseEvent* e) override;
  bool keyPress(Viewport& vp, QKeyEvent* e) override;
  void drawLines(Viewport& vp, LineBatch& lines) override;
  void drawOverlay(Viewport& vp, QPainter& p) override;
  bool cancel() override;
  bool deleteSelection() override;
  QString prompt() const override;

  // ---- Picking helpers (widget pixels) ----
  struct Hit {
    int vert = -1;
    uint64_t edge = 0;
    bool hasEdge = false;
    float edgeT = 0.0f;  // along edge from edgeKeyA to edgeKeyB
    int face = -1;
    Vec3 world;          // picked point (world space)
  };
  Hit pick(Viewport& vp, float px, float py, int level) const;
  int pickVertex(Viewport& vp, float px, float py, float radius = 9.0f) const;
  bool pickEdge(Viewport& vp, float px, float py, uint64_t& edge, float& t, float radius = 7.0f) const;
  int pickFace(Viewport& vp, float px, float py, Vec3* worldHit = nullptr) const;
  // Applies a click on a sub-object to the selection (mods: Ctrl adds/toggles, Alt removes).
  void clickSelect(Viewport& vp, float px, float py, Qt::KeyboardModifiers mods, bool doubleClick);
  void regionSelect(Viewport& vp, const QRect& r, Qt::KeyboardModifiers mods);
  Vec3 selectionCenterWorld() const;

 private:
  bool facesFront(Viewport& vp, int face) const;
  bool vertFront(Viewport& vp, int v) const;
  void beginTransform(Viewport& vp, GizmoHandle h, float px, float py);
  void updateTransform(Viewport& vp, float px, float py);
  bool pickModePress(Viewport& vp, QMouseEvent* e);
  void finishCreate();
  void finishMode();
  void setupGizmo(Viewport& vp);
  bool gizmoShown(Viewport& vp) const;

  PolyTarget target_;
  PolyPickMode mode_ = PolyPickMode::None;
  Gizmo gizmo_;
  GizmoDrag gizmoDrag_;
  GizmoHandle hot_ = GizmoHandle::None;
  bool transforming_ = false;
  Vec3 dragCenter_;
  Vec3 dragValue_;
  bool pressed_ = false;
  bool region_ = false;
  QPoint pressPos_, curPos_;
  Qt::KeyboardModifiers pressMods_;
  // Pick-mode state.
  int pickVert_ = -1;
  uint64_t pickEdgeKey_ = 0;
  std::vector<int> createVerts_;
  Hit cutFrom_;
  bool cutActive_ = false;
  bool sliceStarted_ = false;
  Vec3 sliceA_;
  QPoint sliceAPx_;
  uint64_t hoverEdge_ = 0;
  float hoverT_ = 0.5f;
  bool hoverValid_ = false;
};

// Drops a running live preview without writing anything (the scene was replaced, e.g. by undo).
void abandonPolyLive();
// Cancels an open operation settings caddy; returns true if one was open (polypanel.cpp).
bool cancelPolyCaddy();

// Draws a transform gizmo with QPainter (on top of everything in the viewport).
void drawGizmoOverlay(QPainter& p, const ViewCamera& cam, const Gizmo& gizmo, GizmoHandle hot);

// Finds the sub-object tool currently installed for this target (or null).
std::shared_ptr<PolyEditTool> activePolyTool(EditorState* state);

}  // namespace veraa
