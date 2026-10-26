#include "mesh.h"

#include "parallel.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <mutex>
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

namespace {

static_assert(sizeof(Vec3) == 12 && sizeof(Vec2) == 8, "mesh encoding copies vectors as packed floats");

enum MeshBinFlags : uint32_t { kWide = 1, kAnyUv = 2, kSomeUv = 4, kAnyMat = 8, kAnySmooth = 16 };

template <typename T>
void put(std::string& out, const T& v) {
  out.append(reinterpret_cast<const char*>(&v), sizeof(T));
}
void putBytes(std::string& out, const void* p, size_t n) {
  if (n) out.append(static_cast<const char*>(p), n);
}

struct Reader {
  const char* p;
  const char* end;
  template <typename T>
  bool get(T& v) {
    if (static_cast<size_t>(end - p) < sizeof(T)) return false;
    std::memcpy(&v, p, sizeof(T));
    p += sizeof(T);
    return true;
  }
  // Points `out` at count elements of size elem, or fails when the data is too short.
  bool take(uint64_t count, size_t elem, const char*& out) {
    if (elem && count > static_cast<uint64_t>(end - p) / elem) return false;
    out = p;
    p += count * elem;
    return true;
  }
};

}  // namespace

void encodePolyMesh(const PolyMesh& mesh, std::string& out) {
  uint64_t nf = mesh.faces.size(), corners = 0, uvCorners = 0;
  uint32_t flags = 0;
  bool allUv = true;
  for (const auto& f : mesh.faces) {
    if (f.v.size() > 255) flags |= kWide;
    bool faceUv = !f.uv.empty() && f.uv.size() == f.v.size();
    if (faceUv) {
      flags |= kAnyUv;
      uvCorners += f.v.size();
    }
    allUv &= faceUv;
    if (f.matId != 0) flags |= kAnyMat;
    if (f.smoothGroups != 1) flags |= kAnySmooth;
    corners += f.v.size();
  }
  if ((flags & kAnyUv) && !allUv) flags |= kSomeUv;
  bool wide = flags & kWide;
  size_t bytes = 48 + mesh.positions.size() * 12 + mesh.uvs.size() * 8 + nf * (wide ? 4 : 1) + corners * 4 +
                 ((flags & kSomeUv) ? nf : 0) + uvCorners * 4 + ((flags & kAnyMat) ? nf * 4 : 0) +
                 ((flags & kAnySmooth) ? nf * 4 : 0);
  out.reserve(out.size() + bytes);
  put(out, static_cast<uint64_t>(mesh.positions.size()));
  put(out, static_cast<uint64_t>(mesh.uvs.size()));
  put(out, nf);
  put(out, corners);
  put(out, uvCorners);
  put(out, flags);
  put(out, uint32_t(0));
  putBytes(out, mesh.positions.data(), mesh.positions.size() * sizeof(Vec3));
  putBytes(out, mesh.uvs.data(), mesh.uvs.size() * sizeof(Vec2));
  // Each array is written in one pass into the reserved string.
  size_t at = out.size();
  out.resize(at + nf * (wide ? 4 : 1));
  char* c = &out[at];
  for (const auto& f : mesh.faces) {
    if (wide) {
      uint32_t n = static_cast<uint32_t>(f.v.size());
      std::memcpy(c, &n, 4);
      c += 4;
    } else {
      *c++ = static_cast<char>(static_cast<uint8_t>(f.v.size()));
    }
  }
  for (const auto& f : mesh.faces) putBytes(out, f.v.data(), f.v.size() * sizeof(int));
  if (flags & kSomeUv)
    for (const auto& f : mesh.faces) out.push_back(!f.uv.empty() && f.uv.size() == f.v.size() ? 1 : 0);
  if (flags & kAnyUv)
    for (const auto& f : mesh.faces)
      if (!f.uv.empty() && f.uv.size() == f.v.size()) putBytes(out, f.uv.data(), f.uv.size() * sizeof(int));
  if (flags & kAnyMat)
    for (const auto& f : mesh.faces) put(out, static_cast<int32_t>(f.matId));
  if (flags & kAnySmooth)
    for (const auto& f : mesh.faces) put(out, f.smoothGroups);
}

