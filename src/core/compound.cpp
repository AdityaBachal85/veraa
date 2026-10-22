#include "compound.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <unordered_map>

#include "shape2d.h"
#include "shape_mesh.h"

namespace veraa {

// ---------------------------------------------------------------------------------------------
// CompoundObject

json CompoundObject::toJson() const {
  json j = SceneObject::toJson();
  json ops = json::array();
  for (const auto& op : operands) {
    json o{{"role", op.role}, {"sourceId", op.sourceId}, {"name", op.name}, {"isShape", op.isShape},
           {"percent", op.percent}};
    if (op.isShape) o["shape"] = bezierShapeToJson(op.shape);
    else o["mesh"] = polyMeshToJson(op.mesh);
    ops.push_back(std::move(o));
  }
  j["operands"] = std::move(ops);
  return j;
}

void CompoundObject::fromJson(const json& j) {
  SceneObject::fromJson(j);
  operands.clear();
  if (!j.contains("operands")) return;
  for (const auto& o : j["operands"]) {
    CompoundOperand op;
    op.role = o.value("role", std::string());
    op.sourceId = o.value("sourceId", 0u);
    op.name = o.value("name", std::string());
    op.isShape = o.value("isShape", false);
    op.percent = o.value("percent", 0.0f);
    if (op.isShape && o.contains("shape")) op.shape = bezierShapeFromJson(o["shape"]);
    if (!op.isShape && o.contains("mesh")) op.mesh = polyMeshFromJson(o["mesh"]);
    operands.push_back(std::move(op));
  }
}

std::vector<const CompoundOperand*> CompoundObject::operandsWithRole(const std::string& role) const {
  std::vector<const CompoundOperand*> out;
  for (const auto& op : operands)
    if (op.role == role) out.push_back(&op);
  return out;
}

const CompoundOperand* CompoundObject::firstOperand(const std::string& role) const {
  for (const auto& op : operands)
    if (op.role == role) return &op;
  return nullptr;
}

const CompoundRole* CompoundObject::findRole(const std::string& role) const {
  static thread_local std::vector<CompoundRole> cache;
  cache = roles();
  for (const auto& r : cache)
    if (r.role == role) return &r;
  return nullptr;
}

void CompoundObject::setOperand(CompoundOperand op) {
  const CompoundRole* r = findRole(op.role);
  bool multiple = r && r->multiple;
  if (!multiple) {
    for (auto& existing : operands)
      if (existing.role == op.role) {
        existing = std::move(op);
        return;
      }
  }
  operands.push_back(std::move(op));
}

namespace {

// Deterministic random numbers (identical on every platform, unlike <random> distributions).
struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull) {
    if (s == 0) s = 1;
  }
  uint32_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return static_cast<uint32_t>(s >> 32);
  }
  float uniform() { return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f); }
  float range(float a, float b) { return a + (b - a) * uniform(); }
  float sym(float a) { return range(-a, a); }
};

// Rotation taking +Z to n.
Mat4 alignZ(const Vec3& nIn) {
  Vec3 n = normalize(nIn);
  Vec3 t, b;
  makeBasis(n, t, b);
  Mat4 r;
  for (int i = 0; i < 3; ++i) {
    r.m[i][0] = (&t.x)[i];
    r.m[i][1] = (&b.x)[i];
    r.m[i][2] = (&n.x)[i];
  }
  return r;
}

Vec3 polyCentroid(const PolyMesh& m, const PolyMesh::Face& f) {
  Vec3 c;
  for (int v : f.v) c += m.positions[v];
  return f.v.empty() ? c : c / static_cast<float>(f.v.size());
}

// Merges vertices closer than eps (snapping them to a grid) and drops degenerate faces.
void weldTolerant(PolyMesh& m, float eps) {
  if (eps <= 0) {
    weldVertices(m);
    return;
  }
  struct Key {
    long long x, y, z;
    bool operator==(const Key& o) const { return x == o.x && y == o.y && z == o.z; }
  };
  struct KeyHash {
    size_t operator()(const Key& k) const {
      return static_cast<size_t>(k.x * 73856093ll ^ k.y * 19349663ll ^ k.z * 83492791ll);
    }
  };
  std::unordered_map<Key, int, KeyHash> map;
  std::vector<int> remap(m.positions.size());
  std::vector<Vec3> pos;
  for (size_t i = 0; i < m.positions.size(); ++i) {
    const Vec3& p = m.positions[i];
    Key k{std::llround(p.x / eps), std::llround(p.y / eps), std::llround(p.z / eps)};
    auto it = map.find(k);
    if (it == map.end()) {
      remap[i] = static_cast<int>(pos.size());
      map.emplace(k, remap[i]);
      pos.push_back(p);
    } else {
      remap[i] = it->second;
    }
  }
  m.positions = std::move(pos);
  std::vector<PolyMesh::Face> faces;
  for (auto& f : m.faces) {
    PolyMesh::Face nf = f;
    nf.v.clear();
    nf.uv.clear();
    bool hasUv = f.uv.size() == f.v.size();
    for (size_t k = 0; k < f.v.size(); ++k) {
      int v = remap[f.v[k]];
      if (!nf.v.empty() && nf.v.back() == v) continue;
      nf.v.push_back(v);
      if (hasUv) nf.uv.push_back(f.uv[k]);
    }
    while (nf.v.size() > 1 && nf.v.front() == nf.v.back()) {
      nf.v.pop_back();
      if (hasUv) nf.uv.pop_back();
    }
    if (nf.v.size() >= 3) faces.push_back(std::move(nf));
  }
  m.faces = std::move(faces);
}

// Resamples a contour into n points spaced evenly along its length.
std::vector<Vec2> resampleContour(const std::vector<Vec2>& pts, bool closed, int n) {
  std::vector<Vec2> out;
  size_t m = pts.size();
  if (m == 0 || n <= 0) return out;
  if (static_cast<int>(m) == n) return pts;
  std::vector<float> cum(1, 0.0f);
  size_t edges = closed ? m : m - 1;
  for (size_t i = 0; i < edges; ++i) {
    Vec2 d = pts[(i + 1) % m] - pts[i];
    cum.push_back(cum.back() + std::sqrt(d.x * d.x + d.y * d.y));
  }
  float total = cum.back();
  if (total <= 0) return std::vector<Vec2>(n, pts[0]);
  size_t e = 0;
  for (int k = 0; k < n; ++k) {
    float s = total * static_cast<float>(k) / (closed ? n : std::max(1, n - 1));
    while (e + 1 < edges && cum[e + 1] < s) ++e;
    float len = cum[e + 1] - cum[e];
    float t = len > 0 ? clampf((s - cum[e]) / len, 0.0f, 1.0f) : 0.0f;
    out.push_back(pts[e] + (pts[(e + 1) % m] - pts[e]) * t);
  }
  return out;
}

// Plane of a (roughly planar) shape: origin, normal and in-plane basis.
struct ShapePlane {
  Vec3 o, n, t, b;
};

