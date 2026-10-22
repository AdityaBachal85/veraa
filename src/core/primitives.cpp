#include "primitives.h"

#include <cstdint>
#include <map>

namespace veraa {
namespace {

// Adds a (nu x nv) quad grid. pos(i, j) gives the vertex at column i, row j. Faces are
// counter-clockwise when the u direction crossed with the v direction points outward.
template <class PosFn>
void addGrid(PolyMesh& m, int nu, int nv, PosFn pos, uint32_t smooth) {
  int base = static_cast<int>(m.positions.size());
  int uvBase = static_cast<int>(m.uvs.size());
  for (int j = 0; j <= nv; ++j)
    for (int i = 0; i <= nu; ++i) {
      m.addVertex(pos(i, j));
      m.addUv(Vec2(static_cast<float>(i) / nu, static_cast<float>(j) / nv));
    }
  for (int j = 0; j < nv; ++j)
    for (int i = 0; i < nu; ++i) {
      int a = j * (nu + 1) + i, b = a + 1, c = a + (nu + 1) + 1, d = a + (nu + 1);
      m.addFace({base + a, base + b, base + c, base + d}, {uvBase + a, uvBase + b, uvBase + c, uvBase + d}, smooth);
    }
}

std::vector<float> linspace(float a, float b, int n) {
  std::vector<float> v(n + 1);
  for (int i = 0; i <= n; ++i) v[i] = a + (b - a) * static_cast<float>(i) / n;
  v.front() = a;
  v.back() = b;
  return v;
}

// Six-sided box surface from coordinate arrays (xs along X, ys along Y, zs along Z).
PolyMesh boxSurface(const std::vector<float>& xs, const std::vector<float>& ys, const std::vector<float>& zs,
                    bool singleSmoothGroup) {
  PolyMesh m;
  int nx = static_cast<int>(xs.size()) - 1, ny = static_cast<int>(ys.size()) - 1, nz = static_cast<int>(zs.size()) - 1;
  float x0 = xs.front(), x1 = xs.back(), y0 = ys.front(), y1 = ys.back(), z0 = zs.front(), z1 = zs.back();
  auto sg = [&](int side) { return singleSmoothGroup ? 1u : (1u << side); };
  addGrid(m, nx, ny, [&](int i, int j) { return Vec3(xs[i], ys[ny - j], z0); }, sg(0));   // bottom (-Z)
  addGrid(m, nx, ny, [&](int i, int j) { return Vec3(xs[i], ys[j], z1); }, sg(1));        // top (+Z)
  addGrid(m, nx, nz, [&](int i, int j) { return Vec3(xs[i], y0, zs[j]); }, sg(2));        // front (-Y)
  addGrid(m, nx, nz, [&](int i, int j) { return Vec3(xs[nx - i], y1, zs[j]); }, sg(3));   // back (+Y)
  addGrid(m, ny, nz, [&](int i, int j) { return Vec3(x0, ys[ny - i], zs[j]); }, sg(4));   // left (-X)
  addGrid(m, ny, nz, [&](int i, int j) { return Vec3(x1, ys[i], zs[j]); }, sg(5));        // right (+X)
  weldVertices(m);
  return m;
}

// Vertex ring around the Z axis. A zero radius gives a single apex vertex.
std::vector<int> makeRing(PolyMesh& m, float r, float z, int sides) {
  if (r <= 1e-7f) return {m.addVertex(Vec3(0, 0, z))};
  std::vector<int> ring(sides);
  for (int j = 0; j < sides; ++j) {
    float a = kTwoPi * static_cast<float>(j) / sides;
    ring[j] = m.addVertex(Vec3(r * std::cos(a), r * std::sin(a), z));
  }
  return ring;
}

// UV row with sides+1 entries at height v.
int makeUvRow(PolyMesh& m, int sides, float v) {
  int base = static_cast<int>(m.uvs.size());
  for (int j = 0; j <= sides; ++j) m.addUv(Vec2(static_cast<float>(j) / sides, v));
  return base;
}

// Connects two rings with quads (or triangles at an apex).
void bridgeRings(PolyMesh& m, const std::vector<int>& lo, const std::vector<int>& hi, int sides, int uvLo, int uvHi,
                 uint32_t smooth, bool inward) {
  for (int j = 0; j < sides; ++j) {
    int jn = (j + 1) % sides;
    std::vector<int> v, t;
    if (lo.size() == 1 && hi.size() == 1) continue;
    if (lo.size() == 1) {
      v = {lo[0], hi[jn], hi[j]};
      t = {uvLo + j, uvHi + j + 1, uvHi + j};
    } else if (hi.size() == 1) {
      v = {lo[j], lo[jn], hi[0]};
      t = {uvLo + j, uvLo + j + 1, uvHi + j};
    } else {
      v = {lo[j], lo[jn], hi[jn], hi[j]};
      t = {uvLo + j, uvLo + j + 1, uvHi + j + 1, uvHi + j};
    }
    if (inward) {
      std::reverse(v.begin(), v.end());
      std::reverse(t.begin(), t.end());
    }
    m.addFace(std::move(v), std::move(t), smooth);
  }
}

// Flat disc cap. outer is the existing outer ring; caps of capSegs > 1 add inner rings.
void addDiscCap(PolyMesh& m, const std::vector<int>& outer, float r, float z, int sides, int capSegs, bool up,
                uint32_t smooth) {
  if (outer.size() < 3) return;
  auto planarUv = [&](int vi) {
    const Vec3& p = m.positions[vi];
    return m.addUv(Vec2(0.5f + p.x / (2 * r), 0.5f + p.y / (2 * r)));
  };
  auto emit = [&](std::vector<int> v) {
    if (!up) std::reverse(v.begin(), v.end());
    std::vector<int> t;
    for (int vi : v) t.push_back(planarUv(vi));
    m.addFace(std::move(v), std::move(t), smooth);
  };
  if (capSegs <= 1) {
    emit(outer);
    return;
  }
  std::vector<int> prev = outer;
  for (int k = capSegs - 1; k >= 0; --k) {
    std::vector<int> cur = makeRing(m, r * static_cast<float>(k) / capSegs, z, sides);
    for (int j = 0; j < sides; ++j) {
      int jn = (j + 1) % sides;
      if (cur.size() == 1)
        emit({prev[j], prev[jn], cur[0]});
      else
        emit({prev[j], prev[jn], cur[jn], cur[j]});
    }
    prev = cur;
  }
}

// Ring-shaped cap between an outer and inner ring (for tubes).
void addAnnulusCap(PolyMesh& m, const std::vector<int>& outer, const std::vector<int>& inner, float rOut, float rIn,
                   float z, int sides, int capSegs, bool up, uint32_t smooth) {
  std::vector<int> prev = outer;
  for (int k = 1; k <= capSegs; ++k) {
    std::vector<int> cur =
        k == capSegs ? inner : makeRing(m, rOut + (rIn - rOut) * static_cast<float>(k) / capSegs, z, sides);
    for (int j = 0; j < sides; ++j) {
      int jn = (j + 1) % sides;
      std::vector<int> v = {prev[j], prev[jn], cur[jn], cur[j]};
      if (!up) std::reverse(v.begin(), v.end());
      std::vector<int> t;
      for (int vi : v) {
        const Vec3& p = m.positions[vi];
        t.push_back(m.addUv(Vec2(0.5f + p.x / (2 * rOut), 0.5f + p.y / (2 * rOut))));
      }
      m.addFace(std::move(v), std::move(t), smooth);
    }
    prev = cur;
  }
}

class Primitive : public GeometryObject {};

class BoxObject : public Primitive {
 public:
  BoxObject() {
    params.addFloat("length", "Length", 100, 0.0, 1e7, 1.0);
    params.addFloat("width", "Width", 100, 0.0, 1e7, 1.0);
    params.addFloat("height", "Height", 100, -1e7, 1e7, 1.0);
    params.addInt("lengthSegs", "Length Segs", 1, 1, 200);
    params.addInt("widthSegs", "Width Segs", 1, 1, 200);
    params.addInt("heightSegs", "Height Segs", 1, 1, 200);
  }
  std::string type() const override { return "Box"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<BoxObject>(*this); }
  PolyMesh build() const override {
    float w = params.getFloat("width") * 0.5f, l = params.getFloat("length") * 0.5f, h = params.getFloat("height");
    PolyMesh m = boxSurface(linspace(-w, w, params.getInt("widthSegs")), linspace(-l, l, params.getInt("lengthSegs")),
                            linspace(0, h, params.getInt("heightSegs")), false);
    if (h < 0) m.flipFaces();
    return m;
  }
};

class ChamferBoxObject : public Primitive {
 public:
  ChamferBoxObject() {
    params.addFloat("length", "Length", 100, 0.0, 1e7, 1.0);
    params.addFloat("width", "Width", 100, 0.0, 1e7, 1.0);
    params.addFloat("height", "Height", 100, 0.0, 1e7, 1.0);
    params.addFloat("fillet", "Fillet", 5, 0.0, 1e7, 0.5);
    params.addInt("lengthSegs", "Length Segs", 1, 1, 200);
    params.addInt("widthSegs", "Width Segs", 1, 1, 200);
    params.addInt("heightSegs", "Height Segs", 1, 1, 200);
    params.addInt("filletSegs", "Fillet Segs", 3, 1, 32);
    params.addBool("smooth", "Smooth", true);
  }
  std::string type() const override { return "ChamferBox"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<ChamferBoxObject>(*this); }

