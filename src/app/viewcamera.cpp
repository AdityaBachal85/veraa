#include "viewcamera.h"

#include <QObject>

#include "scene.h"

namespace veraa {

QString viewTypeName(ViewType t) {
  switch (t) {
    case ViewType::Top: return QObject::tr("Top");
    case ViewType::Bottom: return QObject::tr("Bottom");
    case ViewType::Front: return QObject::tr("Front");
    case ViewType::Back: return QObject::tr("Back");
    case ViewType::Left: return QObject::tr("Left");
    case ViewType::Right: return QObject::tr("Right");
    case ViewType::Perspective: return QObject::tr("Perspective");
    case ViewType::User: return QObject::tr("Orthographic");
    case ViewType::Camera: return QObject::tr("Camera");
  }
  return {};
}

bool isOrthoAxisView(ViewType t) {
  return t == ViewType::Top || t == ViewType::Bottom || t == ViewType::Front || t == ViewType::Back ||
         t == ViewType::Left || t == ViewType::Right;
}

namespace {

void axisViewBasis(ViewType t, Vec3& fwd, Vec3& up) {
  switch (t) {
    case ViewType::Top: fwd = {0, 0, -1}; up = {0, 1, 0}; break;
    case ViewType::Bottom: fwd = {0, 0, 1}; up = {0, -1, 0}; break;
    case ViewType::Front: fwd = {0, 1, 0}; up = {0, 0, 1}; break;
    case ViewType::Back: fwd = {0, -1, 0}; up = {0, 0, 1}; break;
    case ViewType::Left: fwd = {1, 0, 0}; up = {0, 0, 1}; break;
    case ViewType::Right: fwd = {-1, 0, 0}; up = {0, 0, 1}; break;
    default: fwd = {0, 1, 0}; up = {0, 0, 1}; break;
  }
}

Vec3 orbitDir(float yawDeg, float pitchDeg) {
  float y = radians(yawDeg), p = radians(pitchDeg);
  return {std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), std::sin(p)};
}

constexpr float kOrthoBack = 50000.0f;

}  // namespace

void ViewCamera::update(int widthPx, int heightPx, const Node* cameraNode) {
  w_ = std::max(widthPx, 1);
  h_ = std::max(heightPx, 1);
  float aspectRatio = aspect();

  if (type == ViewType::Camera && cameraNode && cameraNode->camera()) {
    Mat4 m = cameraNode->worldMatrix();
    ortho_ = false;
    eye_ = m.translation();
    forward_ = normalize(-m.column(2));
    Vec3 upHint = normalize(m.column(1));
    right_ = normalize(cross(forward_, upHint));
    up_ = cross(right_, forward_);
    fovY_ = cameraNode->camera()->verticalFovDeg(aspectRatio);
    near_ = 1.0f;
    far_ = 1e6f;
  } else if (type == ViewType::Perspective || type == ViewType::Camera) {
    ortho_ = false;
    pitchDeg = clampf(pitchDeg, -89.5f, 89.5f);
    Vec3 dir = orbitDir(yawDeg, pitchDeg);
    eye_ = target + dir * distance;
    forward_ = -dir;
    right_ = normalize(cross(forward_, Vec3(0, 0, 1)));
    up_ = cross(right_, forward_);
    fovY_ = fovYDeg;
    near_ = std::max(distance * 0.005f, 0.05f);
    far_ = distance * 400.0f + 2e4f;
  } else {
    ortho_ = true;
    if (type == ViewType::User) {
      pitchDeg = clampf(pitchDeg, -89.9f, 89.9f);
      Vec3 dir = orbitDir(yawDeg, pitchDeg);
      forward_ = -dir;
      right_ = normalize(cross(forward_, Vec3(0, 0, 1)));
      up_ = cross(right_, forward_);
    } else {
      Vec3 upv;
      axisViewBasis(type, forward_, upv);
      right_ = normalize(cross(forward_, upv));
      up_ = upv;
    }
    eye_ = center - forward_ * kOrthoBack;
    orthoH_ = orthoHeight;
    near_ = 1.0f;
    far_ = kOrthoBack * 2.0f;
  }

  view_ = Mat4::lookAt(eye_, eye_ + forward_, up_);
  if (ortho_) {
    float hh = orthoH_ * 0.5f, hw = hh * aspectRatio;
    proj_ = Mat4::ortho(-hw, hw, -hh, hh, near_, far_);
  } else {
    proj_ = Mat4::perspective(radians(fovY_), aspectRatio, near_, far_);
  }
  viewProj_ = proj_ * view_;
}

Ray ViewCamera::rayAt(float px, float py) const {
  float x = 2.0f * px / w_ - 1.0f;
  float y = 1.0f - 2.0f * py / h_;
  Ray r;
  if (ortho_) {
    float hh = orthoH_ * 0.5f, hw = hh * aspect();
    r.o = eye_ + right_ * (x * hw) + up_ * (y * hh);
    r.d = forward_;
  } else {
    float t = std::tan(radians(fovY_) * 0.5f);
    r.o = eye_;
    r.d = normalize(forward_ + right_ * (x * t * aspect()) + up_ * (y * t));
  }
  return r;
}

bool ViewCamera::project(const Vec3& world, float& px, float& py, float* depth) const {
  Vec4 c = viewProj_.transform(Vec4(world.x, world.y, world.z, 1.0f));
  if (!ortho_ && c.w <= 1e-6f) return false;
  float iw = ortho_ ? 1.0f : 1.0f / c.w;
  px = (c.x * iw * 0.5f + 0.5f) * w_;
  py = (0.5f - c.y * iw * 0.5f) * h_;
  if (depth) *depth = dot(world - eye_, forward_);
  return true;
}

