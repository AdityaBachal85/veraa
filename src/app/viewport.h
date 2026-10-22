// One 3D viewport (Top / Front / Left / Perspective / ...), drawn with OpenGL 3.3.
#pragma once

#include <QEvent>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLWidget>
#include <QPoint>
#include <QPolygonF>

#include <memory>
#include <optional>
#include <vector>

#include "document.h"
#include "editorstate.h"
#include "gizmo.h"
#include "glscene.h"
#include "viewcamera.h"
#include "viewcube.h"

class QTimer;

namespace veraa {

// Visual styles from the shading label menu. Shaded is "Default Shading".
enum class ShadingMode { Wireframe, Shaded, Facets, Clay, BoundingBox };
// Viewport lighting: Default (headlight and key light) or the scene's lights.
enum class ViewportLighting { Default, Scene };
// The label menu's quality presets ([Standard] / [High Quality] / [Performance]).
enum class ViewportQuality { Standard, HighQuality, Performance };

// Viewport background (Views > Viewport Background, Alt+B).
struct ViewportBackground {
  enum Mode { Gradient, Solid, Image, Environment };
  Mode mode = Gradient;
  QColor solid{0x3f, 0x3f, 0x3f};
  QColor top{0x52, 0x55, 0x5a};
  QColor bottom{0x2c, 0x2d, 0x2f};
  QString imagePath;
};

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
  ViewportLighting lighting() const { return lighting_; }
  void setLighting(ViewportLighting l);
  ViewportQuality quality() const { return quality_; }
  void setQuality(ViewportQuality q);
  bool safeFrame() const { return safeFrame_; }
  void setSafeFrame(bool on);
  const ViewportBackground& background() const { return background_; }
  void setBackground(const ViewportBackground& b);
  void setActive(bool active);
  bool isActive() const { return active_; }

  void zoomExtents(bool selectedOnly);
  // View history (Shift+Z / Shift+Y). Call recordViewChange() before changing the camera.
  void undoViewChange();
  void redoViewChange();
  void recordViewChange() { pushViewHistory(); }
  // Views > Save / Restore Active View.
  void saveNamedView();
  bool restoreNamedView();
  bool hasNamedView() const { return savedView_.has_value(); }
  // Turns the view to look from `dir` (from target toward the eye), animated when `animate`.
  // Axis views switch to the matching axis view; other views orbit.
  void lookFrom(const Vec3& dir, bool animate);
  void goHome();
  // Camera for rendering what this viewport shows (scene cameras resolved).
  void updateCamera();
  QString label() const;
  QString viewLabel() const;
  QString qualityLabel() const;
  QString shadingLabel() const;

  // Creates a camera node matching this (perspective) view; returns its id or 0.
  uint32_t createCameraFromView();

  // Helpers for viewport tools: picking and construction-plane hits in widget pixels.
  uint32_t pickNode(float px, float py, float* depth = nullptr) const { return pick(px, py, depth); }
  QVector<uint32_t> pickNodesInRect(const QRect& r) const { return regionPick(r); }
  // Nodes inside (window) or touching (crossing) a screen polygon.
  QVector<uint32_t> pickNodesInPolygon(const QPolygonF& poly, bool window) const;
  // Nodes touched by a brush of `radius` pixels along these screen points.
  QVector<uint32_t> pickNodesWithBrush(const QVector<QPointF>& centers, float radius) const;
  bool hitConstructionPlane(float px, float py, Vec3& hit) const { return constructionHit(px, py, hit); }
  SceneDocument* document() const { return doc_; }
  EditorState* editorState() const { return state_; }
  const ViewCube& viewCube() const { return cube_; }
  // Gizmo axes for the current reference coordinate system and the transform center.
  void transformFrame(Vec3 axes[3], Vec3& origin) const;

  // Simulated input for the self test.
  void simulateMouse(QEvent::Type type, QPointF pos, Qt::MouseButton button, Qt::MouseButtons buttons,
                     Qt::KeyboardModifiers mods = Qt::NoModifier);
  void simulateDoubleClick(QPointF pos, Qt::MouseButton button = Qt::LeftButton);
  bool cancelInteraction();  // Esc: cancels creation or a drag; returns true if something was cancelled