ShapePlane shapePlane(const BezierShape& s) {
  ShapePlane pl;
  Vec3 nsum;
  int count = 0;
  for (const auto& sp : s.splines) {
    std::vector<Vec3> pts = sp.polyline(4, true);
    for (size_t i = 0; i < pts.size(); ++i) {
      const Vec3& a = pts[i];
      const Vec3& b = pts[(i + 1) % pts.size()];
      nsum += Vec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y));
      pl.o += a;
      ++count;
    }
  }
  if (count) pl.o /= static_cast<float>(count);
  pl.n = length2(nsum) > 1e-12f ? normalize(nsum) : Vec3(0, 0, 1);
  if (pl.n.z < 0 && std::fabs(pl.n.z) > 0.5f) pl.n = pl.n * -1.0f;  // flat shapes project downward along -Z
  makeBasis(pl.n, pl.t, pl.b);
  return pl;
}

// Bridge triangles between two closed rings (lists of vertex ids) of possibly different sizes.
// Ring a runs along the bridge's start, ring b along its end, both in the same direction.
void zipRings(PolyMesh& m, const std::vector<int>& a, const std::vector<int>& b, uint32_t smooth, int matId) {
  auto fractions = [&](const std::vector<int>& r) {
    std::vector<float> f(r.size() + 1, 0.0f);
    for (size_t i = 0; i < r.size(); ++i)
      f[i + 1] = f[i] + length(m.positions[r[(i + 1) % r.size()]] - m.positions[r[i]]);
    float total = f.back() > 0 ? f.back() : 1.0f;
    for (auto& x : f) x /= total;
    return f;
  };
  std::vector<float> fa = fractions(a), fb = fractions(b);
  size_t na = a.size(), nb = b.size();
  size_t i = 0, j = 0;
  while (i < na || j < nb) {
    bool advanceA = j >= nb || (i < na && fa[i + 1] <= fb[j + 1]);
    if (advanceA) {
      m.addFace({a[(i + 1) % na], a[i % na], b[j % nb]}, {}, smooth, matId);
      ++i;
    } else {
      m.addFace({a[i % na], b[j % nb], b[(j + 1) % nb]}, {}, smooth, matId);
      ++j;
    }
  }
}

// Point-in-mesh test by counting crossings of a +Z ray (closed meshes).
bool insideMesh(const TriMesh& t, const Vec3& p) {
  int crossings = 0;
  for (size_t i = 0; i + 2 < t.indices.size(); i += 3) {
    const Vec3& a = t.positions[t.indices[i]];
    const Vec3& b = t.positions[t.indices[i + 1]];
    const Vec3& c = t.positions[t.indices[i + 2]];
    // Barycentric test in XY, then the hit height.
    float d = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
    if (std::fabs(d) < 1e-20f) continue;
    float l1 = ((b.y - c.y) * (p.x - c.x) + (c.x - b.x) * (p.y - c.y)) / d;
    float l2 = ((c.y - a.y) * (p.x - c.x) + (a.x - c.x) * (p.y - c.y)) / d;
    float l3 = 1.0f - l1 - l2;
    if (l1 < 0 || l2 < 0 || l3 < 0) continue;
    float z = l1 * a.z + l2 * b.z + l3 * c.z;
    if (z > p.z) ++crossings;
  }
  return crossings % 2 == 1;
}

// ---------------------------------------------------------------------------------------------
// Loft

class LoftObject : public CompoundObject {
 public:
  LoftObject() {
    params.beginGroup("Surface Parameters");
    params.addBool("smoothLength", "Smooth Length", true);
    params.addBool("smoothWidth", "Smooth Width", true);
    params.addBool("applyMapping", "Apply Mapping", true);
    params.addFloat("lengthRepeat", "Length Repeat", 1, 0.0, 1e5, 0.1);
    params.addFloat("widthRepeat", "Width Repeat", 1, 0.0, 1e5, 0.1);
    params.addBool("genMatIds", "Generate Material IDs", true);
    params.beginGroup("Skin Parameters");
    params.addBool("capStart", "Cap Start", true);
    params.addBool("capEnd", "Cap End", true);
    params.addInt("shapeSteps", "Shape Steps", 5, 0, 100);
    params.addInt("pathSteps", "Path Steps", 5, 0, 100);
    params.addBool("optimizeShapes", "Optimize Shapes", false);
    params.addBool("optimizePath", "Optimize Path", false);
    params.addBool("contour", "Contour", true);
    params.addBool("banking", "Banking", true);
    params.addBool("flipNormals", "Flip Normals", false);
    params.beginGroup("Deformations");
    params.addFloat("scaleStart", "Scale at Start %", 100, 0.0, 1e5, 1.0, 1);
    params.addFloat("scaleEnd", "Scale at End %", 100, 0.0, 1e5, 1.0, 1);
    params.addFloat("twist", "Twist", 0, -36000, 36000, 1.0, 1);
  }
  std::string type() const override { return "Loft"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<LoftObject>(*this); }
  std::vector<CompoundRole> roles() const override {
    return {{"path", "Get Path", true, false, OperandSpace::Compound},
            {"shape", "Get Shape", true, true, OperandSpace::Local}};
  }
  PolyMesh build() const override;
};