float ViewCamera::worldPerPixel(const Vec3& at) const {
  if (ortho_) return orthoH_ / h_;
  float d = std::max(dot(at - eye_, forward_), near_);
  return 2.0f * d * std::tan(radians(fovY_) * 0.5f) / h_;
}

void ViewCamera::constructionPlane(Vec3& point, Vec3& normal) const {
  point = Vec3(0, 0, 0);
  switch (type) {
    case ViewType::Front:
    case ViewType::Back: normal = {0, 1, 0}; break;
    case ViewType::Left:
    case ViewType::Right: normal = {1, 0, 0}; break;
    default: normal = {0, 0, 1}; break;
  }
}

Vec3 ViewCamera::creationRotation() const {
  switch (type) {
    case ViewType::Front: return {90, 0, 0};
    case ViewType::Back: return {90, 0, 180};
    case ViewType::Left: return {90, 0, -90};
    case ViewType::Right: return {90, 0, 90};
    case ViewType::Bottom: return {180, 0, 0};
    default: return {0, 0, 0};
  }
}

void ViewCamera::pan(float dxPx, float dyPx) {
  if (type == ViewType::Camera) return;
  if (ortho_) {
    float wpp = orthoH_ / h_;
    center += (-right_ * dxPx + up_ * dyPx) * wpp;
  } else {
    float wpp = worldPerPixel(target);
    target += (-right_ * dxPx + up_ * dyPx) * wpp;
  }
}

void ViewCamera::zoom(float factor, float px, float py) {
  if (type == ViewType::Camera) return;
  factor = clampf(factor, 0.05f, 20.0f);
  if (ortho_) {
    float x = 2.0f * px / w_ - 1.0f, y = 1.0f - 2.0f * py / h_;
    float hh = orthoH_ * 0.5f, hw = hh * aspect();
    Vec3 wpt = center + right_ * (x * hw) + up_ * (y * hh);
    float newH = clampf(orthoHeight * factor, 0.01f, 1e7f);
    float f = newH / orthoHeight;
    center = wpt + (center - wpt) * f;
    orthoHeight = newH;
  } else {
    Ray r = rayAt(px, py);
    float denom = dot(r.d, forward_);
    float t = denom > 1e-4f ? dot(target - eye_, forward_) / denom : distance;
    Vec3 p = r.o + r.d * t;
    float newDist = clampf(distance * factor, 0.01f, 1e7f);
    float f = newDist / distance;
    target = p + (target - p) * f;
    distance = newDist;
  }
}

void ViewCamera::orbit(float dxPx, float dyPx) {
  if (type == ViewType::Camera) return;
  if (isOrthoAxisView(type)) {
    Vec3 dir = -forward_;
    pitchDeg = degrees(std::asin(clampf(dir.z, -1.0f, 1.0f)));
    if (std::fabs(dir.z) > 0.999f) {
      // Looking straight down/up: derive yaw from the screen right vector.
      yawDeg = degrees(std::atan2(-right_.x, right_.y));
    } else {
      yawDeg = degrees(std::atan2(dir.y, dir.x));
    }
    type = ViewType::User;
  }
  yawDeg -= dxPx * 0.4f;
  pitchDeg = clampf(pitchDeg + dyPx * 0.4f, -89.5f, 89.5f);
}

void ViewCamera::frame(const Aabb& boxIn) {
  if (type == ViewType::Camera) return;
  Aabb box = boxIn;
  if (!box.valid()) {
    box.expand(Vec3(-50, -50, 0));
    box.expand(Vec3(50, 50, 50));
  }
  float r = std::max(box.radius(), 5.0f);
  if (!ortho_) {
    target = box.center();
    float halfFov = radians(fovYDeg) * 0.5f;
    float halfFovX = std::atan(std::tan(halfFov) * aspect());
    distance = r / std::sin(std::min(halfFov, halfFovX)) * 1.05f;
  } else {
    center = box.center();
    float er = 0, eu = 0;
    for (int i = 0; i < 8; ++i) {
      Vec3 c((i & 1) ? box.hi.x : box.lo.x, (i & 2) ? box.hi.y : box.lo.y, (i & 4) ? box.hi.z : box.lo.z);
      er = std::max(er, std::fabs(dot(c - center, right_)));
      eu = std::max(eu, std::fabs(dot(c - center, up_)));
    }
    orthoHeight = std::max(std::max(eu * 2.0f, er * 2.0f / aspect()) * 1.2f, 10.0f);
  }
}

void ViewCamera::setType(ViewType t) { type = t; }

Vec3 rotationLookingAlong(const Vec3& dirIn) {
  Vec3 dir = normalize(dirIn);
  Vec3 right = cross(dir, Vec3(0, 0, 1));
  if (length2(right) < 1e-8f) right = Vec3(1, 0, 0);
  right = normalize(right);
  Vec3 up = cross(right, dir);
  Vec3 back = -dir;
  Mat4 m;
  m.m[0][0] = right.x; m.m[1][0] = right.y; m.m[2][0] = right.z;
  m.m[0][1] = up.x;    m.m[1][1] = up.y;    m.m[2][1] = up.z;
  m.m[0][2] = back.x;  m.m[1][2] = back.y;  m.m[2][2] = back.z;
  return eulerDegFromMatrix(m);
}

}  // namespace veraa
