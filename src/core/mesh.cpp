#include "mesh.h"

#include <cstring>
#include <unordered_map>

namespace veraa {

Aabb PolyMesh::bounds() const {
  Aabb b;
  for (const auto& p : positions) b.expand(p);
  return b;
}

size_t PolyMesh::triangleCount() const {
  size_t n = 0;
  for (const auto& f : faces)
    if (f.v.size() >= 3) n += f.v.size() - 2;
  return n;
}

void PolyMesh::addFace(std::vector<int> v, std::vector<int> uv, uint32_t smooth, int matId) {
  Face f;
  f.v = std::move(v);
  f.uv = std::move(uv);
  f.smoothGroups = smooth;
  f.matId = matId;
  faces.push_back(std::move(f));
}

void PolyMesh::append(const PolyMesh& other) {
  int vOff = static_cast<int>(positions.size());
  int tOff = static_cast<int>(uvs.size());
  positions.insert(positions.end(), other.positions.begin(), other.positions.end());
  uvs.insert(uvs.end(), other.uvs.begin(), other.uvs.end());
  for (Face f : other.faces) {
    for (int& i : f.v) i += vOff;
    for (int& i : f.uv) i += tOff;
    faces.push_back(std::move(f));
  }
}

void PolyMesh::transform(const Mat4& m) {
  for (auto& p : positions) p = m.transformPoint(p);
  // A mirroring transform turns faces inside out; flip them back.
  if (m.determinant3x3() < 0.0f) flipFaces();
}

void PolyMesh::flipFaces() {
  for (auto& f : faces) {
    std::reverse(f.v.begin(), f.v.end());
    std::reverse(f.uv.begin(), f.uv.end());
  }
}

Vec3 PolyMesh::faceNormal(const Face& f) const {
  Vec3 n;
  size_t count = f.v.size();
  for (size_t i = 0; i < count; ++i) {
    const Vec3& a = positions[f.v[i]];
    const Vec3& b = positions[f.v[(i + 1) % count]];
    n.x += (a.y - b.y) * (a.z + b.z);
    n.y += (a.z - b.z) * (a.x + b.x);
    n.z += (a.x - b.x) * (a.y + b.y);
  }
  return n;  // length = 2 * area, so callers can area-weight before normalizing
}

std::vector<Vec3> PolyMesh::vertexNormals() const {
  std::vector<Vec3> n(positions.size(), Vec3());
  for (const auto& f : faces) {
    Vec3 fn = faceNormal(f);
    for (int vi : f.v) n[vi] += fn;
  }
  for (auto& v : n) v = normalize(v);
  return n;
}

bool PolyMesh::isValid() const {
  int np = static_cast<int>(positions.size());
  int nt = static_cast<int>(uvs.size());
  for (const auto& f : faces) {
    if (f.v.size() < 3) return false;
    if (!f.uv.empty() && f.uv.size() != f.v.size()) return false;
    for (int i : f.v)
      if (i < 0 || i >= np) return false;
    for (int i : f.uv)
      if (i < 0 || i >= nt) return false;
  }
  return true;
}

namespace {

// Ear-clipping triangulation of a simple polygon given in 2D. Returns local corner
// indices, 3 per triangle, preserving the input winding.
void earClip(const std::vector<Vec2>& pts, std::vector<int>& out) {
  int n = static_cast<int>(pts.size());
  if (n < 3) return;
  if (n == 3) { out.insert(out.end(), {0, 1, 2}); return; }

  float area = 0;
  for (int i = 0; i < n; ++i) {
    const Vec2& a = pts[i];
    const Vec2& b = pts[(i + 1) % n];
    area += a.x * b.y - b.x * a.y;
  }
  float orient = area >= 0 ? 1.0f : -1.0f;

  auto cross2 = [](const Vec2& o, const Vec2& a, const Vec2& b) {
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
  };

  std::vector<int> idx(n);
  for (int i = 0; i < n; ++i) idx[i] = i;

  int guard = 0;
  while (idx.size() > 3 && guard < n * n) {
    ++guard;
    bool clipped = false;
    int m = static_cast<int>(idx.size());
    for (int i = 0; i < m; ++i) {
      int ip = idx[(i + m - 1) % m], ic = idx[i], in = idx[(i + 1) % m];
      const Vec2 &a = pts[ip], &b = pts[ic], &c = pts[in];
      if (cross2(a, b, c) * orient <= 1e-12f) continue;  // reflex or degenerate corner
      bool inside = false;
      for (int k = 0; k < m && !inside; ++k) {
        int ik = idx[k];
        if (ik == ip || ik == ic || ik == in) continue;
        const Vec2& p = pts[ik];
        float d1 = cross2(a, b, p) * orient, d2 = cross2(b, c, p) * orient, d3 = cross2(c, a, p) * orient;
        inside = d1 >= 0 && d2 >= 0 && d3 >= 0;
      }
      if (inside) continue;
      out.insert(out.end(), {ip, ic, in});
      idx.erase(idx.begin() + i);
      clipped = true;
      break;
    }
    if (!clipped) break;  // degenerate polygon: fan the rest below
  }
  for (size_t i = 1; i + 1 < idx.size(); ++i) out.insert(out.end(), {idx[0], idx[i], idx[i + 1]});
}

struct CornerKey {
  int v, uv;
  uint32_t nx, ny, nz;
  bool operator==(const CornerKey& o) const {
    return v == o.v && uv == o.uv && nx == o.nx && ny == o.ny && nz == o.nz;
  }
};
struct CornerKeyHash {
  size_t operator()(const CornerKey& k) const {
    size_t h = static_cast<size_t>(k.v) * 73856093u;
    h ^= static_cast<size_t>(k.uv + 1) * 19349663u;
    h ^= static_cast<size_t>(k.nx) * 83492791u;
    h ^= static_cast<size_t>(k.ny) * 2654435761u;
    h ^= static_cast<size_t>(k.nz) * 40503u;
    return h;
  }
};

uint32_t floatBits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, sizeof(u));
  return u;
}

}  // namespace