  static std::vector<float> filletCoords(float half, float fillet, int segs, int filletSegs) {
    if (fillet <= 0.0f) return linspace(-half, half, segs);
    float inner = half - fillet;
    std::vector<float> c;
    for (int k = 0; k < filletSegs; ++k)
      c.push_back(-inner - fillet * std::tan(kPi * 0.25f * (1.0f - static_cast<float>(k) / filletSegs)));
    c.front() = -half;
    if (inner > 1e-6f) {
      for (int s = 0; s < segs; ++s) c.push_back(-inner + 2 * inner * static_cast<float>(s) / segs);
      c.push_back(inner);
    } else {
      c.push_back(0.0f);
    }
    for (int k = 1; k <= filletSegs; ++k)
      c.push_back(inner + fillet * std::tan(kPi * 0.25f * static_cast<float>(k) / filletSegs));
    c.back() = half;
    return c;
  }

  PolyMesh build() const override {
    float w = params.getFloat("width") * 0.5f, l = params.getFloat("length") * 0.5f;
    float hh = params.getFloat("height") * 0.5f;
    float f = std::min(params.getFloat("fillet"), std::min(w, std::min(l, hh)));
    int fs = params.getInt("filletSegs");
    std::vector<float> zs = filletCoords(hh, f, params.getInt("heightSegs"), fs);
    PolyMesh m = boxSurface(filletCoords(w, f, params.getInt("widthSegs"), fs),
                            filletCoords(l, f, params.getInt("lengthSegs"), fs), zs, params.getBool("smooth"));
    if (f > 0.0f) {
      Vec3 inner(w - f, l - f, hh - f);
      for (auto& p : m.positions) {
        Vec3 q(clampf(p.x, -inner.x, inner.x), clampf(p.y, -inner.y, inner.y), clampf(p.z, -inner.z, inner.z));
        Vec3 d = p - q;
        if (length2(d) > 1e-18f) p = q + normalize(d) * f;
      }
    }
    for (auto& p : m.positions) p.z += hh;  // pivot at the base
    weldVertices(m);
    return m;
  }
};

class SphereObject : public Primitive {
 public:
  SphereObject() {
    params.addFloat("radius", "Radius", 50, 0.0, 1e7, 1.0);
    params.addInt("segments", "Segments", 32, 4, 512);
    params.addBool("smooth", "Smooth", true);
    params.addBool("basePivot", "Base To Pivot", false);
  }
  std::string type() const override { return "Sphere"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<SphereObject>(*this); }
  PolyMesh build() const override {
    PolyMesh m;
    float r = params.getFloat("radius");
    int segs = params.getInt("segments");
    int rows = std::max(2, segs / 2);
    uint32_t sg = params.getBool("smooth") ? 1u : 0u;
    float zOff = params.getBool("basePivot") ? r : 0.0f;

    int top = m.addVertex(Vec3(0, 0, r + zOff));
    std::vector<int> ringStart(rows + 1, 0);
    for (int i = 1; i < rows; ++i) {
      float th = kPi * static_cast<float>(i) / rows;
      ringStart[i] = static_cast<int>(m.positions.size());
      for (int j = 0; j < segs; ++j) {
        float ph = kTwoPi * static_cast<float>(j) / segs;
        m.addVertex(Vec3(r * std::sin(th) * std::cos(ph), r * std::sin(th) * std::sin(ph), r * std::cos(th) + zOff));
      }
    }
    int bottom = m.addVertex(Vec3(0, 0, -r + zOff));

    // UV grid (rows+1) x (segs+1); pole rows use per-segment centered U.
    int uvBase = static_cast<int>(m.uvs.size());
    for (int i = 0; i <= rows; ++i)
      for (int j = 0; j <= segs; ++j) {
        float u = static_cast<float>(j) / segs;
        if (i == 0 || i == rows) u = (j + 0.5f) / segs;
        m.addUv(Vec2(u, 1.0f - static_cast<float>(i) / rows));
      }
    auto uv = [&](int i, int j) { return uvBase + i * (segs + 1) + j; };
    auto vtx = [&](int i, int j) { return ringStart[i] + (j % segs); };

    for (int j = 0; j < segs; ++j) m.addFace({top, vtx(1, j), vtx(1, j + 1)}, {uv(0, j), uv(1, j), uv(1, j + 1)}, sg);
    for (int i = 1; i + 1 < rows; ++i)
      for (int j = 0; j < segs; ++j)
        m.addFace({vtx(i, j), vtx(i + 1, j), vtx(i + 1, j + 1), vtx(i, j + 1)},
                  {uv(i, j), uv(i + 1, j), uv(i + 1, j + 1), uv(i, j + 1)}, sg);
    for (int j = 0; j < segs; ++j)
      m.addFace({bottom, vtx(rows - 1, j + 1), vtx(rows - 1, j)}, {uv(rows, j), uv(rows - 1, j + 1), uv(rows - 1, j)},
                sg);
    return m;
  }
};

class GeoSphereObject : public Primitive {
 public:
  GeoSphereObject() {
    params.addFloat("radius", "Radius", 50, 0.0, 1e7, 1.0);
    params.addInt("segments", "Segments", 4, 1, 64);
    params.addBool("smooth", "Smooth", true);
  }
  std::string type() const override { return "GeoSphere"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<GeoSphereObject>(*this); }
  PolyMesh build() const override {
    static const float t = (1.0f + std::sqrt(5.0f)) * 0.5f;
    static const Vec3 ico[12] = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t},
                                 {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
    static const int faces[20][3] = {{0, 11, 5}, {0, 5, 1},  {0, 1, 7},   {0, 7, 10}, {0, 10, 11},
                                     {1, 5, 9},  {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
                                     {3, 9, 4},  {3, 4, 2},  {3, 2, 6},   {3, 6, 8},  {3, 8, 9},
                                     {4, 9, 5},  {2, 4, 11}, {6, 2, 10},  {8, 6, 7},  {9, 8, 1}};
    float r = params.getFloat("radius");
    int n = params.getInt("segments");
    uint32_t sg = params.getBool("smooth") ? 1u : 0u;
    PolyMesh m;
    std::map<uint64_t, int> vertexOf;  // canonical barycentric key -> vertex

    auto pointIndex = [&](const int vid[3], int a, int b, int c) {
      // Key on the non-zero (vertex id, weight) pairs sorted by id, so a point shared by two
      // faces gets the same key and the same (bit-identical) position from both.
      std::vector<std::pair<int, int>> w;
      if (a) w.emplace_back(vid[0], a);
      if (b) w.emplace_back(vid[1], b);
      if (c) w.emplace_back(vid[2], c);
      std::sort(w.begin(), w.end());
      uint64_t key = 0;
      for (auto& e : w) key = key * 4096 + static_cast<uint64_t>((e.first + 1) * 128 + e.second);
      auto it = vertexOf.find(key);
      if (it != vertexOf.end()) return it->second;
      Vec3 p;
      for (auto& e : w) p += ico[e.first] * static_cast<float>(e.second);
      int idx = m.addVertex(normalize(p) * r);
      vertexOf.emplace(key, idx);
      return idx;
    };
    auto sphericalUv = [](const Vec3& p) {
      Vec3 d = normalize(p);
      return Vec2(0.5f + std::atan2(d.y, d.x) / kTwoPi, 0.5f + std::asin(clampf(d.z, -1, 1)) / kPi);
    };

    for (const auto& f : faces) {
      Vec3 A = ico[f[0]], B = ico[f[1]], C = ico[f[2]];
      int vid[3] = {f[0], f[1], f[2]};
      if (dot(cross(B - A, C - A), A + B + C) < 0) std::swap(vid[1], vid[2]);
      for (int i = 0; i < n; ++i)
        for (int j = 0; i + j < n; ++j) {
          // Barycentric weights (on vid[0], vid[1], vid[2]) scaled by n.
          int tri[2][3][2] = {{{i, j}, {i + 1, j}, {i, j + 1}}, {{i + 1, j}, {i + 1, j + 1}, {i, j + 1}}};
          for (int k = 0; k < (i + j < n - 1 ? 2 : 1); ++k) {
            std::vector<int> v(3), t(3);
            Vec2 uvs[3];
            for (int c = 0; c < 3; ++c) {
              int bi = tri[k][c][0], bj = tri[k][c][1];
              v[c] = pointIndex(vid, n - bi - bj, bi, bj);
              uvs[c] = sphericalUv(m.positions[v[c]]);
            }
            float umax = std::max(uvs[0].x, std::max(uvs[1].x, uvs[2].x));
            float umin = std::min(uvs[0].x, std::min(uvs[1].x, uvs[2].x));
            if (umax - umin > 0.5f)
              for (auto& u : uvs)
                if (u.x < 0.5f) u.x += 1.0f;
            for (int c = 0; c < 3; ++c) t[c] = m.addUv(uvs[c]);
            m.addFace(std::move(v), std::move(t), sg);
          }
        }
    }
    return m;
  }
};

class CylinderObject : public Primitive {
 public:
  CylinderObject() {
    params.addFloat("radius", "Radius", 50, 0.0, 1e7, 1.0);
    params.addFloat("height", "Height", 100, -1e7, 1e7, 1.0);
    params.addInt("heightSegs", "Height Segments", 5, 1, 200);
    params.addInt("capSegs", "Cap Segments", 1, 1, 100);
    params.addInt("sides", "Sides", 18, 3, 512);
    params.addBool("smooth", "Smooth", true);
  }
  std::string type() const override { return "Cylinder"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<CylinderObject>(*this); }
  PolyMesh build() const override {
    return buildCone(params.getFloat("radius"), params.getFloat("radius"), params.getFloat("height"),
                     params.getInt("heightSegs"), params.getInt("capSegs"), params.getInt("sides"),
                     params.getBool("smooth"));
  }
  static PolyMesh buildCone(float r1, float r2, float h, int hs, int cs, int sides, bool smooth) {
    PolyMesh m;
    uint32_t sideSg = smooth ? 1u : 0u;
    std::vector<std::vector<int>> rings;
    std::vector<int> uvRows;
    for (int i = 0; i <= hs; ++i) {
      float t = static_cast<float>(i) / hs;
      rings.push_back(makeRing(m, lerpf(r1, r2, t), h * t, sides));
      uvRows.push_back(makeUvRow(m, sides, t));
    }
    for (int i = 0; i < hs; ++i) bridgeRings(m, rings[i], rings[i + 1], sides, uvRows[i], uvRows[i + 1], sideSg, false);
    addDiscCap(m, rings.front(), r1, 0.0f, sides, cs, false, 2u);
    addDiscCap(m, rings.back(), r2, h, sides, cs, true, 4u);
    if (h < 0) m.flipFaces();
    return m;
  }
};

class ConeObject : public Primitive {
 public:
  ConeObject() {
    params.addFloat("radius1", "Radius 1", 50, 0.0, 1e7, 1.0);
    params.addFloat("radius2", "Radius 2", 0, 0.0, 1e7, 1.0);
    params.addFloat("height", "Height", 100, -1e7, 1e7, 1.0);
    params.addInt("heightSegs", "Height Segments", 5, 1, 200);
    params.addInt("capSegs", "Cap Segments", 1, 1, 100);
    params.addInt("sides", "Sides", 24, 3, 512);
    params.addBool("smooth", "Smooth", true);
  }
  std::string type() const override { return "Cone"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<ConeObject>(*this); }
  PolyMesh build() const override {
    return CylinderObject::buildCone(params.getFloat("radius1"), params.getFloat("radius2"), params.getFloat("height"),
                                     params.getInt("heightSegs"), params.getInt("capSegs"), params.getInt("sides"),
                                     params.getBool("smooth"));
  }
};

class TubeObject : public Primitive {
 public:
  TubeObject() {
    params.addFloat("radius1", "Radius 1", 50, 0.0, 1e7, 1.0);
    params.addFloat("radius2", "Radius 2", 40, 0.0, 1e7, 1.0);
    params.addFloat("height", "Height", 100, -1e7, 1e7, 1.0);
    params.addInt("heightSegs", "Height Segments", 5, 1, 200);
    params.addInt("capSegs", "Cap Segments", 1, 1, 100);
    params.addInt("sides", "Sides", 18, 3, 512);
    params.addBool("smooth", "Smooth", true);
  }
  std::string type() const override { return "Tube"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<TubeObject>(*this); }
  PolyMesh build() const override {
    float ra = params.getFloat("radius1"), rb = params.getFloat("radius2");
    float rOut = std::max(ra, rb), rIn = std::min(ra, rb);
    float h = params.getFloat("height");
    int hs = params.getInt("heightSegs"), cs = params.getInt("capSegs"), sides = params.getInt("sides");
    uint32_t sg = params.getBool("smooth") ? 1u : 0u;
    PolyMesh m;
    if (rOut <= 0.0f) return m;
    std::vector<std::vector<int>> outer, inner;
    std::vector<int> uvRows;
    for (int i = 0; i <= hs; ++i) {
      float t = static_cast<float>(i) / hs;
      outer.push_back(makeRing(m, rOut, h * t, sides));
      inner.push_back(makeRing(m, std::max(rIn, 1e-4f), h * t, sides));
      uvRows.push_back(makeUvRow(m, sides, t));
    }
    for (int i = 0; i < hs; ++i) {
      bridgeRings(m, outer[i], outer[i + 1], sides, uvRows[i], uvRows[i + 1], sg, false);
      bridgeRings(m, inner[i], inner[i + 1], sides, uvRows[i], uvRows[i + 1], sg ? 8u : 0u, true);
    }
    addAnnulusCap(m, outer.front(), inner.front(), rOut, rIn, 0.0f, sides, cs, false, 2u);
    addAnnulusCap(m, outer.back(), inner.back(), rOut, rIn, h, sides, cs, true, 4u);
    if (h < 0) m.flipFaces();
    return m;
  }
};

class TorusObject : public Primitive {
 public:
  TorusObject() {
    params.addFloat("radius1", "Radius 1", 50, 0.0, 1e7, 1.0);
    params.addFloat("radius2", "Radius 2", 10, 0.0, 1e7, 0.5);
    params.addInt("segments", "Segments", 24, 3, 512);
    params.addInt("sides", "Sides", 12, 3, 512);
    params.addBool("smooth", "Smooth", true);
  }
  std::string type() const override { return "Torus"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<TorusObject>(*this); }
  PolyMesh build() const override {
    float R = params.getFloat("radius1"), r = params.getFloat("radius2");
    int segs = params.getInt("segments"), sides = params.getInt("sides");
    uint32_t sg = params.getBool("smooth") ? 1u : 0u;
    PolyMesh m;
    for (int i = 0; i < segs; ++i)
      for (int j = 0; j < sides; ++j) {
        float u = kTwoPi * static_cast<float>(i) / segs, v = kTwoPi * static_cast<float>(j) / sides;
        float d = R + r * std::cos(v);
        m.addVertex(Vec3(d * std::cos(u), d * std::sin(u), r * std::sin(v)));
      }
    int uvBase = static_cast<int>(m.uvs.size());
    for (int i = 0; i <= segs; ++i)
      for (int j = 0; j <= sides; ++j) m.addUv(Vec2(static_cast<float>(i) / segs, static_cast<float>(j) / sides));
    auto vtx = [&](int i, int j) { return (i % segs) * sides + (j % sides); };
    auto uv = [&](int i, int j) { return uvBase + i * (sides + 1) + j; };
    for (int i = 0; i < segs; ++i)
      for (int j = 0; j < sides; ++j)
        m.addFace({vtx(i, j), vtx(i + 1, j), vtx(i + 1, j + 1), vtx(i, j + 1)},
                  {uv(i, j), uv(i + 1, j), uv(i + 1, j + 1), uv(i, j + 1)}, sg);
    return m;
  }
};

class PyramidObject : public Primitive {
 public:
  PyramidObject() {
    params.addFloat("width", "Width", 100, 0.0, 1e7, 1.0);
    params.addFloat("depth", "Depth", 100, 0.0, 1e7, 1.0);
    params.addFloat("height", "Height", 100, -1e7, 1e7, 1.0);
  }
  std::string type() const override { return "Pyramid"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<PyramidObject>(*this); }
  PolyMesh build() const override {
    float w = params.getFloat("width") * 0.5f, d = params.getFloat("depth") * 0.5f, h = params.getFloat("height");
    PolyMesh m;
    int A = m.addVertex({-w, -d, 0}), B = m.addVertex({w, -d, 0}), C = m.addVertex({w, d, 0}),
        D = m.addVertex({-w, d, 0}), T = m.addVertex({0, 0, h});
    int u0 = m.addUv({0, 0}), u1 = m.addUv({1, 0}), u2 = m.addUv({1, 1}), u3 = m.addUv({0, 1}), ut = m.addUv({0.5f, 1});
    m.addFace({D, C, B, A}, {u3, u2, u1, u0}, 1u);
    m.addFace({A, B, T}, {u0, u1, ut}, 2u);
    m.addFace({B, C, T}, {u0, u1, ut}, 4u);
    m.addFace({C, D, T}, {u0, u1, ut}, 8u);
    m.addFace({D, A, T}, {u0, u1, ut}, 16u);
    if (h < 0) m.flipFaces();
    return m;
  }
};

class PlaneObject : public Primitive {
 public:
  PlaneObject() {
    params.addFloat("length", "Length", 100, 0.0, 1e7, 1.0);
    params.addFloat("width", "Width", 100, 0.0, 1e7, 1.0);
    params.addInt("lengthSegs", "Length Segs", 4, 1, 1000);
    params.addInt("widthSegs", "Width Segs", 4, 1, 1000);
  }
  std::string type() const override { return "Plane"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<PlaneObject>(*this); }
  PolyMesh build() const override {
    float w = params.getFloat("width") * 0.5f, l = params.getFloat("length") * 0.5f;
    int nx = params.getInt("widthSegs"), ny = params.getInt("lengthSegs");
    std::vector<float> xs = linspace(-w, w, nx), ys = linspace(-l, l, ny);
    PolyMesh m;
    addGrid(m, nx, ny, [&](int i, int j) { return Vec3(xs[i], ys[j], 0.0f); }, 1u);
    return m;
  }
};

}  // namespace

void registerPrimitiveTypes() {
  auto add = [](const char* type, const char* name, const char* sub, std::function<std::unique_ptr<SceneObject>()> f) {
    registerObjectType({type, name, "Geometry", sub, std::move(f)});
  };
  const char* standard = "Standard Primitives";
  add("Box", "Box", standard, [] { return std::make_unique<BoxObject>(); });
  add("Cone", "Cone", standard, [] { return std::make_unique<ConeObject>(); });
  add("Sphere", "Sphere", standard, [] { return std::make_unique<SphereObject>(); });
  add("GeoSphere", "GeoSphere", standard, [] { return std::make_unique<GeoSphereObject>(); });
  add("Cylinder", "Cylinder", standard, [] { return std::make_unique<CylinderObject>(); });
  add("Tube", "Tube", standard, [] { return std::make_unique<TubeObject>(); });
  add("Torus", "Torus", standard, [] { return std::make_unique<TorusObject>(); });
  add("Pyramid", "Pyramid", standard, [] { return std::make_unique<PyramidObject>(); });
  add("Plane", "Plane", standard, [] { return std::make_unique<PlaneObject>(); });
  add("ChamferBox", "Chamfer Box", "Extended Primitives", [] { return std::make_unique<ChamferBoxObject>(); });
}

}  // namespace veraa