PolyMesh LoftObject::build() const {
  PolyMesh m;
  const CompoundOperand* path = firstOperand("path");
  std::vector<const CompoundOperand*> shapes = operandsWithRole("shape");
  if (!path || shapes.empty() || path->shape.empty()) return m;
  std::stable_sort(shapes.begin(), shapes.end(),
                   [](const CompoundOperand* a, const CompoundOperand* b) { return a->percent < b->percent; });
  int shapeSteps = params.getInt("shapeSteps");
  bool optShapes = params.getBool("optimizeShapes");
  // Contours of every shape; all shapes share the smallest contour count.
  std::vector<std::vector<Contour2D>> secs;
  size_t contourCount = SIZE_MAX;
  for (const auto* s : shapes) {
    secs.push_back(shapeContours(s->shape, shapeSteps, optShapes));
    contourCount = std::min(contourCount, secs.back().size());
  }
  if (contourCount == 0 || contourCount == SIZE_MAX) return m;
  // Equal point counts per contour index (resampled when shapes differ).
  std::vector<int> counts(contourCount, 0);
  std::vector<bool> closedC(contourCount, true);
  for (size_t c = 0; c < contourCount; ++c)
    for (auto& sc : secs) {
      counts[c] = std::max(counts[c], static_cast<int>(sc[c].pts.size()));
      closedC[c] = closedC[c] && sc[c].closed;
    }
  for (size_t c = 0; c < contourCount; ++c)
    for (auto& sc : secs) sc[c].pts = resampleContour(sc[c].pts, sc[c].closed, counts[c]);

  const Spline& sp = path->shape.splines.front();
  PathFrames F = pathFrames(sp.polyline(params.getInt("pathSteps"), params.getBool("optimizePath")), sp.closed,
                            params.getBool("banking"));
  int M = static_cast<int>(F.P.size());
  if (M < 2) return m;
  if (!params.getBool("contour")) {
    // Shapes keep one orientation (the first frame's) along the whole path.
    for (int i = 1; i < M; ++i) {
      F.R[i] = F.R[0];
      F.U[i] = F.U[0];
      F.miter[i] = 1.0f;
    }
  }
  float total = F.length > 0 ? F.length : 1.0f;
  float s0 = params.getFloat("scaleStart") / 100.0f, s1 = params.getFloat("scaleEnd") / 100.0f;
  float twist = radians(params.getFloat("twist"));
  // Section (per contour) at path point i.
  auto sectionAt = [&](int i, size_t c) {
    float f = F.dist[i] / total;
    float pct = f * 100.0f;
    size_t hi = 0;
    while (hi < shapes.size() && shapes[hi]->percent < pct) ++hi;
    std::vector<Vec2> pts;
    if (hi == 0) pts = secs[0][c].pts;
    else if (hi == shapes.size()) pts = secs.back()[c].pts;
    else {
      float pa = shapes[hi - 1]->percent, pb = shapes[hi]->percent;
      float t = pb > pa ? (pct - pa) / (pb - pa) : 0.0f;
      const auto& A = secs[hi - 1][c].pts;
      const auto& B = secs[hi][c].pts;
      for (size_t k = 0; k < A.size(); ++k) pts.push_back(A[k] + (B[k] - A[k]) * t);
    }
    float sc = s0 + (s1 - s0) * f, ang = twist * f, ca = std::cos(ang), sa = std::sin(ang);
    for (auto& p : pts) p = Vec2((p.x * ca - p.y * sa) * sc, (p.x * sa + p.y * ca) * sc);
    return pts;
  };
  bool uvs = params.getBool("applyMapping");
  float lr = params.getFloat("lengthRepeat"), wr = params.getFloat("widthRepeat");
  bool smoothW = params.getBool("smoothWidth"), smoothL = params.getBool("smoothLength");
  int segCount = F.segments();
  std::vector<int> base(contourCount);
  for (size_t c = 0; c < contourCount; ++c) {
    int n = counts[c];
    bool closedSec = closedC[c];
    base[c] = static_cast<int>(m.positions.size());
    for (int i = 0; i < M; ++i) {
      std::vector<Vec2> sec = sectionAt(i, c);
      for (int k = 0; k < n; ++k) m.addVertex(F.place(i, sec[k]));
    }
    std::vector<int> bits = contourSmoothBits(secs[0][c].pts, closedSec, smoothW ? 30.0f : 0.0f);
    int edges = closedSec ? n : n - 1;
    int uvBase = static_cast<int>(m.uvs.size());
    if (uvs)
      for (int i = 0; i <= (F.closed ? M : M - 1); ++i) {
        float v = (i < M ? F.dist[i] : F.length) / total * lr;
        for (int k = 0; k <= n; ++k) m.addUv(Vec2(static_cast<float>(k) / std::max(1, edges) * wr, v));
      }
    for (int i = 0; i < segCount; ++i) {
      int i1 = (i + 1) % M;
      int pathBit = smoothL ? 0 : ((F.closed && segCount % 2 == 1 && i == segCount - 1) ? 2 : i % 2);
      for (int k = 0; k < edges; ++k) {
        int k1 = (k + 1) % n;
        std::vector<int> f = {base[c] + i1 * n + k, base[c] + i1 * n + k1, base[c] + i * n + k1, base[c] + i * n + k};
        std::vector<int> t;
        if (uvs)
          t = std::vector<int>{uvBase + (i + 1) * (n + 1) + k, uvBase + (i + 1) * (n + 1) + k + 1,
                               uvBase + i * (n + 1) + k + 1, uvBase + i * (n + 1) + k};
        m.addFace(std::move(f), std::move(t), 1u << (bits[k] * 3 + pathBit), 2);
      }
    }
  }
  if (!F.closed) {
    auto cap = [&](int ring, bool reverse, int matId, uint32_t sg) {
      std::vector<Contour2D> cs;
      for (size_t c = 0; c < contourCount; ++c) cs.push_back({sectionAt(ring, c), closedC[c]});
      auto vertexOf = [&](int c, int k) { return base[c] + ring * counts[c] + k; };
      capContours(m, cs, vertexOf, reverse, matId, sg, uvs);
    };
    if (params.getBool("capStart")) cap(0, false, 0, 1u << 20);
    if (params.getBool("capEnd")) cap(M - 1, true, 1, 1u << 21);
  }
  if (!params.getBool("genMatIds"))
    for (auto& f : m.faces) f.matId = 0;
  if (params.getBool("flipNormals")) m.flipFaces();
  return m;
}

// ---------------------------------------------------------------------------------------------
// Scatter

class ScatterObject : public CompoundObject {
 public:
  ScatterObject() {
    params.beginGroup("Scatter Objects");
    params.addInt("duplicates", "Duplicates", 10, 1, 100000);
    params.addFloat("baseScale", "Base Scale %", 100, 0.0, 1e5, 1.0, 1);
    params.addFloat("vertexChaos", "Vertex Chaos", 0, 0.0, 1e5, 0.1);
    params.addBool("perpendicular", "Perpendicular", true);
    params.addEnum("distribution", "Distribute Using",
                   {"Area", "Even", "Skip N", "Random Faces", "Along Edges", "All Vertices", "All Edge Midpoints",
                    "All Face Centers", "Volume"},
                   0);
    params.addInt("skipN", "Skip N", 1, 0, 100000);
    params.beginGroup("Transforms");
    params.addFloat("rotX", "Rotation X", 0, 0.0, 360.0, 1.0, 1);
    params.addFloat("rotY", "Rotation Y", 0, 0.0, 360.0, 1.0, 1);
    params.addFloat("rotZ", "Rotation Z", 0, 0.0, 360.0, 1.0, 1);
    params.addFloat("transX", "Local Translation X", 0, 0.0, 1e7, 0.5);
    params.addFloat("transY", "Local Translation Y", 0, 0.0, 1e7, 0.5);
    params.addFloat("transZ", "Local Translation Z", 0, 0.0, 1e7, 0.5);
    params.addFloat("scaleX", "Scaling X %", 0, 0.0, 100.0, 1.0, 1);
    params.addFloat("scaleY", "Scaling Y %", 0, 0.0, 100.0, 1.0, 1);
    params.addFloat("scaleZ", "Scaling Z %", 0, 0.0, 100.0, 1.0, 1);
    params.addBool("uniformScale", "Uniform Scaling", true);
    params.beginGroup("Display");
    params.addBool("hideDistribution", "Hide Distribution Object", false);
    params.addInt("seed", "Seed", 12345, 0, 99999999);
  }
  std::string type() const override { return "Scatter"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<ScatterObject>(*this); }
  std::vector<CompoundRole> roles() const override {
    return {{"source", "Pick Source", false, false, OperandSpace::Local},
            {"distribution", "Pick Distribution Object", false, false, OperandSpace::Compound}};
  }
  PolyMesh build() const override;
};