bool decodePolyMesh(const char* data, size_t size, PolyMesh& mesh) {
  mesh.clear();
  Reader r{data, data + size};
  uint64_t nv = 0, nt = 0, nf = 0, corners = 0, uvCorners = 0;
  uint32_t flags = 0, reserved = 0;
  if (!r.get(nv) || !r.get(nt) || !r.get(nf) || !r.get(corners) || !r.get(uvCorners) || !r.get(flags) ||
      !r.get(reserved))
    return false;
  // Indices are stored as int: larger meshes cannot be represented.
  const uint64_t kMax = 0x7fffffffull;
  if (nv > kMax || nt > kMax || nf > kMax) return false;
  bool wide = flags & kWide;
  const char *pp, *tp, *cp, *vp, *hp = nullptr, *up = nullptr, *mp = nullptr, *sp = nullptr;
  if (!r.take(nv, 12, pp) || !r.take(nt, 8, tp) || !r.take(nf, wide ? 4 : 1, cp) || !r.take(corners, 4, vp))
    return false;
  if ((flags & kSomeUv) && !r.take(nf, 1, hp)) return false;
  if ((flags & kAnyUv) && !r.take(uvCorners, 4, up)) return false;
  if ((flags & kAnyMat) && !r.take(nf, 4, mp)) return false;
  if ((flags & kAnySmooth) && !r.take(nf, 4, sp)) return false;
  mesh.positions.resize(nv);
  if (nv) std::memcpy(mesh.positions.data(), pp, nv * 12);
  mesh.uvs.resize(nt);
  if (nt) std::memcpy(mesh.uvs.data(), tp, nt * 8);
  mesh.faces.resize(nf);
  uint64_t c = 0, cu = 0;
  const int np = static_cast<int>(nv), ntv = static_cast<int>(nt);
  for (uint64_t f = 0; f < nf; ++f) {
    PolyMesh::Face& face = mesh.faces[f];
    uint32_t n;
    if (wide) std::memcpy(&n, cp + f * 4, 4);
    else n = static_cast<uint8_t>(cp[f]);
    if (n < 3 || n > corners - c) {
      mesh.clear();
      return false;
    }
    face.v.resize(n);
    std::memcpy(face.v.data(), vp + c * 4, n * 4);
    c += n;
    for (int i : face.v)
      if (i < 0 || i >= np) {
        mesh.clear();
        return false;
      }
    if ((flags & kAnyUv) && (!hp || hp[f])) {
      if (n > uvCorners - cu) {
        mesh.clear();
        return false;
      }
      face.uv.resize(n);
      std::memcpy(face.uv.data(), up + cu * 4, n * 4);
      cu += n;
      for (int i : face.uv)
        if (i < 0 || i >= ntv) {
          mesh.clear();
          return false;
        }
    }
    if (mp) std::memcpy(&face.matId, mp + f * 4, 4);
    if (sp) std::memcpy(&face.smoothGroups, sp + f * 4, 4);
  }
  return true;
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

uint32_t floatBits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, sizeof(u));
  return u;
}

// Work per parallel range when triangulating (small meshes stay on the calling thread).
constexpr size_t kTriGrain = 32768;

}  // namespace

