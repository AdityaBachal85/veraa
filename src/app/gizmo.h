// Move / Rotate / Scale transform gizmos: drawing, hit testing and drag math.
#pragma once

#include "glscene.h"
#include "viewcamera.h"

namespace veraa {

enum class TransformTool { Select, Move, Rotate, Scale };

// Handles double as axis constraints (F5-F8 / toolbar).
enum class GizmoHandle { None, X, Y, Z, XY, YZ, ZX, Center };

int handleAxisMask(GizmoHandle h);  // bit 0 = X, 1 = Y, 2 = Z
QString handleName(GizmoHandle h);

class Gizmo {
 public:
  TransformTool tool = TransformTool::Move;
  Vec3 origin;
  Vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  float size = 50.0f;  // world units, recomputed for a constant screen size

  void setup(const ViewCamera& cam, TransformTool t, const Vec3& o, const Vec3 ax[3], float pixels = 85.0f);
  GizmoHandle hitTest(const ViewCamera& cam, float px, float py) const;
  void draw(LineBatch& batch, const ViewCamera& cam, GizmoHandle hot) const;
  // Screen positions of the axis tips for labels (returns false if hidden).
  bool axisTip(const ViewCamera& cam, int axis, float& px, float& py) const;

 private:
  bool axisVisible(const ViewCamera& cam, int a) const;
  bool planeVisible(const ViewCamera& cam, int normalAxis) const;
};

// Tracks one drag of a gizmo handle (or an axis-constrained drag of the object itself).
class GizmoDrag {
 public:
  void begin(const ViewCamera& cam, const Gizmo& g, GizmoHandle h, float px, float py);
  // Move: world-space translation since the drag started.
  Vec3 translation(const ViewCamera& cam, float px, float py) const;
  // Rotate: rotation axis and accumulated angle (radians).
  float rotation(const ViewCamera& cam, float px, float py, Vec3& axis);
  // Scale: per-axis factors (in gizmo axes; 1 for unconstrained axes).
  Vec3 scaling(const ViewCamera& cam, float px, float py) const;
  GizmoHandle handle() const { return handle_; }

 private:
  float axisParam(const Ray& r, const Vec3& axis) const;
  bool planeHit(const Ray& r, const Vec3& n, Vec3& p) const;
  GizmoHandle handle_ = GizmoHandle::None;
  Gizmo g_;
  Vec3 viewFwd_;
  float startX_ = 0, startY_ = 0;
  float s0_ = 0;
  Vec3 p0_;
  bool p0Valid_ = false;
  float lastAngle_ = 0, accumAngle_ = 0;
  bool edgeOn_ = false;
};

}  // namespace veraa