PolyMesh ScatterObject::build() const {
  PolyMesh out;
  const CompoundOperand* src = firstOperand("source");
  const CompoundOperand* dist = firstOperand("distribution");
  if (!src || src->mesh.empty()) return out;
  Rng rng(static_cast<uint64_t>(params.getInt("seed")) + 1);
  struct Place {
    Vec3 p, n;
  };
  std::vector<Place> places;
  int dup = params.getInt("duplicates");
  if (dist && !dist->mesh.empty()) {
    const PolyMesh& d = dist->mesh;
    // Fan triangles with areas.
    struct Tri {
      Vec3 a, b, c, n;
      float area;
      int face;
    };
    std::vector<Tri> tris;
    std::vector<float> cum;
    float totalArea = 0;
    for (size_t fi = 0; fi < d.faces.size(); ++fi) {
      const auto& f = d.faces[fi];
      Vec3 fn = normalize(d.faceNormal(f));
      for (size_t k = 1; k + 1 < f.v.size(); ++k) {
        Tri t{d.positions[f.v[0]], d.positions[f.v[k]], d.positions[f.v[k + 1]], fn, 0, static_cast<int>(fi)};
        t.area = 0.5f * length(cross(t.b - t.a, t.c - t.a));
        totalArea += t.area;
        tris.push_back(t);
        cum.push_back(totalArea);
      }
    }
    auto triAt = [&](float x) {
      size_t i = static_cast<size_t>(std::lower_bound(cum.begin(), cum.end(), x) - cum.begin());
      return std::min(i, tris.size() - 1);
    };
    auto randomIn = [&](const Tri& t) {
      float r1 = std::sqrt(rng.uniform()), r2 = rng.uniform();
      return t.a * (1 - r1) + t.b * (r1 * (1 - r2)) + t.c * (r1 * r2);
    };
    int mode = params.getInt("distribution");
    if (!tris.empty()) switch (mode) {
        case 0:  // Area
          for (int i = 0; i < dup; ++i) {
            const Tri& t = tris[triAt(rng.uniform() * totalArea)];
            places.push_back({randomIn(t), t.n});
          }
          break;
        case 1:  // Even
          for (int i = 0; i < dup; ++i) {
            const Tri& t = tris[triAt((i + 0.5f) / dup * totalArea)];
            places.push_back({(t.a + t.b + t.c) / 3.0f, t.n});
          }
          break;
        case 2: {  // Skip N
          int step = params.getInt("skipN") + 1;
          for (size_t fi = 0; fi < d.faces.size() && static_cast<int>(places.size()) < dup; fi += step)
            places.push_back({polyCentroid(d, d.faces[fi]), normalize(d.faceNormal(d.faces[fi]))});
          break;
        }
        case 3:  // Random Faces
          for (int i = 0; i < dup; ++i) {
            const auto& f = d.faces[rng.next() % d.faces.size()];
            places.push_back({polyCentroid(d, f), normalize(d.faceNormal(f))});
          }
          break;
        case 4:  // Along Edges
          for (int i = 0; i < dup; ++i) {
            const auto& f = d.faces[rng.next() % d.faces.size()];
            size_t k = rng.next() % f.v.size();
            Vec3 a = d.positions[f.v[k]], b = d.positions[f.v[(k + 1) % f.v.size()]];
            places.push_back({lerp(a, b, rng.uniform()), normalize(d.faceNormal(f))});
          }
          break;
        case 5: {  // All Vertices
          std::vector<Vec3> vn = d.vertexNormals();
          for (size_t v = 0; v < d.positions.size(); ++v) places.push_back({d.positions[v], vn[v]});
          break;
        }
        case 6: {  // All Edge Midpoints
          std::map<std::pair<int, int>, Vec3> mids;
          for (const auto& f : d.faces)
            for (size_t k = 0; k < f.v.size(); ++k) {
              int a = f.v[k], b = f.v[(k + 1) % f.v.size()];
              auto key = std::make_pair(std::min(a, b), std::max(a, b));
              if (!mids.count(key)) mids[key] = normalize(d.faceNormal(f));
            }
          for (const auto& [e, n] : mids)
            places.push_back({(d.positions[e.first] + d.positions[e.second]) * 0.5f, n});
          break;
        }
        case 7:  // All Face Centers
          for (const auto& f : d.faces) places.push_back({polyCentroid(d, f), normalize(d.faceNormal(f))});
          break;
        default: {  // Volume
          TriMesh t = triangulate(d);
          Aabb b = d.bounds();
          for (int i = 0, tries = 0; i < dup && tries < dup * 50; ++tries) {
            Vec3 p(rng.range(b.lo.x, b.hi.x), rng.range(b.lo.y, b.hi.y), rng.range(b.lo.z, b.hi.z));
            if (!insideMesh(t, p)) continue;
            places.push_back({p, Vec3(0, 0, 1)});
            ++i;
          }
          break;
        }
      }
  } else {
    for (int i = 0; i < dup; ++i) places.push_back({Vec3(), Vec3(0, 0, 1)});
  }
  bool perp = params.getBool("perpendicular");
  float base = params.getFloat("baseScale") / 100.0f;
  float chaos = params.getFloat("vertexChaos");
  Vec3 rot(params.getFloat("rotX"), params.getFloat("rotY"), params.getFloat("rotZ"));
  Vec3 tr(params.getFloat("transX"), params.getFloat("transY"), params.getFloat("transZ"));
  Vec3 sc(params.getFloat("scaleX"), params.getFloat("scaleY"), params.getFloat("scaleZ"));
  bool uniform = params.getBool("uniformScale");
  float srcSize = src->mesh.bounds().radius();
  for (const Place& pl : places) {
    Vec3 r(rng.sym(rot.x), rng.sym(rot.y), rng.sym(rot.z));
    Vec3 t(rng.sym(tr.x), rng.sym(tr.y), rng.sym(tr.z));
    Vec3 s(1 + rng.sym(sc.x) / 100.0f, 1 + rng.sym(sc.y) / 100.0f, 1 + rng.sym(sc.z) / 100.0f);
    if (uniform) s = Vec3(s.x, s.x, s.x);
    Mat4 xf = Mat4::translate(pl.p) * (perp ? alignZ(pl.n) : Mat4()) * Mat4::translate(t) * Mat4::fromEulerDeg(r) *
              Mat4::scale(s * base);
    PolyMesh copy = src->mesh;
    if (chaos > 0)
      for (auto& p : copy.positions)
        p += Vec3(rng.sym(1), rng.sym(1), rng.sym(1)) * (chaos * srcSize * 0.05f);
    copy.transform(xf);
    out.append(copy);
  }
  if (dist && !params.getBool("hideDistribution")) out.append(dist->mesh);
  return out;
}

// ---------------------------------------------------------------------------------------------
// Connect

class ConnectObject : public CompoundObject {
 public:
  ConnectObject() {
    params.beginGroup("Interpolation");
    params.addInt("segments", "Segments", 1, 1, 1000);
    params.addFloat("tension", "Tension", 0.5, 0.0, 10.0, 0.05);
    params.beginGroup("Smoothing");
    params.addBool("smoothBridge", "Bridge", true);
    params.addBool("smoothEnds", "Ends", false);
  }
  std::string type() const override { return "Connect"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<ConnectObject>(*this); }
  std::vector<CompoundRole> roles() const override {
    return {{"operand", "Pick Operand", false, true, OperandSpace::Compound}};
  }
  PolyMesh build() const override;
};