TriMesh triangulate(const PolyMesh& mesh) {
  TriMesh out;
  const size_t nv = mesh.positions.size();
  const size_t nf = mesh.faces.size();

  // Corner numbering (faces with fewer than 3 corners are skipped), face normals and triangles.
  std::vector<uint32_t> faceStart(nf + 1, 0), triStart(nf + 1, 0);
  for (size_t fi = 0; fi < nf; ++fi) {
    size_t n = mesh.faces[fi].v.size();
    bool ok = n >= 3;
    faceStart[fi + 1] = faceStart[fi] + static_cast<uint32_t>(ok ? n : 0);
    triStart[fi + 1] = triStart[fi] + static_cast<uint32_t>(ok ? n - 2 : 0);
  }
  const size_t corners = faceStart[nf];
  std::vector<Vec3> faceN(nf);
  parallelRanges(nf, kTriGrain, [&](size_t a, size_t b) {
    for (size_t fi = a; fi < b; ++fi)
      if (mesh.faces[fi].v.size() >= 3) faceN[fi] = mesh.faceNormal(mesh.faces[fi]);
  });

  // Corners around each vertex, in face order (CSR: vertStart[v] .. vertStart[v + 1]).
  std::vector<uint32_t> vertStart(nv + 1, 0), vertCorner(corners), cornerFace(corners);
  for (size_t fi = 0; fi < nf; ++fi)
    if (mesh.faces[fi].v.size() >= 3)
      for (int vi : mesh.faces[fi].v) ++vertStart[static_cast<size_t>(vi) + 1];
  for (size_t v = 0; v < nv; ++v) vertStart[v + 1] += vertStart[v];
  {
    std::vector<uint32_t> fill(vertStart.begin(), vertStart.end() - 1);
    for (size_t fi = 0; fi < nf; ++fi) {
      const auto& f = mesh.faces[fi];
      if (f.v.size() < 3) continue;
      for (size_t k = 0; k < f.v.size(); ++k) {
        uint32_t c = faceStart[fi] + static_cast<uint32_t>(k);
        vertCorner[fill[static_cast<size_t>(f.v[k])]++] = c;
        cornerFace[c] = static_cast<uint32_t>(fi);
      }
    }
  }

  // Corner normals: the face normal, or the sum over the vertex's faces sharing a smoothing group.
  std::vector<Vec3> cornerN(corners);
  parallelRanges(nf, kTriGrain / 4, [&](size_t a, size_t b) {
    for (size_t fi = a; fi < b; ++fi) {
      const auto& f = mesh.faces[fi];
      if (f.v.size() < 3) continue;
      Vec3 fnUnit = normalize(faceN[fi]);
      for (size_t k = 0; k < f.v.size(); ++k) {
        Vec3 normal = fnUnit;
        if (f.smoothGroups != 0) {
          size_t vi = static_cast<size_t>(f.v[k]);
          Vec3 acc;
          for (uint32_t i = vertStart[vi]; i < vertStart[vi + 1]; ++i) {
            uint32_t g = cornerFace[vertCorner[i]];
            if (g == fi || (mesh.faces[g].smoothGroups & f.smoothGroups) != 0) acc += faceN[g];
          }
          normal = length2(acc) > 1e-30f ? normalize(acc) : fnUnit;
        }
        cornerN[faceStart[fi] + k] = normal;
      }
    }
  });

  // Output vertices: one per distinct (vertex, uv, normal), numbered vertex by vertex.
  auto cornerUv = [&](uint32_t c) {
    const auto& f = mesh.faces[cornerFace[c]];
    return f.uv.empty() ? -1 : f.uv[c - faceStart[cornerFace[c]]];
  };
  auto sameCorner = [&](uint32_t a, uint32_t b) {
    const Vec3 &na = cornerN[a], &nb = cornerN[b];
    return cornerUv(a) == cornerUv(b) && floatBits(na.x) == floatBits(nb.x) && floatBits(na.y) == floatBits(nb.y) &&
           floatBits(na.z) == floatBits(nb.z);
  };
  std::vector<uint32_t> cornerOut(corners), vertOut(nv + 1, 0);
  parallelRanges(nv, kTriGrain, [&](size_t a, size_t b) {
    std::vector<uint32_t> reps;
    for (size_t v = a; v < b; ++v) {
      reps.clear();
      for (uint32_t i = vertStart[v]; i < vertStart[v + 1]; ++i) {
        uint32_t c = vertCorner[i];
        uint32_t local = 0;
        while (local < reps.size() && !sameCorner(reps[local], c)) ++local;
        if (local == reps.size()) reps.push_back(c);
        cornerOut[c] = local;
      }
      vertOut[v + 1] = static_cast<uint32_t>(reps.size());
    }
  });
  for (size_t v = 0; v < nv; ++v) vertOut[v + 1] += vertOut[v];
  const size_t outCount = vertOut[nv];
  out.positions.resize(outCount);
  out.normals.resize(outCount);
  out.uvs.resize(outCount);
  parallelRanges(nv, kTriGrain, [&](size_t a, size_t b) {
    for (size_t v = a; v < b; ++v)
      for (uint32_t i = vertStart[v]; i < vertStart[v + 1]; ++i) {
        uint32_t c = vertCorner[i];
        uint32_t o = vertOut[v] + cornerOut[c];
        cornerOut[c] = o;
        out.positions[o] = mesh.positions[v];
        out.normals[o] = cornerN[c];
        int uv = cornerUv(c);
        out.uvs[o] = uv >= 0 ? mesh.uvs[static_cast<size_t>(uv)] : Vec2();
      }
  });
  cornerN = std::vector<Vec3>();

  // Triangles: n - 2 per face (ear clipping for polygons), written in face order.
  out.indices.resize(static_cast<size_t>(triStart[nf]) * 3);
  out.triMatIds.resize(triStart[nf]);
  parallelRanges(nf, kTriGrain / 2, [&](size_t a, size_t b) {
    std::vector<Vec2> projected;
    std::vector<int> tris;
    for (size_t fi = a; fi < b; ++fi) {
      const auto& f = mesh.faces[fi];
      size_t n = f.v.size();
      if (n < 3) continue;
      tris.clear();
      if (n == 3) {
        tris = {0, 1, 2};
      } else {
        // Project onto the plane most aligned with the face normal, then ear-clip.
        Vec3 an = vabs(normalize(faceN[fi]));
        int drop = (an.x > an.y && an.x > an.z) ? 0 : (an.y > an.z ? 1 : 2);
        projected.resize(n);
        for (size_t k = 0; k < n; ++k) {
          const Vec3& p = mesh.positions[static_cast<size_t>(f.v[k])];
          projected[k] = drop == 0 ? Vec2(p.y, p.z) : (drop == 1 ? Vec2(p.z, p.x) : Vec2(p.x, p.y));
        }
        earClip(projected, tris);
      }
      const uint32_t* co = &cornerOut[faceStart[fi]];
      uint32_t* dst = &out.indices[static_cast<size_t>(triStart[fi]) * 3];
      for (size_t t = 0; t + 2 < tris.size(); t += 3) {
        *dst++ = co[tris[t]];
        *dst++ = co[tris[t + 1]];
        *dst++ = co[tris[t + 2]];
      }
      std::fill(out.triMatIds.begin() + triStart[fi], out.triMatIds.begin() + triStart[fi + 1], f.matId);
    }
  });

  // Unique polygon edges for wireframe: each edge listed once, at its lower vertex.
  out.wirePositions = mesh.positions;
  std::vector<uint32_t> edgeStart(nv + 1, 0);
  std::vector<std::vector<uint32_t>> rangeEdges;  // per range: other ends, vertex by vertex
  const size_t ranges = std::max<size_t>(1, std::min<size_t>(nv / kTriGrain, size_t(parallelThreads()) * 4));
  rangeEdges.resize(ranges);
  parallelFor(ranges, [&](size_t r) {
    size_t a = nv * r / ranges, b = nv * (r + 1) / ranges;
    std::vector<uint32_t>& list = rangeEdges[r];
    std::vector<uint32_t> nb;
    for (size_t v = a; v < b; ++v) {
      nb.clear();
      for (uint32_t i = vertStart[v]; i < vertStart[v + 1]; ++i) {
        uint32_t c = vertCorner[i];
        uint32_t fi = cornerFace[c];
        const auto& f = mesh.faces[fi];
        size_t n = f.v.size(), k = c - faceStart[fi];
        uint32_t next = static_cast<uint32_t>(f.v[(k + 1) % n]), prev = static_cast<uint32_t>(f.v[(k + n - 1) % n]);
        if (next >= v) nb.push_back(next);
        if (prev >= v) nb.push_back(prev);
      }
      std::sort(nb.begin(), nb.end());
      nb.erase(std::unique(nb.begin(), nb.end()), nb.end());
      edgeStart[v + 1] = static_cast<uint32_t>(nb.size());
      list.insert(list.end(), nb.begin(), nb.end());
    }
  });
  for (size_t v = 0; v < nv; ++v) edgeStart[v + 1] += edgeStart[v];
  out.wireIndices.resize(static_cast<size_t>(edgeStart[nv]) * 2);
  parallelFor(ranges, [&](size_t r) {
    size_t a = nv * r / ranges, b = nv * (r + 1) / ranges;
    const std::vector<uint32_t>& list = rangeEdges[r];
    size_t j = 0;
    for (size_t v = a; v < b; ++v)
      for (uint32_t e = edgeStart[v]; e < edgeStart[v + 1]; ++e) {
        out.wireIndices[size_t(e) * 2] = static_cast<uint32_t>(v);
        out.wireIndices[size_t(e) * 2 + 1] = list[j++];
      }
  });

  for (const auto& p : out.positions) out.bounds.expand(p);
  return out;
}

