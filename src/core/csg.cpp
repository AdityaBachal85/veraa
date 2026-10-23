#include "csg.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <tuple>

#include "polyops.h"
#include "registry.h"

namespace veraa {

namespace {

struct CVert {
  Vec3 p;
  Vec2 uv;
};

struct CPoly {
  std::vector<CVert> v;
  Vec3 n;
  float w = 0.0f;
  int matId = 0;
  uint32_t sg = 1;
  bool hasUv = false;

  void flip() {
    std::reverse(v.begin(), v.end());
    n = -n;
    w = -w;
  }
};

struct Plane {
  Vec3 n;
  float w = 0.0f;
};

bool computePlane(CPoly& p) {
  Vec3 n, c;
  size_t k = p.v.size();
  for (size_t i = 0; i < k; ++i) {
    const Vec3& a = p.v[i].p;
    const Vec3& b = p.v[(i + 1) % k].p;
    n.x += (a.y - b.y) * (a.z + b.z);
    n.y += (a.z - b.z) * (a.x + b.x);
    n.z += (a.x - b.x) * (a.y + b.y);
    c += a;
  }
  float len = length(n);
  if (len < 1e-12f) return false;
  p.n = n / len;
  p.w = dot(p.n, c / static_cast<float>(k));
  return true;
}

enum : int { kCoplanar = 0, kFront = 1, kBack = 2, kSpanning = 3 };

void splitPolygon(const Plane& pl, const CPoly& poly, float eps, std::vector<CPoly>& coFront, std::vector<CPoly>& coBack,
                  std::vector<CPoly>& front, std::vector<CPoly>& back) {
  int polyType = 0;
  std::vector<int> types(poly.v.size());
  std::vector<float> dists(poly.v.size());
  for (size_t i = 0; i < poly.v.size(); ++i) {
    float t = dot(pl.n, poly.v[i].p) - pl.w;
    int type = t < -eps ? kBack : (t > eps ? kFront : kCoplanar);
    polyType |= type;
    types[i] = type;
    dists[i] = t;
  }
  switch (polyType) {
    case kCoplanar: (dot(pl.n, poly.n) > 0 ? coFront : coBack).push_back(poly); break;
    case kFront: front.push_back(poly); break;
    case kBack: back.push_back(poly); break;
    default: {
      CPoly f = poly, b = poly;
      f.v.clear();
      b.v.clear();
      size_t k = poly.v.size();
      for (size_t i = 0; i < k; ++i) {
        size_t j = (i + 1) % k;
        int ti = types[i], tj = types[j];
        const CVert& vi = poly.v[i];
        const CVert& vj = poly.v[j];
        if (ti != kBack) f.v.push_back(vi);
        if (ti != kFront) b.v.push_back(vi);
        if ((ti | tj) == kSpanning) {
          float denom = dists[i] - dists[j];
          float t = std::fabs(denom) > 1e-20f ? dists[i] / denom : 0.5f;
          CVert v{lerp(vi.p, vj.p, t), vi.uv + (vj.uv - vi.uv) * t};
          f.v.push_back(v);
          b.v.push_back(v);
        }
      }
      if (f.v.size() >= 3) front.push_back(std::move(f));
      if (b.v.size() >= 3) back.push_back(std::move(b));
      break;
    }
  }
}

struct BspNode {
  Plane plane;
  bool hasPlane = false;
  std::unique_ptr<BspNode> front, back;
  std::vector<CPoly> polys;
};

class Bsp {
 public:
  explicit Bsp(float eps) : eps_(eps), root_(std::make_unique<BspNode>()) {}

  void build(std::vector<CPoly> polys) {
    std::vector<std::pair<BspNode*, std::vector<CPoly>>> stack;
    stack.emplace_back(root_.get(), std::move(polys));
    while (!stack.empty()) {
      auto [node, list] = std::move(stack.back());
      stack.pop_back();
      if (list.empty()) continue;
      if (!node->hasPlane) {
        node->plane = {list[0].n, list[0].w};
        node->hasPlane = true;
      }
      std::vector<CPoly> f, b;
      for (const auto& p : list) splitPolygon(node->plane, p, eps_, node->polys, node->polys, f, b);
      if (!f.empty()) {
        if (!node->front) node->front = std::make_unique<BspNode>();
        stack.emplace_back(node->front.get(), std::move(f));
      }
      if (!b.empty()) {
        if (!node->back) node->back = std::make_unique<BspNode>();
        stack.emplace_back(node->back.get(), std::move(b));
      }
    }
  }

