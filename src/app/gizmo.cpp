#include "gizmo.h"

#include <QObject>

#include "theme.h"

namespace veraa {

int handleAxisMask(GizmoHandle h) {
  switch (h) {
    case GizmoHandle::X: return 1;
    case GizmoHandle::Y: return 2;
    case GizmoHandle::Z: return 4;
    case GizmoHandle::XY: return 3;
    case GizmoHandle::YZ: return 6;
    case GizmoHandle::ZX: return 5;
    case GizmoHandle::Center: return 7;
    default: return 0;
  }
}

QString handleName(GizmoHandle h) {
  switch (h) {
    case GizmoHandle::X: return "X";
    case GizmoHandle::Y: return "Y";
    case GizmoHandle::Z: return "Z";
    case GizmoHandle::XY: return "XY";
    case GizmoHandle::YZ: return "YZ";
    case GizmoHandle::ZX: return "ZX";
    case GizmoHandle::Center: return QObject::tr("Screen");
    default: return {};
  }
}

namespace {

QColor axisColor(int a) { return a == 0 ? theme::axisX() : a == 1 ? theme::axisY() : theme::axisZ(); }

float distToSegment(float px, float py, float ax, float ay, float bx, float by) {
  float dx = bx - ax, dy = by - ay;
  float len2 = dx * dx + dy * dy;
  float t = len2 > 1e-6f ? clampf(((px - ax) * dx + (py - ay) * dy) / len2, 0.0f, 1.0f) : 0.0f;
  float cx = ax + dx * t - px, cy = ay + dy * t - py;
  return std::sqrt(cx * cx + cy * cy);
}

bool pointInQuad(float px, float py, const float* xs, const float* ys) {
  bool inside = false;
  for (int i = 0, j = 3; i < 4; j = i++) {
    if (((ys[i] > py) != (ys[j] > py)) && (px < (xs[j] - xs[i]) * (py - ys[i]) / (ys[j] - ys[i]) + xs[i]))
      inside = !inside;
  }
  return inside;
}

constexpr int kRingSegments = 72;
constexpr float kPlaneFrac = 0.32f;

// Plane handles: (axis a, axis b, normal axis, handle).
struct PlaneDef {
  int a, b, n;
  GizmoHandle h;
};
const PlaneDef kPlanes[3] = {{0, 1, 2, GizmoHandle::XY}, {1, 2, 0, GizmoHandle::YZ}, {2, 0, 1, GizmoHandle::ZX}};

}  // namespace

void Gizmo::setup(const ViewCamera& cam, TransformTool t, const Vec3& o, const Vec3 ax[3], float pixels) {
  tool = t;
  origin = o;
  for (int i = 0; i < 3; ++i) axes[i] = normalize(ax[i]);
  size = cam.worldPerPixel(o) * pixels;
}

bool Gizmo::axisVisible(const ViewCamera& cam, int a) const {
  return std::fabs(dot(axes[a], cam.forward())) < 0.985f;
}

bool Gizmo::planeVisible(const ViewCamera& cam, int n) const {
  return std::fabs(dot(axes[n], cam.forward())) > 0.2f;
}

bool Gizmo::axisTip(const ViewCamera& cam, int a, float& px, float& py) const {
  if (!axisVisible(cam, a)) return false;
  return cam.project(origin + axes[a] * (size * 1.12f), px, py);
}

GizmoHandle Gizmo::hitTest(const ViewCamera& cam, float px, float py) const {
  float ox, oy;
  if (!cam.project(origin, ox, oy)) return GizmoHandle::None;
  const GizmoHandle axisHandles[3] = {GizmoHandle::X, GizmoHandle::Y, GizmoHandle::Z};

  if (tool == TransformTool::Rotate) {
    float best = 7.0f;
    GizmoHandle hit = GizmoHandle::None;
    for (int a = 0; a < 3; ++a) {
      Vec3 u, v;
      makeBasis(axes[a], u, v);
      float lx = 0, ly = 0;
      bool havePrev = false;
      for (int i = 0; i <= kRingSegments; ++i) {
        float ang = kTwoPi * i / kRingSegments;
        Vec3 p = origin + (u * std::cos(ang) + v * std::sin(ang)) * size;
        // Only the half of the ring facing the viewer is grabbable.
        bool front = dot(p - origin, cam.forward()) <= size * 0.15f;
        float sx, sy;
        if (!cam.project(p, sx, sy)) { havePrev = false; continue; }
        if (havePrev && front) {
          float d = distToSegment(px, py, lx, ly, sx, sy);
          if (d < best) { best = d; hit = axisHandles[a]; }
        }
        lx = sx; ly = sy; havePrev = true;
      }
    }
    if (hit != GizmoHandle::None) return hit;
    // Outer screen ring rotates around the view axis.
    float r = size * 1.25f / std::max(cam.worldPerPixel(origin), 1e-9f);
    float d = std::fabs(std::hypot(px - ox, py - oy) - r);
    if (d < 6.0f) return GizmoHandle::Center;
    return GizmoHandle::None;
  }

  // Center handle (move in screen plane / uniform scale).
  if (std::hypot(px - ox, py - oy) < 8.0f) return GizmoHandle::Center;
  if (tool == TransformTool::Scale) {
    // Uniform-scale triangle between the axes.
    float xs[4], ys[4];
    bool ok = true;
    Vec3 pts[3] = {origin + axes[0] * size * 0.3f, origin + axes[1] * size * 0.3f, origin + axes[2] * size * 0.3f};
    for (int i = 0; i < 3; ++i) ok &= cam.project(pts[i], xs[i], ys[i]);
    xs[3] = xs[2]; ys[3] = ys[2];
    if (ok && pointInQuad(px, py, xs, ys)) return GizmoHandle::Center;
  }
  // Plane handles.
  for (const auto& pl : kPlanes) {
    if (!planeVisible(cam, pl.n)) continue;
    float s = size * (tool == TransformTool::Scale ? 0.55f : kPlaneFrac);
    float xs[4], ys[4];
    Vec3 q[4] = {origin, origin + axes[pl.a] * s, origin + (axes[pl.a] + axes[pl.b]) * s, origin + axes[pl.b] * s};
    if (tool == TransformTool::Scale) {
      q[0] = origin + axes[pl.a] * (size * 0.3f);
      q[3] = origin + axes[pl.b] * (size * 0.3f);
      q[2] = origin + axes[pl.b] * s;
      q[1] = origin + axes[pl.a] * s;
    }
    bool ok = true;
    for (int i = 0; i < 4; ++i) ok &= cam.project(q[i], xs[i], ys[i]);
    if (ok && pointInQuad(px, py, xs, ys)) return pl.h;
  }
  // Axes.
  float best = 7.0f;
  GizmoHandle hit = GizmoHandle::None;
  for (int a = 0; a < 3; ++a) {
    if (!axisVisible(cam, a)) continue;
    float tx, ty;
    if (!cam.project(origin + axes[a] * size, tx, ty)) continue;
    float d = distToSegment(px, py, ox, oy, tx, ty);
    if (d < best) { best = d; hit = axisHandles[a]; }
  }
  return hit;
}

void Gizmo::draw(LineBatch& b, const ViewCamera& cam, GizmoHandle hot) const {
  int hotMask = handleAxisMask(hot);
  QColor hotColor = theme::gizmoHot();
  Vec3 fwd = cam.forward();

  if (tool == TransformTool::Rotate) {
    for (int a = 0; a < 3; ++a) {
      Vec3 u, v;
      makeBasis(axes[a], u, v);
      bool isHot = (hot == GizmoHandle::X && a == 0) || (hot == GizmoHandle::Y && a == 1) ||
                   (hot == GizmoHandle::Z && a == 2);
      QColor c = isHot ? hotColor : axisColor(a);
      QColor back = c;
      back.setAlphaF(0.22f);
      Vec3 prev = origin + u * size;
      for (int i = 1; i <= kRingSegments; ++i) {
        float ang = kTwoPi * i / kRingSegments;
        Vec3 p = origin + (u * std::cos(ang) + v * std::sin(ang)) * size;
        Vec3 mid = (p + prev) * 0.5f - origin;
        b.line(prev, p, dot(mid, fwd) <= size * 0.15f ? c : back);
        prev = p;
      }
    }
    // View-aligned circles: outer screen ring and inner silhouette.
    Vec3 r = cam.right(), up = cam.up();
    QColor outer = hot == GizmoHandle::Center ? hotColor : QColor(200, 200, 200);
    QColor inner(150, 150, 150, 150);
    for (int i = 0; i < kRingSegments; ++i) {
      float a0 = kTwoPi * i / kRingSegments, a1 = kTwoPi * (i + 1) / kRingSegments;
      Vec3 d0 = r * std::cos(a0) + up * std::sin(a0), d1 = r * std::cos(a1) + up * std::sin(a1);
      b.line(origin + d0 * size * 1.25f, origin + d1 * size * 1.25f, outer);
      b.line(origin + d0 * size, origin + d1 * size, inner);
    }
    return;
  }

  // Plane handles.
  for (const auto& pl : kPlanes) {
    if (!planeVisible(cam, pl.n)) continue;
    bool isHot = hot == pl.h;
    if (tool == TransformTool::Move) {
      float s = size * kPlaneFrac;
      Vec3 pa = origin + axes[pl.a] * s, pb = origin + axes[pl.b] * s, pab = origin + (axes[pl.a] + axes[pl.b]) * s;
      b.line(pa, pab, isHot ? hotColor : axisColor(pl.b));
      b.line(pb, pab, isHot ? hotColor : axisColor(pl.a));
      if (isHot) {
        QColor fill = hotColor;
        fill.setAlphaF(0.35f);
        b.tri(origin, pa, pab, fill);
        b.tri(origin, pab, pb, fill);
      }
    } else {
      float s0 = size * 0.3f, s1 = size * 0.55f;
      Vec3 a0 = origin + axes[pl.a] * s0, a1 = origin + axes[pl.a] * s1;
      Vec3 b0 = origin + axes[pl.b] * s0, b1 = origin + axes[pl.b] * s1;
      QColor c = isHot ? hotColor : QColor(200, 200, 200, 160);
      b.line(a1, b1, c);
      if (isHot) {
        QColor fill = hotColor;
        fill.setAlphaF(0.35f);
        b.tri(a0, a1, b1, fill);
        b.tri(a0, b1, b0, fill);
      }
    }
  }
  if (tool == TransformTool::Scale) {
    Vec3 p0 = origin + axes[0] * size * 0.3f, p1 = origin + axes[1] * size * 0.3f, p2 = origin + axes[2] * size * 0.3f;
    QColor c = hot == GizmoHandle::Center ? hotColor : QColor(220, 220, 220, 120);
    b.line(p0, p1, QColor(220, 220, 220, 160));
    b.line(p1, p2, QColor(220, 220, 220, 160));
    b.line(p2, p0, QColor(220, 220, 220, 160));
    if (hot == GizmoHandle::Center) {
      c.setAlphaF(0.35f);
      b.tri(p0, p1, p2, c);
    }
  }

  // Axes with arrow heads (move) or boxes (scale).
  for (int a = 0; a < 3; ++a) {
    if (!axisVisible(cam, a)) continue;
    bool isHot = (hotMask & (1 << a)) && hot != GizmoHandle::Center &&
                 (hot == GizmoHandle::X || hot == GizmoHandle::Y || hot == GizmoHandle::Z);
    QColor c = isHot ? hotColor : axisColor(a);
    Vec3 tip = origin + axes[a] * size;
    b.line(origin, tip, c);
    Vec3 side = cross(axes[a], fwd);
    if (length2(side) < 1e-10f) side = cross(axes[a], Vec3(0, 0, 1));
    side = normalize(side) * (size * 0.07f);
    if (tool == TransformTool::Move) {
      Vec3 base = origin + axes[a] * (size * 0.8f);
      Vec3 depth = normalize(cross(axes[a], side)) * (size * 0.07f);
      b.tri(tip, base + side, base - side, c);
      b.tri(tip, base + depth, base - depth, c);
    } else {
      Vec3 up = normalize(cross(fwd, side)) * (size * 0.06f);
      Vec3 s = side * 0.85f;
      b.tri(tip + s + up, tip - s + up, tip - s - up, c);
      b.tri(tip + s + up, tip - s - up, tip + s - up, c);
    }
  }
  // Center marker.
  if (tool == TransformTool::Move) {
    Vec3 r = cam.right() * (size * 0.06f), u = cam.up() * (size * 0.06f);
    QColor c = hot == GizmoHandle::Center ? hotColor : QColor(230, 230, 230, 200);
    b.line(origin - r - u, origin + r - u, c);
    b.line(origin + r - u, origin + r + u, c);
    b.line(origin + r + u, origin - r + u, c);
    b.line(origin - r + u, origin - r - u, c);
  }
}

void GizmoDrag::begin(const ViewCamera& cam, const Gizmo& g, GizmoHandle h, float px, float py) {
  handle_ = h;
  g_ = g;
  viewFwd_ = cam.forward();
  startX_ = px;
  startY_ = py;
  Ray r = cam.rayAt(px, py);
  int mask = handleAxisMask(h);
  s0_ = 0;
  p0Valid_ = false;
  accumAngle_ = lastAngle_ = 0;
  edgeOn_ = false;
  if (g.tool == TransformTool::Rotate) {
    Vec3 axis = h == GizmoHandle::Center ? -viewFwd_ : g.axes[h == GizmoHandle::X ? 0 : h == GizmoHandle::Y ? 1 : 2];
    edgeOn_ = std::fabs(dot(axis, viewFwd_)) < 0.12f;
    p0Valid_ = !edgeOn_ && planeHit(r, axis, p0_);
    return;
  }
  if (mask == 1 || mask == 2 || mask == 4) {
    int a = mask == 1 ? 0 : mask == 2 ? 1 : 2;
    s0_ = axisParam(r, g.axes[a]);
  } else if (h == GizmoHandle::Center) {
    p0Valid_ = planeHit(r, viewFwd_, p0_);
  } else if (mask) {
    int n = h == GizmoHandle::XY ? 2 : h == GizmoHandle::YZ ? 0 : 1;
    p0Valid_ = planeHit(r, g.axes[n], p0_);
  }
}

float GizmoDrag::axisParam(const Ray& r, const Vec3& a) const {
  Vec3 w0 = g_.origin - r.o;
  float b = dot(a, r.d);
  float d = dot(a, w0), e = dot(r.d, w0);
  float denom = 1.0f - b * b;
  if (std::fabs(denom) < 1e-6f) return s0_;
  return (b * e - d) / denom;
}

bool GizmoDrag::planeHit(const Ray& r, const Vec3& n, Vec3& p) const {
  float denom = dot(r.d, n);
  if (std::fabs(denom) < 1e-5f) return false;
  float t = dot(g_.origin - r.o, n) / denom;
  p = r.o + r.d * t;
  return true;
}

Vec3 GizmoDrag::translation(const ViewCamera& cam, float px, float py) const {
  Ray r = cam.rayAt(px, py);
  int mask = handleAxisMask(handle_);
  if (mask == 1 || mask == 2 || mask == 4) {
    int a = mask == 1 ? 0 : mask == 2 ? 1 : 2;
    float s = axisParam(r, g_.axes[a]);
    // Guard against shooting off when the axis points almost at the viewer.
    float limit = g_.size * 200.0f;
    return g_.axes[a] * clampf(s - s0_, -limit, limit);
  }
  if (!p0Valid_) return Vec3();
  Vec3 n = handle_ == GizmoHandle::Center ? viewFwd_
           : handle_ == GizmoHandle::XY   ? g_.axes[2]
           : handle_ == GizmoHandle::YZ   ? g_.axes[0]
                                          : g_.axes[1];
  Vec3 p;
  if (!planeHit(r, n, p)) return Vec3();
  Vec3 d = p - p0_;
  float limit = g_.size * 200.0f;
  if (length(d) > limit) d = normalize(d) * limit;
  return d;
}

float GizmoDrag::rotation(const ViewCamera& cam, float px, float py, Vec3& axis) {
  axis = handle_ == GizmoHandle::Center ? -viewFwd_
                                        : g_.axes[handle_ == GizmoHandle::X ? 0 : handle_ == GizmoHandle::Y ? 1 : 2];
  if (edgeOn_ || !p0Valid_) {
    // Ring seen edge-on: drag sideways/vertically to rotate.
    return radians(((px - startX_) - (py - startY_)) * 0.5f);
  }
  Vec3 p;
  if (!planeHit(cam.rayAt(px, py), axis, p)) return accumAngle_;
  Vec3 v0 = p0_ - g_.origin, v = p - g_.origin;
  float ang = std::atan2(dot(cross(v0, v), axis), dot(v0, v));
  float delta = ang - lastAngle_;
  if (delta > kPi) delta -= kTwoPi;
  if (delta < -kPi) delta += kTwoPi;
  accumAngle_ += delta;
  lastAngle_ = ang;
  return accumAngle_;
}

Vec3 GizmoDrag::scaling(const ViewCamera& cam, float px, float py) const {
  int mask = handleAxisMask(handle_);
  if (handle_ == GizmoHandle::Center || mask == 0) {
    float f = std::max(0.01f, 1.0f + (startY_ - py) / 120.0f);
    return Vec3(f);
  }
  Ray r = cam.rayAt(px, py);
  float f = 1.0f;
  if (mask == 1 || mask == 2 || mask == 4) {
    int a = mask == 1 ? 0 : mask == 2 ? 1 : 2;
    f = 1.0f + (axisParam(r, g_.axes[a]) - s0_) / g_.size;
  } else if (p0Valid_) {
    int n = handle_ == GizmoHandle::XY ? 2 : handle_ == GizmoHandle::YZ ? 0 : 1;
    Vec3 p;
    if (planeHit(r, g_.axes[n], p)) {
      Vec3 diag = normalize(g_.axes[(n + 1) % 3] + g_.axes[(n + 2) % 3]);
      f = 1.0f + dot(p - p0_, diag) / g_.size;
    }
  }
  f = std::max(f, 0.01f);
  return Vec3((mask & 1) ? f : 1.0f, (mask & 2) ? f : 1.0f, (mask & 4) ? f : 1.0f);
}

}  // namespace veraa