struct TriBvh {
  struct Node {
    Aabb box;
    uint32_t first = 0;  // leaf: first entry in tris; inner: index of the left child
    uint32_t count = 0;  // triangles in a leaf, 0 for inner nodes
    uint32_t right = 0;  // inner: index of the right child
  };
  std::vector<Node> nodes;
  std::vector<uint32_t> tris;
};

namespace {

constexpr size_t kBvhMinTriangles = 2048;
constexpr uint32_t kBvhLeaf = 4;

// Spreads the low 10 bits of v so there are two zero bits between each.
inline uint32_t spreadBits(uint32_t v) {
  v &= 0x3ff;
  v = (v | (v << 16)) & 0x030000ff;
  v = (v | (v << 8)) & 0x0300f00f;
  v = (v | (v << 4)) & 0x030c30c3;
  v = (v | (v << 2)) & 0x09249249;
  return v;
}

// Linear BVH: triangles sorted along a Morton curve of their centers, then split at the middle
// of each range. Building is O(n) after a radix sort (fast enough to do on the first pick).
std::shared_ptr<const TriBvh> buildBvh(const TriMesh& mesh) {
  auto bvh = std::make_shared<TriBvh>();
  const size_t nt = mesh.indices.size() / 3;
  auto triBox = [&](size_t t) {
    Aabb box;
    for (int k = 0; k < 3; ++k) box.expand(mesh.positions[mesh.indices[t * 3 + k]]);
    return box;
  };
  Aabb cb = mesh.bounds;
  Vec3 ext = cb.extent();
  Vec3 scale(ext.x > 0 ? 1023.0f / ext.x : 0.0f, ext.y > 0 ? 1023.0f / ext.y : 0.0f, ext.z > 0 ? 1023.0f / ext.z : 0.0f);
  // (code << 32 | triangle), radix-sorted on the 30 code bits.
  std::vector<uint64_t> keys(nt), tmp(nt);
  parallelRanges(nt, 65536, [&](size_t a, size_t b) {
    for (size_t t = a; t < b; ++t) {
      Vec3 c = (mesh.positions[mesh.indices[t * 3]] + mesh.positions[mesh.indices[t * 3 + 1]] +
                mesh.positions[mesh.indices[t * 3 + 2]]) * (1.0f / 3.0f);
      Vec3 q = (c - cb.lo) * scale;
      uint32_t code = (spreadBits(static_cast<uint32_t>(q.x)) << 2) | (spreadBits(static_cast<uint32_t>(q.y)) << 1) |
                      spreadBits(static_cast<uint32_t>(q.z));
      keys[t] = (static_cast<uint64_t>(code) << 32) | t;
    }
  });
  for (int pass = 0; pass < 2; ++pass) {
    const int shift = 32 + pass * 15;
    std::vector<uint32_t> count(32769, 0);
    for (uint64_t k : keys) ++count[((k >> shift) & 0x7fff) + 1];
    for (size_t i = 1; i < count.size(); ++i) count[i] += count[i - 1];
    for (uint64_t k : keys) tmp[count[(k >> shift) & 0x7fff]++] = k;
    keys.swap(tmp);
  }
  tmp = std::vector<uint64_t>();
  bvh->tris.resize(nt);
  for (size_t i = 0; i < nt; ++i) bvh->tris[i] = static_cast<uint32_t>(keys[i] & 0xffffffffu);
  keys = std::vector<uint64_t>();
  bvh->nodes.reserve(nt / kBvhLeaf * 2 + 1);
  // Recursive midpoint split; depth is about log2(n / kBvhLeaf).
  std::function<uint32_t(uint32_t, uint32_t)> build = [&](uint32_t begin, uint32_t end) -> uint32_t {
    uint32_t index = static_cast<uint32_t>(bvh->nodes.size());
    bvh->nodes.emplace_back();
    if (end - begin <= kBvhLeaf) {
      Aabb box;
      for (uint32_t i = begin; i < end; ++i) box.expand(triBox(bvh->tris[i]));
      bvh->nodes[index].box = box;
      bvh->nodes[index].first = begin;
      bvh->nodes[index].count = end - begin;
      return index;
    }
    uint32_t mid = begin + (end - begin) / 2;
    uint32_t l = build(begin, mid);
    uint32_t r = build(mid, end);
    bvh->nodes[index].first = l;
    bvh->nodes[index].count = 0;
    bvh->nodes[index].box = bvh->nodes[l].box;
    bvh->nodes[index].box.expand(bvh->nodes[r].box);
    bvh->nodes[index].right = r;
    return index;
  };
  if (nt) build(0, static_cast<uint32_t>(nt));
  return bvh;
}

inline float rayTriangle(const TriMesh& mesh, size_t t, const Ray& ray) {
  const Vec3& p0 = mesh.positions[mesh.indices[t * 3]];
  const Vec3& p1 = mesh.positions[mesh.indices[t * 3 + 1]];
  const Vec3& p2 = mesh.positions[mesh.indices[t * 3 + 2]];
  Vec3 e1 = p1 - p0, e2 = p2 - p0;
  Vec3 pv = cross(ray.d, e2);
  float det = dot(e1, pv);
  if (std::fabs(det) < 1e-12f) return -1.0f;
  float inv = 1.0f / det;
  Vec3 tv = ray.o - p0;
  float u = dot(tv, pv) * inv;
  if (u < 0 || u > 1) return -1.0f;
  Vec3 qv = cross(tv, e1);
  float v = dot(ray.d, qv) * inv;
  if (v < 0 || u + v > 1) return -1.0f;
  float d = dot(e2, qv) * inv;
  return d > 1e-5f ? d : -1.0f;
}

std::mutex& bvhMutex() {
  static std::mutex m;
  return m;
}

}  // namespace

