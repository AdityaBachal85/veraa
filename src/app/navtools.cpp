#include "navtools.h"

#include <algorithm>
#include <QMouseEvent>
#include <QPainter>

#include <cmath>

#include "extension.h"
#include "viewportpanel.h"

namespace veraa {

namespace {

class NavTool : public ViewportTool {
 public:
  NavTool(NavMode mode, ViewportPanel* panel) : mode_(mode), panel_(panel) {}

  bool mousePress(Viewport& vp, QMouseEvent* e) override {
    if (e->button() == Qt::RightButton) {
      vp.editorState()->setViewportTool(nullptr);
      return true;
    }
    if (e->button() != Qt::LeftButton) return false;
    dragging_ = true;
    press_ = last_ = e->position();
    vp.updateCamera();
    if (mode_ == NavMode::ZoomAll) {
      for (Viewport* v : panel_->visibleViewports()) v->recordViewChange();
    } else {
      vp.recordViewChange();
    }
    if (mode_ == NavMode::OrbitSelected || mode_ == NavMode::OrbitSubObject) pivotOnSelection(vp);
    vp.setProperty("navigating", true);
    return true;
  }

  bool mouseMove(Viewport& vp, QMouseEvent* e) override {
    if (!dragging_) {
      vp.setCursor(cursor());
      return false;
    }
    QPointF d = e->position() - last_;
    last_ = e->position();
    ViewCamera& cam = vp.camera();
    vp.updateCamera();
    switch (mode_) {
      case NavMode::Zoom:
        cam.zoom(std::pow(1.01f, static_cast<float>(d.y())), vp.width() * 0.5f, vp.height() * 0.5f);
        break;
      case NavMode::ZoomAll:
        for (Viewport* v : panel_->visibleViewports()) {
          v->updateCamera();
          v->camera().zoom(std::pow(1.01f, static_cast<float>(d.y())), v->width() * 0.5f, v->height() * 0.5f);
          v->update();
        }
        break;
      case NavMode::Fov:
        if (cam.type == ViewType::Perspective)
          cam.fovYDeg = std::clamp(cam.fovYDeg * std::pow(1.006f, static_cast<float>(d.y())), 1.0f, 150.0f);
        else
          cam.zoom(std::pow(1.01f, static_cast<float>(d.y())), vp.width() * 0.5f, vp.height() * 0.5f);
        break;
      case NavMode::ZoomRegion: break;
      case NavMode::Pan: cam.pan(static_cast<float>(d.x()), static_cast<float>(d.y())); break;
      case NavMode::Orbit:
      case NavMode::OrbitSelected:
      case NavMode::OrbitSubObject: cam.orbit(static_cast<float>(d.x()), static_cast<float>(d.y())); break;
    }
    vp.update();
    return true;
  }

  bool mouseRelease(Viewport& vp, QMouseEvent* e) override {
    if (!dragging_ || e->button() != Qt::LeftButton) return false;
    dragging_ = false;
    vp.setProperty("navigating", false);
    if (mode_ == NavMode::ZoomRegion) zoomToRegion(vp, QRectF(press_, e->position()).normalized());
    vp.update();
    Q_EMIT vp.viewChanged();
    return true;
  }

  void drawOverlay(Viewport& vp, QPainter& p) override {
    if (mode_ != NavMode::ZoomRegion || !dragging_) return;
    p.setPen(QPen(QColor(255, 255, 255), 1, Qt::DashLine));
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(press_, last_).normalized());
    Q_UNUSED(vp);
  }

  bool cancel() override {
    bool was = dragging_;
    dragging_ = false;
    return was;
  }
  QString prompt() const override { return navToolPrompt(mode_); }
  NavMode mode() const { return mode_; }

 private:
  Qt::CursorShape cursor() const {
    switch (mode_) {
      case NavMode::Pan: return Qt::OpenHandCursor;
      case NavMode::ZoomRegion: return Qt::CrossCursor;
      case NavMode::Orbit:
      case NavMode::OrbitSelected:
      case NavMode::OrbitSubObject: return Qt::SizeAllCursor;
      default: return Qt::SizeVerCursor;
    }
  }

  // Orbit Selected: make the selection center the orbit pivot while keeping the eye in place.
  void pivotOnSelection(Viewport& vp) {
    SceneDocument* doc = vp.document();
    if (doc->selection().isEmpty()) return;
    Aabb b = doc->selectionBounds();
    Vec3 c = b.valid() ? b.center() : doc->selectionCenter();
    ViewCamera& cam = vp.camera();
    if (cam.type == ViewType::Perspective) {
      Vec3 eye = cam.eye();
      Vec3 dir = eye - c;
      float dist = length(dir);
      if (dist < 1e-3f) return;
      cam.target = c;
      cam.distance = dist;
      cam.pitchDeg = degrees(std::asin(clampf(dir.z / dist, -1.0f, 1.0f)));
      cam.yawDeg = degrees(std::atan2(dir.y, dir.x));
    } else if (cam.type != ViewType::Camera) {
      cam.center = c;
    }
    vp.updateCamera();
  }

  void zoomToRegion(Viewport& vp, const QRectF& r) {
    if (r.width() < 4 || r.height() < 4) return;
    vp.updateCamera();
    ViewCamera& cam = vp.camera();
    float w = static_cast<float>(vp.width()), h = static_cast<float>(vp.height());
    float dx = static_cast<float>(r.center().x()) - w * 0.5f, dy = static_cast<float>(r.center().y()) - h * 0.5f;
    float ratio = std::max(static_cast<float>(r.width()) / w, static_cast<float>(r.height()) / h);
    if (cam.orthographic()) {
      float wpp = cam.worldPerPixel(cam.center);
      cam.center = cam.center + cam.right() * (dx * wpp) - cam.up() * (dy * wpp);
      cam.orthoHeight = std::max(cam.orthoHeight * ratio, 0.01f);
    } else if (cam.type == ViewType::Perspective) {
      float wpp = cam.worldPerPixel(cam.target);
      cam.target = cam.target + cam.right() * (dx * wpp) - cam.up() * (dy * wpp);
      cam.distance = std::max(cam.distance * ratio, 0.01f);
    }
  }

  NavMode mode_;
  ViewportPanel* panel_;
  bool dragging_ = false;
  QPointF press_, last_;
};

}  // namespace

std::shared_ptr<ViewportTool> makeNavTool(NavMode mode, ViewportPanel* panel) {
  return std::make_shared<NavTool>(mode, panel);
}

bool isNavTool(const ViewportTool* tool) { return dynamic_cast<const NavTool*>(tool) != nullptr; }

QString navToolPrompt(NavMode mode) {
  switch (mode) {
    case NavMode::Zoom: return QObject::tr("Zoom: drag up or down in a viewport; right-click to stop");
    case NavMode::ZoomAll: return QObject::tr("Zoom All: drag up or down to zoom every viewport");
    case NavMode::Fov: return QObject::tr("Field-of-View: drag up or down to change the lens angle");
    case NavMode::ZoomRegion: return QObject::tr("Zoom Region: drag a rectangle to zoom into");
    case NavMode::Pan: return QObject::tr("Pan View: drag to pan; right-click to stop");
    case NavMode::Orbit: return QObject::tr("Orbit: drag to orbit around the view center");
    case NavMode::OrbitSelected: return QObject::tr("Orbit Selected: drag to orbit around the selection");
    case NavMode::OrbitSubObject: return QObject::tr("Orbit SubObject: drag to orbit around the sub-object selection");
  }
  return QString();
}

}  // namespace veraa
