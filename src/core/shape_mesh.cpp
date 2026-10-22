#include "shape_mesh.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <set>
#include <utility>

#include "object.h"
#include "shape2d.h"

namespace veraa {

using shape2d::Polygon;
using shape2d::Region;

namespace {

Vec2 normalize2(const Vec2& v) {
  float l = std::sqrt(v.x * v.x + v.y * v.y);
  return l > 1e-20f ? v / l : Vec2(1, 0);
}

float len2d(const Vec2& v) { return std::sqrt(v.x * v.x + v.y * v.y); }

// Sharp-corner smoothing: assigns each edge of a contour a run id; runs break at corners sharper
// than the threshold. Returns per-edge bit index in {0, 1, 2}, adjacent runs always differ.
std::vector<int> edgeSmoothBits(const std::vector<Vec2>& pts, bool closed, float sharpDeg) {
  size_t n = pts.size();
  size_t edges = closed ? n : (n > 0 ? n - 1 : 0);
  std::vector<int> bits(edges, 0);
  if (edges == 0) return bits;
  float cosLimit = std::cos(radians(sharpDeg));
  auto sharpAt = [&](size_t i) {  // vertex i between edge i-1 and edge i
    if (!closed && (i == 0 || i >= n - 1)) return false;
    Vec2 a = normalize2(pts[i] - pts[(i + n - 1) % n]);
    Vec2 b = normalize2(pts[(i + 1) % n] - pts[i]);
    return a.x * b.x + a.y * b.y < cosLimit;
  };
  // Start counting at the first sharp vertex so a closed contour's runs don't wrap mid-run.
  size_t start = 0;
  if (closed)
    for (size_t i = 0; i < n; ++i)
      if (sharpAt(i)) {
        start = i;
        break;
      }
  std::vector<int> runOfEdge(edges, 0);
  int run = 0;
  for (size_t k = 0; k < edges; ++k) {
    size_t e = (start + k) % edges;
    if (k > 0 && sharpAt(e)) ++run;
    runOfEdge[e] = run;
  }
  int runs = run + 1;
  std::vector<int> runBit(runs, 0);
  for (int r = 0; r < runs; ++r) {
    int prev = r > 0 ? runBit[r - 1] : -1;
    int first = (closed && r == runs - 1 && runs > 1) ? runBit[0] : -1;
    for (int b = 0; b < 3; ++b)
      if (b != prev && b != first) {
        runBit[r] = b;
        break;
      }
  }
  for (size_t e = 0; e < edges; ++e) bits[e] = runBit[runOfEdge[e]];
  return bits;
}

std::vector<float> cumulativeLengths(const std::vector<Vec2>& pts, bool closed) {
  std::vector<float> u(pts.size() + 1, 0.0f);
  for (size_t i = 1; i <= pts.size(); ++i) {
    if (i == pts.size() && !closed) {
      u[i] = u[i - 1];
      break;
    }
    u[i] = u[i - 1] + len2d(pts[i % pts.size()] - pts[i - 1]);
  }
  float total = u.back() > 0 ? u.back() : 1.0f;
  for (auto& x : u) x /= total;
  return u;
}

// Region with the contour index of every part (outer first, then holes).
struct IndexedRegion {
  Region region;
  std::vector<int> parts;
};

std::vector<IndexedRegion> closedRegions(const std::vector<Contour2D>& contours) {
  std::vector<Polygon> polys;
  std::vector<int> owner;
  for (size_t i = 0; i < contours.size(); ++i)
    if (contours[i].closed && contours[i].pts.size() >= 3) {
      polys.push_back(contours[i].pts);
      owner.push_back(static_cast<int>(i));
    }
  std::vector<std::vector<int>> idx;
  std::vector<Region> regions = shape2d::buildRegions(polys, &idx);
  std::vector<IndexedRegion> out;
  for (size_t r = 0; r < regions.size(); ++r) {
    IndexedRegion ir;
    ir.region = regions[r];
    for (int k : idx[r]) ir.parts.push_back(owner[k]);
    out.push_back(std::move(ir));
  }
  return out;
}

float segmentDistance(const Vec2& p, const Vec2& a, const Vec2& b) {
  Vec2 ab = b - a, ap = p - a;
  float l2 = ab.x * ab.x + ab.y * ab.y;
  float t = l2 > 0 ? clampf((ap.x * ab.x + ap.y * ab.y) / l2, 0.0f, 1.0f) : 0.0f;
  return len2d(p - (a + ab * t));
}

// Grid capping: Delaunay triangulation of the region's boundary points plus interior points on a
// square grid. Indices >= the boundary point count refer to `interior`. Returns false when the
// triangulation doesn't reproduce every boundary edge (very narrow parts); callers then fall
// back to plain capping.
bool gridTriangulate(const Region& r, float grid, std::vector<Vec2>& interior, std::vector<uint32_t>& tris) {
  interior.clear();
  tris.clear();
  std::vector<Vec2> pts;
  std::vector<std::pair<uint32_t, uint32_t>> edges;
  auto addPoly = [&](const Polygon& poly) {
    uint32_t b = static_cast<uint32_t>(pts.size()), n = static_cast<uint32_t>(poly.size());
    for (uint32_t k = 0; k < n; ++k) {
      pts.push_back(poly[k]);
      edges.emplace_back(b + k, b + (k + 1) % n);
    }
  };
  addPoly(r.outer);
  for (const auto& h : r.holes) addPoly(h);
  double regionArea = shape2d::signedArea(r.outer);
  for (const auto& h : r.holes) regionArea += shape2d::signedArea(h);
  if (regionArea <= 0 || grid <= 0) return false;
  // Keep the point count bounded (the triangulation is quadratic).
  grid = std::max(grid, static_cast<float>(std::sqrt(regionArea / 2500.0)));
  auto inside = [&](const Vec2& p) {
    if (!shape2d::pointInPolygon(p, r.outer)) return false;
    for (const auto& h : r.holes)
      if (shape2d::pointInPolygon(p, h)) return false;
    return true;
  };
  Vec2 lo = r.outer[0], hi = r.outer[0];
  for (const auto& p : r.outer) {
    lo = Vec2(std::min(lo.x, p.x), std::min(lo.y, p.y));
    hi = Vec2(std::max(hi.x, p.x), std::max(hi.y, p.y));
  }
  for (float y = std::ceil(lo.y / grid) * grid; y < hi.y; y += grid)
    for (float x = std::ceil(lo.x / grid) * grid; x < hi.x; x += grid) {
      Vec2 p(x, y);
      if (!inside(p)) continue;
      float d = 1e30f;
      for (const auto& e : edges) d = std::min(d, segmentDistance(p, pts[e.first], pts[e.second]));
      if (d >= 0.6f * grid) interior.push_back(p);
    }
  if (interior.empty()) return false;
  std::vector<Vec2> all = pts;
  all.insert(all.end(), interior.begin(), interior.end());
  std::vector<uint32_t> dt = shape2d::delaunay(all);
  std::set<std::pair<uint32_t, uint32_t>> directed;
  double area = 0;
  for (size_t i = 0; i + 2 < dt.size(); i += 3) {
    const Vec2& a = all[dt[i]];
    const Vec2& b = all[dt[i + 1]];
    const Vec2& c = all[dt[i + 2]];
    if (!inside((a + b + c) / 3.0f)) continue;
    double ar = 0.5 * ((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y));
    if (ar <= 0) return false;
    area += ar;
    tris.insert(tris.end(), {dt[i], dt[i + 1], dt[i + 2]});
    directed.emplace(dt[i], dt[i + 1]);
    directed.emplace(dt[i + 1], dt[i + 2]);
    directed.emplace(dt[i + 2], dt[i]);
  }
  for (const auto& e : edges)
    if (!directed.count(e)) return false;
  return std::fabs(area - regionArea) <= 1e-3 * regionArea;
}

// Adds cap faces for regions using per-contour vertex index bases and 2D positions. With
// grid > 0 and newVertex set, caps get interior grid vertices (Grid capping) where possible.
void addCaps(PolyMesh& m, const std::vector<Contour2D>& contours, const std::vector<IndexedRegion>& regions,
             const std::function<int(int contour, int point)>& vertexOf,
             const std::function<Vec2(int contour, int point)>& pos2d, bool reverse, int matId, uint32_t smooth,
             bool uvs, const Aabb& box, float grid = 0.0f,
             const std::function<int(const Vec2&)>& newVertex = std::function<int(const Vec2&)>()) {
  Vec3 ext = box.extent();
  float sz = std::max(ext.x, ext.y);
  if (sz <= 0) sz = 1;
  auto uvOf = [&](const Vec2& p) { return Vec2((p.x - box.lo.x) / sz, (p.y - box.lo.y) / sz); };
  for (const auto& ir : regions) {
    if (grid > 0 && newVertex) {
      Region r;
      std::vector<std::pair<int, int>> flat;
      for (size_t part = 0; part < ir.parts.size(); ++part) {
        int c = ir.parts[part];
        Polygon poly;
        for (int k = 0; k < static_cast<int>(contours[c].pts.size()); ++k) {
          poly.push_back(pos2d(c, k));
          flat.emplace_back(c, k);
        }
        if (part == 0) r.outer = std::move(poly);
        else r.holes.push_back(std::move(poly));
      }
      std::vector<Vec2> interior;
      std::vector<uint32_t> tris;
      if (gridTriangulate(r, grid, interior, tris)) {
        std::vector<int> inner;
        for (const auto& p : interior) inner.push_back(newVertex(p));
        auto posOf = [&](uint32_t i) {
          return i < flat.size() ? pos2d(flat[i].first, flat[i].second) : interior[i - flat.size()];
        };
        auto vertOf = [&](uint32_t i) {
          return i < flat.size() ? vertexOf(flat[i].first, flat[i].second) : inner[i - flat.size()];
        };
        for (size_t i = 0; i + 2 < tris.size(); i += 3) {
          std::vector<int> v, t;
          for (int k = 0; k < 3; ++k) {
            v.push_back(vertOf(tris[i + k]));
            if (uvs) t.push_back(m.addUv(uvOf(posOf(tris[i + k]))));
          }
          if (reverse) {
            std::swap(v[1], v[2]);
            if (uvs) std::swap(t[1], t[2]);
          }
          m.addFace(std::move(v), std::move(t), smooth, matId);
        }
        continue;
      }
    }
    if (ir.region.holes.empty()) {
      int c = ir.parts[0];
      int n = static_cast<int>(contours[c].pts.size());
      std::vector<int> v, t;
      for (int k = 0; k < n; ++k) {
        v.push_back(vertexOf(c, k));
        if (uvs) t.push_back(m.addUv(uvOf(pos2d(c, k))));
      }
      if (reverse) {
        std::reverse(v.begin(), v.end());
        std::reverse(t.begin(), t.end());
      }
      m.addFace(std::move(v), std::move(t), smooth, matId);
      continue;
    }
    // Triangulate this cap's actual 2D positions (they may be offset from the contours).
    Region r;
    std::vector<std::pair<int, int>> flat;  // (contour, point) per region vertex
    for (size_t part = 0; part < ir.parts.size(); ++part) {
      int c = ir.parts[part];
      Polygon poly;
      for (int k = 0; k < static_cast<int>(contours[c].pts.size()); ++k) {
        poly.push_back(pos2d(c, k));
        flat.emplace_back(c, k);
      }
      if (part == 0) r.outer = std::move(poly);
      else r.holes.push_back(std::move(poly));
    }
    std::vector<uint32_t> tris = shape2d::triangulate(r);
    for (size_t i = 0; i + 2 < tris.size(); i += 3) {
      std::vector<int> v, t;
      for (int k = 0; k < 3; ++k) {
        auto [c, pidx] = flat[tris[i + k]];
        v.push_back(vertexOf(c, pidx));
        if (uvs) t.push_back(m.addUv(uvOf(pos2d(c, pidx))));
      }
      if (reverse) {
        std::swap(v[1], v[2]);
        if (uvs) std::swap(t[1], t[2]);
      }
      m.addFace(std::move(v), std::move(t), smooth, matId);
    }
  }
}

Aabb contoursBounds(const std::vector<Contour2D>& contours) {
  Aabb b;
  for (const auto& c : contours)
    for (const auto& p : c.pts) b.expand(Vec3(p.x, p.y, 0.0f));
  return b;
}

}  // namespace

std::vector<Contour2D> shapeContours(const BezierShape& shape, int steps, bool optimize) {
  std::vector<Contour2D> out;
  for (const auto& s : shape.splines) {
    std::vector<Vec3> pts3 = s.polyline(steps, optimize);
    Contour2D c;
    for (const auto& p : pts3) c.pts.emplace_back(p.x, p.y);
    c.pts = shape2d::cleaned(c.pts);
    c.closed = s.closed && c.pts.size() >= 3;
    if (c.pts.size() >= 2) out.push_back(std::move(c));
  }
  // Orient closed contours by nesting.
  std::vector<IndexedRegion> regions = closedRegions(out);
  std::vector<bool> keep(out.size(), true);
  for (size_t i = 0; i < out.size(); ++i)
    if (out[i].closed) keep[i] = false;
  for (const auto& ir : regions)
    for (size_t part = 0; part < ir.parts.size(); ++part) {
      int c = ir.parts[part];
      out[c].pts = part == 0 ? ir.region.outer : ir.region.holes[part - 1];
      keep[c] = true;
    }
  std::vector<Contour2D> kept;
  for (size_t i = 0; i < out.size(); ++i)
    if (keep[i]) kept.push_back(std::move(out[i]));
  return kept;
}

std::vector<Contour2D> resampledContours(const std::vector<Contour2D>& in, float maxEdge) {
  if (maxEdge <= 0) return in;
  std::vector<Contour2D> out;
  for (const auto& c : in) {
    if (!c.closed) {
      out.push_back(c);
      continue;
    }
    Contour2D r;
    r.closed = true;
    size_t n = c.pts.size();
    for (size_t i = 0; i < n; ++i) {
      Vec2 a = c.pts[i], b = c.pts[(i + 1) % n];
      int parts = std::max(1, static_cast<int>(std::ceil(len2d(b - a) / maxEdge - 1e-4f)));
      parts = std::min(parts, 256);
      for (int k = 0; k < parts; ++k) r.pts.push_back(a + (b - a) * (static_cast<float>(k) / parts));
    }
    out.push_back(std::move(r));
  }
  return out;
}

PolyMesh extrudeContours(const std::vector<Contour2D>& contoursIn, const std::vector<ExtrudeLevel>& levels,
                         const ExtrudeOptions& opt) {
  PolyMesh m;
  if (levels.size() < 2 || contoursIn.empty()) return m;
  // Grid capping needs boundary edges no longer than the grid spacing.
  const std::vector<Contour2D> contours =
      opt.gridCapSize > 0 ? resampledContours(contoursIn, opt.gridCapSize) : contoursIn;
  int L = static_cast<int>(levels.size());
  std::vector<int> base(contours.size(), 0);
  Aabb box = contoursBounds(contours);
  float zTotal = levels.back().z - levels.front().z;
  if (std::fabs(zTotal) < 1e-12f) zTotal = 1.0f;

  // Offset directions per contour vertex (miter-scaled outward normals).
  std::vector<std::vector<Vec2>> offsetDir(contours.size());
  for (size_t c = 0; c < contours.size(); ++c) {
    const auto& pts = contours[c].pts;
    size_t n = pts.size();
    bool closed = contours[c].closed;
    auto edgeN = [&](size_t i) {  // right-hand normal of edge i -> i+1
      Vec2 d = normalize2(pts[(i + 1) % n] - pts[i]);
      return Vec2(d.y, -d.x);
    };
    offsetDir[c].resize(n);
    for (size_t i = 0; i < n; ++i) {
      bool hasPrev = closed || i > 0, hasNext = closed || i + 1 < n;
      Vec2 np = hasPrev ? edgeN((i + n - 1) % n) : edgeN(i);
      Vec2 nn = hasNext ? edgeN(i) : np;
      Vec2 v = normalize2(np + nn);
      float cosA = v.x * nn.x + v.y * nn.y;
      offsetDir[c][i] = v / std::max(0.3f, cosA);
    }
  }

  for (size_t c = 0; c < contours.size(); ++c) {
    const auto& pts = contours[c].pts;
    int n = static_cast<int>(pts.size());
    base[c] = static_cast<int>(m.positions.size());
    for (int l = 0; l < L; ++l)
      for (int i = 0; i < n; ++i) {
        Vec2 p = pts[i] + offsetDir[c][i] * levels[l].outline;
        m.addVertex(Vec3(p.x, p.y, levels[l].z));
      }
    bool closed = contours[c].closed;
    int edges = closed ? n : n - 1;
    std::vector<int> bits = edgeSmoothBits(pts, closed, opt.sharpAngleDeg);
    std::vector<float> u = cumulativeLengths(pts, closed);
    int uvBase = static_cast<int>(m.uvs.size());
    if (opt.generateUVs)
      for (int l = 0; l < L; ++l)
        for (int i = 0; i <= n; ++i) m.addUv(Vec2(u[i], (levels[l].z - levels.front().z) / zTotal));
    auto V = [&](int l, int i) { return base[c] + l * n + (i % n); };
    auto T = [&](int l, int i) { return uvBase + l * (n + 1) + i; };
    for (int l = 0; l + 1 < L; ++l) {
      if (levels[l].z == levels[l + 1].z && levels[l].outline == levels[l + 1].outline) continue;
      int spanShift = opt.smoothAcrossLevels ? 0 : 3 * (l % 3);
      for (int i = 0; i < edges; ++i) {
        std::vector<int> v = {V(l, i), V(l, i + 1), V(l + 1, i + 1), V(l + 1, i)};
        std::vector<int> t;
        if (opt.generateUVs) t = std::vector<int>{T(l, i), T(l, i + 1), T(l + 1, i + 1), T(l + 1, i)};
        m.addFace(std::move(v), std::move(t), 1u << (bits[i] + spanShift), opt.sideMatId);
      }
    }
  }

  bool up = levels.back().z >= levels.front().z;
  if (!up) {
    // Walls were built for an upward extrusion; a downward one flips them (caps come later).
    for (auto& f : m.faces) {
      std::reverse(f.v.begin(), f.v.end());
      std::reverse(f.uv.begin(), f.uv.end());
    }
  }
  std::vector<IndexedRegion> regions = closedRegions(contours);
  auto capAt = [&](int l, bool reverse, int matId, uint32_t sg) {
    auto vertexOf = [&](int c, int k) { return base[c] + l * static_cast<int>(contours[c].pts.size()) + k; };
    auto pos2d = [&](int c, int k) { return contours[c].pts[k] + offsetDir[c][k] * levels[l].outline; };
    float z = levels[l].z;
    auto newVertex = [&m, z](const Vec2& p) { return m.addVertex(Vec3(p.x, p.y, z)); };
    addCaps(m, contours, regions, vertexOf, pos2d, reverse, matId, sg, opt.generateUVs, box, opt.gridCapSize,
            newVertex);
  };
  if (opt.capStart) capAt(0, up, opt.capStartMatId, 1u << 20);
  if (opt.capEnd) capAt(L - 1, !up, opt.capEndMatId, 1u << 21);
  return m;
}

PolyMesh latheContours(const std::vector<Contour2D>& contours, const LatheOptions& opt) {
  PolyMesh m;
  if (contours.empty()) return m;
  Aabb box = contoursBounds(contours);
  Vec3 D = opt.axis == 0 ? Vec3(1, 0, 0) : opt.axis == 1 ? Vec3(0, 1, 0) : Vec3(0, 0, 1);
  auto pick = [&](float lo, float hi) { return opt.align == 0 ? lo : opt.align == 2 ? hi : (lo + hi) * 0.5f; };
  Vec3 A;
  if (opt.axis == 1) A = Vec3(pick(box.lo.x, box.hi.x), 0, 0);
  else if (opt.axis == 0) A = Vec3(0, pick(box.lo.y, box.hi.y), 0);
  else A = Vec3(pick(box.lo.x, box.hi.x), box.center().y, 0);
  float size = std::max(box.extent().x, box.extent().y);
  float eps = (opt.weldCore ? 1e-2f : 1e-5f) * std::max(size, 1e-3f);
  float deg = clampf(opt.degrees, -360.0f, 360.0f);
  bool full = std::fabs(deg) >= 359.999f;
  int segs = std::max(full ? 3 : 1, opt.segments);
  int rings = full ? segs : segs + 1;

  std::vector<int> base(contours.size());
  auto revolve = [&](const Vec2& p2, int k) {
    Vec3 d = Vec3(p2.x, p2.y, 0.0f) - A;
    float along = dot(d, D);
    Vec3 radial = d - D * along;
    if (length(radial) < eps) return A + D * along;
    float ang = radians(deg) * static_cast<float>(k) / segs;
    return A + Mat4::rotateAxis(D, ang).transformDir(d);
  };
  for (size_t c = 0; c < contours.size(); ++c) {
    const auto& pts = contours[c].pts;
    int n = static_cast<int>(pts.size());
    base[c] = static_cast<int>(m.positions.size());
    for (int k = 0; k < rings; ++k)
      for (int i = 0; i < n; ++i) m.addVertex(revolve(pts[i], k));
    bool closed = contours[c].closed;
    int edges = closed ? n : n - 1;
    std::vector<int> bits = edgeSmoothBits(pts, closed, 30.0f);
    std::vector<float> v = cumulativeLengths(pts, closed);
    int uvBase = static_cast<int>(m.uvs.size());
    if (opt.generateUVs)
      for (int k = 0; k <= segs; ++k)
        for (int i = 0; i <= n; ++i) m.addUv(Vec2(static_cast<float>(k) / segs, v[i]));
    for (int k = 0; k < segs; ++k) {
      int kn = full ? (k + 1) % segs : k + 1;
      for (int i = 0; i < edges; ++i) {
        int i1 = (i + 1) % n;
        std::vector<int> f = {base[c] + k * n + i, base[c] + k * n + i1, base[c] + kn * n + i1, base[c] + kn * n + i};
        std::vector<int> t;
        if (opt.generateUVs) {
          int ub = uvBase;
          t = std::vector<int>{ub + k * (n + 1) + i, ub + k * (n + 1) + i + 1, ub + (k + 1) * (n + 1) + i + 1, ub + (k + 1) * (n + 1) + i};
        }
        m.addFace(std::move(f), std::move(t), 1u << bits[i], 2);
      }
    }
  }
  // Orient walls: closed full revolutions by volume, otherwise away from the axis.
  bool allClosed = std::all_of(contours.begin(), contours.end(), [](const Contour2D& c) { return c.closed; });
  {
    PolyMesh tmp = m;
    weldVertices(tmp);
    double score = 0;
    if (full && allClosed) {
      TriMesh t = triangulate(tmp);
      for (size_t i = 0; i + 2 < t.indices.size(); i += 3)
        score += dot(t.positions[t.indices[i]], cross(t.positions[t.indices[i + 1]], t.positions[t.indices[i + 2]]));
    } else {
      for (const auto& f : tmp.faces) {
        Vec3 c;
        for (int vi : f.v) c += tmp.positions[vi];
        c /= static_cast<float>(f.v.size());
        Vec3 r = (c - A) - D * dot(c - A, D);
        score += dot(tmp.faceNormal(f), r);
      }
    }
    if (score < 0) m.flipFaces();
  }
  // End caps for partial revolutions of closed profiles.
  if (!full) {
    std::vector<IndexedRegion> regions = closedRegions(contours);
    Vec3 sweepAxis = D * (deg >= 0 ? 1.0f : -1.0f);
    auto cap = [&](int ring, bool wantForward, int matId, uint32_t sg) {
      size_t first = m.faces.size();
      auto vertexOf = [&](int c, int k) { return base[c] + ring * static_cast<int>(contours[c].pts.size()) + k; };
      auto pos2d = [&](int c, int k) { return contours[c].pts[k]; };
      addCaps(m, contours, regions, vertexOf, pos2d, false, matId, sg, opt.generateUVs, box);
      for (size_t f = first; f < m.faces.size(); ++f) {
        auto& face = m.faces[f];
        Vec3 c;
        for (int vi : face.v) c += m.positions[vi];
        c /= static_cast<float>(face.v.size());
        Vec3 tangent = cross(sweepAxis, c - A);
        bool forward = dot(m.faceNormal(face), tangent) > 0;
        if (forward != wantForward) {
          std::reverse(face.v.begin(), face.v.end());
          std::reverse(face.uv.begin(), face.uv.end());
        }
      }
    };
    if (opt.capStart) cap(0, false, 0, 1u << 20);
    if (opt.capEnd) cap(segs, true, 1, 1u << 21);
  }
  weldVertices(m);
  if (opt.flipNormals) m.flipFaces();
  return m;
}

PolyMesh sweepSection(const BezierShape& path, int pathSteps, bool pathOptimize, const std::vector<Contour2D>& sectionIn,
                      const SweepOptions& opt) {
  PolyMesh m;
  if (sectionIn.empty()) return m;
  // Place the section: pivot alignment, mirror, rotation and offset.
  std::vector<Contour2D> section = sectionIn;
  Aabb sb = contoursBounds(section);
  int col = opt.pivotAlign % 3, row = opt.pivotAlign / 3;
  Vec2 anchor(col == 0 ? sb.lo.x : col == 2 ? sb.hi.x : (sb.lo.x + sb.hi.x) * 0.5f,
              row == 0 ? sb.lo.y : row == 2 ? sb.hi.y : (sb.lo.y + sb.hi.y) * 0.5f);
  float ca = std::cos(radians(opt.angleDeg)), sa = std::sin(radians(opt.angleDeg));
  bool mirrored = opt.mirrorX != opt.mirrorY;
  for (auto& c : section) {
    for (auto& p : c.pts) {
      Vec2 q = p - anchor;
      if (opt.mirrorX) q.x = -q.x;
      if (opt.mirrorY) q.y = -q.y;
      p = Vec2(q.x * ca - q.y * sa, q.x * sa + q.y * ca) + opt.offset;
    }
    if (mirrored) std::reverse(c.pts.begin(), c.pts.end());
  }
  std::vector<IndexedRegion> regions = closedRegions(section);
  Aabb box = contoursBounds(section);

  for (const Spline& sp : path.splines) {
    PathFrames F = pathFrames(sp.polyline(pathSteps, pathOptimize), sp.closed, opt.banking);
    int M = static_cast<int>(F.P.size());
    if (M < 2) continue;
    bool closed = F.closed;
    int segCount = F.segments();
    std::vector<int> base(section.size());
    for (size_t c = 0; c < section.size(); ++c) {
      const auto& sec = section[c].pts;
      int n = static_cast<int>(sec.size());
      base[c] = static_cast<int>(m.positions.size());
      for (int i = 0; i < M; ++i)
        for (int k = 0; k < n; ++k) m.addVertex(F.place(i, sec[k]));
      bool secClosed = section[c].closed;
      int edges = secClosed ? n : n - 1;
      std::vector<int> bits = edgeSmoothBits(sec, secClosed, opt.smoothSection ? 30.0f : 0.0f);
      // Without path smoothing every path segment gets its own bit (adjacent ones differ).
      auto pathBit = [&](int i) {
        if (opt.smoothPath) return 0;
        if (closed && segCount % 2 == 1 && i == segCount - 1) return 2;
        return i % 2;
      };
      std::vector<float> u = cumulativeLengths(sec, secClosed);
      float perim = 0;
      for (int k = 0; k < edges; ++k) perim += len2d(sec[(k + 1) % n] - sec[k]);
      float vScale = perim > 0 ? 1.0f / perim : 1.0f;
      int uvBase = static_cast<int>(m.uvs.size());
      int ringsUv = closed ? M + 1 : M;
      if (opt.generateUVs)
        for (int i = 0; i < ringsUv; ++i) {
          float vv = (i < M ? F.dist[i] : F.length) * vScale;
          for (int k = 0; k <= n; ++k) m.addUv(Vec2(u[k], vv));
        }
      for (int i = 0; i < segCount; ++i) {
        int i1 = (i + 1) % M;
        for (int k = 0; k < edges; ++k) {
          int k1 = (k + 1) % n;
          std::vector<int> f = {base[c] + i1 * n + k, base[c] + i1 * n + k1, base[c] + i * n + k1, base[c] + i * n + k};
          std::vector<int> t;
          if (opt.generateUVs)
            t = std::vector<int>{uvBase + (i + 1) * (n + 1) + k, uvBase + (i + 1) * (n + 1) + k + 1,
                                 uvBase + i * (n + 1) + k + 1, uvBase + i * (n + 1) + k};
          m.addFace(std::move(f), std::move(t), 1u << (bits[k] * 3 + pathBit(i)), 2);
        }
      }
    }
    if (!closed && opt.capEnds) {
      auto cap = [&](int ring, bool reverse, int matId, uint32_t sg) {
        auto vertexOf = [&](int c, int k) { return base[c] + ring * static_cast<int>(section[c].pts.size()) + k; };
        auto pos2d = [&](int c, int k) { return section[c].pts[k]; };
        addCaps(m, section, regions, vertexOf, pos2d, reverse, matId, sg, opt.generateUVs, box);
      };
      cap(0, false, 0, 1u << 20);
      cap(M - 1, true, 1, 1u << 21);
    }
  }
  return m;
}

// ---------------------------------------------------------------------------------------------
// Shared helpers

std::vector<int> contourSmoothBits(const std::vector<Vec2>& pts, bool closed, float sharpDeg) {
  return edgeSmoothBits(pts, closed, sharpDeg);
}

void capContours(PolyMesh& m, const std::vector<Contour2D>& contours, const std::function<int(int, int)>& vertexOf,
                 bool reverse, int matId, uint32_t smooth, bool uvs) {
  std::vector<IndexedRegion> regions = closedRegions(contours);
  auto pos2d = [&](int c, int k) { return contours[c].pts[k]; };
  addCaps(m, contours, regions, vertexOf, pos2d, reverse, matId, smooth, uvs, contoursBounds(contours));
}

int PathFrames::segments() const {
  int M = static_cast<int>(P.size());
  return M < 2 ? 0 : (closed ? M : M - 1);
}

Vec3 PathFrames::place(int i, const Vec2& q2) const {
  Vec3 q = R[i] * q2.x + U[i] * q2.y;
  if (miter[i] != 1.0f && length2(bend[i]) > 0) q += bend[i] * (dot(q, bend[i]) * (miter[i] - 1.0f));
  return P[i] + q;
}

PathFrames pathFrames(const std::vector<Vec3>& polyline, bool closedIn, bool banking) {
  PathFrames F;
  // Drop duplicate consecutive points.
  for (const auto& p : polyline)
    if (F.P.empty() || length2(p - F.P.back()) > 1e-12f) F.P.push_back(p);
  F.closed = closedIn && F.P.size() > 2;
  if (F.closed && length2(F.P.front() - F.P.back()) < 1e-12f) F.P.pop_back();
  int M = static_cast<int>(F.P.size());
  if (M < 2) {
    F.P.clear();
    return F;
  }
  const auto& pts = F.P;
  bool closed = F.closed;
  int segCount = F.segments();
  std::vector<Vec3> dir(segCount);
  for (int j = 0; j < segCount; ++j) dir[j] = normalize(pts[(j + 1) % M] - pts[j]);
  F.T.resize(M);
  F.R.resize(M);
  F.U.resize(M);
  F.bend.assign(M, Vec3());
  F.miter.assign(M, 1.0f);
  for (int i = 0; i < M; ++i) {
    bool hasPrev = closed || i > 0, hasNext = closed || i < M - 1;
    Vec3 a = hasPrev ? dir[(i + segCount - 1) % segCount] : dir[0];
    Vec3 b = hasNext ? dir[i % segCount] : dir[segCount - 1];
    Vec3 t = a + b;
    F.T[i] = length2(t) > 1e-12f ? normalize(t) : b;
    if (hasPrev && hasNext) {
      F.miter[i] = 1.0f / std::max(0.25f, dot(b, F.T[i]));
      Vec3 bd = b - a;
      bd = bd - F.T[i] * dot(bd, F.T[i]);
      F.bend[i] = length2(bd) > 1e-12f ? normalize(bd) : Vec3();
    }
  }
  Vec3 up(0, 0, 1);
  for (int i = 0; i < M; ++i) {
    Vec3 r;
    if (i == 0 || !banking) {
      r = cross(F.T[i], up);
      if (length2(r) < 1e-10f) r = i > 0 ? F.R[i - 1] : cross(F.T[i], Vec3(1, 0, 0));
    } else {
      r = F.R[i - 1] - F.T[i] * dot(F.R[i - 1], F.T[i]);
    }
    F.R[i] = normalize(r);
    F.U[i] = cross(F.R[i], F.T[i]);
  }
  // A closed path transported around the loop may come back rotated: spread the correction.
  if (closed && banking) {
    Vec3 back = F.R[M - 1] - F.T[0] * dot(F.R[M - 1], F.T[0]);
    if (length2(back) > 1e-12f) {
      back = normalize(back);
      float ang = std::atan2(dot(cross(back, F.R[0]), F.T[0]), dot(back, F.R[0]));
      for (int i = 1; i < M; ++i) {
        float a = ang * static_cast<float>(i) / M;
        F.R[i] = normalize(Mat4::rotateAxis(F.T[i], a).transformDir(F.R[i]));
        F.U[i] = cross(F.R[i], F.T[i]);
      }
    }
  }
  F.dist.assign(M, 0.0f);
  for (int i = 1; i < M; ++i) F.dist[i] = F.dist[i - 1] + length(pts[i] - pts[i - 1]);
  F.length = F.dist.back() + (closed ? length(pts[0] - pts[M - 1]) : 0.0f);
  return F;
}

}  // namespace veraa