PolyMesh ConnectObject::build() const {
  PolyMesh m;
  std::vector<int> owner;  // operand index per loop
  std::vector<std::vector<int>> loops;
  int idx = 0;
  for (const auto& op : operands) {
    if (op.isShape || op.mesh.empty()) continue;
    PolyMesh part = op.mesh;
    weldVertices(part);
    int offset = static_cast<int>(m.positions.size());
    for (auto& l : boundaryLoops(part)) {
      for (int& v : l) v += offset;
      if (l.size() >= 3) {
        loops.push_back(l);
        owner.push_back(idx);
      }
    }
    m.append(part);
    ++idx;
  }
  int segs = params.getInt("segments");
  float tension = params.getFloat("tension");
  uint32_t bridgeSg = params.getBool("smoothBridge") ? (1u << 25) : 0u;
  if (params.getBool("smoothEnds")) {
    // Ends share the bridge's group so the joint is smooth.
    for (auto& f : m.faces) f.smoothGroups |= bridgeSg ? bridgeSg : (1u << 25);
    if (!bridgeSg) bridgeSg = 1u << 25;
  }
  auto centroid = [&](const std::vector<int>& l) {
    Vec3 c;
    for (int v : l) c += m.positions[v];
    return c / static_cast<float>(l.size());
  };
  auto loopNormal = [&](const std::vector<int>& l) {  // Newell normal
    Vec3 n;
    for (size_t i = 0; i < l.size(); ++i) {
      const Vec3& a = m.positions[l[i]];
      const Vec3& b = m.positions[l[(i + 1) % l.size()]];
      n += Vec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y));
    }
    return length2(n) > 1e-20f ? normalize(n) : Vec3(0, 0, 1);
  };
  std::vector<bool> used(loops.size(), false);
  for (;;) {
    // Closest pair of open loops from different operands.
    int bi = -1, bj = -1;
    float best = kInfinity;
    for (size_t i = 0; i < loops.size(); ++i) {
      if (used[i]) continue;
      for (size_t j = i + 1; j < loops.size(); ++j) {
        if (used[j] || owner[i] == owner[j]) continue;
        float d = length(centroid(loops[i]) - centroid(loops[j]));
        if (d < best) {
          best = d;
          bi = static_cast<int>(i);
          bj = static_cast<int>(j);
        }
      }
    }
    if (bi < 0) break;
    used[bi] = used[bj] = true;
    std::vector<int> A = loops[bi];
    std::vector<int> B(loops[bj].rbegin(), loops[bj].rend());  // run the same way as A
    Vec3 ca = centroid(A), cb = centroid(B);
    Vec3 openA = loopNormal(A) * -1.0f, openB = loopNormal(loops[bj]) * -1.0f;
    float D = length(cb - ca);
    // Resample both loops to N points and align B's start to A's.
    int N = static_cast<int>(std::max(A.size(), B.size()));
    auto sample = [&](const std::vector<int>& l) {
      std::vector<float> cum(1, 0.0f);
      for (size_t i = 0; i < l.size(); ++i)
        cum.push_back(cum.back() + length(m.positions[l[(i + 1) % l.size()]] - m.positions[l[i]]));
      std::vector<Vec3> out;
      size_t e = 0;
      for (int k = 0; k < N; ++k) {
        float s = cum.back() * static_cast<float>(k) / N;
        while (e + 1 < l.size() && cum[e + 1] < s) ++e;
        float len = cum[e + 1] - cum[e];
        float t = len > 0 ? (s - cum[e]) / len : 0.0f;
        out.push_back(lerp(m.positions[l[e]], m.positions[l[(e + 1) % l.size()]], t));
      }
      return out;
    };
    std::vector<Vec3> sa = sample(A), sb = sample(B);
    int bestShift = 0;
    float bestCost = kInfinity;
    for (int s = 0; s < N; ++s) {
      float cost = 0;
      for (int k = 0; k < N; ++k) cost += length2((sa[k] - ca) - (sb[(k + s) % N] - cb));
      if (cost < bestCost) {
        bestCost = cost;
        bestShift = s;
      }
    }
    // Rotate the original B loop so its start is near the sampled start.
    {
      Vec3 start = sb[bestShift];
      size_t bestK = 0;
      float bd = kInfinity;
      for (size_t k = 0; k < B.size(); ++k) {
        float d = length2(m.positions[B[k]] - start);
        if (d < bd) {
          bd = d;
          bestK = k;
        }
      }
      std::rotate(B.begin(), B.begin() + static_cast<long>(bestK), B.end());
      std::rotate(sb.begin(), sb.begin() + bestShift, sb.end());
    }
    // Hermite rings between the loops.
    std::vector<int> prev = A;
    for (int r = 1; r < segs; ++r) {
      float t = static_cast<float>(r) / segs;
      float h00 = 2 * t * t * t - 3 * t * t + 1, h10 = t * t * t - 2 * t * t + t, h01 = -2 * t * t * t + 3 * t * t,
            h11 = t * t * t - t * t;
      std::vector<int> ring;
      for (int k = 0; k < N; ++k) {
        Vec3 p = sa[k] * h00 + openA * (tension * D * h10) + sb[k] * h01 + openB * (-tension * D * h11);
        ring.push_back(m.addVertex(p));
      }
      zipRings(m, prev, ring, bridgeSg, 0);
      prev = std::move(ring);
    }
    zipRings(m, prev, B, bridgeSg, 0);
  }
  return m;
}

// ---------------------------------------------------------------------------------------------
// ShapeMerge

class ShapeMergeObject : public CompoundObject {
 public:
  ShapeMergeObject() {
    params.beginGroup("Operations");
    params.addEnum("operation", "Operation", {"Cookie Cutter", "Merge"}, 1);
    params.addBool("invert", "Invert", false);
    params.addBool("setMatId", "Set Inside Material ID", false);
    params.addInt("matId", "Material ID", 2, 1, 65535);
  }
  std::string type() const override { return "ShapeMerge"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<ShapeMergeObject>(*this); }
  std::vector<CompoundRole> roles() const override {
    return {{"mesh", "Pick Mesh", false, false, OperandSpace::Compound},
            {"shape", "Pick Shape", true, true, OperandSpace::Compound}};
  }
  PolyMesh build() const override;
};

