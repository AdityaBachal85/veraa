#include "viewcube.h"

#include <QObject>
#include <QPainter>
#include <QPainterPath>
#include <QStringList>

#include <algorithm>
#include <cmath>

#include "viewcamera.h"

namespace veraa {

namespace {

// Cube faces: normal and two tangents (u to the right, v up when looking at the face).
struct Face {
  Vec3 n, u, v;
  const char* label;
};
const Face kFaces[6] = {
    {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}, "TOP"},     {{0, 0, -1}, {1, 0, 0}, {0, -1, 0}, "BOTTOM"},
    {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}, "FRONT"},  {{0, 1, 0}, {-1, 0, 0}, {0, 0, 1}, "BACK"},
    {{-1, 0, 0}, {0, -1, 0}, {0, 0, 1}, "LEFT"},  {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, "RIGHT"},
};
constexpr float kEdge = 0.62f;  // inner square of each face (edges/corners are the border strips)

bool pointInPolygon(const QPolygonF& poly, QPointF p) { return poly.containsPoint(p, Qt::OddEvenFill); }

}  // namespace

void ViewCube::layout(const QSize& widget) {
  float size = std::clamp(std::min(widget.width(), widget.height()) * 0.11f, 46.0f, 84.0f);
  scale_ = size * 0.36f;
  center_ = QPointF(widget.width() - size * 0.62 - 8, size * 0.62 + 8);
  rect_ = QRectF(center_.x() - size * 0.75, center_.y() - size * 0.72, size * 1.5, size * 1.5);
}

QPointF ViewCube::toScreen(const ViewCamera& cam, const Vec3& v) const {
  return center_ + QPointF(dot(v, cam.right()), -dot(v, cam.up())) * scale_;
}

void ViewCube::cells(Cell out[54]) const {
  const float t[4] = {-1.0f, -kEdge, kEdge, 1.0f};
  int k = 0;
  for (const Face& f : kFaces)
    for (int j = 0; j < 3; ++j)
      for (int i = 0; i < 3; ++i) {
        Cell& c = out[k++];
        c.normal = f.n;
        float iu = static_cast<float>(i - 1), jv = static_cast<float>(j - 1);
        c.dir = normalize(f.n + f.u * iu + f.v * jv);
        c.corners[0] = f.n + f.u * t[i] + f.v * t[j];
        c.corners[1] = f.n + f.u * t[i + 1] + f.v * t[j];
        c.corners[2] = f.n + f.u * t[i + 1] + f.v * t[j + 1];
        c.corners[3] = f.n + f.u * t[i] + f.v * t[j + 1];
      }
}