 Q_SIGNALS:
  void activated(Viewport* vp);
  void contextMenuRequested(Viewport* vp, const QPoint& globalPos);
  void maximizeToggleRequested();
  void viewChanged();
  // [+] menu > Configure Viewports (tab 0 layout, 1 display, 2 background).
  void configureRequested(int tab);

 protected:
  void initializeGL() override;
  void paintGL() override;
  void resizeGL(int w, int h) override;
  void mousePressEvent(QMouseEvent* e) override;
  void mouseMoveEvent(QMouseEvent* e) override;
  void mouseReleaseEvent(QMouseEvent* e) override;
  void mouseDoubleClickEvent(QMouseEvent* e) override;
  void wheelEvent(QWheelEvent* e) override;
  void keyPressEvent(QKeyEvent* e) override;
  void leaveEvent(QEvent* e) override;
  void dragEnterEvent(QDragEnterEvent* e) override;
  void dragMoveEvent(QDragMoveEvent* e) override;
  void dropEvent(QDropEvent* e) override;

 private:
  enum class Drag { None, Pan, Orbit, ZoomDrag, Region, Gizmo, Create, Cube, Ring };

  const Node* cameraNode() const;
  // Visible here: node and layer visibility plus Display panel category and frozen filters.
  bool nodeShown(const Node& n) const;
  void drawGrid(LineBatch& b);
  void drawIcons(LineBatch& b);
  void drawBrackets(LineBatch& b, const Node& n);
  void drawBox(LineBatch& b, const Node& n, const QColor& c);
  void drawOverlay(QPainter& p);
  void drawRegion(QPainter& p);
  void drawBackgroundImage(QPainter& p);
  void setupGizmo();
  bool gizmoAvailable() const;
  bool gizmoVisible() const;
  uint32_t pick(float px, float py, float* depth = nullptr) const;
  QVector<uint32_t> regionPick(const QRect& r) const;
  // Screen points of a node used for region tests (subsampled).
  bool nodeScreenPoints(const Node& n, std::vector<QPointF>& pts, std::vector<std::pair<int, int>>* edges) const;
  void finishRegion(const QPoint& releasePos);
  void applyRegionSelection(const QVector<uint32_t>& ids);
  void pushViewHistory();
  void showLabelMenu(int segment, const QPoint& pos);
  // Label segments: 0 [+], 1 view, 2 quality, 3 shading.
  QRect labelRect() const;
  QRect labelSegment(int i) const;
  int labelSegmentAt(const QPoint& p) const;
  void startViewAnimation(float yaw, float pitch);

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
  std::vector<ViewCamera> history_, redo_;
  std::optional<ViewCamera> savedView_;
  ShadingMode shading_ = ShadingMode::Wireframe;
  ViewportLighting lighting_ = ViewportLighting::Default;
  ViewportQuality quality_ = ViewportQuality::Standard;
  ViewportBackground background_;
  QImage backgroundImage_;
  QString backgroundImagePath_;
  bool edgedFaces_ = false;
  bool safeFrame_ = false;
  bool active_ = false;
  std::unique_ptr<GlScene> gl_;
  QString glError_;

  Drag drag_ = Drag::None;
  QPoint pressPos_, lastPos_;
  bool moved_ = false;
  Qt::KeyboardModifiers pressMods_;

  // Selection regions (rectangle, circle, fence, lasso, paint).
  QVector<QPointF> regionPoints_;
  bool fenceOpen_ = false;

  // ViewCube.
  ViewCube cube_;
  QPointF hover_;
  QTimer* viewAnim_ = nullptr;
  float animFromYaw_ = 0, animFromPitch_ = 0, animToYaw_ = 0, animToPitch_ = 0;
  qint64 animStart_ = 0;

  Gizmo gizmo_;
  GizmoHandle hot_ = GizmoHandle::None;
  GizmoDrag gizmoDrag_;
  struct StartXf {
    uint32_t id;
    Transform xf;
    Vec3 worldPos;
  };
  std::vector<StartXf> dragStart_;
  Vec3 dragCenter_;
  Vec3 dragAxes_[3];
  Vec3 dragDisplay_;  // value shown in the overlay during a drag
  bool cloneDrag_ = false;
  std::vector<std::pair<uint32_t, uint32_t>> cloneSources_;  // clone id -> original id

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
