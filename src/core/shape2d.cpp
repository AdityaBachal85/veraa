#include "shape2d.h"

#include <array>
#include <map>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#endif
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <clipper2/clipper.h>
#include <earcut/earcut.hpp>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace veraa::shape2d {

namespace {

constexpr int kClipPrecision = 4;  // decimal places kept by Clipper2

Clipper2Lib::PathsD toPaths(const std::vector<Polygon>& polys) {
  Clipper2Lib::PathsD out;
  out.reserve(polys.size());
  for (const auto& p : polys) {
    Clipper2Lib::PathD path;
    path.reserve(p.size());
    for (const auto& v : p) path.emplace_back(v.x, v.y);
    out.push_back(std::move(path));
  }
  return out;
}

std::vector<Polygon> fromPaths(const Clipper2Lib::PathsD& paths) {
  std::vector<Polygon> out;
  for (const auto& path : paths) {
    Polygon p;
    for (const auto& v : path) p.emplace_back(static_cast<float>(v.x), static_cast<float>(v.y));
    p = cleaned(p);
    if (p.size() >= 3) out.push_back(std::move(p));
  }
  // Orient by nesting so callers get outer CCW / holes CW.
  std::vector<Polygon> oriented;
  for (auto& r : buildRegions(out)) {
    oriented.push_back(r.outer);
    for (auto& h : r.holes) oriented.push_back(h);
  }
  return oriented;
}

}  // namespace

float signedArea(const Polygon& p) {
  double a = 0;
  size_t n = p.size();
  for (size_t i = 0; i < n; ++i) {
    const Vec2& u = p[i];
    const Vec2& v = p[(i + 1) % n];
    a += static_cast<double>(u.x) * v.y - static_cast<double>(v.x) * u.y;
  }
  return static_cast<float>(a * 0.5);
}

bool pointInPolygon(const Vec2& pt, const Polygon& poly) {
  bool inside = false;
  size_t n = poly.size();
  for (size_t i = 0, j = n - 1; i < n; j = i++) {
    const Vec2& a = poly[i];
    const Vec2& b = poly[j];
    if ((a.y > pt.y) != (b.y > pt.y)) {
      float x = (b.x - a.x) * (pt.y - a.y) / (b.y - a.y) + a.x;
      if (pt.x < x) inside = !inside;
    }
  }
  return inside;
}

Polygon cleaned(const Polygon& p, float eps) {
  Polygon out;
  out.reserve(p.size());
  for (const auto& v : p) {
    if (!out.empty() && std::fabs(out.back().x - v.x) <= eps && std::fabs(out.back().y - v.y) <= eps) continue;
    out.push_back(v);
  }
  while (out.size() > 1 && std::fabs(out.front().x - out.back().x) <= eps &&
         std::fabs(out.front().y - out.back().y) <= eps)
    out.pop_back();
  return out;
}