PolyMesh ShapeMergeObject::build() const {
  const CompoundOperand* meshOp = firstOperand("mesh");
  if (!meshOp) return PolyMesh();
  PolyMesh m = meshOp->mesh;
  bool cookie = params.getInt("operation") == 0;
  bool invert = params.getBool("invert");
  bool setId = params.getBool("setMatId");
  int insideId = params.getInt("matId") - 1;
  float eps = std::max(1e-5f, m.bounds().radius() * 1e-5f);
  for (const auto* sop : operandsWithRole("shape")) {
    ShapePlane pl = shapePlane(sop->shape);
    // Shape regions in plane coordinates.
    BezierShape local = sop->shape;
    std::vector<shape2d::Polygon> clipPolys;
    for (const auto& s : local.splines) {
      if (!s.closed) continue;
      shape2d::Polygon poly;
      for (const auto& p : s.polyline(6, true)) {
        Vec3 d = p - pl.o;
        poly.emplace_back(dot(d, pl.t), dot(d, pl.b));
      }
      if (poly.size() >= 3) clipPolys.push_back(std::move(poly));
    }
    if (clipPolys.empty()) continue;
    {
      // Orient by nesting so holes cut out under non-zero filling.
      std::vector<shape2d::Region> regs = shape2d::buildRegions(clipPolys);
      clipPolys.clear();
      for (auto& r : regs) {
        clipPolys.push_back(r.outer);
        for (auto& h : r.holes) clipPolys.push_back(h);
      }
    }
    PolyMesh next;
    next.positions = m.positions;
    next.uvs = m.uvs;
    auto to2 = [&](const Vec3& p) { Vec3 d = p - pl.o; return Vec2(dot(d, pl.t), dot(d, pl.b)); };
    for (const auto& f : m.faces) {
      shape2d::Polygon poly;
      for (int v : f.v) poly.push_back(to2(m.positions[v]));
      float area = shape2d::signedArea(poly);
      Vec3 fn = m.faceNormal(f);
      float facing = dot(normalize(fn), pl.n);
      auto keepWhole = [&](bool inside) {
        bool keep = cookie ? (inside == invert) : true;
        if (!keep) return;
        PolyMesh::Face nf = f;
        if (!cookie && setId && inside != invert) nf.matId = insideId;
        next.faces.push_back(std::move(nf));
      };
      if (std::fabs(area) < 1e-9f || std::fabs(facing) < 1e-4f) {
        // Seen edge-on from the shape: classify by its center.
        Vec2 c = to2(polyCentroid(m, f));
        int wind = 0;
        for (const auto& cp : clipPolys)
          if (shape2d::pointInPolygon(c, cp)) wind += shape2d::signedArea(cp) > 0 ? 1 : -1;
        keepWhole(wind != 0);
        continue;
      }
      shape2d::Polygon subject = poly;
      if (area < 0) std::reverse(subject.begin(), subject.end());
      std::vector<shape2d::Polygon> in = shape2d::clip(subject, clipPolys, true);
      std::vector<shape2d::Polygon> out = shape2d::clip(subject, clipPolys, false);
      double inArea = 0, outArea = 0;
      for (const auto& p : in) inArea += shape2d::signedArea(p);
      for (const auto& p : out) outArea += shape2d::signedArea(p);
      double whole = std::fabs(area);
      if (inArea <= whole * 1e-6) {
        keepWhole(false);
        continue;
      }
      if (outArea <= whole * 1e-6) {
        keepWhole(true);
        continue;
      }
      // Split: lift the clipped pieces back onto the face's plane and triangulate them.
      Vec3 p0 = m.positions[f.v[0]];
      float denom = dot(fn, pl.n);
      auto lift = [&](const Vec2& q) {
        Vec3 x = pl.o + pl.t * q.x + pl.b * q.y;
        float s = dot(fn, p0 - x) / denom;
        return x + pl.n * s;
      };
      bool hasUv = f.uv.size() == f.v.size();
      auto uvAt = [&](const Vec2& q) {
        // Barycentric UV from the face's fan triangle containing q.
        for (size_t k = 1; k + 1 < f.v.size(); ++k) {
          Vec2 a = poly[0], b = poly[k], c = poly[k + 1];
          float d = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
          if (std::fabs(d) < 1e-20f) continue;
          float l1 = ((b.y - c.y) * (q.x - c.x) + (c.x - b.x) * (q.y - c.y)) / d;
          float l2 = ((c.y - a.y) * (q.x - c.x) + (a.x - c.x) * (q.y - c.y)) / d;
          float l3 = 1 - l1 - l2;
          if (l1 >= -1e-3f && l2 >= -1e-3f && l3 >= -1e-3f)
            return m.uvs[f.uv[0]] * l1 + m.uvs[f.uv[k]] * l2 + m.uvs[f.uv[k + 1]] * l3;
        }
        return m.uvs[f.uv[0]];
      };
      auto emit = [&](const std::vector<shape2d::Polygon>& pieces, bool inside) {
        bool keep = cookie ? (inside == invert) : true;
        if (!keep) return;
        int matId = (!cookie && setId && inside != invert) ? insideId : f.matId;
        for (const auto& reg : shape2d::buildRegions(pieces)) {
          std::vector<Vec2> flat = reg.outer;
          for (const auto& h : reg.holes) flat.insert(flat.end(), h.begin(), h.end());
          std::vector<uint32_t> tris = shape2d::triangulate(reg);
          std::vector<int> vid, tid;
          for (const auto& q : flat) {
            vid.push_back(next.addVertex(lift(q)));
            if (hasUv) tid.push_back(next.addUv(uvAt(q)));
          }
          for (size_t i = 0; i + 2 < tris.size(); i += 3) {
            std::vector<int> v = {vid[tris[i]], vid[tris[i + 1]], vid[tris[i + 2]]};
            std::vector<int> t;
            if (hasUv) t = std::vector<int>{tid[tris[i]], tid[tris[i + 1]], tid[tris[i + 2]]};
            if (facing < 0) {
              std::swap(v[1], v[2]);
              if (hasUv) std::swap(t[1], t[2]);
            }
            next.addFace(std::move(v), std::move(t), f.smoothGroups, matId);
          }
        }
      };
      emit(in, true);
      emit(out, false);
    }
    weldTolerant(next, eps);
    m = std::move(next);
  }
  return m;
}

// ---------------------------------------------------------------------------------------------
// Terrain

class TerrainObject : public CompoundObject {
 public:
  TerrainObject() {
    params.beginGroup("Form");
    params.addEnum("form", "Form", {"Graded Surface", "Graded Solid", "Layered Solid"}, 0);
    params.addInt("steps", "Contour Steps", 2, 0, 20);
    params.beginGroup("Color by Elevation");
    params.addInt("zones", "Zones", 0, 0, 64);
    params.addFloat("baseElevation", "Base Elevation", 0, -1e7, 1e7, 1.0);
    params.addBool("autoBase", "Automatic Base", true);
  }
  std::string type() const override { return "Terrain"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<TerrainObject>(*this); }
  std::vector<CompoundRole> roles() const override {
    return {{"contour", "Pick Operand", true, true, OperandSpace::Compound}};
  }
  PolyMesh build() const override;
};

PolyMesh TerrainObject::build() const {
  std::vector<Vec3> pts;
  std::vector<std::pair<std::vector<Vec2>, float>> closedContours;
  int steps = params.getInt("steps");
  float zmin = kInfinity, zmax = -kInfinity;
  for (const auto* op : operandsWithRole("contour"))
    for (const auto& sp : op->shape.splines) {
      std::vector<Vec3> line = sp.polyline(steps, true);
      float zsum = 0;
      std::vector<Vec2> c2;
      for (const auto& p : line) {
        pts.push_back(p);
        zsum += p.z;
        c2.emplace_back(p.x, p.y);
        zmin = std::min(zmin, p.z);
        zmax = std::max(zmax, p.z);
      }
      if (sp.closed && line.size() >= 3) closedContours.push_back({c2, zsum / line.size()});
    }
  if (pts.size() < 3) return PolyMesh();
  float base = params.getBool("autoBase") ? zmin - std::max(1.0f, (zmax - zmin) * 0.1f) : params.getFloat("baseElevation");
  int zones = params.getInt("zones");
  int form = params.getInt("form");
  if (form == 2) {
    // Layered solid: every closed contour becomes a terrace from the base to its height.
    std::sort(closedContours.begin(), closedContours.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });
    PolyMesh m;
    for (size_t i = 0; i < closedContours.size(); ++i) {
      Contour2D c{shape2d::cleaned(closedContours[i].first), true};
      if (c.pts.size() < 3) continue;
      if (shape2d::signedArea(c.pts) < 0) std::reverse(c.pts.begin(), c.pts.end());
      float z = closedContours[i].second;
      PolyMesh layer = extrudeContours({c}, {{base, 0.0f}, {z, 0.0f}}, ExtrudeOptions());
      int zone = zones > 0 && zmax > zmin ? std::min(zones - 1, static_cast<int>((z - zmin) / (zmax - zmin) * zones)) : 0;
      for (auto& f : layer.faces) f.matId = zone;
      m.append(layer);
    }
    return m;
  }
  return terrainMesh(pts, form == 1, base, zones);
}