  // Removes the parts of `polys` inside this solid.
  std::vector<CPoly> clipPolygons(std::vector<CPoly> polys) const {
    std::vector<CPoly> out;
    std::vector<std::pair<const BspNode*, std::vector<CPoly>>> stack;
    stack.emplace_back(root_.get(), std::move(polys));
    while (!stack.empty()) {
      auto [node, list] = std::move(stack.back());
      stack.pop_back();
      if (!node->hasPlane) {
        out.insert(out.end(), list.begin(), list.end());
        continue;
      }
      std::vector<CPoly> f, b;
      for (const auto& p : list) splitPolygon(node->plane, p, eps_, f, b, f, b);
      if (node->front) stack.emplace_back(node->front.get(), std::move(f));
      else out.insert(out.end(), f.begin(), f.end());
      if (node->back) stack.emplace_back(node->back.get(), std::move(b));
    }
    return out;
  }

  void clipTo(const Bsp& other) {
    forEach([&](BspNode& n) { n.polys = other.clipPolygons(std::move(n.polys)); });
  }

  void invert() {
    forEach([](BspNode& n) {
      for (auto& p : n.polys) p.flip();
      n.plane.n = -n.plane.n;
      n.plane.w = -n.plane.w;
      std::swap(n.front, n.back);
    });
  }

  std::vector<CPoly> allPolygons() const {
    std::vector<CPoly> out;
    std::vector<const BspNode*> stack = {root_.get()};
    while (!stack.empty()) {
      const BspNode* n = stack.back();
      stack.pop_back();
      out.insert(out.end(), n->polys.begin(), n->polys.end());
      if (n->front) stack.push_back(n->front.get());
      if (n->back) stack.push_back(n->back.get());
    }
    return out;
  }

 private:
  template <class F>
  void forEach(F fn) {
    std::vector<BspNode*> stack = {root_.get()};
    while (!stack.empty()) {
      BspNode* n = stack.back();
      stack.pop_back();
      fn(*n);
      if (n->front) stack.push_back(n->front.get());
      if (n->back) stack.push_back(n->back.get());
    }
  }

