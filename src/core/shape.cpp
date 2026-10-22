#include "shape.h"

namespace veraa {

namespace {

Vec3 bezier(const Vec3& p0, const Vec3& p1, const Vec3& p2, const Vec3& p3, float t) {
  float u = 1.0f - t;
  return p0 * (u * u * u) + p1 * (3 * u * u * t) + p2 * (3 * u * t * t) + p3 * (t * t * t);
}

Vec3 bezierDerivative(const Vec3& p0, const Vec3& p1, const Vec3& p2, const Vec3& p3, float t) {
  float u = 1.0f - t;
  return (p1 - p0) * (3 * u * u) + (p2 - p1) * (6 * u * t) + (p3 - p2) * (3 * t * t);
}

bool segmentIsStraight(const Knot& a, const Knot& b) {
  Vec3 d = b.p - a.p;
  float len = length(d);
  if (len < 1e-12f) return true;
  Vec3 dir = d / len;
  auto onLine = [&](const Vec3& q) {
    Vec3 v = q - a.p;
    return length(v - dir * dot(v, dir)) < 1e-4f * std::max(1.0f, len);
  };
  return onLine(a.out) && onLine(b.in);
}

}  // namespace

int Spline::segmentCount() const {
  int n = static_cast<int>(knots.size());
  if (n < 2) return 0;
  return closed ? n : n - 1;
}

Vec3 Spline::evaluate(int seg, float t) const {
  const Knot& a = knots[seg];
  const Knot& b = knots[(seg + 1) % knots.size()];
  return bezier(a.p, a.out, b.in, b.p, t);
}

Vec3 Spline::tangent(int seg, float t) const {
  const Knot& a = knots[seg];
  const Knot& b = knots[(seg + 1) % knots.size()];
  Vec3 d = bezierDerivative(a.p, a.out, b.in, b.p, t);
  if (length2(d) < 1e-20f) d = b.p - a.p;
  return normalize(d);
}

std::vector<Vec3> Spline::polyline(int steps, bool optimize) const {
  std::vector<Vec3> out;
  int segs = segmentCount();
  if (segs == 0) {
    if (!knots.empty()) out.push_back(knots[0].p);
    return out;
  }
  steps = std::max(0, steps);
  for (int s = 0; s < segs; ++s) {
    const Knot& a = knots[s];
    const Knot& b = knots[(s + 1) % knots.size()];
    int n = (optimize && segmentIsStraight(a, b)) ? 0 : steps;
    for (int i = 0; i <= n; ++i) out.push_back(evaluate(s, static_cast<float>(i) / (n + 1)));
  }
  if (!closed) out.push_back(knots.back().p);
  return out;
}

float Spline::length(int steps) const {
  std::vector<Vec3> pts = polyline(steps, false);
  float len = 0.0f;
  for (size_t i = 1; i < pts.size(); ++i) len += veraa::length(pts[i] - pts[i - 1]);
  if (closed && pts.size() > 1) len += veraa::length(pts.front() - pts.back());
  return len;
}

void Spline::updateAutoHandles() {
  size_t n = knots.size();
  for (size_t i = 0; i < n; ++i) {
    Knot& k = knots[i];
    bool hasPrev = closed || i > 0, hasNext = closed || i + 1 < n;
    const Vec3& prev = hasPrev ? knots[(i + n - 1) % n].p : k.p;
    const Vec3& next = hasNext ? knots[(i + 1) % n].p : k.p;
    if (k.type == KnotType::Corner) {
      k.in = hasPrev ? k.p + (prev - k.p) / 3.0f : k.p;
      k.out = hasNext ? k.p + (next - k.p) / 3.0f : k.p;
    } else if (k.type == KnotType::Smooth) {
      // Catmull-Rom style tangent through the neighbours.
      Vec3 dir = next - prev;
      if (length2(dir) < 1e-20f) dir = Vec3(1, 0, 0);
      dir = normalize(dir);
      float lin = hasPrev ? veraa::length(k.p - prev) / 3.0f : 0.0f;
      float lout = hasNext ? veraa::length(next - k.p) / 3.0f : 0.0f;
      k.in = k.p - dir * lin;
      k.out = k.p + dir * lout;
    }
  }
}

Aabb BezierShape::bounds() const {
  Aabb b;
  for (const auto& s : splines)
    for (const auto& k : s.knots) {
      b.expand(k.p);
      b.expand(k.in);
      b.expand(k.out);
    }
  return b;
}

void BezierShape::transform(const Mat4& m) {
  for (auto& s : splines)
    for (auto& k : s.knots) {
      k.p = m.transformPoint(k.p);
      k.in = m.transformPoint(k.in);
      k.out = m.transformPoint(k.out);
    }
}

void BezierShape::updateAutoHandles() {
  for (auto& s : splines) s.updateAutoHandles();
}

std::vector<std::vector<Vec3>> BezierShape::polylines(int steps, bool optimize) const {
  std::vector<std::vector<Vec3>> out;
  out.reserve(splines.size());
  for (const auto& s : splines) out.push_back(s.polyline(steps, optimize));
  return out;
}

Knot cornerKnot(const Vec3& p) { return Knot{p, p, p, KnotType::Corner}; }

Knot bezierKnot(const Vec3& p, const Vec3& in, const Vec3& out) { return Knot{p, in, out, KnotType::Bezier}; }

PolyMesh shapeToTubeMesh(const BezierShape& shape, float thickness, int sides, float angleDeg, int steps,
                         bool optimize) {
  PolyMesh m;
  float r = thickness * 0.5f;
  sides = std::max(3, sides);
  if (r <= 0.0f) return m;
  float a0 = radians(angleDeg);
  for (const Spline& s : shape.splines) {
    std::vector<Vec3> pts = s.polyline(steps, optimize);
    size_t n = pts.size();
    if (n < 2) continue;
    bool closed = s.closed && n > 2;
    // Rotation-minimizing frames (double reflection would be better; parallel transport is enough).
    std::vector<Vec3> tan(n), nor(n);
    for (size_t i = 0; i < n; ++i) {
      Vec3 prev = (i > 0) ? pts[i - 1] : (closed ? pts[n - 1] : pts[i]);
      Vec3 next = (i + 1 < n) ? pts[i + 1] : (closed ? pts[0] : pts[i]);
      tan[i] = normalize(next - prev);
    }
    Vec3 up = std::fabs(tan[0].z) < 0.99f ? Vec3(0, 0, 1) : Vec3(1, 0, 0);
    nor[0] = normalize(cross(cross(tan[0], up), tan[0]));
    for (size_t i = 1; i < n; ++i) {
      Vec3 v = nor[i - 1] - tan[i] * dot(nor[i - 1], tan[i]);
      nor[i] = length2(v) > 1e-20f ? normalize(v) : nor[i - 1];
    }
    int base = static_cast<int>(m.positions.size());
    for (size_t i = 0; i < n; ++i) {
      Vec3 b = cross(tan[i], nor[i]);
      for (int k = 0; k < sides; ++k) {
        float a = a0 + kTwoPi * k / sides;
        m.addVertex(pts[i] + (nor[i] * std::cos(a) + b * std::sin(a)) * r);
      }
    }
    size_t rings = closed ? n : n - 1;
    for (size_t i = 0; i < rings; ++i) {
      int r0 = base + static_cast<int>(i) * sides, r1 = base + static_cast<int>((i + 1) % n) * sides;
      for (int k = 0; k < sides; ++k) {
        int kn = (k + 1) % sides;
        m.addFace({r0 + k, r1 + k, r1 + kn, r0 + kn}, {}, 1u);
      }
    }
    if (!closed) {
      std::vector<int> start, end;
      for (int k = sides - 1; k >= 0; --k) start.push_back(base + k);
      for (int k = 0; k < sides; ++k) end.push_back(base + static_cast<int>(n - 1) * sides + k);
      m.addFace(start, {}, 2u);
      m.addFace(end, {}, 2u);
    }
  }
  return m;
}

}  // namespace veraa