std::vector<Region> buildRegions(const std::vector<Polygon>& contoursIn, std::vector<std::vector<int>>* contourIndex) {
  std::vector<Polygon> contours;
  std::vector<int> srcIndex;
  for (size_t i = 0; i < contoursIn.size(); ++i) {
    Polygon c = cleaned(contoursIn[i]);
    if (c.size() < 3 || std::fabs(signedArea(c)) < 1e-12f) continue;
    contours.push_back(std::move(c));
    srcIndex.push_back(static_cast<int>(i));
  }
  size_t n = contours.size();
  std::vector<float> area(n);
  for (size_t i = 0; i < n; ++i) area[i] = std::fabs(signedArea(contours[i]));
  // Depth and innermost container of each contour.
  std::vector<int> depth(n, 0), parent(n, -1);
  for (size_t i = 0; i < n; ++i) {
    // A point just inside contour i: midpoint of its first edge nudged inward is unreliable for
    // tiny contours, so test all vertices and use the majority result per container.
    for (size_t j = 0; j < n; ++j) {
      if (i == j || area[j] <= area[i]) continue;
      int votes = 0, total = 0;
      for (size_t k = 0; k < contours[i].size(); k += std::max<size_t>(1, contours[i].size() / 8)) {
        votes += pointInPolygon(contours[i][k], contours[j]) ? 1 : 0;
        ++total;
      }
      if (votes * 2 > total) {
        ++depth[i];
        if (parent[i] < 0 || area[j] < area[parent[i]]) parent[i] = static_cast<int>(j);
      }
    }
  }
  std::vector<Region> regions;
  std::vector<int> regionOf(n, -1);
  for (size_t i = 0; i < n; ++i) {
    if (depth[i] % 2 != 0) continue;
    Region r;
    r.outer = contours[i];
    if (signedArea(r.outer) < 0) std::reverse(r.outer.begin(), r.outer.end());
    regionOf[i] = static_cast<int>(regions.size());
    regions.push_back(std::move(r));
    if (contourIndex) contourIndex->push_back({srcIndex[i]});
  }
  for (size_t i = 0; i < n; ++i) {
    if (depth[i] % 2 == 0 || parent[i] < 0 || regionOf[parent[i]] < 0) continue;
    Polygon h = contours[i];
    if (signedArea(h) > 0) std::reverse(h.begin(), h.end());
    regions[regionOf[parent[i]]].holes.push_back(std::move(h));
    if (contourIndex) (*contourIndex)[regionOf[parent[i]]].push_back(srcIndex[i]);
  }
  return regions;
}

std::vector<uint32_t> triangulate(const Region& region) {
  using Point = std::array<double, 2>;
  std::vector<std::vector<Point>> poly;
  std::vector<Vec2> flat;
  auto add = [&](const Polygon& p) {
    std::vector<Point> ring;
    for (const auto& v : p) {
      ring.push_back({v.x, v.y});
      flat.push_back(v);
    }
    poly.push_back(std::move(ring));
  };
  add(region.outer);
  for (const auto& h : region.holes) add(h);
  std::vector<uint32_t> idx = mapbox::earcut<uint32_t>(poly);
  for (size_t t = 0; t + 2 < idx.size(); t += 3) {
    const Vec2 &a = flat[idx[t]], &b = flat[idx[t + 1]], &c = flat[idx[t + 2]];
    float cr = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (cr < 0) std::swap(idx[t + 1], idx[t + 2]);
  }
  return idx;
}

std::vector<Polygon> boolean(const std::vector<Polygon>& a, const std::vector<Polygon>& b, BoolOp op) {
  using namespace Clipper2Lib;
  ClipType ct = op == BoolOp::Union ? ClipType::Union : op == BoolOp::Subtract ? ClipType::Difference
                                                                               : ClipType::Intersection;
  return fromPaths(BooleanOp(ct, FillRule::NonZero, toPaths(a), toPaths(b), kClipPrecision));
}

std::vector<Polygon> offset(const std::vector<Polygon>& polys, float delta, bool joinRound) {
  using namespace Clipper2Lib;
  // Normalize orientation first so "outward" is consistent for outers and holes.
  std::vector<Polygon> oriented;
  for (auto& r : buildRegions(polys)) {
    oriented.push_back(r.outer);
    for (auto& h : r.holes) oriented.push_back(h);
  }
  PathsD res = InflatePaths(toPaths(oriented), delta, joinRound ? JoinType::Round : JoinType::Miter,
                            EndType::Polygon, 4.0, kClipPrecision);
  return fromPaths(res);
}

std::vector<Polygon> clip(const Polygon& subject, const std::vector<Polygon>& clipPolys, bool keepInside) {
  using namespace Clipper2Lib;
  PathsD s = toPaths({subject});
  PathsD c = toPaths(clipPolys);
  PathsD r = BooleanOp(keepInside ? ClipType::Intersection : ClipType::Difference, FillRule::NonZero, s, c,
                       kClipPrecision);
  std::vector<Polygon> out;
  for (const auto& path : r) {
    Polygon p;
    for (const auto& v : path) p.emplace_back(static_cast<float>(v.x), static_cast<float>(v.y));
    p = cleaned(p);
    if (p.size() >= 3) out.push_back(std::move(p));
  }
  return out;
}