// ---------------------------------------------------------------------------------------------
// Conform

class ConformObject : public CompoundObject {
 public:
  ConformObject() {
    params.beginGroup("Vertex Projection Direction");
    params.addEnum("projection", "Projection",
                   {"Along Wrapper -Z", "Towards Wrapper Center", "Towards Wrapper Pivot", "Along Vertex Normals"}, 0);
    params.beginGroup("Wrapper Parameters");
    params.addFloat("defaultDistance", "Default Projection Distance", 10, 0.0, 1e7, 0.5);
    params.addFloat("standoff", "Standoff Distance", 1, 0.0, 1e7, 0.1);
    params.beginGroup("Display");
    params.addBool("hideTarget", "Hide Wrap-To Object", false);
  }
  std::string type() const override { return "Conform"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<ConformObject>(*this); }
  std::vector<CompoundRole> roles() const override {
    return {{"wrapper", "Pick Wrapper", false, false, OperandSpace::Compound},
            {"target", "Pick Wrap-To Object", false, false, OperandSpace::Compound}};
  }
  PolyMesh build() const override;
};

PolyMesh ConformObject::build() const {
  const CompoundOperand* w = firstOperand("wrapper");
  const CompoundOperand* t = firstOperand("target");
  if (!w) return PolyMesh();
  PolyMesh m = w->mesh;
  if (t && !t->mesh.empty()) {
    TriMesh target = triangulate(t->mesh);
    int mode = params.getInt("projection");
    float standoff = params.getFloat("standoff"), defDist = params.getFloat("defaultDistance");
    Vec3 center = m.bounds().center();
    std::vector<Vec3> normals = m.vertexNormals();
    for (size_t i = 0; i < m.positions.size(); ++i) {
      Vec3 p = m.positions[i];
      Vec3 d;
      switch (mode) {
        case 1: d = center - p; break;
        case 2: d = Vec3() - p; break;
        case 3: d = normals[i] * -1.0f; break;
        default: d = Vec3(0, 0, -1); break;
      }
      if (length2(d) < 1e-12f) continue;
      d = normalize(d);
      float hit = intersectTriMesh(target, Ray{p, d});
      if (hit >= 0) m.positions[i] = p + d * std::max(0.0f, hit - standoff);
      else m.positions[i] = p + d * defDist;
    }
  }
  if (t && !params.getBool("hideTarget")) m.append(t->mesh);
  return m;
}

// ---------------------------------------------------------------------------------------------
// BlobMesh

class BlobMeshObject : public CompoundObject {
 public:
  BlobMeshObject() {
    params.beginGroup("Parameters");
    params.addFloat("size", "Size", 20, 0.01, 1e7, 1.0);
    params.addFloat("tension", "Tension", 1, 0.01, 1.0, 0.01);
    params.addFloat("coarseness", "Evaluation Coarseness", 3, 0.001, 1e7, 0.5);
    params.addBool("relative", "Relative Coarseness", false);
    params.addBool("useVertices", "Use All Vertices", true);
  }
  std::string type() const override { return "BlobMesh"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<BlobMeshObject>(*this); }
  std::vector<CompoundRole> roles() const override {
    return {{"source", "Add", false, true, OperandSpace::Compound}};
  }
  PolyMesh build() const override {
    std::vector<Vec3> centers;
    for (const auto* op : operandsWithRole("source")) {
      if (params.getBool("useVertices") || op->mesh.positions.empty()) {
        for (const auto& p : op->mesh.positions) centers.push_back(p);
        if (op->mesh.positions.empty()) centers.push_back(Vec3());
      } else {
        centers.push_back(op->mesh.bounds().center());
      }
    }
    if (operandsWithRole("source").empty()) centers.push_back(Vec3());  // a lone blob at the pivot
    float size = params.getFloat("size");
    float cell = params.getFloat("coarseness");
    if (params.getBool("relative")) cell = size / std::max(0.001f, cell);
    return metaballMesh(centers, size, params.getFloat("tension"), cell);
  }
};

}  // namespace

// ---------------------------------------------------------------------------------------------
// Shared building blocks

std::vector<std::vector<int>> boundaryLoops(const PolyMesh& m) {
  std::map<std::pair<int, int>, int> directed;
  for (const auto& f : m.faces)
    for (size_t k = 0; k < f.v.size(); ++k) directed[{f.v[k], f.v[(k + 1) % f.v.size()]}]++;
  std::multimap<int, int> next;  // boundary half-edges a -> b
  for (const auto& [e, n] : directed)
    if (!directed.count({e.second, e.first})) next.emplace(e.first, e.second);
  std::vector<std::vector<int>> loops;
  while (!next.empty()) {
    auto it = next.begin();
    int start = it->first;
    std::vector<int> loop = {start};
    int cur = it->second;
    next.erase(it);
    while (cur != start) {
      loop.push_back(cur);
      auto jt = next.find(cur);
      if (jt == next.end()) break;
      cur = jt->second;
      next.erase(jt);
    }
    if (cur == start && loop.size() >= 3) loops.push_back(std::move(loop));
  }
  return loops;
}

PolyMesh terrainMesh(const std::vector<Vec3>& pointsIn, bool solid, float baseZ, int zones) {
  PolyMesh m;
  // Unique XY points (the first height wins).
  std::map<std::pair<long long, long long>, int> seen;
  std::vector<Vec2> xy;
  for (const auto& p : pointsIn) {
    auto key = std::make_pair(std::llround(p.x * 1000.0), std::llround(p.y * 1000.0));
    if (seen.count(key)) continue;
    seen[key] = static_cast<int>(m.positions.size());
    m.addVertex(p);
    xy.emplace_back(p.x, p.y);
  }
  if (xy.size() < 3) return PolyMesh();
  std::vector<uint32_t> tris = shape2d::delaunay(xy);
  float zmin = kInfinity, zmax = -kInfinity;
  for (const auto& p : m.positions) {
    zmin = std::min(zmin, p.z);
    zmax = std::max(zmax, p.z);
  }
  for (size_t i = 0; i + 2 < tris.size(); i += 3) {
    Vec2 a = xy[tris[i]], b = xy[tris[i + 1]], c = xy[tris[i + 2]];
    if (std::fabs((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y)) < 1e-9f) continue;  // sliver
    float z = (m.positions[tris[i]].z + m.positions[tris[i + 1]].z + m.positions[tris[i + 2]].z) / 3.0f;
    int zone = zones > 0 && zmax > zmin ? std::min(zones - 1, static_cast<int>((z - zmin) / (zmax - zmin) * zones)) : 0;
    m.addFace({static_cast<int>(tris[i]), static_cast<int>(tris[i + 1]), static_cast<int>(tris[i + 2])}, {}, 1,
              zone);
  }
  // Planar UVs over the footprint.
  Aabb b = m.bounds();
  float sz = std::max(b.extent().x, b.extent().y);
  if (sz > 0) {
    for (const auto& p : m.positions) m.addUv(Vec2((p.x - b.lo.x) / sz, (p.y - b.lo.y) / sz));
    for (auto& f : m.faces) f.uv = f.v;
  }
  if (solid) {
    for (const auto& loop : boundaryLoops(m)) {
      std::vector<int> low;
      for (int v : loop) low.push_back(m.addVertex(Vec3(m.positions[v].x, m.positions[v].y, baseZ)));
      size_t n = loop.size();
      for (size_t i = 0; i < n; ++i) {
        size_t j = (i + 1) % n;
        m.addFace({loop[j], loop[i], low[i], low[j]}, {}, 2, 0);
      }
      std::vector<int> bottom(low.rbegin(), low.rend());
      m.addFace(bottom, {}, 4, 0);
    }
    // Faces without UVs next to faces with UVs are fine: the triangulator handles both.
  }
  return m;
}

