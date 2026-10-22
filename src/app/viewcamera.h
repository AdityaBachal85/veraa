// Viewport camera: Top/Front/Left/... orthographic views, an orbiting perspective view,
// a free orthographic "User" view, or a scene camera.
#pragma once

#include <QString>

#include "vmath.h"

namespace veraa {

class Node;

enum class ViewType { Top, Bottom, Front, Back, Left, Right, Perspective, User, Camera };

QString viewTypeName(ViewType t);
bool isOrthoAxisView(ViewType t);  // Top/Bottom/Front/Back/Left/Right

class ViewCamera {
 public:
  ViewType type = ViewType::Perspective;

  // Orbit state (Perspective and User views).
  Vec3 target{0, 0, 0};
  float distance = 600.0f;
  float yawDeg = -60.0f;   // around +Z, 0 = looking from +X
  float pitchDeg = 28.0f;  // elevation above the XY plane
  float fovYDeg = 40.0f;

  // Orthographic state (axis and User views).
  Vec3 center{0, 0, 0};
  float orthoHeight = 400.0f;  // world units visible vertically

  // Scene camera (ViewType::Camera); resolved by the viewport each frame.
  uint32_t cameraNodeId = 0;

  // Derived for the current viewport size (call update()).
  void update(int widthPx, int heightPx, const Node* cameraNode);
  Vec3 eye() const { return eye_; }
  Vec3 forward() const { return forward_; }
  Vec3 right() const { return right_; }
  Vec3 up() const { return up_; }
  bool orthographic() const { return ortho_; }
  float verticalFov() const { return fovY_; }
  float orthoHalfHeight() const { return orthoH_ * 0.5f; }
  const Mat4& view() const { return view_; }
  const Mat4& proj() const { return proj_; }
  const Mat4& viewProj() const { return viewProj_; }
  int width() const { return w_; }
  int height() const { return h_; }
  float aspect() const { return h_ > 0 ? static_cast<float>(w_) / h_ : 1.0f; }

  // Ray through a pixel (origin, normalized direction).
  Ray rayAt(float px, float py) const;
  // World point to pixel; returns false when behind the camera.
  bool project(const Vec3& world, float& px, float& py, float* depth = nullptr) const;
  // World units per pixel at a world point (for constant-size gizmos and panning).
  float worldPerPixel(const Vec3& at) const;
  // The construction plane used for creating objects in this view: point + normal.
  void constructionPlane(Vec3& point, Vec3& normal) const;
  // Rotation (Euler XYZ degrees) that makes an object's local +Z face the viewer in this view.
  Vec3 creationRotation() const;

  // Navigation.
  void pan(float dxPx, float dyPx);
  void zoom(float factor, float px, float py);  // factor < 1 zooms in, toward the pixel
  void orbit(float dxPx, float dyPx);           // turns axis views into a User view
  void frame(const Aabb& box);                  // zoom extents
  void setType(ViewType t);

 private:
  int w_ = 1, h_ = 1;
  bool ortho_ = false;
  float fovY_ = 40.0f, orthoH_ = 400.0f;
  Vec3 eye_, forward_{0, 1, 0}, right_{1, 0, 0}, up_{0, 0, 1};
  Mat4 view_, proj_, viewProj_;
  float near_ = 1.0f, far_ = 1e5f;
};

// Euler XYZ (degrees) of a rotation whose local -Z axis points along dir, keeping +Z world up.
Vec3 rotationLookingAlong(const Vec3& dir);

}  // namespace veraa