  float eps_;
  std::unique_ptr<BspNode> root_;
};

bool isConvex(const PolyMesh& m, const PolyMesh::Face& f) {
  if (f.v.size() <= 3) return true;
  Vec3 n = normalize(m.faceNormal(f));
  size_t k = f.v.size();
  for (size_t i = 0; i < k; ++i) {
    const Vec3& a = m.positions[f.v[(i + k - 1) % k]];
    const Vec3& b = m.positions[f.v[i]];
    const Vec3& c = m.positions[f.v[(i + 1) % k]];
    if (dot(cross(b - a, c - b), n) < 0.0f) return false;
    // Non-planar polygons are triangulated too.
    if (std::fabs(dot(b - m.positions[f.v[0]], n)) > 1e-3f * std::max(1.0f, length(b - a))) return false;
  }
  return true;
}

std::vector<CPoly> toPolys(const PolyMesh& m) {
  std::vector<CPoly> out;
  out.reserve(m.faces.size());
  for (const auto& f : m.faces) {
    bool uv = f.uv.size() == f.v.size() && !m.uvs.empty();
    auto make = [&](const std::vector<int>& corners) {
      CPoly p;
      p.matId = f.matId;
      p.sg = f.smoothGroups;
      p.hasUv = uv;
      for (int c : corners) p.v.push_back({m.positions[f.v[c]], uv ? m.uvs[f.uv[c]] : Vec2()});
      if (computePlane(p)) out.push_back(std::move(p));
    };
    if (isConvex(m, f)) {
      std::vector<int> c(f.v.size());
      for (size_t i = 0; i < c.size(); ++i) c[i] = static_cast<int>(i);
      make(c);
    } else {
      std::vector<int> tris = faceTriangles(m, f);
      for (size_t i = 0; i + 2 < tris.size(); i += 3) make({tris[i], tris[i + 1], tris[i + 2]});
    }
  }
  return out;
}

PolyMesh toMesh(const std::vector<CPoly>& polys, float weld) {
  PolyMesh m;
  std::map<std::tuple<int64_t, int64_t, int64_t>, int> index;
  auto key = [weld](const Vec3& p) {
    return std::make_tuple(static_cast<int64_t>(std::llround(p.x / weld)), static_cast<int64_t>(std::llround(p.y / weld)),
                           static_cast<int64_t>(std::llround(p.z / weld)));
  };
  bool anyUv = false;
  for (const auto& p : polys) anyUv = anyUv || p.hasUv;
  for (const auto& p : polys) {
    PolyMesh::Face f;
    f.matId = p.matId;
    f.smoothGroups = p.sg;
    for (const auto& v : p.v) {
      auto k = key(v.p);
      auto it = index.find(k);
      if (it == index.end()) it = index.emplace(k, m.addVertex(v.p)).first;
      if (!f.v.empty() && f.v.back() == it->second) continue;
      f.v.push_back(it->second);
      if (anyUv) f.uv.push_back(m.addUv(v.uv));
    }
    while (f.v.size() > 1 && f.v.front() == f.v.back()) {
      f.v.pop_back();
      if (anyUv) f.uv.pop_back();
    }
    if (f.v.size() < 3) continue;
    if (length(m.faceNormal(f)) < weld * weld * 1e-3f) continue;  // sliver
    m.faces.push_back(std::move(f));
  }
  return m;
}

}  // namespace

PolyMesh csgMesh(const PolyMesh& a, const PolyMesh& b, CsgOp op) {
  if (op == CsgOp::Merge) {
    PolyMesh r = a;
    if (!a.uvs.empty() || !b.uvs.empty()) {
      PolyMesh bb = b;
      ensureFaceUvs(r);
      ensureFaceUvs(bb);
      r.append(bb);
      ensureFaceUvs(r);
    } else {
      r.append(b);
    }
    return r;
  }
  Aabb box = a.bounds();
  box.expand(b.bounds().lo);
  box.expand(b.bounds().hi);
  float scale = std::max(1.0f, box.radius());
  float eps = 1e-5f * scale;
  Bsp ta(eps), tb(eps);
  if (op == CsgOp::SubtractBA) {
    ta.build(toPolys(b));
    tb.build(toPolys(a));
  } else {
    ta.build(toPolys(a));
    tb.build(toPolys(b));
  }
  switch (op) {
    case CsgOp::Union:
      ta.clipTo(tb);
      tb.clipTo(ta);
      tb.invert();
      tb.clipTo(ta);
      tb.invert();
      ta.build(tb.allPolygons());
      break;
    case CsgOp::Intersection:
      ta.invert();
      tb.clipTo(ta);
      tb.invert();
      ta.clipTo(tb);
      tb.clipTo(ta);
      ta.build(tb.allPolygons());
      ta.invert();
      break;
    default:  // subtraction (operands already ordered)
      ta.invert();
      ta.clipTo(tb);
      tb.clipTo(ta);
      tb.invert();
      tb.clipTo(ta);
      tb.invert();
      ta.build(tb.allPolygons());
      ta.invert();
      break;
  }
  PolyMesh out = toMesh(ta.allPolygons(), eps * 10.0f);
  ensureFaceUvs(out);
  return out;
}

// ---------------------------------------------------------------------------------------------
// Boolean compound object

BooleanObject::BooleanObject() {
  params.beginGroup("Operation (for newly picked operands)");
  params.addEnum("operation", "Operation", {"Union", "Intersection", "Subtraction (A-B)", "Subtraction (B-A)", "Merge"}, 2);
  params.beginGroup("Display");
  params.addBool("showOperands", "Show Operands", false);
}

PolyMesh BooleanObject::build() const {
  PolyMesh r = meshA;
  for (const auto& o : operands) {
    PolyMesh b = o.mesh;
    b.transform(o.transform);
    if (o.transform.determinant3x3() < 0.0f) b.flipFaces();
    if (params.getBool("showOperands")) {
      r.append(b);
      continue;
    }
    r = csgMesh(r, b, o.op);
  }
  return r;
}

json BooleanObject::toJson() const {
  json j = SceneObject::toJson();
  j["nameA"] = nameA;
  j["meshA"] = polyMeshToJson(meshA);
  json ops = json::array();
  for (const auto& o : operands) {
    json m = json::array();
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c) m.push_back(o.transform.m[r][c]);
    ops.push_back({{"name", o.name}, {"mesh", polyMeshToJson(o.mesh)}, {"transform", m}, {"op", static_cast<int>(o.op)},
                   {"material", o.materialId}});
  }
  j["operands"] = ops;
  return j;
}

void BooleanObject::fromJson(const json& j) {
  SceneObject::fromJson(j);
  nameA = j.value("nameA", std::string("A"));
  meshA = j.contains("meshA") ? polyMeshFromJson(j["meshA"]) : PolyMesh();
  operands.clear();
  if (j.contains("operands"))
    for (const auto& o : j["operands"]) {
      Operand op;
      op.name = o.value("name", std::string());
      op.mesh = o.contains("mesh") ? polyMeshFromJson(o["mesh"]) : PolyMesh();
      if (o.contains("transform") && o["transform"].size() == 16)
        for (int r = 0; r < 4; ++r)
          for (int c = 0; c < 4; ++c) op.transform.m[r][c] = o["transform"][r * 4 + c].get<float>();
      op.op = static_cast<CsgOp>(std::clamp(o.value("op", 2), 0, 4));
      op.materialId = o.value("material", 0u);
      operands.push_back(std::move(op));
    }
}

void registerCompoundObjectTypes() {
  registerObjectType({"Boolean", "Boolean", "Geometry", "Compound Objects", [] { return std::make_unique<BooleanObject>(); }});
}

}  // namespace veraa