PolyMesh metaballMesh(const std::vector<Vec3>& centers, float radius, float tension, float cellSize) {
  PolyMesh m;
  if (centers.empty() || radius <= 0) return m;
  tension = clampf(tension, 0.01f, 1.0f);
  float R = radius * (1.0f + 1.0f / tension);
  auto falloff = [&](float d2) {
    float s = d2 / (R * R);
    if (s >= 1.0f) return 0.0f;
    float u = 1.0f - s;
    return u * u * u;
  };
  float iso = falloff(radius * radius);
  Aabb box;
  for (const auto& c : centers) box.expand(c);
  Vec3 pad(R, R, R);
  box.lo = box.lo - pad;
  box.hi = box.hi + pad;
  Vec3 ext = box.extent();
  float cell = std::max(cellSize, radius * 0.05f);
  // Bound the grid size.
  float maxCells = 120.0f;
  cell = std::max(cell, std::max(ext.x, std::max(ext.y, ext.z)) / maxCells);
  int nx = static_cast<int>(std::ceil(ext.x / cell)) + 1, ny = static_cast<int>(std::ceil(ext.y / cell)) + 1,
      nz = static_cast<int>(std::ceil(ext.z / cell)) + 1;
  std::vector<float> field(static_cast<size_t>(nx) * ny * nz, 0.0f);
  auto id = [&](int i, int j, int k) { return static_cast<size_t>(i) + static_cast<size_t>(nx) * (j + static_cast<size_t>(ny) * k); };
  auto nodePos = [&](int i, int j, int k) { return box.lo + Vec3(i * cell, j * cell, k * cell); };
  for (const auto& c : centers) {
    Vec3 lo = (c - box.lo - pad) / cell, hi = (c - box.lo + pad) / cell;
    int i0 = std::max(0, static_cast<int>(std::floor(lo.x))), i1 = std::min(nx - 1, static_cast<int>(std::ceil(hi.x)));
    int j0 = std::max(0, static_cast<int>(std::floor(lo.y))), j1 = std::min(ny - 1, static_cast<int>(std::ceil(hi.y)));
    int k0 = std::max(0, static_cast<int>(std::floor(lo.z))), k1 = std::min(nz - 1, static_cast<int>(std::ceil(hi.z)));
    for (int k = k0; k <= k1; ++k)
      for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i) field[id(i, j, k)] += falloff(length2(nodePos(i, j, k) - c));
  }
  // Marching tetrahedra (6 tetrahedra per cube around its main diagonal, conforming across cubes).
  static const int kTets[6][4] = {{0, 1, 3, 7}, {0, 1, 5, 7}, {0, 2, 3, 7}, {0, 2, 6, 7}, {0, 4, 5, 7}, {0, 4, 6, 7}};
  std::unordered_map<uint64_t, int> edgeVert;
  auto edgeVertex = [&](size_t a, size_t b, const Vec3& pa, const Vec3& pb, float fa, float fb) {
    if (a > b) {
      std::swap(a, b);
    }
    uint64_t key = static_cast<uint64_t>(a) * 0x100000000ull + b;
    auto it = edgeVert.find(key);
    if (it != edgeVert.end()) return it->second;
    float t = clampf((iso - fa) / (fb - fa), 0.001f, 0.999f);
    int v = m.addVertex(lerp(pa, pb, t));
    edgeVert.emplace(key, v);
    return v;
  };
  for (int k = 0; k + 1 < nz; ++k)
    for (int j = 0; j + 1 < ny; ++j)
      for (int i = 0; i + 1 < nx; ++i) {
        size_t cid[8];
        Vec3 cp[8];
        float cf[8];
        bool any = false, all = true;
        for (int c = 0; c < 8; ++c) {
          int ci = i + (c & 1), cj = j + ((c >> 1) & 1), ck = k + ((c >> 2) & 1);
          cid[c] = id(ci, cj, ck);
          cp[c] = nodePos(ci, cj, ck);
          cf[c] = field[cid[c]];
          bool in = cf[c] > iso;
          any = any || in;
          all = all && in;
        }
        if (!any || all) continue;
        for (const auto& tet : kTets) {
          int in[4], out[4], ni = 0, no = 0;
          for (int q = 0; q < 4; ++q) {
            if (cf[tet[q]] > iso) in[ni++] = tet[q];
            else out[no++] = tet[q];
          }
          if (ni == 0 || ni == 4) continue;
          Vec3 cin, cout;
          for (int q = 0; q < ni; ++q) cin += cp[in[q]];
          for (int q = 0; q < no; ++q) cout += cp[out[q]];
          Vec3 outward = cout / static_cast<float>(no) - cin / static_cast<float>(ni);
          auto ev = [&](int a, int b) { return edgeVertex(cid[a], cid[b], cp[a], cp[b], cf[a], cf[b]); };
          auto tri = [&](int a, int b, int c) {
            Vec3 n = cross(m.positions[b] - m.positions[a], m.positions[c] - m.positions[a]);
            if (dot(n, outward) < 0) std::swap(b, c);
            m.addFace({a, b, c}, {}, 1, 0);
          };
          if (ni == 1) tri(ev(in[0], out[0]), ev(in[0], out[1]), ev(in[0], out[2]));
          else if (ni == 3) tri(ev(out[0], in[0]), ev(out[0], in[1]), ev(out[0], in[2]));
          else {
            int p0 = ev(in[0], out[0]), p1 = ev(in[0], out[1]), p2 = ev(in[1], out[1]), p3 = ev(in[1], out[0]);
            tri(p0, p1, p2);
            tri(p0, p2, p3);
          }
        }
      }
  return m;
}

void registerCompoundTypes() {
  const char* geo = "Geometry";
  const char* sub = "Compound Objects";
  registerObjectType({"Scatter", "Scatter", geo, sub, [] { return std::make_unique<ScatterObject>(); }});
  registerObjectType({"Conform", "Conform", geo, sub, [] { return std::make_unique<ConformObject>(); }});
  registerObjectType({"Connect", "Connect", geo, sub, [] { return std::make_unique<ConnectObject>(); }});
  registerObjectType({"BlobMesh", "BlobMesh", geo, sub, [] { return std::make_unique<BlobMeshObject>(); }});
  registerObjectType({"ShapeMerge", "ShapeMerge", geo, sub, [] { return std::make_unique<ShapeMergeObject>(); }});
  registerObjectType({"Terrain", "Terrain", geo, sub, [] { return std::make_unique<TerrainObject>(); }});
  registerObjectType({"Loft", "Loft", geo, sub, [] { return std::make_unique<LoftObject>(); }});
}

}  // namespace veraa