bool segmentIntersect(const Vec2& a0, const Vec2& a1, const Vec2& b0, const Vec2& b1, float& ta, float& tb) {
  Vec2 r = a1 - a0, s = b1 - b0;
  float denom = r.x * s.y - r.y * s.x;
  if (std::fabs(denom) < 1e-12f) return false;
  Vec2 q = b0 - a0;
  ta = (q.x * s.y - q.y * s.x) / denom;
  tb = (q.x * r.y - q.y * r.x) / denom;
  return ta >= 0.0f && ta <= 1.0f && tb >= 0.0f && tb <= 1.0f;
}

std::vector<uint32_t> delaunay(const std::vector<Vec2>& pts) {
  struct Tri {
    uint32_t a, b, c;
    double cx, cy, r2;
  };
  size_t n = pts.size();
  std::vector<uint32_t> out;
  if (n < 3) return out;
  double minX = pts[0].x, minY = pts[0].y, maxX = minX, maxY = minY;
  for (const auto& p : pts) {
    minX = std::min<double>(minX, p.x); maxX = std::max<double>(maxX, p.x);
    minY = std::min<double>(minY, p.y); maxY = std::max<double>(maxY, p.y);
  }
  double d = std::max(maxX - minX, maxY - minY) * 20.0 + 1.0;
  double mx = (minX + maxX) * 0.5, my = (minY + maxY) * 0.5;
  std::vector<std::array<double, 2>> P;
  for (const auto& p : pts) P.push_back({p.x, p.y});
  P.push_back({mx - d, my - d});
  P.push_back({mx + d, my - d});
  P.push_back({mx, my + d});
  auto circum = [&](uint32_t a, uint32_t b, uint32_t c) {
    Tri t{a, b, c, 0, 0, 0};
    double ax = P[a][0], ay = P[a][1], bx = P[b][0], by = P[b][1], cx = P[c][0], cy = P[c][1];
    double D = 2 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
    if (std::fabs(D) < 1e-18) D = 1e-18;
    double a2 = ax * ax + ay * ay, b2 = bx * bx + by * by, c2 = cx * cx + cy * cy;
    t.cx = (a2 * (by - cy) + b2 * (cy - ay) + c2 * (ay - by)) / D;
    t.cy = (a2 * (cx - bx) + b2 * (ax - cx) + c2 * (bx - ax)) / D;
    t.r2 = (ax - t.cx) * (ax - t.cx) + (ay - t.cy) * (ay - t.cy);
    return t;
  };
  std::vector<Tri> tris = {circum(static_cast<uint32_t>(n), static_cast<uint32_t>(n + 1), static_cast<uint32_t>(n + 2))};
  for (uint32_t i = 0; i < n; ++i) {
    double px = P[i][0], py = P[i][1];
    std::map<std::pair<uint32_t, uint32_t>, int> edges;
    std::vector<Tri> keep;
    keep.reserve(tris.size());
    for (const Tri& t : tris) {
      double dx = px - t.cx, dy = py - t.cy;
      if (dx * dx + dy * dy <= t.r2 * (1.0 + 1e-12)) {
        for (auto e : {std::make_pair(t.a, t.b), std::make_pair(t.b, t.c), std::make_pair(t.c, t.a)}) {
          auto key = std::minmax(e.first, e.second);
          edges[{key.first, key.second}]++;
        }
      } else {
        keep.push_back(t);
      }
    }
    for (const auto& [e, count] : edges)
      if (count == 1) keep.push_back(circum(e.first, e.second, i));
    tris = std::move(keep);
  }
  for (const Tri& t : tris) {
    if (t.a >= n || t.b >= n || t.c >= n) continue;
    uint32_t a = t.a, b = t.b, c = t.c;
    double cr = (P[b][0] - P[a][0]) * (P[c][1] - P[a][1]) - (P[b][1] - P[a][1]) * (P[c][0] - P[a][0]);
    if (cr < 0) std::swap(b, c);
    out.insert(out.end(), {a, b, c});
  }
  return out;
}

}  // namespace veraa::shape2d