float intersectTriMesh(const TriMesh& mesh, const Ray& ray) {
  if (!mesh.bounds.intersect(ray, 0.0f, kInfinity)) return -1.0f;
  const size_t nt = mesh.indices.size() / 3;
  float best = kInfinity;
  std::shared_ptr<const TriBvh> bvh;
  if (nt >= kBvhMinTriangles) {
    std::lock_guard<std::mutex> lock(bvhMutex());
    if (!mesh.bvh) mesh.bvh = buildBvh(mesh);
    bvh = mesh.bvh;
  }
  if (!bvh) {
    for (size_t t = 0; t < nt; ++t) {
      float d = rayTriangle(mesh, t, ray);
      if (d > 0 && d < best) best = d;
    }
    return best == kInfinity ? -1.0f : best;
  }
  uint32_t stack[128];
  int top = 0;
  stack[top++] = 0;
  while (top > 0) {
    const TriBvh::Node& node = bvh->nodes[stack[--top]];
    if (!node.box.intersect(ray, 0.0f, best)) continue;
    if (node.count) {
      for (uint32_t i = node.first; i < node.first + node.count; ++i) {
        float d = rayTriangle(mesh, bvh->tris[i], ray);
        if (d > 0 && d < best) best = d;
      }
    } else if (top + 2 <= 128) {
      stack[top++] = node.right;
      stack[top++] = node.first;
    }
  }
  return best == kInfinity ? -1.0f : best;
}

}  // namespace veraa
