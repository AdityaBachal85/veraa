#include "meshmods.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <queue>
#include <random>
#include <set>
#include <unordered_map>

#include "polyops.h"
#include "registry.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <stb/stb_image.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace veraa {

namespace {

const std::vector<std::string> kAxes = {"X", "Y", "Z"};

template <class Derived>
class ModBase : public Modifier {
 public:
  std::unique_ptr<Modifier> clone() const override { return std::make_unique<Derived>(static_cast<const Derived&>(*this)); }
};

double binomial(int n, int k) {
  double r = 1.0;
  for (int i = 1; i <= k; ++i) r = r * (n - k + i) / i;
  return r;
}

float bernstein(int n, int i, float t) {
  return static_cast<float>(binomial(n, i) * std::pow(static_cast<double>(t), i) * std::pow(1.0 - t, n - i));
}

// Uniform cubic B-spline basis value for distance d from a knot center.
float bsplineKernel(float d) {
  d = std::fabs(d);
  if (d >= 2.0f) return 0.0f;
  if (d >= 1.0f) {
    float t = 2.0f - d;
    return t * t * t / 6.0f;
  }
  return (4.0f - 6.0f * d * d + 3.0f * d * d * d) / 6.0f;
}

// Faces whose vertices are all selected by the stack selection (all faces without one).
std::vector<int> stackSelectedFaces(const GeomState& st) {
  std::vector<int> faces;
  bool all = st.selWeights.size() != st.mesh.positions.size();
  for (size_t f = 0; f < st.mesh.faces.size(); ++f) {
    bool in = true;
    if (!all)
      for (int v : st.mesh.faces[f].v) in = in && st.selWeights[v] >= 0.5f;
    if (in) faces.push_back(static_cast<int>(f));
  }
  return faces;
}

// Merges vertices given as (from -> to) pairs.
void weldPairs(PolyMesh& m, const std::vector<std::pair<int, int>>& pairs) {
  if (pairs.empty()) return;
  json p = json::array();
  for (const auto& [a, b] : pairs) p.push_back(json::array({a, b}));
  PolySelection sel;
  runPolyOp(m, sel, {{"op", "weldPairs"}, {"pairs", p}});
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// FFD

FfdModifier::FfdModifier(Kind kind) : kind_(kind) {
  if (kind == Box) {
    params.beginGroup("Dimensions");
    params.addInt("lenPts", "Length Points", 4, 2, 10);
    params.addInt("widPts", "Width Points", 4, 2, 10);
    params.addInt("hgtPts", "Height Points", 4, 2, 10);
  } else if (kind == Cylinder) {
    params.beginGroup("Dimensions");
    params.addInt("sidePts", "Sides", 6, 3, 32);
    params.addInt("radialPts", "Radial", 2, 2, 10);
    params.addInt("hgtPts", "Height", 2, 2, 10);
  }
  params.beginGroup("Deform");
  params.addBool("allVertices", "All Vertices", false);
  params.addFloat("falloff", "Falloff", 0.0, 0.0, 1e6, 1.0);
}

std::string FfdModifier::type() const {
  switch (kind_) {
    case Fixed2: return "FFD2";
    case Fixed3: return "FFD3";
    case Fixed4: return "FFD4";
    case Box: return "FFDBox";
    default: return "FFDCyl";
  }
}

void FfdModifier::dims(int& a, int& b, int& c) const {
  switch (kind_) {
    case Fixed2: a = b = c = 2; break;
    case Fixed3: a = b = c = 3; break;
    case Fixed4: a = b = c = 4; break;
    case Box:
      a = params.getInt("widPts");
      b = params.getInt("lenPts");
      c = params.getInt("hgtPts");
      break;
    case Cylinder:
      a = params.getInt("sidePts");
      b = params.getInt("radialPts");
      c = params.getInt("hgtPts");
      break;
  }
}

int FfdModifier::pointCount() const {
  int a, b, c;
  dims(a, b, c);
  return a * b * c;
}

Vec3 FfdModifier::offset(int i) const {
  return i >= 0 && i < static_cast<int>(offsets_.size()) ? offsets_[i] : Vec3();
}

void FfdModifier::setOffset(int i, const Vec3& d) {
  int n = pointCount();
  if (i < 0 || i >= n) return;
  if (static_cast<int>(offsets_.size()) != n) offsets_.resize(n);
  offsets_[i] = d;
  touch();
}

Vec3 FfdModifier::restPoint(int i) const {
  int a, b, c;
  dims(a, b, c);
  Aabb box = boundsValid_ ? bounds_ : Aabb{Vec3(-50, -50, 0), Vec3(50, 50, 100)};
  Vec3 lo = box.lo, ext = box.extent();
  int ia = i % a, ib = (i / a) % b, ic = i / (a * b);
  if (kind_ == Cylinder) {
    float th = kTwoPi * ia / a;
    float r = static_cast<float>(ib) / (b - 1);
    Vec3 c0 = box.center();
    // The cylinder encloses the box: radius = half diagonal of the XY footprint.
    float rx = ext.x * 0.5f * 1.41421356f, ry = ext.y * 0.5f * 1.41421356f;
    return Vec3(c0.x + std::cos(th) * rx * r, c0.y + std::sin(th) * ry * r, lo.z + ext.z * ic / (c - 1));
  }
  return Vec3(lo.x + ext.x * ia / (a - 1), lo.y + ext.y * ib / (b - 1), lo.z + ext.z * ic / (c - 1));
}

std::vector<std::pair<int, int>> FfdModifier::latticeEdges() const {
  int a, b, c;
  dims(a, b, c);
  std::vector<std::pair<int, int>> e;
  auto idx = [&](int i, int j, int k) { return (k * b + j) * a + i; };
  for (int k = 0; k < c; ++k)
    for (int j = 0; j < b; ++j)
      for (int i = 0; i < a; ++i) {
        if (kind_ == Cylinder) {
          e.push_back({idx(i, j, k), idx((i + 1) % a, j, k)});
        } else if (i + 1 < a) {
          e.push_back({idx(i, j, k), idx(i + 1, j, k)});
        }
        if (j + 1 < b) e.push_back({idx(i, j, k), idx(i, j + 1, k)});
        if (k + 1 < c) e.push_back({idx(i, j, k), idx(i, j, k + 1)});
      }
  return e;
}

void FfdModifier::apply(PolyMesh& m) const {
  Aabb box = m.bounds();
  if (!box.valid()) return;
  // Flat inputs still get a usable lattice.
  Vec3 ext = box.extent();
  float pad = std::max(1e-3f, std::max(ext.x, std::max(ext.y, ext.z)) * 1e-3f);
  for (int k = 0; k < 3; ++k)
    if (ext[k] < pad) {
      box.lo[k] -= pad;
      box.hi[k] += pad;
    }
  bounds_ = box;
  boundsValid_ = true;
  int a, b, c;
  dims(a, b, c);
  int n = a * b * c;
  bool any = false;
  for (int i = 0; i < n && i < static_cast<int>(offsets_.size()); ++i) any = any || length2(offsets_[i]) > 0.0f;
  if (!any) return;
  bool all = params.getBool("allVertices");
  float falloff = params.getFloat("falloff");
  ext = box.extent();
  Vec3 center = box.center();
  float rx = ext.x * 0.5f * 1.41421356f, ry = ext.y * 0.5f * 1.41421356f;
  std::vector<float> wa(a), wb(b), wc(c);
  for (auto& p : m.positions) {
    float s, t, u;
    if (kind_ == Cylinder) {
      float dx = (p.x - center.x) / std::max(rx, 1e-6f), dy = (p.y - center.y) / std::max(ry, 1e-6f);
      t = std::sqrt(dx * dx + dy * dy);
      float th = std::atan2(dy, dx);
      if (th < 0) th += kTwoPi;
      s = th / kTwoPi * a;
      u = (p.z - box.lo.z) / ext.z;
    } else {
      s = (p.x - box.lo.x) / ext.x;
      t = (p.y - box.lo.y) / ext.y;
      u = (p.z - box.lo.z) / ext.z;
    }
    bool inside = t >= -1e-4f && t <= 1.0001f && u >= -1e-4f && u <= 1.0001f &&
                  (kind_ == Cylinder || (s >= -1e-4f && s <= 1.0001f));
    float weight = 1.0f;
    if (!inside && !all) {
      if (falloff <= 0.0f) continue;
      // Distance outside the lattice box fades the deformation out.
      Vec3 q = vmax(vmax(box.lo - p, p - box.hi), Vec3(0.0f));
      float d = length(q);
      if (d >= falloff) continue;
      weight = 0.5f * (1.0f + std::cos(kPi * d / falloff));
      t = clampf(t, 0.0f, 1.0f);
      u = clampf(u, 0.0f, 1.0f);
      if (kind_ != Cylinder) s = clampf(s, 0.0f, 1.0f);
    }
    for (int i = 0; i < a; ++i) {
      if (kind_ == Cylinder) {
        // Periodic cubic B-spline around the cylinder.
        float d = s - i;
        while (d > a * 0.5f) d -= a;
        while (d < -a * 0.5f) d += a;
        wa[i] = bsplineKernel(d);
      } else {
        wa[i] = bernstein(a - 1, i, s);
      }
    }
    for (int j = 0; j < b; ++j) wb[j] = bernstein(b - 1, j, std::min(t, 1.0f));
    for (int k = 0; k < c; ++k) wc[k] = bernstein(c - 1, k, u);
    if (kind_ == Cylinder) {
      float sum = 0.0f;
      for (float w : wa) sum += w;
      if (sum > 0.0f)
        for (float& w : wa) w /= sum;
    }
    Vec3 d;
    for (int k = 0; k < c; ++k)
      for (int j = 0; j < b; ++j)
        for (int i = 0; i < a; ++i) {
          int idx = (k * b + j) * a + i;
          if (idx >= static_cast<int>(offsets_.size())) continue;
          float w = wa[i] * wb[j] * wc[k];
          if (w != 0.0f) d += offsets_[idx] * w;
        }
    p += d * weight;
  }
}

json FfdModifier::toJson() const {
  json j = Modifier::toJson();
  json pts = json::array();
  for (const auto& o : offsets_) pts.push_back(vec3ToJson(o));
  j["offsets"] = pts;
  return j;
}

void FfdModifier::fromJson(const json& j) {
  Modifier::fromJson(j);
  offsets_.clear();
  if (j.contains("offsets"))
    for (const auto& o : j["offsets"]) offsets_.push_back(vec3FromJson(o));
}

// ---------------------------------------------------------------------------------------------
// UV algorithms

void compactUvList(std::vector<Vec2>& uvs, std::vector<std::vector<int>>& faceUvs) {
  std::vector<int> map(uvs.size(), -1);
  std::vector<Vec2> out;
  for (auto& f : faceUvs)
    for (int& t : f) {
      if (t < 0 || t >= static_cast<int>(uvs.size())) {
        t = 0;
        continue;
      }
      if (map[t] < 0) {
        map[t] = static_cast<int>(out.size());
        out.push_back(uvs[t]);
      }
      t = map[t];
    }
  if (out.empty()) out.push_back(Vec2());
  uvs = std::move(out);
}

namespace {

Vec3 faceNormalOf(const PolyMesh& m, int f) { return normalize(m.faceNormal(m.faces[f])); }

// UV charts: faces connected through shared UV indices.
std::vector<std::vector<int>> uvCharts(const std::vector<std::vector<int>>& faceUvs, size_t uvCount) {
  std::vector<int> parent(uvCount);
  for (size_t i = 0; i < uvCount; ++i) parent[i] = static_cast<int>(i);
  std::function<int(int)> find = [&](int x) {
    while (parent[x] != x) x = parent[x] = parent[parent[x]];
    return x;
  };
  for (const auto& f : faceUvs)
    for (size_t k = 1; k < f.size(); ++k)
      if (f[0] >= 0 && f[k] >= 0 && f[0] < static_cast<int>(uvCount) && f[k] < static_cast<int>(uvCount))
        parent[find(f[k])] = find(f[0]);
  std::map<int, std::vector<int>> charts;
  for (size_t fi = 0; fi < faceUvs.size(); ++fi)
    if (!faceUvs[fi].empty() && faceUvs[fi][0] >= 0 && faceUvs[fi][0] < static_cast<int>(uvCount))
      charts[find(faceUvs[fi][0])].push_back(static_cast<int>(fi));
  std::vector<std::vector<int>> out;
  for (auto& [k, v] : charts) out.push_back(std::move(v));
  return out;
}

}  // namespace

void projectFaceUvs(const PolyMesh& mesh, std::vector<Vec2>& uvs, std::vector<std::vector<int>>& faceUvs,
                    const std::vector<int>& faces, int mapping) {
  if (faces.empty()) return;
  faceUvs.resize(mesh.faces.size());
  Aabb box;
  Vec3 avgN;
  for (int f : faces) {
    for (int v : mesh.faces[f].v) box.expand(mesh.positions[v]);
    avgN += mesh.faceNormal(mesh.faces[f]);
  }
  Vec3 c = box.center(), ext = vmax(box.extent(), Vec3(1e-6f));
  float size = std::max(ext.x, std::max(ext.y, ext.z));
  Vec3 n = length2(avgN) > 1e-20f ? normalize(avgN) : Vec3(0, 0, 1);
  Vec3 tu, tv;
  makeBasis(n, tu, tv);
  // Each face's corners get fresh UV vertices shared within the projected set.
  std::map<int, int> shared;  // mesh vertex -> uv (planar/cylindrical/spherical keep faces joined)
  for (int f : faces) {
    const auto& face = mesh.faces[f];
    std::vector<int> idx;
    int axis = 2;
    if (mapping == 1) {
      Vec3 fn = vabs(faceNormalOf(mesh, f));
      axis = fn.x >= fn.y && fn.x >= fn.z ? 0 : (fn.y >= fn.z ? 1 : 2);
    }
    // Cylindrical seam: faces straddling the back get their angles unwrapped consistently.
    float refAngle = 0.0f;
    if (mapping == 2) {
      Vec3 fc;
      for (int v : face.v) fc += mesh.positions[v];
      fc /= static_cast<float>(face.v.size());
      refAngle = std::atan2(fc.y - c.y, fc.x - c.x);
    }
    for (int v : face.v) {
      const Vec3& p = mesh.positions[v];
      Vec3 d = p - c;
      Vec2 uv;
      switch (mapping) {
        case 1: {
          static const int us[3] = {1, 0, 0}, vs[3] = {2, 2, 1};
          uv = Vec2(d[us[axis]] / size + 0.5f, d[vs[axis]] / size + 0.5f);
          break;
        }
        case 2: {
          float ang = std::atan2(d.y, d.x);
          while (ang - refAngle > kPi) ang -= kTwoPi;
          while (ang - refAngle < -kPi) ang += kTwoPi;
          uv = Vec2(ang / kTwoPi + 0.5f, d.z / ext.z + 0.5f);
          break;
        }
        case 3: {
          Vec3 dn = normalize(d);
          float ang = std::atan2(dn.y, dn.x);
          uv = Vec2(ang / kTwoPi + 0.5f, std::asin(clampf(dn.z, -1.0f, 1.0f)) / kPi + 0.5f);
          break;
        }
        default: uv = Vec2(dot(d, tu) / size + 0.5f, dot(d, tv) / size + 0.5f); break;
      }
      int key = mapping == 1 ? v * 3 + axis : v;
      bool share = mapping == 0 || mapping == 1;
      auto it = share ? shared.find(key) : shared.end();
      if (it != shared.end()) {
        idx.push_back(it->second);
      } else {
        uvs.push_back(uv);
        idx.push_back(static_cast<int>(uvs.size()) - 1);
        if (share) shared[key] = idx.back();
      }
    }
    faceUvs[f] = idx;
  }
}

void packUvCharts(std::vector<Vec2>& uvs, const std::vector<std::vector<int>>& faceUvs, float spacing) {
  auto charts = uvCharts(faceUvs, uvs.size());
  if (charts.empty()) return;
  struct Box {
    int chart;
    Vec2 lo, hi;
  };
  std::vector<Box> boxes;
  double area = 0;
  for (size_t ci = 0; ci < charts.size(); ++ci) {
    Vec2 lo(1e30f, 1e30f), hi(-1e30f, -1e30f);
    for (int f : charts[ci])
      for (int t : faceUvs[f]) {
        lo = Vec2(std::min(lo.x, uvs[t].x), std::min(lo.y, uvs[t].y));
        hi = Vec2(std::max(hi.x, uvs[t].x), std::max(hi.y, uvs[t].y));
      }
    boxes.push_back({static_cast<int>(ci), lo, hi});
    area += static_cast<double>(hi.x - lo.x + spacing) * (hi.y - lo.y + spacing);
  }
  // Shelf packing, tallest first, into a square of side ~sqrt(area).
  std::sort(boxes.begin(), boxes.end(), [](const Box& a, const Box& b) { return (a.hi.y - a.lo.y) > (b.hi.y - b.lo.y); });
  float side = static_cast<float>(std::sqrt(area)) * 1.1f;
  float maxW = 0;
  for (const auto& b : boxes) maxW = std::max(maxW, b.hi.x - b.lo.x + spacing);
  side = std::max(side, maxW);
  float x = 0, y = 0, shelf = 0, usedW = 0;
  std::vector<Vec2> shift(charts.size());
  for (const auto& b : boxes) {
    float w = b.hi.x - b.lo.x + spacing, h = b.hi.y - b.lo.y + spacing;
    if (x + w > side && x > 0) {
      x = 0;
      y += shelf;
      shelf = 0;
    }
    shift[b.chart] = Vec2(x - b.lo.x + spacing * 0.5f, y - b.lo.y + spacing * 0.5f);
    x += w;
    usedW = std::max(usedW, x);
    shelf = std::max(shelf, h);
  }
  float total = std::max(usedW, y + shelf);
  float scale = total > 0 ? 1.0f / total : 1.0f;
  std::vector<char> moved(uvs.size(), 0);
  for (size_t ci = 0; ci < charts.size(); ++ci)
    for (int f : charts[ci])
      for (int t : faceUvs[f])
        if (!moved[t]) {
          moved[t] = 1;
          uvs[t] = (uvs[t] + shift[ci]) * scale;
        }
}

void flattenUvs(const PolyMesh& mesh, std::vector<Vec2>& uvs, std::vector<std::vector<int>>& faceUvs, float angleDeg,
                float spacing) {
  uvs.clear();
  faceUvs.assign(mesh.faces.size(), {});
  PolyTopology topo(mesh);
  float cosLimit = std::cos(radians(angleDeg));
  std::vector<int> chart(mesh.faces.size(), -1);
  int charts = 0;
  for (size_t f0 = 0; f0 < mesh.faces.size(); ++f0) {
    if (chart[f0] >= 0) continue;
    Vec3 n0 = faceNormalOf(mesh, static_cast<int>(f0));
    std::vector<int> stack = {static_cast<int>(f0)}, members;
    chart[f0] = charts;
    while (!stack.empty()) {
      int f = stack.back();
      stack.pop_back();
      members.push_back(f);
      for (int e : topo.faceEdges[f])
        for (int g : topo.edges[e].faces)
          if (chart[g] < 0 && dot(faceNormalOf(mesh, g), n0) >= cosLimit) {
            chart[g] = charts;
            stack.push_back(g);
          }
    }
    projectFaceUvs(mesh, uvs, faceUvs, members, 0);
    ++charts;
  }
  // Charts were projected around their own centers; scale them to world size before packing.
  auto list = uvCharts(faceUvs, uvs.size());
  for (const auto& faces : list) {
    // Rescale the chart so UV units match world units (projection normalized by chart size).
    Aabb box;
    for (int f : faces)
      for (int v : mesh.faces[f].v) box.expand(mesh.positions[v]);
    Vec3 e = box.extent();
    float size = std::max(e.x, std::max(e.y, e.z));
    std::set<int> done;
    for (int f : faces)
      for (int t : faceUvs[f])
        if (done.insert(t).second) uvs[t] = uvs[t] * size;
  }
  packUvCharts(uvs, faceUvs, spacing);
}

void relaxUvs(std::vector<Vec2>& uvs, const std::vector<std::vector<int>>& faceUvs, const std::vector<int>& selected,
              float amount, int iterations) {
  std::vector<std::set<int>> nbr(uvs.size());
  std::vector<std::map<int, int>> edgeUse(uvs.size());
  for (const auto& f : faceUvs)
    for (size_t k = 0; k < f.size(); ++k) {
      int a = f[k], b = f[(k + 1) % f.size()];
      if (a < 0 || b < 0 || a >= static_cast<int>(uvs.size()) || b >= static_cast<int>(uvs.size())) continue;
      nbr[a].insert(b);
      nbr[b].insert(a);
      edgeUse[std::min(a, b)][std::max(a, b)]++;
    }
  // Chart borders stay put so the chart keeps its outline.
  std::vector<char> border(uvs.size(), 0);
  for (size_t a = 0; a < uvs.size(); ++a)
    for (const auto& [b, n] : edgeUse[a])
      if (n == 1) border[a] = border[b] = 1;
  std::vector<char> active(uvs.size(), selected.empty() ? 1 : 0);
  for (int s : selected)
    if (s >= 0 && s < static_cast<int>(uvs.size())) active[s] = 1;
  for (int it = 0; it < std::max(1, iterations); ++it) {
    std::vector<Vec2> next = uvs;
    for (size_t i = 0; i < uvs.size(); ++i) {
      if (!active[i] || border[i] || nbr[i].empty()) continue;
      Vec2 avg;
      for (int n : nbr[i]) avg += uvs[n];
      avg = avg / static_cast<float>(nbr[i].size());
      next[i] = uvs[i] + (avg - uvs[i]) * amount;
    }
    uvs = next;
  }
}

void stitchUvs(const PolyMesh& mesh, std::vector<Vec2>& uvs, std::vector<std::vector<int>>& faceUvs,
               const std::vector<int>& selected) {
  std::set<int> sel(selected.begin(), selected.end());
  // Mesh vertex -> selected UV vertices that belong to it.
  std::map<int, std::set<int>> groups;
  for (size_t f = 0; f < mesh.faces.size() && f < faceUvs.size(); ++f)
    for (size_t k = 0; k < mesh.faces[f].v.size() && k < faceUvs[f].size(); ++k)
      if (sel.count(faceUvs[f][k])) groups[mesh.faces[f].v[k]].insert(faceUvs[f][k]);
  std::map<int, int> target;
  for (const auto& [v, set] : groups) {
    if (set.size() < 2) continue;
    Vec2 avg;
    for (int t : set) avg += uvs[t];
    avg = avg / static_cast<float>(set.size());
    int keep = *set.begin();
    uvs[keep] = avg;
    for (int t : set) target[t] = keep;
  }
  // Only remap corners of the same mesh vertex.
  for (size_t f = 0; f < mesh.faces.size() && f < faceUvs.size(); ++f)
    for (size_t k = 0; k < mesh.faces[f].v.size() && k < faceUvs[f].size(); ++k) {
      auto g = groups.find(mesh.faces[f].v[k]);
      if (g == groups.end() || g->second.size() < 2) continue;
      auto it = target.find(faceUvs[f][k]);
      if (it != target.end()) faceUvs[f][k] = it->second;
    }
}

// ---------------------------------------------------------------------------------------------
// Unwrap UVW

UnwrapUvwModifier::UnwrapUvwModifier() {
  params.beginGroup("Channel");
  params.addInt("channel", "Map Channel", 1, 1, 99);
  params.beginGroup("Display");
  params.addBool("showSeams", "Show Map Seams", true);
}

bool UnwrapUvwModifier::matches(const PolyMesh& mesh) const {
  if (faceUvs.size() != mesh.faces.size()) return false;
  for (size_t f = 0; f < mesh.faces.size(); ++f)
    if (faceUvs[f].size() != mesh.faces[f].v.size()) return false;
  return true;
}

void UnwrapUvwModifier::apply(PolyMesh& mesh) const {
  lastInput = mesh;
  lastInputValid = true;
  if (!matches(mesh) || uvs.empty()) return;
  mesh.uvs = uvs;
  for (size_t f = 0; f < mesh.faces.size(); ++f) {
    mesh.faces[f].uv = faceUvs[f];
    for (int& t : mesh.faces[f].uv) t = std::clamp(t, 0, static_cast<int>(uvs.size()) - 1);
  }
}

void UnwrapUvwModifier::initFrom(const PolyMesh& input) {
  uvs = input.uvs;
  faceUvs.assign(input.faces.size(), {});
  bool allHave = true;
  for (size_t f = 0; f < input.faces.size(); ++f) {
    faceUvs[f] = input.faces[f].uv;
    allHave = allHave && input.faces[f].uv.size() == input.faces[f].v.size();
  }
  if (!allHave || uvs.empty()) flattenUvs(input, uvs, faceUvs, 60.0f, 0.02f);
  compactUvList(uvs, faceUvs);
  touch();
}

json UnwrapUvwModifier::toJson() const {
  json j = Modifier::toJson();
  json u = json::array();
  for (const auto& t : uvs) u.push_back(json::array({t.x, t.y}));
  j["uvs"] = u;
  j["faceUvs"] = faceUvs;
  return j;
}

void UnwrapUvwModifier::fromJson(const json& j) {
  Modifier::fromJson(j);
  uvs.clear();
  faceUvs.clear();
  if (j.contains("uvs"))
    for (const auto& t : j["uvs"])
      if (t.is_array() && t.size() == 2) uvs.push_back(Vec2(t[0].get<float>(), t[1].get<float>()));
  if (j.contains("faceUvs")) faceUvs = j["faceUvs"].get<std::vector<std::vector<int>>>();
}

// ---------------------------------------------------------------------------------------------
// Quadric error decimation

namespace {

struct Quadric {
  double a[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};  // symmetric 4x4: xx xy xz xw yy yz yw zz zw ww
  void addPlane(double nx, double ny, double nz, double d, double w) {
    a[0] += w * nx * nx; a[1] += w * nx * ny; a[2] += w * nx * nz; a[3] += w * nx * d;
    a[4] += w * ny * ny; a[5] += w * ny * nz; a[6] += w * ny * d;
    a[7] += w * nz * nz; a[8] += w * nz * d; a[9] += w * d * d;
  }
  Quadric operator+(const Quadric& o) const {
    Quadric q;
    for (int i = 0; i < 10; ++i) q.a[i] = a[i] + o.a[i];
    return q;
  }
  double error(const Vec3& p) const {
    double x = p.x, y = p.y, z = p.z;
    return a[0] * x * x + 2 * a[1] * x * y + 2 * a[2] * x * z + 2 * a[3] * x + a[4] * y * y + 2 * a[5] * y * z +
           2 * a[6] * y + a[7] * z * z + 2 * a[8] * z + a[9];
  }
  bool optimum(Vec3& out) const {
    double m[3][3] = {{a[0], a[1], a[2]}, {a[1], a[4], a[5]}, {a[2], a[5], a[7]}};
    double b[3] = {-a[3], -a[6], -a[8]};
    double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                 m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    if (std::fabs(det) < 1e-9) return false;
    double inv = 1.0 / det;
    double x = (b[0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (b[1] * m[2][2] - m[1][2] * b[2]) +
                m[0][2] * (b[1] * m[2][1] - m[1][1] * b[2])) * inv;
    double y = (m[0][0] * (b[1] * m[2][2] - m[1][2] * b[2]) - b[0] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                m[0][2] * (m[1][0] * b[2] - b[1] * m[2][0])) * inv;
    double z = (m[0][0] * (m[1][1] * b[2] - b[1] * m[2][1]) - m[0][1] * (m[1][0] * b[2] - b[1] * m[2][0]) +
                b[0] * (m[1][0] * m[2][1] - m[1][1] * m[2][0])) * inv;
    out = Vec3(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
    return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
  }
};

}  // namespace

PolyMesh decimateMesh(const PolyMesh& input, int targetFaces, bool keepBorders) {
  // Triangle soup with shared vertices.
  struct Tri {
    int v[3];
    int uv[3];
    int matId;
    uint32_t sg;
    bool dead = false;
  };
  std::vector<Vec3> pos = input.positions;
  std::vector<Tri> tris;
  bool hasUv = !input.uvs.empty();
  for (const auto& f : input.faces) {
    std::vector<int> t = faceTriangles(input, f);
    bool uv = f.uv.size() == f.v.size();
    for (size_t i = 0; i + 2 < t.size(); i += 3) {
      Tri tr;
      for (int c = 0; c < 3; ++c) {
        tr.v[c] = f.v[t[i + c]];
        tr.uv[c] = uv ? f.uv[t[i + c]] : -1;
      }
      tr.matId = f.matId;
      tr.sg = f.smoothGroups;
      tris.push_back(tr);
    }
  }
  int alive = static_cast<int>(tris.size());
  if (targetFaces >= alive || targetFaces < 1) {
    PolyMesh out;
    out.positions = pos;
    out.uvs = input.uvs;
    for (const auto& t : tris) {
      PolyMesh::Face f;
      f.v = {t.v[0], t.v[1], t.v[2]};
      if (hasUv && t.uv[0] >= 0) f.uv = {t.uv[0], t.uv[1], t.uv[2]};
      f.matId = t.matId;
      f.smoothGroups = t.sg;
      out.faces.push_back(f);
    }
    compactVertices(out);
    return out;
  }
  size_t nv = pos.size();
  std::vector<std::vector<int>> vtris(nv);
  for (size_t i = 0; i < tris.size(); ++i)
    for (int c = 0; c < 3; ++c) vtris[tris[i].v[c]].push_back(static_cast<int>(i));
  std::vector<Quadric> q(nv);
  for (const auto& t : tris) {
    Vec3 a = pos[t.v[0]], b = pos[t.v[1]], c = pos[t.v[2]];
    Vec3 n = cross(b - a, c - a);
    double area = length(n);
    if (area < 1e-20) continue;
    n = n / static_cast<float>(area);
    double d = -dot(n, a);
    for (int k = 0; k < 3; ++k) q[t.v[k]].addPlane(n.x, n.y, n.z, d, area);
  }
  // Border edges get a perpendicular constraint plane so outlines are kept.
  {
    std::map<std::pair<int, int>, int> edgeCount;
    for (const auto& t : tris)
      for (int k = 0; k < 3; ++k) edgeCount[{std::min(t.v[k], t.v[(k + 1) % 3]), std::max(t.v[k], t.v[(k + 1) % 3])}]++;
    for (const auto& t : tris) {
      Vec3 fn = normalize(cross(pos[t.v[1]] - pos[t.v[0]], pos[t.v[2]] - pos[t.v[0]]));
      for (int k = 0; k < 3; ++k) {
        int a = t.v[k], b = t.v[(k + 1) % 3];
        if (edgeCount[{std::min(a, b), std::max(a, b)}] != 1) continue;
        Vec3 e = pos[b] - pos[a];
        Vec3 n = normalize(cross(e, fn));
        double d = -dot(n, pos[a]);
        double w = (keepBorders ? 1000.0 : 10.0) * length2(e);
        q[a].addPlane(n.x, n.y, n.z, d, w);
        q[b].addPlane(n.x, n.y, n.z, d, w);
      }
    }
  }
  std::vector<int> version(nv, 0);
  std::vector<char> removed(nv, 0);
  struct Cand {
    double cost;
    int a, b, va, vb;
    Vec3 p;
    bool operator<(const Cand& o) const { return cost > o.cost; }
  };
  std::priority_queue<Cand> heap;
  auto evaluate = [&](int a, int b) {
    Quadric qq = q[a] + q[b];
    Vec3 p;
    if (!qq.optimum(p)) {
      Vec3 mid = (pos[a] + pos[b]) * 0.5f;
      double ea = qq.error(pos[a]), eb = qq.error(pos[b]), em = qq.error(mid);
      p = ea <= eb && ea <= em ? pos[a] : (eb <= em ? pos[b] : mid);
    }
    heap.push({qq.error(p), a, b, version[a], version[b], p});
  };
  {
    std::set<std::pair<int, int>> seen;
    for (const auto& t : tris)
      for (int k = 0; k < 3; ++k) {
        int a = t.v[k], b = t.v[(k + 1) % 3];
        if (seen.insert({std::min(a, b), std::max(a, b)}).second) evaluate(std::min(a, b), std::max(a, b));
      }
  }
  while (alive > targetFaces && !heap.empty()) {
    Cand c = heap.top();
    heap.pop();
    if (removed[c.a] || removed[c.b] || version[c.a] != c.va || version[c.b] != c.vb) continue;
    // Reject collapses that flip triangles.
    bool flip = false;
    for (int v : {c.a, c.b})
      for (int ti : vtris[v]) {
        const Tri& t = tris[ti];
        if (t.dead) continue;
        bool hasA = false, hasB = false;
        for (int k = 0; k < 3; ++k) {
          hasA |= t.v[k] == c.a;
          hasB |= t.v[k] == c.b;
        }
        if (hasA && hasB) continue;  // collapses away
        Vec3 p[3];
        for (int k = 0; k < 3; ++k) p[k] = (t.v[k] == c.a || t.v[k] == c.b) ? c.p : pos[t.v[k]];
        Vec3 n0 = cross(pos[t.v[1]] - pos[t.v[0]], pos[t.v[2]] - pos[t.v[0]]);
        Vec3 n1 = cross(p[1] - p[0], p[2] - p[0]);
        if (dot(n0, n1) <= 0.0f || length2(n1) < 1e-14f * std::max(1e-12f, length2(n0))) flip = true;
      }
    if (flip) continue;
    // Collapse b into a.
    int a = c.a, b = c.b;
    // UV of a in a triangle shared with b (to remap b's corners).
    int uvA = -1, uvB = -1;
    for (int ti : vtris[b]) {
      Tri& t = tris[ti];
      if (t.dead) continue;
      int ka = -1, kb = -1;
      for (int k = 0; k < 3; ++k) {
        if (t.v[k] == a) ka = k;
        if (t.v[k] == b) kb = k;
      }
      if (ka >= 0 && kb >= 0) {
        uvA = t.uv[ka];
        uvB = t.uv[kb];
        t.dead = true;
        --alive;
      }
    }
    pos[a] = c.p;
    q[a] = q[a] + q[b];
    removed[b] = 1;
    for (int ti : vtris[b]) {
      Tri& t = tris[ti];
      if (t.dead) continue;
      for (int k = 0; k < 3; ++k)
        if (t.v[k] == b) {
          t.v[k] = a;
          if (t.uv[k] == uvB && uvA >= 0) t.uv[k] = uvA;
        }
      vtris[a].push_back(ti);
    }
    vtris[b].clear();
    ++version[a];
    // Re-evaluate edges around a.
    std::set<int> nb;
    std::vector<int> keep;
    for (int ti : vtris[a]) {
      if (tris[ti].dead) continue;
      keep.push_back(ti);
      for (int k = 0; k < 3; ++k)
        if (tris[ti].v[k] != a) nb.insert(tris[ti].v[k]);
    }
    sortUnique(keep);
    vtris[a] = keep;
    for (int n : nb) evaluate(std::min(a, n), std::max(a, n));
  }
  PolyMesh out;
  out.positions = pos;
  out.uvs = input.uvs;
  for (const auto& t : tris) {
    if (t.dead || t.v[0] == t.v[1] || t.v[1] == t.v[2] || t.v[0] == t.v[2]) continue;
    PolyMesh::Face f;
    f.v = {t.v[0], t.v[1], t.v[2]};
    if (hasUv && t.uv[0] >= 0 && t.uv[1] >= 0 && t.uv[2] >= 0) f.uv = {t.uv[0], t.uv[1], t.uv[2]};
    f.matId = t.matId;
    f.smoothGroups = t.sg;
    out.faces.push_back(f);
  }
  compactVertices(out);
  ensureFaceUvs(out);
  return out;
}

namespace {

// ---------------------------------------------------------------------------------------------
// Symmetry / Slice / Cap Holes / Chamfer / Relax / Optimize

class SymmetryModifier : public ModBase<SymmetryModifier> {
 public:
  SymmetryModifier() {
    params.beginGroup("Parameters");
    params.addEnum("axis", "Mirror Axis", kAxes, 0);
    params.addBool("flip", "Flip", false);
    params.addFloat("offset", "Mirror Offset", 0.0, -1e7, 1e7, 1.0);
    params.addBool("weld", "Weld Seam", true);
    params.addFloat("threshold", "Threshold", 0.1, 0.0, 1e5, 0.01, 3);
  }
  std::string type() const override { return "Symmetry"; }
  void apply(PolyMesh& m) const override {
    int a = params.getInt("axis");
    bool flip = params.getBool("flip");
    float off = params.getFloat("offset");
    Vec3 n;
    n[a] = flip ? -1.0f : 1.0f;
    Vec3 p;
    p[a] = off;
    slicePlane(m, p, n, nullptr, false);
    // Keep the positive side (relative to n).
    std::vector<int> drop;
    for (size_t f = 0; f < m.faces.size(); ++f) {
      Vec3 c;
      for (int v : m.faces[f].v) c += m.positions[v];
      c /= static_cast<float>(m.faces[f].v.size());
      if (dot(c - p, n) < 0.0f) drop.push_back(static_cast<int>(f));
    }
    removeFaces(m, drop);
    PolyMesh mirror = m;
    for (auto& q : mirror.positions) q[a] = 2.0f * off - q[a];
    mirror.flipFaces();
    int base = static_cast<int>(m.positions.size());
    m.append(mirror);
    if (params.getBool("weld")) {
      float th = std::max(params.getFloat("threshold"), 1e-5f);
      std::vector<std::pair<int, int>> pairs;
      for (int v = 0; v < base; ++v)
        if (std::fabs(m.positions[v][a] - off) <= th) {
          m.positions[v][a] = off;
          pairs.push_back({base + v, v});
        }
      weldPairs(m, pairs);
    }
  }
};

class SliceModifier : public ModBase<SliceModifier> {
 public:
  SliceModifier() {
    params.beginGroup("Slice Type");
    params.addEnum("mode", "Type", {"Refine Mesh", "Split Mesh", "Remove Top", "Remove Bottom"}, 0);
    params.beginGroup("Slice Plane (offset from the object's center)");
    params.addFloat("posX", "Position X", 0.0, -1e7, 1e7, 1.0);
    params.addFloat("posY", "Position Y", 0.0, -1e7, 1e7, 1.0);
    params.addFloat("posZ", "Position Z", 0.0, -1e7, 1e7, 1.0);
    params.addFloat("rotX", "Rotation X", 0.0, -360, 360, 1.0, 1);
    params.addFloat("rotY", "Rotation Y", 0.0, -360, 360, 1.0, 1);
    params.addFloat("rotZ", "Rotation Z", 0.0, -360, 360, 1.0, 1);
  }
  std::string type() const override { return "Slice"; }
  void apply(PolyMesh& m) const override {
    Aabb b = m.bounds();
    if (!b.valid()) return;
    Vec3 p = b.center() + Vec3(params.getFloat("posX"), params.getFloat("posY"), params.getFloat("posZ"));
    Vec3 n = Mat4::fromEulerDeg(Vec3(params.getFloat("rotX"), params.getFloat("rotY"), params.getFloat("rotZ")))
                 .transformDir(Vec3(0, 0, 1));
    int mode = params.getInt("mode");
    slicePlane(m, p, n, nullptr, mode == 1);
    if (mode >= 2) {
      std::vector<int> drop;
      for (size_t f = 0; f < m.faces.size(); ++f) {
        Vec3 c;
        for (int v : m.faces[f].v) c += m.positions[v];
        c /= static_cast<float>(m.faces[f].v.size());
        float s = dot(c - p, n);
        if ((mode == 2 && s > 0.0f) || (mode == 3 && s < 0.0f)) drop.push_back(static_cast<int>(f));
      }
      removeFaces(m, drop);
    }
    ensureFaceUvs(m);
  }
};

class CapHolesModifier : public ModBase<CapHolesModifier> {
 public:
  CapHolesModifier() {
    params.beginGroup("Parameters");
    params.addBool("smoothNew", "Smooth New Faces", false);
  }
  std::string type() const override { return "CapHoles"; }
  void apply(PolyMesh& m) const override {
    size_t before = m.faces.size();
    capAllHoles(m);
    if (params.getBool("smoothNew"))
      for (size_t f = before; f < m.faces.size(); ++f) m.faces[f].smoothGroups = 1;
  }
};

class ChamferModifier : public ModBase<ChamferModifier> {
 public:
  ChamferModifier() {
    params.beginGroup("Chamfer");
    params.addFloat("amount", "Amount", 2.0, 0.0, 1e6, 0.1);
    params.addFloat("minAngle", "Minimum Angle", 30.0, 0.0, 180.0, 1.0, 1);
    params.addBool("open", "Open Chamfer", false);
  }
  std::string type() const override { return "Chamfer"; }
  void apply(PolyMesh& m) const override {
    float amount = params.getFloat("amount");
    if (amount <= 0.0f) return;
    float cosLimit = std::cos(radians(params.getFloat("minAngle")));
    PolyTopology topo(m);
    PolySelection sel;
    sel.level = PolyEdge;
    for (const auto& e : topo.edges) {
      if (e.faces.size() != 2) continue;
      Vec3 n0 = normalize(m.faceNormal(m.faces[e.faces[0]])), n1 = normalize(m.faceNormal(m.faces[e.faces[1]]));
      if (dot(n0, n1) <= cosLimit + 1e-6f) sel.edges.push_back(edgeKey(e.a, e.b));
    }
    if (sel.edges.empty()) return;
    sel.normalize();
    runPolyOp(m, sel, {{"op", "chamferEdges"}, {"amount", amount}, {"open", params.getBool("open")}});
  }
};

class RelaxModifier : public ModBase<RelaxModifier> {
 public:
  RelaxModifier() {
    params.beginGroup("Parameters");
    params.addFloat("amount", "Relax Value", 0.5, -1.0, 1.0, 0.05);
    params.addInt("iterations", "Iterations", 1, 1, 100);
    params.addBool("keepBoundary", "Keep Boundary Pts Fixed", true);
  }
  std::string type() const override { return "Relax"; }
  void apply(PolyMesh& m) const override {
    PolySelection sel;
    runPolyOp(m, sel,
              {{"op", "relax"},
               {"amount", params.getFloat("amount")},
               {"iterations", params.getInt("iterations")},
               {"keepBoundary", params.getBool("keepBoundary")}});
  }
};

class OptimizeModifier : public ModBase<OptimizeModifier> {
 public:
  OptimizeModifier() {
    params.beginGroup("Optimization Level");
    params.addFloat("percent", "Face Percent", 50.0, 0.1, 100.0, 1.0, 1);
    params.addBool("keepBorders", "Protect Borders", true);
  }
  std::string type() const override { return "Optimize"; }
  void apply(PolyMesh& m) const override {
    size_t tris = m.triangleCount();
    int target = std::max(1, static_cast<int>(std::lround(tris * params.getFloat("percent") / 100.0)));
    m = decimateMesh(m, target, params.getBool("keepBorders"));
  }
};

// ---------------------------------------------------------------------------------------------
// Lattice

void addCylinder(PolyMesh& out, const Vec3& a, const Vec3& b, float r, int sides, int segs, uint32_t sg) {
  Vec3 axis = b - a;
  float len = length(axis);
  if (len < 1e-6f) return;
  Vec3 d = axis / len, u, v;
  makeBasis(d, u, v);
  int base = static_cast<int>(out.positions.size());
  for (int s = 0; s <= segs; ++s)
    for (int k = 0; k < sides; ++k) {
      float t = kTwoPi * k / sides;
      out.positions.push_back(a + axis * (static_cast<float>(s) / segs) + (u * std::cos(t) + v * std::sin(t)) * r);
    }
  for (int s = 0; s < segs; ++s)
    for (int k = 0; k < sides; ++k) {
      int k1 = (k + 1) % sides;
      out.addFace({base + s * sides + k, base + s * sides + k1, base + (s + 1) * sides + k1, base + (s + 1) * sides + k}, {},
                  sg);
    }
  std::vector<int> cap0, cap1;
  for (int k = sides - 1; k >= 0; --k) cap0.push_back(base + k);
  for (int k = 0; k < sides; ++k) cap1.push_back(base + segs * sides + k);
  out.addFace(cap0, {}, 0);
  out.addFace(cap1, {}, 0);
}

void addPolyhedron(PolyMesh& out, const Vec3& c, float r, int type, uint32_t sg) {
  std::vector<Vec3> pts;
  std::vector<std::array<int, 3>> faces;
  if (type == 0) {  // tetrahedron
    pts = {{1, 1, 1}, {1, -1, -1}, {-1, 1, -1}, {-1, -1, 1}};
    faces = {{0, 1, 2}, {0, 3, 1}, {0, 2, 3}, {1, 3, 2}};
  } else if (type == 1) {  // octahedron
    pts = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    faces = {{0, 2, 4}, {2, 1, 4}, {1, 3, 4}, {3, 0, 4}, {2, 0, 5}, {1, 2, 5}, {3, 1, 5}, {0, 3, 5}};
  } else {  // icosahedron
    float t = (1.0f + std::sqrt(5.0f)) * 0.5f;
    pts = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t},
           {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
    faces = {{0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4},
             {11, 10, 2}, {10, 7, 6}, {7, 1, 8}, {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8},
             {3, 8, 9}, {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}};
  }
  int base = static_cast<int>(out.positions.size());
  for (const auto& p : pts) out.positions.push_back(c + normalize(p) * r);
  for (const auto& f : faces) {
    // Wind outward.
    Vec3 n = cross(pts[f[1]] - pts[f[0]], pts[f[2]] - pts[f[0]]);
    if (dot(n, pts[f[0]] + pts[f[1]] + pts[f[2]]) >= 0)
      out.addFace({base + f[0], base + f[1], base + f[2]}, {}, sg);
    else
      out.addFace({base + f[0], base + f[2], base + f[1]}, {}, sg);
  }
}

class LatticeModifier : public ModBase<LatticeModifier> {
 public:
  LatticeModifier() {
    params.beginGroup("Geometry");
    params.addEnum("geometry", "Apply To", {"Struts Only", "Joints Only", "Both"}, 2);
    params.beginGroup("Struts");
    params.addFloat("strutRadius", "Radius", 2.0, 0.001, 1e6, 0.1);
    params.addInt("strutSegments", "Segments", 1, 1, 100);
    params.addInt("strutSides", "Sides", 4, 3, 100);
    params.addBool("strutSmooth", "Smooth", false);
    params.beginGroup("Joints");
    params.addEnum("jointType", "Geodesic Base Type", {"Tetra", "Octa", "Icosa"}, 2);
    params.addFloat("jointRadius", "Radius", 5.0, 0.001, 1e6, 0.1);
    params.addBool("jointSmooth", "Smooth", false);
  }
  std::string type() const override { return "Lattice"; }
  void apply(PolyMesh& m) const override {
    int geo = params.getInt("geometry");
    PolyMesh out;
    PolyTopology topo(m);
    if (geo != 1)
      for (const auto& e : topo.edges)
        addCylinder(out, m.positions[e.a], m.positions[e.b], params.getFloat("strutRadius"), params.getInt("strutSides"),
                    params.getInt("strutSegments"), params.getBool("strutSmooth") ? 1u : 0u);
    if (geo != 0) {
      std::vector<char> used(m.positions.size(), 0);
      for (const auto& f : m.faces)
        for (int v : f.v) used[v] = 1;
      for (size_t v = 0; v < m.positions.size(); ++v)
        if (used[v])
          addPolyhedron(out, m.positions[v], params.getFloat("jointRadius"), params.getInt("jointType"),
                        params.getBool("jointSmooth") ? 1u : 0u);
    }
    m = std::move(out);
  }
};

// ---------------------------------------------------------------------------------------------
// Deformers

void axisFrame(int a, int& b, int& c) {
  b = (a + 1) % 3;
  c = (a + 2) % 3;
  if (a == 2) {
    b = 0;
    c = 1;
  }
}

class MeltModifier : public ModBase<MeltModifier> {
 public:
  MeltModifier() {
    params.beginGroup("Melt");
    params.addFloat("amount", "Amount", 0.0, 0.0, 1000.0, 1.0, 1);
    params.addFloat("spread", "% of Melt", 19.0, 0.0, 100.0, 1.0, 1);
    params.addEnum("solidity", "Solidity", {"Ice (Default)", "Glass", "Jelly", "Plastic", "Custom"}, 0);
    params.addFloat("custom", "Custom Solidity", 1.0, 0.2, 30.0, 0.1);
    params.addEnum("axis", "Axis to Melt", kAxes, 2);
    params.addBool("negative", "Flip Axis", false);
  }
  std::string type() const override { return "Melt"; }
  void apply(PolyMesh& m) const override {
    float amount = params.getFloat("amount") / 100.0f;
    if (amount <= 0.0f) return;
    static const float solidities[] = {2.0f, 12.0f, 0.4f, 0.7f};
    int sIdx = params.getInt("solidity");
    float solidity = sIdx < 4 ? solidities[sIdx] : params.getFloat("custom");
    int a = params.getInt("axis"), b, c;
    axisFrame(a, b, c);
    Aabb box = m.bounds();
    if (!box.valid()) return;
    bool neg = params.getBool("negative");
    float lo = box.lo[a], hi = box.hi[a], h = std::max(hi - lo, 1e-6f);
    Vec3 center = box.center();
    float t = amount / (amount + 1.0f / solidity);  // saturates as the amount grows
    float spread = params.getFloat("spread") / 100.0f;
    for (auto& p : m.positions) {
      float hn = neg ? (hi - p[a]) / h : (p[a] - lo) / h;
      float newHn = hn * (1.0f - t);
      float sink = (hn - newHn) * h;
      p[a] += neg ? sink : -sink;
      // Material flows outward the more it sank, most near the ground.
      float flow = 1.0f + spread * t * (1.0f - newHn) * 2.0f;
      p[b] = center[b] + (p[b] - center[b]) * flow;
      p[c] = center[c] + (p[c] - center[c]) * flow;
    }
  }
};

class RippleModifier : public ModBase<RippleModifier> {
 public:
  RippleModifier() {
    params.beginGroup("Ripple");
    params.addFloat("amp1", "Amplitude 1", 5.0, -1e6, 1e6, 0.5);
    params.addFloat("amp2", "Amplitude 2", 5.0, -1e6, 1e6, 0.5);
    params.addFloat("wavelength", "Wave Length", 50.0, 0.001, 1e6, 1.0);
    params.addFloat("phase", "Phase", 0.0, -1e6, 1e6, 0.05);
    params.addFloat("decay", "Decay", 0.0, 0.0, 1e6, 0.001, 4);
  }
  std::string type() const override { return "Ripple"; }
  void apply(PolyMesh& m) const override {
    float a1 = params.getFloat("amp1"), a2 = params.getFloat("amp2"), wl = params.getFloat("wavelength");
    float ph = params.getFloat("phase"), decay = params.getFloat("decay");
    for (auto& p : m.positions) {
      float d = std::sqrt(p.x * p.x + p.y * p.y);
      float ax = d > 1e-6f ? (p.x * p.x) / (d * d) : 0.5f;
      float amp = a1 * ax + a2 * (1.0f - ax);
      p.z += amp * std::sin(kTwoPi * (d / wl - ph)) * std::exp(-decay * d);
    }
  }
};

class WaveModifier : public ModBase<WaveModifier> {
 public:
  WaveModifier() {
    params.beginGroup("Wave");
    params.addFloat("amp1", "Amplitude 1", 5.0, -1e6, 1e6, 0.5);
    params.addFloat("amp2", "Amplitude 2", 5.0, -1e6, 1e6, 0.5);
    params.addFloat("wavelength", "Wave Length", 50.0, 0.001, 1e6, 1.0);
    params.addFloat("phase", "Phase", 0.0, -1e6, 1e6, 0.05);
    params.addFloat("decay", "Decay", 0.0, 0.0, 1e6, 0.001, 4);
  }
  std::string type() const override { return "Wave"; }
  void apply(PolyMesh& m) const override {
    Aabb box = m.bounds();
    if (!box.valid()) return;
    float a1 = params.getFloat("amp1"), a2 = params.getFloat("amp2"), wl = params.getFloat("wavelength");
    float ph = params.getFloat("phase"), decay = params.getFloat("decay");
    float ylo = box.lo.y, yext = std::max(box.extent().y, 1e-6f);
    for (auto& p : m.positions) {
      float amp = a1 + (a2 - a1) * (p.y - ylo) / yext;
      p.z += amp * std::sin(kTwoPi * (p.x / wl - ph)) * std::exp(-decay * std::fabs(p.x));
    }
  }
};

class SpherifyModifier : public ModBase<SpherifyModifier> {
 public:
  SpherifyModifier() {
    params.beginGroup("Parameters");
    params.addFloat("percent", "Percent", 100.0, 0.0, 100.0, 1.0, 1);
  }
  std::string type() const override { return "Spherify"; }
  void apply(PolyMesh& m) const override {
    if (m.positions.empty()) return;
    Vec3 c = m.bounds().center();
    double r = 0;
    for (const auto& p : m.positions) r += length(p - c);
    r /= static_cast<double>(m.positions.size());
    float t = params.getFloat("percent") / 100.0f;
    for (auto& p : m.positions) {
      Vec3 d = p - c;
      float l = length(d);
      if (l < 1e-9f) continue;
      p = lerp(p, c + d / l * static_cast<float>(r), t);
    }
  }
};

class SkewModifier : public ModBase<SkewModifier> {
 public:
  SkewModifier() {
    params.beginGroup("Skew");
    params.addFloat("amount", "Amount", 20.0, -1e6, 1e6, 1.0);
    params.addFloat("direction", "Direction", 0.0, -360, 360, 1.0, 1);
    params.addEnum("axis", "Skew Axis", kAxes, 2);
    params.beginGroup("Limits");
    params.addBool("limit", "Limit Effect", false);
    params.addFloat("upper", "Upper Limit", 0.0, -1e6, 1e6, 1.0);
    params.addFloat("lower", "Lower Limit", 0.0, -1e6, 1e6, 1.0);
  }
  std::string type() const override { return "Skew"; }
  void apply(PolyMesh& m) const override {
    Aabb box = m.bounds();
    if (!box.valid()) return;
    int a = params.getInt("axis"), b, c;
    axisFrame(a, b, c);
    float lo = box.lo[a], hi = box.hi[a];
    bool limit = params.getBool("limit");
    if (limit) {
      lo = std::max(lo, params.getFloat("lower"));
      hi = std::min(hi, params.getFloat("upper"));
    }
    float h = std::max(hi - lo, 1e-6f);
    float dir = radians(params.getFloat("direction"));
    float amount = params.getFloat("amount");
    for (auto& p : m.positions) {
      float t = clampf((p[a] - lo) / h, 0.0f, 1.0f);
      p[b] += amount * t * std::cos(dir);
      p[c] += amount * t * std::sin(dir);
    }
  }
};

class StretchModifier : public ModBase<StretchModifier> {
 public:
  StretchModifier() {
    params.beginGroup("Stretch");
    params.addFloat("stretch", "Stretch", 0.5, -0.99, 1e3, 0.05);
    params.addFloat("amplify", "Amplify", 0.0, -1e3, 1e3, 0.1);
    params.addEnum("axis", "Stretch Axis", kAxes, 2);
  }
  std::string type() const override { return "Stretch"; }
  void apply(PolyMesh& m) const override {
    Aabb box = m.bounds();
    if (!box.valid()) return;
    int a = params.getInt("axis"), b, c;
    axisFrame(a, b, c);
    float s = params.getFloat("stretch"), amp = params.getFloat("amplify");
    Vec3 ctr = box.center();
    float lo = box.lo[a], h = std::max(box.extent()[a], 1e-6f);
    float base = 1.0f / std::sqrt(std::max(1.0f + s, 0.01f));
    for (auto& p : m.positions) {
      float hn = (p[a] - lo) / h;
      float bulge = 4.0f * hn * (1.0f - hn);  // 0 at the ends, 1 in the middle
      float perp = 1.0f + (base - 1.0f) * (1.0f + amp * 0.25f) * bulge;
      p[a] = ctr[a] + (p[a] - ctr[a]) * (1.0f + s);
      p[b] = ctr[b] + (p[b] - ctr[b]) * perp;
      p[c] = ctr[c] + (p[c] - ctr[c]) * perp;
    }
  }
};

class SqueezeModifier : public ModBase<SqueezeModifier> {
 public:
  SqueezeModifier() {
    params.beginGroup("Axial Bulge");
    params.addFloat("bulge", "Amount", 0.0, -10, 10, 0.05);
    params.addFloat("bulgeCurve", "Curve", 2.0, 0.01, 10, 0.1);
    params.beginGroup("Radial Squeeze");
    params.addFloat("squeeze", "Amount", 0.0, -10, 10, 0.05);
    params.addFloat("squeezeCurve", "Curve", 2.0, 0.01, 10, 0.1);
    params.beginGroup("Effect Balance");
    params.addFloat("bias", "Bias", 0.0, -100, 100, 1.0, 1);
    params.addFloat("volume", "Volume", 100.0, 0.0, 1000, 1.0, 1);
  }
  std::string type() const override { return "Squeeze"; }
  void apply(PolyMesh& m) const override {
    Aabb box = m.bounds();
    if (!box.valid()) return;
    Vec3 c = box.center(), ext = box.extent();
    float lo = box.lo.z, h = std::max(ext.z, 1e-6f);
    float rmax = std::max(1e-6f, 0.5f * std::sqrt(ext.x * ext.x + ext.y * ext.y));
    float bias = params.getFloat("bias") / 100.0f, vol = params.getFloat("volume") / 100.0f;
    float bulge = params.getFloat("bulge") * (1.0f + bias) * vol, squeeze = params.getFloat("squeeze") * (1.0f - bias) * vol;
    float bc = params.getFloat("bulgeCurve"), sc = params.getFloat("squeezeCurve");
    for (auto& p : m.positions) {
      float hn = (p.z - lo) / h;
      float rn = std::sqrt((p.x - c.x) * (p.x - c.x) + (p.y - c.y) * (p.y - c.y)) / rmax;
      // Radial squeeze is strongest at mid height; axial bulge raises the center.
      float mid = std::pow(clampf(4.0f * hn * (1.0f - hn), 0.0f, 1.0f), 1.0f / sc);
      float s = 1.0f - squeeze * mid;
      p.x = c.x + (p.x - c.x) * s;
      p.y = c.y + (p.y - c.y) * s;
      float rise = bulge * h * 0.5f * std::pow(clampf(1.0f - rn, 0.0f, 1.0f), bc) * hn;
      p.z += rise;
    }
  }
};

class DisplaceModifier : public ModBase<DisplaceModifier> {
 public:
  DisplaceModifier() {
    params.beginGroup("Displacement");
    params.addFloat("strength", "Strength", 10.0, -1e6, 1e6, 0.5);
    params.addFloat("decay", "Decay", 0.0, 0.0, 1e6, 0.01, 3);
    params.addBool("center", "Luminance Center", false);
    params.beginGroup("Image");
    params.addEnum("source", "Source", {"Noise", "Bitmap"}, 0);
    params.addFile("image", "Bitmap", "Images (*.png *.jpg *.jpeg *.bmp *.tga *.hdr)");
    params.addFloat("noiseSize", "Noise Size", 25.0, 0.001, 1e6, 1.0);
    params.addInt("seed", "Seed", 0, 0, 100000);
    params.beginGroup("Map");
    params.addEnum("mapping", "Mapping", {"Planar", "Cylindrical", "Spherical", "Along Normals"}, 0);
    params.addFloat("uTile", "U Tile", 1.0, -1e4, 1e4, 0.1);
    params.addFloat("vTile", "V Tile", 1.0, -1e4, 1e4, 0.1);
  }
  std::string type() const override { return "Displace"; }

  float sampleImage(float u, float v) const {
    std::lock_guard<std::mutex> lock(imgMutex_);
    const std::string& path = params.getString("image");
    if (path != imgPath_) {
      imgPath_ = path;
      img_.clear();
      imgW_ = imgH_ = 0;
      int w = 0, h = 0, n = 0;
      if (!path.empty())
        if (unsigned char* data = stbi_load(path.c_str(), &w, &h, &n, 1)) {
          img_.assign(data, data + static_cast<size_t>(w) * h);
          imgW_ = w;
          imgH_ = h;
          stbi_image_free(data);
        }
    }
    if (img_.empty()) return 0.0f;
    u -= std::floor(u);
    v -= std::floor(v);
    float x = u * (imgW_ - 1), y = (1.0f - v) * (imgH_ - 1);
    int x0 = static_cast<int>(x), y0 = static_cast<int>(y);
    int x1 = std::min(x0 + 1, imgW_ - 1), y1 = std::min(y0 + 1, imgH_ - 1);
    float fx = x - x0, fy = y - y0;
    auto at = [&](int xx, int yy) { return img_[static_cast<size_t>(yy) * imgW_ + xx] / 255.0f; };
    return lerpf(lerpf(at(x0, y0), at(x1, y0), fx), lerpf(at(x0, y1), at(x1, y1), fx), fy);
  }

  void apply(PolyMesh& m) const override {
    Aabb box = m.bounds();
    if (!box.valid()) return;
    float strength = params.getFloat("strength"), decay = params.getFloat("decay");
    bool center = params.getBool("center");
    int mapping = params.getInt("mapping"), source = params.getInt("source");
    float ut = params.getFloat("uTile"), vt = params.getFloat("vTile");
    float size = params.getFloat("noiseSize");
    float seed = static_cast<float>(params.getInt("seed")) * 17.31f;
    Vec3 c = box.center(), ext = vmax(box.extent(), Vec3(1e-6f));
    std::vector<Vec3> normals = m.vertexNormals();
    for (size_t i = 0; i < m.positions.size(); ++i) {
      Vec3& p = m.positions[i];
      Vec3 d = p - c;
      float value;
      if (source == 0) {
        value = 0.5f + 0.5f * perlinNoise(p.x / size + seed, p.y / size, p.z / size);
      } else {
        float u, v;
        if (mapping == 1) {
          u = std::atan2(d.y, d.x) / kTwoPi + 0.5f;
          v = d.z / ext.z + 0.5f;
        } else if (mapping == 2) {
          Vec3 n = normalize(d);
          u = std::atan2(n.y, n.x) / kTwoPi + 0.5f;
          v = std::asin(clampf(n.z, -1.0f, 1.0f)) / kPi + 0.5f;
        } else {
          u = d.x / ext.x + 0.5f;
          v = d.y / ext.y + 0.5f;
        }
        value = sampleImage(u * ut, v * vt);
      }
      if (center) value -= 0.5f;
      float amount = strength * value * std::exp(-decay * length(d));
      Vec3 dir;
      switch (mapping) {
        case 1: dir = normalize(Vec3(d.x, d.y, 0.0f)); break;
        case 2: dir = normalize(d); break;
        case 3: dir = normalize(normals[i]); break;
        default: dir = Vec3(0, 0, 1); break;
      }
      p += dir * amount;
    }
  }

 private:
  mutable std::mutex imgMutex_;
  mutable std::string imgPath_;
  mutable std::vector<unsigned char> img_;
  mutable int imgW_ = 0, imgH_ = 0;

 public:
  DisplaceModifier(const DisplaceModifier& o) : ModBase<DisplaceModifier>(o) {}
};

// ---------------------------------------------------------------------------------------------
// Topology modifiers

class SubdivideModifier : public ModBase<SubdivideModifier> {
 public:
  SubdivideModifier() {
    params.beginGroup("Parameters");
    params.addFloat("size", "Size", 20.0, 0.01, 1e6, 1.0);
  }
  std::string type() const override { return "Subdivide"; }
  void apply(PolyMesh& m) const override {
    float size = params.getFloat("size");
    for (int it = 0; it < 6; ++it) {
      PolySelection sel;
      sel.level = PolyFace;
      for (size_t f = 0; f < m.faces.size(); ++f) {
        const auto& face = m.faces[f];
        float longest = 0.0f;
        for (size_t k = 0; k < face.v.size(); ++k)
          longest = std::max(longest, length(m.positions[face.v[k]] - m.positions[face.v[(k + 1) % face.v.size()]]));
        if (longest > size) sel.faces.push_back(static_cast<int>(f));
      }
      if (sel.faces.empty() || m.faces.size() > 2000000) break;
      runPolyOp(m, sel, {{"op", "tessellate"}});
    }
  }
};

class TessellateModifier : public ModBase<TessellateModifier> {
 public:
  TessellateModifier() {
    params.beginGroup("Parameters");
    params.addInt("iterations", "Iterations", 1, 1, 4);
    params.addFloat("tension", "Tension", 0.0, -100.0, 100.0, 1.0, 1);
  }
  std::string type() const override { return "Tessellate"; }
  void applyState(GeomState& st) const override {
    if (st.isShape) return;
    PolySelection sel;
    sel.level = PolyFace;
    if (!st.selWeights.empty()) sel.faces = stackSelectedFaces(st);
    size_t before = st.mesh.positions.size();
    runPolyOp(st.mesh, sel, {{"op", "tessellate"}, {"iterations", params.getInt("iterations")}});
    float tension = params.getFloat("tension") / 100.0f;
    if (tension != 0.0f && st.mesh.positions.size() > before) {
      // Push new vertices along the vertex normals (rounds or pinches the surface).
      std::vector<Vec3> normals = st.mesh.vertexNormals();
      Aabb b = st.mesh.bounds();
      float scale = b.valid() ? b.radius() * 0.1f : 1.0f;
      for (size_t v = before; v < st.mesh.positions.size(); ++v) st.mesh.positions[v] += normalize(normals[v]) * tension * scale;
    }
    st.selWeights.clear();
  }
};

class FaceExtrudeModifier : public ModBase<FaceExtrudeModifier> {
 public:
  FaceExtrudeModifier() {
    params.beginGroup("Parameters");
    params.addFloat("amount", "Amount", 10.0, -1e6, 1e6, 0.5);
    params.addFloat("scale", "Scale", 100.0, 0.0, 1000.0, 1.0, 1);
    params.addBool("fromCenter", "Extrude From Center", false);
  }
  std::string type() const override { return "FaceExtrude"; }
  void applyState(GeomState& st) const override {
    if (st.isShape) return;
    PolySelection sel;
    sel.level = PolyFace;
    sel.faces = stackSelectedFaces(st);
    if (sel.faces.empty()) return;
    PolyMesh& m = st.mesh;
    float amount = params.getFloat("amount");
    if (params.getBool("fromCenter")) {
      // Move outward from the object's center instead of along normals.
      Vec3 c = m.bounds().center();
      runPolyOp(m, sel, {{"op", "extrudeFaces"}, {"height", 0.0}, {"mode", 0}});
      std::set<int> verts;
      for (int f : sel.faces)
        for (int v : m.faces[f].v) verts.insert(v);
      for (int v : verts) m.positions[v] += normalize(m.positions[v] - c) * amount;
    } else {
      runPolyOp(m, sel, {{"op", "extrudeFaces"}, {"height", amount}, {"mode", 0}});
    }
    float s = params.getFloat("scale") / 100.0f;
    if (std::fabs(s - 1.0f) > 1e-6f) {
      Vec3 c;
      std::set<int> verts;
      for (int f : sel.faces)
        for (int v : m.faces[f].v) verts.insert(v);
      for (int v : verts) c += m.positions[v];
      c /= static_cast<float>(std::max<size_t>(1, verts.size()));
      for (int v : verts) m.positions[v] = c + (m.positions[v] - c) * s;
    }
    st.selWeights.clear();
  }
};

class MaterialModifier : public ModBase<MaterialModifier> {
 public:
  MaterialModifier() {
    params.beginGroup("Parameters");
    params.addInt("id", "Material ID", 1, 1, 65535);
  }
  std::string type() const override { return "Material"; }
  void applyState(GeomState& st) const override {
    if (st.isShape) return;
    int id = params.getInt("id") - 1;
    for (int f : stackSelectedFaces(st)) st.mesh.faces[f].matId = id;
  }
};

class MaterialByElementModifier : public ModBase<MaterialByElementModifier> {
 public:
  MaterialByElementModifier() {
    params.beginGroup("Parameters");
    params.addInt("idCount", "ID Count", 2, 1, 1000);
    params.addInt("firstId", "First ID", 1, 1, 65535);
    params.addInt("seed", "Seed", 12345, 0, 100000000);
  }
  std::string type() const override { return "MaterialByElement"; }
  void apply(PolyMesh& m) const override {
    std::mt19937 rng(static_cast<uint32_t>(params.getInt("seed")));
    int count = params.getInt("idCount"), first = params.getInt("firstId") - 1;
    std::vector<char> done(m.faces.size(), 0);
    for (size_t f = 0; f < m.faces.size(); ++f) {
      if (done[f]) continue;
      int id = first + static_cast<int>(rng() % static_cast<uint32_t>(count));
      for (int g : elementOf(m, static_cast<int>(f))) {
        done[g] = 1;
        m.faces[g].matId = id;
      }
    }
  }
};

class AutoSmoothModifier : public ModBase<AutoSmoothModifier> {
 public:
  AutoSmoothModifier() {
    params.beginGroup("Parameters");
    params.addBool("autoSmooth", "Auto Smooth", true);
    params.addFloat("threshold", "Threshold", 30.0, 0.0, 180.0, 1.0, 1);
    params.addInt("group", "Smoothing Group (manual)", 1, 0, 32);
  }
  std::string type() const override { return "AutoSmooth"; }
  void applyState(GeomState& st) const override {
    if (st.isShape) return;
    std::vector<int> faces = stackSelectedFaces(st);
    if (params.getBool("autoSmooth")) {
      autoSmooth(st.mesh, &faces, params.getFloat("threshold"));
    } else {
      int g = params.getInt("group");
      for (int f : faces) st.mesh.faces[f].smoothGroups = g > 0 ? 1u << (g - 1) : 0u;
    }
  }
};

class VertexWeldModifier : public ModBase<VertexWeldModifier> {
 public:
  VertexWeldModifier() {
    params.beginGroup("Parameters");
    params.addFloat("threshold", "Threshold", 0.1, 0.0, 1e6, 0.01, 3);
  }
  std::string type() const override { return "VertexWeld"; }
  void apply(PolyMesh& m) const override {
    float th = params.getFloat("threshold");
    if (th <= 0.0f || m.positions.empty()) return;
    float cell = th;
    std::unordered_map<int64_t, std::vector<int>> grid;
    auto key = [cell](int64_t x, int64_t y, int64_t z) { return (x * 73856093) ^ (y * 19349663) ^ (z * 83492791); };
    auto q = [cell](float v) { return static_cast<int64_t>(std::floor(v / cell)); };
    std::vector<int> target(m.positions.size());
    std::vector<std::pair<int, int>> pairs;
    for (size_t i = 0; i < m.positions.size(); ++i) {
      const Vec3& p = m.positions[i];
      int64_t cx = q(p.x), cy = q(p.y), cz = q(p.z);
      int found = -1;
      for (int64_t dx = -1; dx <= 1 && found < 0; ++dx)
        for (int64_t dy = -1; dy <= 1 && found < 0; ++dy)
          for (int64_t dz = -1; dz <= 1 && found < 0; ++dz) {
            auto it = grid.find(key(cx + dx, cy + dy, cz + dz));
            if (it == grid.end()) continue;
            for (int j : it->second)
              if (length(m.positions[j] - p) <= th) {
                found = j;
                break;
              }
          }
      if (found >= 0) pairs.push_back({static_cast<int>(i), found});
      else grid[key(cx, cy, cz)].push_back(static_cast<int>(i));
    }
    weldPairs(m, pairs);
  }
};

class UvwXformModifier : public ModBase<UvwXformModifier> {
 public:
  UvwXformModifier() {
    params.beginGroup("Parameters");
    params.addFloat("uTile", "U Tile", 1.0, -1e4, 1e4, 0.1);
    params.addFloat("vTile", "V Tile", 1.0, -1e4, 1e4, 0.1);
    params.addBool("uFlip", "U Flip", false);
    params.addBool("vFlip", "V Flip", false);
    params.addFloat("uOffset", "U Offset", 0.0, -1e4, 1e4, 0.01);
    params.addFloat("vOffset", "V Offset", 0.0, -1e4, 1e4, 0.01);
    params.addFloat("rotation", "Rotation Angle", 0.0, -360, 360, 1.0, 1);
  }
  std::string type() const override { return "UVWXform"; }
  void apply(PolyMesh& m) const override {
    float ut = params.getFloat("uTile") * (params.getBool("uFlip") ? -1.0f : 1.0f);
    float vt = params.getFloat("vTile") * (params.getBool("vFlip") ? -1.0f : 1.0f);
    float uo = params.getFloat("uOffset"), vo = params.getFloat("vOffset");
    float a = radians(params.getFloat("rotation"));
    float ca = std::cos(a), sa = std::sin(a);
    for (auto& t : m.uvs) {
      // Rotate about the tile center, then tile and offset.
      float x = t.x - 0.5f, y = t.y - 0.5f;
      float rx = x * ca - y * sa, ry = x * sa + y * ca;
      t = Vec2((rx + 0.5f) * ut + uo, (ry + 0.5f) * vt + vo);
    }
  }
};

}  // namespace

void registerMeshModifierTypes() {
  auto add = [](const char* type, const char* name, const char* category, std::function<std::unique_ptr<Modifier>()> f) {
    registerModifierType({type, name, category, true, false, std::move(f)});
  };
  const char* ffd = "Free Form Deformers";
  add("FFD2", "FFD 2x2x2", ffd, [] { return std::make_unique<FfdModifier>(FfdModifier::Fixed2); });
  add("FFD3", "FFD 3x3x3", ffd, [] { return std::make_unique<FfdModifier>(FfdModifier::Fixed3); });
  add("FFD4", "FFD 4x4x4", ffd, [] { return std::make_unique<FfdModifier>(FfdModifier::Fixed4); });
  add("FFDBox", "FFD(box)", ffd, [] { return std::make_unique<FfdModifier>(FfdModifier::Box); });
  add("FFDCyl", "FFD(cyl)", ffd, [] { return std::make_unique<FfdModifier>(FfdModifier::Cylinder); });
  const char* mesh = "Mesh Editing";
  add("Symmetry", "Symmetry", mesh, [] { return std::make_unique<SymmetryModifier>(); });
  add("Slice", "Slice", mesh, [] { return std::make_unique<SliceModifier>(); });
  add("CapHoles", "Cap Holes", mesh, [] { return std::make_unique<CapHolesModifier>(); });
  add("Chamfer", "Chamfer", mesh, [] { return std::make_unique<ChamferModifier>(); });
  add("Optimize", "Optimize", mesh, [] { return std::make_unique<OptimizeModifier>(); });
  add("FaceExtrude", "Face Extrude", mesh, [] { return std::make_unique<FaceExtrudeModifier>(); });
  add("AutoSmooth", "Smooth", mesh, [] { return std::make_unique<AutoSmoothModifier>(); });
  add("VertexWeld", "Vertex Weld", mesh, [] { return std::make_unique<VertexWeldModifier>(); });
  add("Tessellate", "Tessellate", mesh, [] { return std::make_unique<TessellateModifier>(); });
  const char* para = "Parametric Deformers";
  add("Relax", "Relax", para, [] { return std::make_unique<RelaxModifier>(); });
  add("Lattice", "Lattice", para, [] { return std::make_unique<LatticeModifier>(); });
  add("Melt", "Melt", para, [] { return std::make_unique<MeltModifier>(); });
  add("Ripple", "Ripple", para, [] { return std::make_unique<RippleModifier>(); });
  add("Wave", "Wave", para, [] { return std::make_unique<WaveModifier>(); });
  add("Spherify", "Spherify", para, [] { return std::make_unique<SpherifyModifier>(); });
  add("Skew", "Skew", para, [] { return std::make_unique<SkewModifier>(); });
  add("Stretch", "Stretch", para, [] { return std::make_unique<StretchModifier>(); });
  add("Squeeze", "Squeeze", para, [] { return std::make_unique<SqueezeModifier>(); });
  add("Displace", "Displace", para, [] { return std::make_unique<DisplaceModifier>(); });
  add("Subdivide", "Subdivide", "Subdivision Surfaces", [] { return std::make_unique<SubdivideModifier>(); });
  const char* surf = "Surface";
  add("Material", "Material", surf, [] { return std::make_unique<MaterialModifier>(); });
  add("MaterialByElement", "MaterialByElement", surf, [] { return std::make_unique<MaterialByElementModifier>(); });
  const char* uv = "UV Coordinates";
  add("UnwrapUVW", "Unwrap UVW", uv, [] { return std::make_unique<UnwrapUvwModifier>(); });
  add("UVWXform", "UVW XForm", uv, [] { return std::make_unique<UvwXformModifier>(); });
}

}  // namespace veraa