TriMesh triangulate(const PolyMesh& mesh) {
  TriMesh out;
  size_t nv = mesh.positions.size();

  std::vector<Vec3> faceN(mesh.faces.size());
  std::vector<std::vector<int>> vertFaces(nv);
  for (size_t fi = 0; fi < mesh.faces.size(); ++fi) {
    const auto& f = mesh.faces[fi];
    if (f.v.size() < 3) continue;
    faceN[fi] = mesh.faceNormal(f);
    for (int vi : f.v) vertFaces[vi].push_back(static_cast<int>(fi));
  }

  std::unordered_map<CornerKey, uint32_t, CornerKeyHash> cornerMap;
  cornerMap.reserve(mesh.faces.size() * 4);
  std::vector<uint32_t> corners;
  std::vector<Vec2> projected;
  std::vector<int> tris;

  for (size_t fi = 0; fi < mesh.faces.size(); ++fi) {
    const auto& f = mesh.faces[fi];
    size_t n = f.v.size();
    if (n < 3) continue;
    Vec3 fnUnit = normalize(faceN[fi]);

    corners.assign(n, 0);
    for (size_t k = 0; k < n; ++k) {
      int vi = f.v[k];
      Vec3 normal = fnUnit;
      if (f.smoothGroups != 0) {
        Vec3 acc;
        for (int g : vertFaces[vi]) {
          if (g == static_cast<int>(fi) || (mesh.faces[g].smoothGroups & f.smoothGroups) != 0) acc += faceN[g];
        }
        normal = length2(acc) > 1e-30f ? normalize(acc) : fnUnit;
      }
      int uvIdx = f.uv.empty() ? -1 : f.uv[k];
      CornerKey key{vi, uvIdx, floatBits(normal.x), floatBits(normal.y), floatBits(normal.z)};
      auto it = cornerMap.find(key);
      if (it == cornerMap.end()) {
        uint32_t idx = static_cast<uint32_t>(out.positions.size());
        out.positions.push_back(mesh.positions[vi]);
        out.normals.push_back(normal);
        out.uvs.push_back(uvIdx >= 0 ? mesh.uvs[uvIdx] : Vec2());
        cornerMap.emplace(key, idx);
        corners[k] = idx;
      } else {
        corners[k] = it->second;
      }
    }

    tris.clear();
    if (n == 3) {
      tris = {0, 1, 2};
    } else {
      // Project onto the plane most aligned with the face normal, then ear-clip.
      Vec3 an = vabs(fnUnit);
      int drop = (an.x > an.y && an.x > an.z) ? 0 : (an.y > an.z ? 1 : 2);
      projected.resize(n);
      for (size_t k = 0; k < n; ++k) {
        const Vec3& p = mesh.positions[f.v[k]];
        projected[k] = drop == 0 ? Vec2(p.y, p.z) : (drop == 1 ? Vec2(p.z, p.x) : Vec2(p.x, p.y));
      }
      earClip(projected, tris);
    }
    for (size_t t = 0; t + 2 < tris.size(); t += 3) {
      out.indices.push_back(corners[tris[t]]);
      out.indices.push_back(corners[tris[t + 1]]);
      out.indices.push_back(corners[tris[t + 2]]);
      out.triMatIds.push_back(f.matId);
    }
  }

  // Unique polygon edges for wireframe.
  out.wirePositions = mesh.positions;
  std::unordered_map<uint64_t, bool> seen;
  seen.reserve(mesh.faces.size() * 4);
  for (const auto& f : mesh.faces) {
    size_t n = f.v.size();
    for (size_t k = 0; k < n; ++k) {
      uint32_t a = static_cast<uint32_t>(f.v[k]), b = static_cast<uint32_t>(f.v[(k + 1) % n]);
      if (a > b) std::swap(a, b);
      uint64_t key = (static_cast<uint64_t>(a) << 32) | b;
      if (seen.emplace(key, true).second) {
        out.wireIndices.push_back(a);
        out.wireIndices.push_back(b);
      }
    }
  }

  for (const auto& p : out.positions) out.bounds.expand(p);
  return out;
}

float intersectTriMesh(const TriMesh& mesh, const Ray& ray) {
  if (!mesh.bounds.intersect(ray, 0.0f, kInfinity)) return -1.0f;
  float best = kInfinity;
  for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
    const Vec3& p0 = mesh.positions[mesh.indices[i]];
    const Vec3& p1 = mesh.positions[mesh.indices[i + 1]];
    const Vec3& p2 = mesh.positions[mesh.indices[i + 2]];
    Vec3 e1 = p1 - p0, e2 = p2 - p0;
    Vec3 pv = cross(ray.d, e2);
    float det = dot(e1, pv);
    if (std::fabs(det) < 1e-12f) continue;
    float inv = 1.0f / det;
    Vec3 tv = ray.o - p0;
    float u = dot(tv, pv) * inv;
    if (u < 0 || u > 1) continue;
    Vec3 qv = cross(tv, e1);
    float v = dot(ray.d, qv) * inv;
    if (v < 0 || u + v > 1) continue;
    float t = dot(e2, qv) * inv;
    if (t > 1e-5f && t < best) best = t;
  }
  return best == kInfinity ? -1.0f : best;
}

}  // namespace veraa