void ViewCube::paint(QPainter& p, const ViewCamera& cam, QPointF hover, bool active) const {
  p.save();
  p.setRenderHint(QPainter::Antialiasing);
  Vec3 toEye = -cam.forward();
  float alpha = active ? 1.0f : 0.55f;

  // Compass ring on the ground plane under the cube.
  QPainterPath ring;
  const int segs = 64;
  for (int i = 0; i <= segs; ++i) {
    float a = kTwoPi * i / segs;
    QPointF s = toScreen(cam, Vec3(std::cos(a) * 1.75f, std::sin(a) * 1.75f, -1.05f));
    if (i == 0) ring.moveTo(s);
    else ring.lineTo(s);
  }
  Vec3 hitDir;
  Hit hovered = hitTest(cam, hover, &hitDir);
  p.setPen(QPen(QColor(150, 150, 150, static_cast<int>(200 * alpha)), hovered == Hit::Ring ? 4.0 : 2.5));
  p.setBrush(Qt::NoBrush);
  p.drawPath(ring);
  QFont f = p.font();
  f.setPixelSize(std::max(8, static_cast<int>(scale_ * 0.42f)));
  f.setBold(true);
  p.setFont(f);
  const struct {
    Vec3 d;
    const char* t;
  } compass[4] = {{{0, 1, 0}, "N"}, {{1, 0, 0}, "E"}, {{0, -1, 0}, "S"}, {{-1, 0, 0}, "W"}};
  for (const auto& c : compass) {
    QPointF s = toScreen(cam, c.d * 2.15f + Vec3(0, 0, -1.05f));
    p.setPen(QColor(200, 200, 200, static_cast<int>(230 * alpha)));
    p.drawText(QRectF(s.x() - 8, s.y() - 8, 16, 16), Qt::AlignCenter, c.t);
  }

  // Visible cube cells, drawn back to front.
  Cell all[54];
  cells(all);
  std::vector<int> order;
  for (int i = 0; i < 54; ++i)
    if (dot(all[i].normal, toEye) > 0.02f) order.push_back(i);
  std::sort(order.begin(), order.end(), [&](int a, int b) { return dot(all[a].normal, toEye) < dot(all[b].normal, toEye); });
  for (int idx : order) {
    const Cell& c = all[idx];
    QPolygonF poly;
    for (const Vec3& v : c.corners) poly << toScreen(cam, v);
    bool hot = hovered == Hit::Region && length(hitDir - c.dir) < 1e-4f;
    float shade = 0.55f + 0.45f * dot(c.normal, toEye);
    QColor fill = hot ? QColor(110, 170, 235) : QColor::fromRgbF(0.78f * shade, 0.79f * shade, 0.81f * shade);
    fill.setAlphaF(alpha);
    p.setPen(QPen(QColor(70, 70, 70, static_cast<int>(255 * alpha)), 0.8));
    p.setBrush(fill);
    p.drawPolygon(poly);
  }
  // Face labels.
  for (const Face& face : kFaces) {
    float facing = dot(face.n, toEye);
    if (facing < 0.25f) continue;
    QPointF s = toScreen(cam, face.n);
    QPointF su = toScreen(cam, face.n + face.u) - s;
    QPointF sv = toScreen(cam, face.n + face.v) - s;
    p.save();
    // Map the label square onto the face (affine): x along u, y along -v.
    QTransform tr(su.x(), su.y(), -sv.x(), -sv.y(), s.x(), s.y());
    p.setTransform(tr, true);
    QFont lf = p.font();
    lf.setPixelSize(10);
    lf.setBold(true);
    p.setFont(lf);
    p.scale(1.0 / 26.0, 1.0 / 26.0);
    p.setPen(QColor(30, 30, 30, static_cast<int>(255 * alpha * std::min(1.0f, facing * 1.6f))));
    p.drawText(QRectF(-26, -10, 52, 20), Qt::AlignCenter, QString::fromLatin1(face.label));
    p.restore();
  }
  // Home button.
  QRectF home(rect_.left() - 2, rect_.top() - 2, 14, 13);
  QPolygonF house;
  house << QPointF(home.center().x(), home.top() + 1) << QPointF(home.right(), home.top() + 6)
        << QPointF(home.right() - 2, home.top() + 6) << QPointF(home.right() - 2, home.bottom())
        << QPointF(home.left() + 2, home.bottom()) << QPointF(home.left() + 2, home.top() + 6)
        << QPointF(home.left(), home.top() + 6);
  p.setPen(QPen(QColor(200, 200, 200, static_cast<int>(255 * alpha)), 1.0));
  p.setBrush(hovered == Hit::Home ? QColor(110, 170, 235) : QColor(90, 90, 90, static_cast<int>(255 * alpha)));
  p.drawPolygon(house);
  p.restore();
}

ViewCube::Hit ViewCube::hitTest(const ViewCamera& cam, QPointF p, Vec3* dir) const {
  QRectF home(rect_.left() - 2, rect_.top() - 2, 14, 13);
  if (home.contains(p)) return Hit::Home;
  if (!rect_.adjusted(-6, -6, 6, 16).contains(p)) return Hit::None;
  Vec3 toEye = -cam.forward();
  Cell all[54];
  cells(all);
  int best = -1;
  float bestFacing = -1.0f;
  for (int i = 0; i < 54; ++i) {
    float facing = dot(all[i].normal, toEye);
    if (facing <= 0.02f) continue;
    QPolygonF poly;
    for (const Vec3& v : all[i].corners) poly << toScreen(cam, v);
    if (pointInPolygon(poly, p) && facing > bestFacing) {
      best = i;
      bestFacing = facing;
    }
  }
  if (best >= 0) {
    if (dir) *dir = all[best].dir;
    return Hit::Region;
  }
  // Compass ring: near the projected circle.
  float bestDist = 1e9f;
  for (int i = 0; i < 64; ++i) {
    float a = kTwoPi * i / 64;
    QPointF s = toScreen(cam, Vec3(std::cos(a) * 1.75f, std::sin(a) * 1.75f, -1.05f));
    bestDist = std::min(bestDist, static_cast<float>(std::hypot(s.x() - p.x(), s.y() - p.y())));
  }
  if (bestDist < 6.0f) return Hit::Ring;
  return rect_.contains(p) ? Hit::Body : Hit::None;
}

QString ViewCube::directionName(const Vec3& d) {
  QStringList parts;
  if (d.z > 0.3f) parts << QObject::tr("Top");
  if (d.z < -0.3f) parts << QObject::tr("Bottom");
  if (d.y < -0.3f) parts << QObject::tr("Front");
  if (d.y > 0.3f) parts << QObject::tr("Back");
  if (d.x < -0.3f) parts << QObject::tr("Left");
  if (d.x > 0.3f) parts << QObject::tr("Right");
  return parts.join(' ');
}

}  // namespace veraa
