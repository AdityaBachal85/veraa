// One 3D viewport (Top / Front / Left / Perspective / ...), drawn with OpenGL 3.3.
#pragma once

#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLWidget>
#include <QPoint>

#include <memory>
#include <vector>

#include "document.h"
#include "editorstate.h"
#include "gizmo.h"
#include "glscene.h"
#include "viewcamera.h"

namespace veraa {

enum class ShadingMode { Wireframe, Shaded };

class Viewport : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core {
  Q_OBJECT
 public:
  Viewport(SceneDocument* doc, EditorState* state, ViewType type, QWidget* parent = nullptr);
  ~Viewport() override;

  ViewCamera& camera() { return cam_; }
  const ViewCamera& camera() const { return cam_; }
  void setViewType(ViewType t);
  void setCameraView(uint32_t cameraNodeId);
  ShadingMode shading() const { return shading_; }
  void setShading(ShadingMode m);
  bool edgedFaces() const { return edgedFaces_; }
  void setEdgedFaces(bool on);
  void setActive(bool active);
  bool isActive() const { return active_; }

  void zoomExtents(bool selectedOnly);
  void undoViewChange();
  // Camera for rendering what this viewport shows (scene cameras resolved).
  void updateCamera();
  QString label() const;

  // Creates a camera node matching this (perspective) view; returns its id or 0.
  uint32_t createCameraFromView();

  // Simulated input for the self test.
  void simulateMouse(QEvent::Type type, QPointF pos, Qt::MouseButton button, Qt::MouseButtons buttons,
                     Qt::KeyboardModifiers mods = Qt::NoModifier);
  bool cancelInteraction();  // Esc: cancels creation or a drag; returns true if something was cancelled

 Q_SIGNALS:
  void activated(Viewport* vp);
  void contextMenuRequested(Viewport* vp, const QPoint& globalPos);
  void maximizeToggleRequested();
  void viewChanged();

 protected:
  void initializeGL() override;
  void paintGL() override;
  void resizeGL(int w, int h) override;
  void mousePressEvent(QMouseEvent* e) override;
  void mouseMoveEvent(QMouseEvent* e) override;
  void mouseReleaseEvent(QMouseEvent* e) override;
  void mouseDoubleClickEvent(QMouseEvent* e) override;
  void wheelEvent(QWheelEvent* e) override;
  void leaveEvent(QEvent* e) override;

 private:
  enum class Drag { None, Pan, Orbit, ZoomDrag, Region, Gizmo, Create };

  const Node* cameraNode() const;
  void drawGrid(LineBatch& b);
  void drawIcons(LineBatch& b);
  void drawBrackets(LineBatch& b, const Node& n);
  void drawOverlay(QPainter& p);
  void setupGizmo();
  bool gizmoAvailable() const;
  uint32_t pick(float px, float py, float* depth = nullptr) const;
  QVector<uint32_t> regionPick(const QRect& r) const;
  void pushViewHistory();
  void showLabelMenu(const QPoint& pos);
  QRect labelRect() const;

  // Transform drags.
  void beginTransformDrag(GizmoHandle h, float px, float py, bool cloneFirst);
  void updateTransformDrag(float px, float py);
  void endTransformDrag();

  // Interactive creation.
  bool constructionHit(float px, float py, Vec3& hit) const;
  void createPress(float px, float py);
  void createMove(float px, float py, bool buttonDown);
  void createRelease(float px, float py);
  void finishCreate();

  SceneDocument* doc_;
  EditorState* state_;
  ViewCamera cam_;
  std::vector<ViewCamera> history_;
  ShadingMode shading_ = ShadingMode::Wireframe;
  bool edgedFaces_ = false;
  bool active_ = false;
  std::unique_ptr<GlScene> gl_;
  QString glError_;

  Drag drag_ = Drag::None;
  QPoint pressPos_, lastPos_;
  bool moved_ = false;
  Qt::KeyboardModifiers pressMods_;

  Gizmo gizmo_;
  GizmoHandle hot_ = GizmoHandle::None;
  GizmoDrag gizmoDrag_;
  struct StartXf {
    uint32_t id;
    Transform xf;
  };
  std::vector<StartXf> dragStart_;
  Vec3 dragCenter_;
  Vec3 dragDisplay_;  // value shown in the overlay during a drag

  // Creation state.
  int createStage_ = 0;  // 0 idle, 1 footprint drag, 2 height
  uint32_t createNode_ = 0;
  Vec3 createP0_, createPlaneN_;
  Mat4 createRot_;
  float createStartY_ = 0;
  float createBase_ = 0;
  QString createKind_;
};

}  // namespace veraa
