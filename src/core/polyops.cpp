// Editable Poly editing operations.
#include "polyops.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <set>

#include "modifier.h"
#include "object.h"

namespace veraa {

namespace {

bool meshHasUvs(const PolyMesh& m) {
  for (const auto& f : m.faces)
    if (!f.uv.empty()) return true;
  return false;
}

int cornerOf(const PolyMesh::Face& f, int v) {
  for (size_t k = 0; k < f.v.size(); ++k)
    if (f.v[k] == v) return static_cast<int>(k);
  return -1;
}

Vec3 faceCenter(const PolyMesh& m, const PolyMesh::Face& f) {
  Vec3 c;
  for (int v : f.v) c += m.positions[v];
  return f.v.empty() ? c : c / static_cast<float>(f.v.size());
}

Vec3 faceUnitNormal(const PolyMesh& m, const PolyMesh::Face& f) { return normalize(m.faceNormal(f)); }

// Removes consecutive duplicate corners; returns false if the face degenerates.
bool cleanFace(PolyMesh::Face& f) {
  bool hasUv = f.uv.size() == f.v.size();
  PolyMesh::Face out;
  out.matId = f.matId;
  out.smoothGroups = f.smoothGroups;
  for (size_t k = 0; k < f.v.size(); ++k) {
    if (!out.v.empty() && out.v.back() == f.v[k]) continue;
    out.v.push_back(f.v[k]);
    if (hasUv) out.uv.push_back(f.uv[k]);
  }
  while (out.v.size() > 1 && out.v.front() == out.v.back()) {
    out.v.pop_back();
    if (hasUv) out.uv.pop_back();
  }
  if (!hasUv) out.uv.clear();
  f = std::move(out);
  std::vector<int> u = f.v;
  sortUnique(u);
  return u.size() >= 3;
}

void cleanAllFaces(PolyMesh& m, std::vector<int>* faceMap = nullptr) {
  std::vector<PolyMesh::Face> kept;
  if (faceMap) faceMap->assign(m.faces.size(), -1);
  for (size_t i = 0; i < m.faces.size(); ++i) {
    PolyMesh::Face f = m.faces[i];
    if (!cleanFace(f)) continue;
    if (faceMap) (*faceMap)[i] = static_cast<int>(kept.size());
    kept.push_back(std::move(f));
  }
  m.faces = std::move(kept);
}

void compactUvs(PolyMesh& m) {
  std::vector<int> map(m.uvs.size(), -1);
  std::vector<Vec2> uvs;
  for (auto& f : m.faces)
    for (int& t : f.uv) {
      if (t < 0 || t >= static_cast<int>(map.size())) continue;
      if (map[t] < 0) {
        map[t] = static_cast<int>(uvs.size());
        uvs.push_back(m.uvs[t]);
      }
      t = map[t];
    }
  m.uvs = std::move(uvs);
}

// Erases faces, compacts vertices and remaps the selection.
void eraseFaces(PolyMesh& m, PolySelection& sel, std::vector<int> faces) {
  sortUnique(faces);
  std::vector<int> faceMap(m.faces.size(), -1);
  std::vector<PolyMesh::Face> kept;
  for (size_t f = 0; f < m.faces.size(); ++f) {
    if (std::binary_search(faces.begin(), faces.end(), static_cast<int>(f))) continue;
    faceMap[f] = static_cast<int>(kept.size());
    kept.push_back(std::move(m.faces[f]));
  }
  m.faces = std::move(kept);
  std::vector<int> vmap = compactVertices(m);
  remapSelection(sel, vmap, faceMap);
}

// Replaces vertex indices through `target` (target[v] = v' to merge v into v'), removes
// degenerate faces and unused vertices, and remaps the selection.
void mergeVerticesInto(PolyMesh& m, PolySelection& sel, const std::vector<int>& target) {
  for (auto& f : m.faces)
    for (int& v : f.v) v = target[v];
  std::vector<int> faceMap;
  cleanAllFaces(m, &faceMap);
  // Selection: vertices map through target first.
  for (int& v : sel.verts) v = target[v];
  std::vector<uint64_t> edges;
  for (uint64_t k : sel.edges) {
    int a = target[edgeKeyA(k)], b = target[edgeKeyB(k)];
    if (a != b) edges.push_back(edgeKey(a, b));
  }
  sel.edges = edges;
  sel.normalize();
  std::vector<int> vmap = compactVertices(m);
  remapSelection(sel, vmap, faceMap);
}

Vec2 uvLerp(const PolyMesh& m, int ta, int tb, float t) { return m.uvs[ta] + (m.uvs[tb] - m.uvs[ta]) * t; }

// Ear clipping of one face (returns local corner triangles).
std::vector<int> earClipFace(const PolyMesh& m, const PolyMesh::Face& f) {
  std::vector<int> out;
  size_t n = f.v.size();
  if (n < 3) return out;
  if (n == 3) return {0, 1, 2};
  Vec3 N = faceUnitNormal(m, f);
  Vec3 t, b;
  makeBasis(N, t, b);
  std::vector<Vec2> p(n);
  for (size_t k = 0; k < n; ++k) {
    const Vec3& q = m.positions[f.v[k]];
    p[k] = Vec2(dot(q, t), dot(q, b));
  }
  float area = 0;
  for (size_t k = 0; k < n; ++k) area += p[k].x * p[(k + 1) % n].y - p[(k + 1) % n].x * p[k].y;
  float orient = area >= 0 ? 1.0f : -1.0f;
  auto cross2 = [](const Vec2& o, const Vec2& a, const Vec2& c) {
    return (a.x - o.x) * (c.y - o.y) - (a.y - o.y) * (c.x - o.x);
  };
  std::vector<int> idx(n);
  for (size_t k = 0; k < n; ++k) idx[k] = static_cast<int>(k);
  int guard = 0;
  while (idx.size() > 3 && guard++ < static_cast<int>(n * n)) {
    bool clipped = false;
    int cnt = static_cast<int>(idx.size());
    for (int i = 0; i < cnt; ++i) {
      int ip = idx[(i + cnt - 1) % cnt], ic = idx[i], in = idx[(i + 1) % cnt];
      if (cross2(p[ip], p[ic], p[in]) * orient <= 1e-12f) continue;
      bool inside = false;
      for (int k = 0; k < cnt && !inside; ++k) {
        int q = idx[k];
        if (q == ip || q == ic || q == in) continue;
        float d1 = cross2(p[ip], p[ic], p[q]) * orient, d2 = cross2(p[ic], p[in], p[q]) * orient,
              d3 = cross2(p[in], p[ip], p[q]) * orient;
        inside = d1 >= 0 && d2 >= 0 && d3 >= 0;
      }
      if (inside) continue;
      out.insert(out.end(), {ip, ic, in});
      idx.erase(idx.begin() + i);
      clipped = true;
      break;
    }
    if (!clipped) break;
  }
  for (size_t i = 1; i + 1 < idx.size(); ++i) out.insert(out.end(), {idx[0], idx[i], idx[i + 1]});
  return out;
}

// Chains directed half-edges (x -> y) into closed loops.
std::vector<std::vector<int>> chainLoops(const std::vector<std::pair<int, int>>& halfEdges) {
  std::multimap<int, size_t> from;
  for (size_t i = 0; i < halfEdges.size(); ++i) from.emplace(halfEdges[i].first, i);
  std::vector<char> used(halfEdges.size(), 0);
  std::vector<std::vector<int>> loops;
  for (size_t s = 0; s < halfEdges.size(); ++s) {
    if (used[s]) continue;
    std::vector<int> loop;
    size_t cur = s;
    bool closed = false;
    for (size_t guard = 0; guard <= halfEdges.size(); ++guard) {
      used[cur] = 1;
      loop.push_back(halfEdges[cur].first);
      int next = halfEdges[cur].second;
      if (next == halfEdges[s].first) {
        closed = true;
        break;
      }
      auto range = from.equal_range(next);
      size_t pick = SIZE_MAX;
      for (auto it = range.first; it != range.second; ++it)
        if (!used[it->second]) {
          pick = it->second;
          break;
        }
      if (pick == SIZE_MAX) break;
      cur = pick;
    }
    if (closed && loop.size() >= 3) loops.push_back(loop);
  }
  return loops;
}

std::vector<std::vector<int>> facesToRegions(const PolyMesh& m, const PolyTopology& topo, const std::vector<int>& faces,
                                             bool byPolygon) {
  std::vector<std::vector<int>> regions;
  if (byPolygon) {
    for (int f : faces) regions.push_back({f});
    return regions;
  }
  std::vector<char> in(m.faces.size(), 0), seen(m.faces.size(), 0);
  for (int f : faces) in[f] = 1;
  for (int f0 : faces) {
    if (seen[f0]) continue;
    std::vector<int> region, stack = {f0};
    seen[f0] = 1;
    while (!stack.empty()) {
      int f = stack.back();
      stack.pop_back();
      region.push_back(f);
      for (int e : topo.faceEdges[f])
        for (int g : topo.edges[e].faces)
          if (in[g] && !seen[g]) {
            seen[g] = 1;
            stack.push_back(g);
          }
    }
    regions.push_back(region);
  }
  return regions;
}

// Region boundary half-edges (u -> v as wound in the region face).
std::vector<std::pair<int, int>> regionBoundary(const PolyMesh& m, const PolyTopology& topo,
                                                const std::vector<int>& region) {
  std::set<int> in(region.begin(), region.end());
  std::vector<std::pair<int, int>> out;
  for (int f : region) {
    const auto& face = m.faces[f];
    size_t n = face.v.size();
    for (size_t k = 0; k < n; ++k) {
      int e = topo.faceEdges[f][k];
      bool inside = false;
      for (int g : topo.edges[e].faces) inside |= g != f && in.count(g);
      if (!inside) out.push_back({face.v[k], face.v[(k + 1) % n]});
    }
  }
  return out;
}

// Duplicates the boundary vertices of a region of faces and stitches side quads between the
// original boundary and the copies. Returns old->new map for the region's vertices (interior
// vertices map to themselves). New side faces are appended to sideFaces.
std::map<int, int> liftRegion(PolyMesh& m, const PolyTopology& topo, const std::vector<int>& region,
                              std::vector<int>* sideFaces) {
  std::vector<std::pair<int, int>> boundary = regionBoundary(m, topo, region);
  std::map<int, int> newv;
  for (int f : region)
    for (int v : m.faces[f].v) newv[v] = v;
  for (const auto& he : boundary)
    for (int v : {he.first, he.second})
      if (newv[v] == v) newv[v] = m.addVertex(m.positions[v]);
  for (int f : region)
    for (int& v : m.faces[f].v) v = newv[v];
  bool uvs = meshHasUvs(m);
  for (const auto& he : boundary) {
    int u = he.first, v = he.second;
    PolyMesh::Face side;
    side.v = std::vector<int>{u, v, newv[v], newv[u]};
    side.smoothGroups = 0;
    side.matId = m.faces[region[0]].matId;
    if (uvs) {
      side.uv.clear();  // filled by ensureFaceUvs
    }
    m.faces.push_back(side);
    if (sideFaces) sideFaces->push_back(static_cast<int>(m.faces.size()) - 1);
  }
  return newv;
}

// Inward in-plane miter offset for each boundary vertex of a region (unit amount).
std::map<int, Vec3> insetOffsets(const PolyMesh& m, const std::vector<int>& region,
                                 const std::vector<std::pair<int, int>>& boundary) {
  std::map<int, Vec3> inward;
  std::map<int, std::vector<Vec3>> perVert;
  // Normal of the region face owning each half-edge.
  std::map<std::pair<int, int>, Vec3> heNormal;
  for (int f : region) {
    const auto& face = m.faces[f];
    Vec3 n = faceUnitNormal(m, face);
    for (size_t k = 0; k < face.v.size(); ++k) heNormal[{face.v[k], face.v[(k + 1) % face.v.size()]}] = n;
  }
  for (const auto& he : boundary) {
    Vec3 dir = normalize(m.positions[he.second] - m.positions[he.first]);
    Vec3 n = heNormal[he];
    Vec3 in = normalize(cross(n, dir));
    perVert[he.first].push_back(in);
    perVert[he.second].push_back(in);
  }
  for (auto& [v, list] : perVert) {
    Vec3 sum;
    for (const auto& d : list) sum += d;
    if (length2(sum) < 1e-12f) {
      inward[v] = list.empty() ? Vec3() : list[0];
      continue;
    }
    Vec3 dir = normalize(sum);
    float c = std::max(0.25f, dot(dir, list[0]));
    inward[v] = dir / c;
  }
  return inward;
}

// Corner-rule chamfer shared by vertex and edge chamfer.
struct ChamferResult {
  std::vector<int> newFaces;
  std::vector<int> newVerts;
};

ChamferResult chamferCore(PolyMesh& m, PolySelection& sel, const std::set<int>& vertSel, const std::set<int>& edgeSel,
                          float amount, bool open) {
  ChamferResult res;
  PolyTopology topo(m);
  size_t nf = m.faces.size();
  bool uvs = meshHasUvs(m);
  auto edgeLen = [&](int e) { return length(m.positions[topo.edges[e].a] - m.positions[topo.edges[e].b]); };
  // Pass 1: which (vertex, edge) slides exist.
  std::map<std::pair<int, int>, int> slide;  // (v, e) -> new vertex
  auto needSlide = [&](int v, int e) { slide[{v, e}] = -1; };
  for (size_t f = 0; f < nf; ++f) {
    const auto& face = m.faces[f];
    size_t n = face.v.size();
    for (size_t k = 0; k < n; ++k) {
      int v = face.v[k];
      int ep = topo.faceEdges[f][(k + n - 1) % n], en = topo.faceEdges[f][k];
      if (vertSel.count(v)) {
        needSlide(v, ep);
        needSlide(v, en);
        continue;
      }
      bool sp = edgeSel.count(ep) > 0, sn = edgeSel.count(en) > 0;
      if (sn && !sp) needSlide(v, ep);
      if (sp && !sn) needSlide(v, en);
    }
  }
  for (auto& [key, idx] : slide) {
    int v = key.first, e = key.second;
    int o = topo.otherVertex(e, v);
    float len = edgeLen(e);
    bool bothEnds = slide.count({o, e}) > 0;
    float a = std::min(amount, len * (bothEnds ? 0.49f : 0.98f));
    Vec3 dir = len > 1e-12f ? (m.positions[o] - m.positions[v]) / len : Vec3();
    idx = m.addVertex(m.positions[v] + dir * a);
    res.newVerts.push_back(idx);
  }
  // Pass 2: rebuild faces from corner lists.
  std::set<std::pair<int, int>> boundaryHalf;  // directed edges that were open borders
  std::map<int, std::vector<std::array<int, 4>>> sides;  // chamfered edge -> {u, v, new u, new v} per face
  for (size_t f = 0; f < nf; ++f) {
    PolyMesh::Face& face = m.faces[f];
    size_t n = face.v.size();
    bool hasUv = face.uv.size() == n;
    std::vector<std::vector<int>> corner(n), cornerUv(n);
    for (size_t k = 0; k < n; ++k) {
      int v = face.v[k];
      int vp = face.v[(k + n - 1) % n], vn = face.v[(k + 1) % n];
      int ep = topo.faceEdges[f][(k + n - 1) % n], en = topo.faceEdges[f][k];
      auto uvAlong = [&](int other, int newVert) -> int {
        if (!hasUv) return -1;
        int ko = static_cast<int>((other == vp) ? (k + n - 1) % n : (k + 1) % n);
        float len = length(m.positions[other] - m.positions[v]);
        float t = len > 1e-12f ? length(m.positions[newVert] - m.positions[v]) / len : 0.0f;
        return m.addUv(uvLerp(m, face.uv[k], face.uv[ko], t));
      };
      auto pushSlide = [&](int e, int other) {
        auto it = slide.find({v, e});
        if (it == slide.end()) {
          corner[k].push_back(v);
          if (hasUv) cornerUv[k].push_back(face.uv[k]);
        } else {
          corner[k].push_back(it->second);
          if (hasUv) cornerUv[k].push_back(uvAlong(other, it->second));
        }
      };
      if (vertSel.count(v)) {
        pushSlide(ep, vp);
        pushSlide(en, vn);
        continue;
      }
      bool sp = edgeSel.count(ep) > 0, sn = edgeSel.count(en) > 0;
      if (sp && sn) {
        Vec3 dp = m.positions[vp] - m.positions[v], dn = m.positions[vn] - m.positions[v];
        float lp = length(dp), ln = length(dn);
        float ap = std::min(amount, lp * 0.49f), an = std::min(amount, ln * 0.49f);
        Vec3 q = m.positions[v] + (lp > 0 ? dp / lp * ap : Vec3()) + (ln > 0 ? dn / ln * an : Vec3());
        int nv = m.addVertex(q);
        res.newVerts.push_back(nv);
        corner[k].push_back(nv);
        if (hasUv) {
          Vec2 uv = m.uvs[face.uv[k]] + (m.uvs[face.uv[(k + n - 1) % n]] - m.uvs[face.uv[k]]) * (lp > 0 ? ap / lp : 0) +
                    (m.uvs[face.uv[(k + 1) % n]] - m.uvs[face.uv[k]]) * (ln > 0 ? an / ln : 0);
          cornerUv[k].push_back(m.addUv(uv));
        }
      } else if (sn) {
        pushSlide(ep, vp);
      } else if (sp) {
        pushSlide(en, vn);
      } else {
        pushSlide(ep, vp);
        pushSlide(en, vn);
        if (corner[k].size() == 2 && corner[k][0] == corner[k][1]) {
          corner[k].pop_back();
          if (hasUv) cornerUv[k].pop_back();
        }
      }
    }
    PolyMesh::Face nfce;
    nfce.matId = face.matId;
    nfce.smoothGroups = face.smoothGroups;
    for (size_t k = 0; k < n; ++k) {
      for (size_t i = 0; i < corner[k].size(); ++i) {
        nfce.v.push_back(corner[k][i]);
        if (hasUv) nfce.uv.push_back(cornerUv[k][i]);
      }
      int e = topo.faceEdges[f][k];
      if (topo.isBoundary(e)) boundaryHalf.insert({corner[k].back(), corner[(k + 1) % n].front()});
      if (edgeSel.count(e))
        sides[e].push_back({face.v[k], face.v[(k + 1) % n], corner[k].back(), corner[(k + 1) % n].front()});
    }
    face = nfce;
  }
  if (!open) {
    // One quad per chamfered edge, between the two faces' moved copies of the edge.
    for (const auto& [e, list] : sides) {
      if (list.size() == 1) {
        // Open (border) edge: a strip between the original border and its moved copy.
        const auto& s0 = list[0];
        PolyMesh::Face q;
        q.v = std::vector<int>{s0[0], s0[1], s0[3], s0[2]};
        q.smoothGroups = 0;
        m.faces.push_back(q);
        res.newFaces.push_back(static_cast<int>(m.faces.size()) - 1);
        boundaryHalf.insert({s0[0], s0[1]});
        boundaryHalf.insert({s0[2], s0[0]});
        boundaryHalf.insert({s0[1], s0[3]});
        continue;
      }
      if (list.size() != 2) continue;
      const auto& s0 = list[0];
      const auto& s1 = list[1];
      if (s1[0] != s0[1] || s1[1] != s0[0]) continue;  // inconsistent winding
      // s0: u -> v moved to A_u -> A_v; s1: v -> u moved to B_v -> B_u.
      PolyMesh::Face q;
      q.v = std::vector<int>{s0[3], s0[2], s1[3], s1[2]};
      q.smoothGroups = 0;
      m.faces.push_back(q);
      res.newFaces.push_back(static_cast<int>(m.faces.size()) - 1);
    }
    // Fill the remaining holes (vertex chamfers and corners where several chamfered edges meet).
    std::multiset<std::pair<int, int>> he;
    for (size_t f = 0; f < m.faces.size(); ++f) {
      const auto& face = m.faces[f];
      for (size_t k = 0; k < face.v.size(); ++k) he.insert({face.v[k], face.v[(k + 1) % face.v.size()]});
    }
    std::vector<std::pair<int, int>> holes;
    for (const auto& e : he) {
      if (e.first == e.second) continue;
      if (he.count({e.second, e.first})) continue;
      if (boundaryHalf.count(e)) continue;
      holes.push_back({e.second, e.first});
    }
    for (auto& loop : chainLoops(holes)) {
      PolyMesh::Face nfce;
      nfce.v = loop;
      nfce.smoothGroups = 0;
      m.faces.push_back(nfce);
      res.newFaces.push_back(static_cast<int>(m.faces.size()) - 1);
    }
  }
  std::vector<int> faceMap;
  cleanAllFaces(m, &faceMap);
  std::vector<int> newFaces;
  for (int f : res.newFaces)
    if (faceMap[f] >= 0) newFaces.push_back(faceMap[f]);
  res.newFaces = newFaces;
  sel.edges.clear();
  sel.verts.clear();
  std::vector<int> faceSel;
  for (int f : sel.faces)
    if (f < static_cast<int>(faceMap.size()) && faceMap[f] >= 0) faceSel.push_back(faceMap[f]);
  sel.faces = faceSel;
  std::vector<int> vmap = compactVertices(m);
  std::vector<int> identity(m.faces.size());
  for (size_t i = 0; i < identity.size(); ++i) identity[i] = static_cast<int>(i);
  remapSelection(sel, vmap, identity);
  std::vector<int> nv;
  for (int v : res.newVerts)
    if (v < static_cast<int>(vmap.size()) && vmap[v] >= 0) nv.push_back(vmap[v]);
  res.newVerts = nv;
  if (uvs) ensureFaceUvs(m);
  return res;
}

std::set<int> edgeSetFromKeys(const PolyTopology& topo, const std::vector<uint64_t>& keys) {
  std::set<int> out;
  for (uint64_t k : keys) {
    int e = topo.edgeOf(edgeKeyA(k), edgeKeyB(k));
    if (e >= 0) out.insert(e);
  }
  return out;
}

// Selected edges that are shared by exactly two faces.
std::set<int> interiorEdges(const PolyTopology& topo, const std::vector<uint64_t>& keys) {
  std::set<int> out;
  for (int e : edgeSetFromKeys(topo, keys))
    if (topo.edges[e].faces.size() == 2) out.insert(e);
  return out;
}

Vec3 jsonVec3(const json& j, const char* key, const Vec3& def) {
  if (!j.contains(key)) return def;
  return vec3FromJson(j[key], def);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Public building blocks

std::vector<int> compactVertices(PolyMesh& m) {
  std::vector<int> map(m.positions.size(), -1);
  std::vector<Vec3> pos;
  for (auto& f : m.faces)
    for (int& v : f.v) {
      if (map[v] < 0) {
        map[v] = static_cast<int>(pos.size());
        pos.push_back(m.positions[v]);
      }
      v = map[v];
    }
  m.positions = std::move(pos);
  return map;
}

void remapSelection(PolySelection& sel, const std::vector<int>& vmap, const std::vector<int>& fmap) {
  auto mapV = [&](int v) { return v >= 0 && v < static_cast<int>(vmap.size()) ? vmap[v] : -1; };
  auto mapF = [&](int f) { return f >= 0 && f < static_cast<int>(fmap.size()) ? fmap[f] : -1; };
  std::vector<int> verts, faces, hf, hv;
  for (int v : sel.verts)
    if (mapV(v) >= 0) verts.push_back(mapV(v));
  for (int v : sel.hiddenVerts)
    if (mapV(v) >= 0) hv.push_back(mapV(v));
  for (int f : sel.faces)
    if (mapF(f) >= 0) faces.push_back(mapF(f));
  for (int f : sel.hiddenFaces)
    if (mapF(f) >= 0) hf.push_back(mapF(f));
  std::vector<uint64_t> edges;
  for (uint64_t k : sel.edges) {
    int a = mapV(edgeKeyA(k)), b = mapV(edgeKeyB(k));
    if (a >= 0 && b >= 0 && a != b) edges.push_back(edgeKey(a, b));
  }
  sel.verts = verts;
  sel.faces = faces;
  sel.edges = edges;
  sel.hiddenFaces = hf;
  sel.hiddenVerts = hv;
  sel.normalize();
}

int insertVertexOnEdge(PolyMesh& m, int a, int b, float t) {
  if (a == b) return -1;
  Vec3 p = lerp(m.positions[a], m.positions[b], t);
  int nv = -1;
  for (auto& f : m.faces) {
    size_t n = f.v.size();
    for (size_t k = 0; k < n; ++k) {
      int x = f.v[k], y = f.v[(k + 1) % n];
      if (!((x == a && y == b) || (x == b && y == a))) continue;
      if (nv < 0) nv = m.addVertex(p);
      float tt = x == a ? t : 1.0f - t;
      if (f.uv.size() == n) {
        int uv = m.addUv(uvLerp(m, f.uv[k], f.uv[(k + 1) % n], tt));
        f.uv.insert(f.uv.begin() + static_cast<long>(k) + 1, uv);
      }
      f.v.insert(f.v.begin() + static_cast<long>(k) + 1, nv);
      break;
    }
  }
  return nv;
}

bool splitFace(PolyMesh& m, int face, int va, int vb) {
  if (face < 0 || face >= static_cast<int>(m.faces.size())) return false;
  PolyMesh::Face f = m.faces[face];
  int n = static_cast<int>(f.v.size());
  int i = cornerOf(f, va), j = cornerOf(f, vb);
  if (i < 0 || j < 0 || i == j) return false;
  if ((i + 1) % n == j || (j + 1) % n == i) return false;
  bool hasUv = static_cast<int>(f.uv.size()) == n;
  PolyMesh::Face a, b;
  a.matId = b.matId = f.matId;
  a.smoothGroups = b.smoothGroups = f.smoothGroups;
  for (int k = i;; k = (k + 1) % n) {
    a.v.push_back(f.v[k]);
    if (hasUv) a.uv.push_back(f.uv[k]);
    if (k == j) break;
  }
  for (int k = j;; k = (k + 1) % n) {
    b.v.push_back(f.v[k]);
    if (hasUv) b.uv.push_back(f.uv[k]);
    if (k == i) break;
  }
  m.faces[face] = a;
  m.faces.push_back(b);
  return true;
}

void removeFaces(PolyMesh& m, std::vector<int> faces) {
  PolySelection dummy;
  eraseFaces(m, dummy, std::move(faces));
}

std::vector<std::vector<int>> openBorderLoops(const PolyMesh& m) {
  PolyTopology topo(m);
  std::vector<std::pair<int, int>> holes;
  for (size_t f = 0; f < m.faces.size(); ++f) {
    const auto& face = m.faces[f];
    for (size_t k = 0; k < face.v.size(); ++k)
      if (topo.isBoundary(topo.faceEdges[f][k])) holes.push_back({face.v[(k + 1) % face.v.size()], face.v[k]});
  }
  return chainLoops(holes);
}

int capAllHoles(PolyMesh& m) {
  auto loops = openBorderLoops(m);
  for (auto& loop : loops) {
    PolyMesh::Face f;
    f.v = loop;
    f.smoothGroups = 0;
    m.faces.push_back(f);
  }
  if (!loops.empty()) ensureFaceUvs(m);
  return static_cast<int>(loops.size());
}

PolyMesh extractFaces(const PolyMesh& m, const std::vector<int>& faces) {
  PolyMesh out;
  std::map<int, int> vmap, tmap;
  for (int fi : faces) {
    if (fi < 0 || fi >= static_cast<int>(m.faces.size())) continue;
    PolyMesh::Face f = m.faces[fi];
    for (int& v : f.v) {
      auto it = vmap.find(v);
      if (it == vmap.end()) it = vmap.emplace(v, out.addVertex(m.positions[v])).first;
      v = it->second;
    }
    for (int& t : f.uv) {
      auto it = tmap.find(t);
      if (it == tmap.end()) it = tmap.emplace(t, out.addUv(m.uvs[t])).first;
      t = it->second;
    }
    out.faces.push_back(f);
  }
  return out;
}

void ensureFaceUvs(PolyMesh& m) {
  if (!meshHasUvs(m)) return;
  for (auto& f : m.faces) {
    if (f.uv.size() == f.v.size()) continue;
    f.uv.clear();
    Vec3 n = vabs(normalize(m.faceNormal(f)));
    for (int v : f.v) {
      const Vec3& p = m.positions[v];
      Vec2 uv = n.x >= n.y && n.x >= n.z ? Vec2(p.y, p.z) : (n.y >= n.z ? Vec2(p.x, p.z) : Vec2(p.x, p.y));
      f.uv.push_back(m.addUv(uv * 0.01f));
    }
  }
}

std::vector<std::vector<Vec3>> edgeChains(const PolyMesh& m, const std::vector<uint64_t>& edges,
                                          std::vector<bool>* closedOut) {
  std::multimap<int, int> adj;
  for (uint64_t k : edges) {
    adj.emplace(edgeKeyA(k), edgeKeyB(k));
    adj.emplace(edgeKeyB(k), edgeKeyA(k));
  }
  std::set<uint64_t> used;
  std::vector<std::vector<Vec3>> chains;
  if (closedOut) closedOut->clear();
  auto degree = [&](int v) { return static_cast<int>(adj.count(v)); };
  auto walk = [&](int start) {
    std::vector<int> path = {start};
    int cur = start;
    for (;;) {
      int next = -1;
      auto range = adj.equal_range(cur);
      for (auto it = range.first; it != range.second; ++it)
        if (!used.count(edgeKey(cur, it->second))) {
          next = it->second;
          break;
        }
      if (next < 0) break;
      used.insert(edgeKey(cur, next));
      path.push_back(next);
      cur = next;
      if (next == start) break;
    }
    return path;
  };
  // Open chains start at endpoints; then closed loops.
  for (int pass = 0; pass < 2; ++pass)
    for (uint64_t k : edges) {
      if (used.count(k)) continue;
      int a = edgeKeyA(k);
      if (pass == 0) {
        int start = degree(a) == 1 ? a : (degree(edgeKeyB(k)) == 1 ? edgeKeyB(k) : -1);
        if (start < 0) continue;
        a = start;
      }
      std::vector<int> path = walk(a);
      if (path.size() < 2) continue;
      bool closed = path.size() > 2 && path.front() == path.back();
      if (closed) path.pop_back();
      std::vector<Vec3> pts;
      for (int v : path) pts.push_back(m.positions[v]);
      chains.push_back(pts);
      if (closedOut) closedOut->push_back(closed);
    }
  return chains;
}

void autoSmooth(PolyMesh& m, const std::vector<int>* faces, float angleDeg) {
  PolyTopology topo(m);
  size_t nf = m.faces.size();
  std::vector<char> in(nf, faces ? 0 : 1);
  if (faces)
    for (int f : *faces) in[f] = 1;
  float cosLimit = std::cos(radians(angleDeg));
  std::vector<Vec3> normals(nf);
  for (size_t f = 0; f < nf; ++f) normals[f] = faceUnitNormal(m, m.faces[f]);
  // Regions connected by smooth edges.
  std::vector<int> region(nf, -1);
  int regions = 0;
  for (size_t f0 = 0; f0 < nf; ++f0) {
    if (!in[f0] || region[f0] >= 0) continue;
    std::vector<int> stack = {static_cast<int>(f0)};
    region[f0] = regions;
    while (!stack.empty()) {
      int f = stack.back();
      stack.pop_back();
      for (int e : topo.faceEdges[f])
        for (int g : topo.edges[e].faces)
          if (in[g] && region[g] < 0 && dot(normals[f], normals[g]) >= cosLimit) {
            region[g] = regions;
            stack.push_back(g);
          }
    }
    ++regions;
  }
  // Color regions so neighbours across hard edges differ.
  std::vector<std::set<int>> adj(regions);
  for (size_t e = 0; e < topo.edges.size(); ++e) {
    const auto& fs = topo.edges[e].faces;
    for (size_t i = 0; i < fs.size(); ++i)
      for (size_t j = i + 1; j < fs.size(); ++j) {
        int a = region[fs[i]], b = region[fs[j]];
        if (a >= 0 && b >= 0 && a != b) {
          adj[a].insert(b);
          adj[b].insert(a);
        }
      }
  }
  std::vector<int> color(regions, -1);
  for (int r = 0; r < regions; ++r) {
    uint32_t usedBits = 0;
    for (int o : adj[r])
      if (color[o] >= 0) usedBits |= 1u << color[o];
    int c = 0;
    while (c < 31 && (usedBits & (1u << c))) ++c;
    color[r] = c;
  }
  for (size_t f = 0; f < nf; ++f)
    if (in[f]) m.faces[f].smoothGroups = 1u << color[region[f]];
}

std::vector<int> faceTriangles(const PolyMesh& m, const PolyMesh::Face& f) { return earClipFace(m, f); }

double meshVolume(const PolyMesh& m) {
  double v = 0;
  for (const auto& f : m.faces) {
    std::vector<int> tris = earClipFace(m, f);
    for (size_t i = 0; i + 2 < tris.size(); i += 3) {
      const Vec3 &a = m.positions[f.v[tris[i]]], &b = m.positions[f.v[tris[i + 1]]], &c = m.positions[f.v[tris[i + 2]]];
      v += dot(a, cross(b, c)) / 6.0;
    }
  }
  return v;
}

bool isClosedManifold(const PolyMesh& m) {
  std::map<std::pair<int, int>, int> directed;
  for (const auto& f : m.faces)
    for (size_t k = 0; k < f.v.size(); ++k) directed[{f.v[k], f.v[(k + 1) % f.v.size()]}]++;
  for (const auto& [e, n] : directed) {
    if (n != 1) return false;
    auto it = directed.find({e.second, e.first});
    if (it == directed.end() || it->second != 1) return false;
  }
  return !m.faces.empty();
}

int slicePlane(PolyMesh& m, const Vec3& p, const Vec3& nIn, const std::vector<int>* onlyFaces, bool split) {
  Vec3 n = normalize(nIn);
  float scale = std::max(1.0f, m.bounds().radius());
  float eps = 1e-5f * scale;
  std::vector<char> active(m.faces.size(), onlyFaces ? 0 : 1);
  if (onlyFaces)
    for (int f : *onlyFaces)
      if (f >= 0 && f < static_cast<int>(active.size())) active[f] = 1;
  // Insert vertices where active faces' edges cross the plane.
  std::set<uint64_t> crossing;
  for (size_t f = 0; f < m.faces.size(); ++f) {
    if (!active[f]) continue;
    const auto& face = m.faces[f];
    for (size_t k = 0; k < face.v.size(); ++k) {
      int a = face.v[k], b = face.v[(k + 1) % face.v.size()];
      float da = dot(m.positions[a] - p, n), db = dot(m.positions[b] - p, n);
      if ((da > eps && db < -eps) || (da < -eps && db > eps)) crossing.insert(edgeKey(a, b));
    }
  }
  std::set<int> onPlane;
  for (uint64_t k : crossing) {
    int a = edgeKeyA(k), b = edgeKeyB(k);
    float da = dot(m.positions[a] - p, n), db = dot(m.positions[b] - p, n);
    int nv = insertVertexOnEdge(m, a, b, da / (da - db));
    if (nv >= 0) onPlane.insert(nv);
  }
  for (size_t v = 0; v < m.positions.size(); ++v)
    if (std::fabs(dot(m.positions[v] - p, n)) <= eps) onPlane.insert(static_cast<int>(v));
  // Split faces between their on-plane corners.
  int cuts = 0;
  size_t originalFaces = active.size();
  std::vector<std::pair<int, int>> newEdges;
  for (size_t f = 0; f < originalFaces; ++f) {
    if (!active[f]) continue;
    std::vector<int> work = {static_cast<int>(f)};
    while (!work.empty()) {
      int fi = work.back();
      work.pop_back();
      const auto& face = m.faces[fi];
      int nn = static_cast<int>(face.v.size());
      std::vector<int> corners;
      for (int k = 0; k < nn; ++k)
        if (onPlane.count(face.v[k])) corners.push_back(k);
      // Find two on-plane corners that are not adjacent and whose chord lies inside.
      bool done = false;
      for (size_t i = 0; i < corners.size() && !done; ++i)
        for (size_t j = i + 1; j < corners.size() && !done; ++j) {
          int ci = corners[i], cj = corners[j];
          if ((ci + 1) % nn == cj || (cj + 1) % nn == ci) continue;
          // Both sides must contain vertices strictly off the plane on opposite sides.
          bool pos = false, neg = false;
          for (int k = (ci + 1) % nn; k != cj; k = (k + 1) % nn) {
            float d = dot(m.positions[face.v[k]] - p, n);
            pos |= d > eps;
            neg |= d < -eps;
          }
          if (pos == neg) continue;
          int va = face.v[ci], vb = face.v[cj];
          if (splitFace(m, fi, va, vb)) {
            ++cuts;
            newEdges.push_back({va, vb});
            work.push_back(fi);
            work.push_back(static_cast<int>(m.faces.size()) - 1);
            done = true;
          }
        }
    }
  }
  if (split && !newEdges.empty()) {
    // Separate the two sides: faces on the positive side get their own copies of plane vertices.
    std::map<int, int> copyOf;
    for (auto& face : m.faces) {
      Vec3 c = faceCenter(m, face);
      if (dot(c - p, n) <= 0) continue;
      for (int& v : face.v)
        if (onPlane.count(v)) {
          auto it = copyOf.find(v);
          if (it == copyOf.end()) it = copyOf.emplace(v, m.addVertex(m.positions[v])).first;
          v = it->second;
        }
    }
  }
  return cuts;
}

// ---------------------------------------------------------------------------------------------
// Operation runner

namespace {

bool fail(std::string* msg, const char* text) {
  if (msg) *msg = text;
  return false;
}

std::vector<int> selectedFaces(const PolyMesh& m, const PolySelection& sel) {
  std::vector<int> out;
  for (int f : sel.faces)
    if (f >= 0 && f < static_cast<int>(m.faces.size())) out.push_back(f);
  return out;
}

// Chains directed half-edges that form one open path (x -> y) into a vertex list.
std::vector<int> chainPath(const std::vector<std::pair<int, int>>& halfEdges) {
  std::map<int, int> next;
  std::set<int> targets;
  for (const auto& e : halfEdges) {
    next[e.first] = e.second;
    targets.insert(e.second);
  }
  int start = -1;
  for (const auto& e : halfEdges)
    if (!targets.count(e.first)) start = e.first;
  std::vector<int> path;
  if (start < 0) return path;
  for (int v = start, guard = 0; guard <= static_cast<int>(halfEdges.size()); ++guard) {
    path.push_back(v);
    auto it = next.find(v);
    if (it == next.end()) break;
    v = it->second;
  }
  return path;
}

bool opRemoveVerts(PolyMesh& m, PolySelection& sel, std::string* msg) {
  std::vector<int> verts = sel.verts;
  if (verts.empty()) return fail(msg, "Select vertices to remove");
  std::vector<char> dead(m.faces.size(), 0);
  int removed = 0;
  for (int v : verts) {
    if (v < 0 || v >= static_cast<int>(m.positions.size())) continue;
    std::vector<int> fan;
    for (size_t f = 0; f < m.faces.size(); ++f)
      if (!dead[f] && cornerOf(m.faces[f], v) >= 0) fan.push_back(static_cast<int>(f));
    if (fan.empty()) continue;
    // Half-edges of the fan that don't touch v and aren't shared inside the fan form the
    // outline of the merged polygon.
    std::multiset<std::pair<int, int>> he;
    std::map<std::pair<int, int>, int> heUv;
    bool uv = true;
    for (int f : fan) {
      const auto& face = m.faces[f];
      size_t n = face.v.size();
      uv = uv && face.uv.size() == n;
      for (size_t k = 0; k < n; ++k) {
        he.insert({face.v[k], face.v[(k + 1) % n]});
        if (face.uv.size() == n) heUv[{face.v[k], face.v[(k + 1) % n]}] = face.uv[k];
      }
    }
    std::vector<std::pair<int, int>> outer;
    for (const auto& e : he) {
      if (e.first == v || e.second == v || he.count({e.second, e.first})) continue;
      outer.push_back(e);
    }
    PolyMesh::Face merged;
    merged.matId = m.faces[fan[0]].matId;
    merged.smoothGroups = m.faces[fan[0]].smoothGroups;
    auto loops = chainLoops(outer);
    if (loops.size() == 1 && loops[0].size() == outer.size()) merged.v = loops[0];
    else if (loops.empty()) merged.v = chainPath(outer);  // v was on an open border
    if (uv && merged.v.size() >= 3) {
      for (size_t k = 0; k < merged.v.size(); ++k) {
        auto it = heUv.find({merged.v[k], merged.v[(k + 1) % merged.v.size()]});
        if (it == heUv.end()) {
          merged.uv.clear();
          break;
        }
        merged.uv.push_back(it->second);
      }
    }
    for (int f : fan) dead[f] = 1;
    if (merged.v.size() >= 3) {
      m.faces.push_back(merged);
      dead.push_back(0);
    }
    ++removed;
  }
  if (!removed) return fail(msg, "Selected vertices are not used by any polygon");
  std::vector<int> erase;
  for (size_t f = 0; f < dead.size(); ++f)
    if (dead[f]) erase.push_back(static_cast<int>(f));
  sel.verts.clear();
  eraseFaces(m, sel, erase);
  ensureFaceUvs(m);
  return true;
}

bool opBreakVerts(PolyMesh& m, PolySelection& sel, std::string* msg) {
  if (sel.verts.empty()) return fail(msg, "Select vertices to break");
  PolyTopology topo(m);
  std::vector<int> created;
  for (int v : sel.verts) {
    if (v >= static_cast<int>(topo.vertFaces.size())) continue;
    const auto& fan = topo.vertFaces[v];
    for (size_t i = 1; i < fan.size(); ++i) {
      int nv = m.addVertex(m.positions[v]);
      created.push_back(nv);
      for (int& c : m.faces[fan[i]].v)
        if (c == v) c = nv;
    }
  }
  sel.verts.insert(sel.verts.end(), created.begin(), created.end());
  sel.normalize();
  return true;
}

bool opWeld(PolyMesh& m, PolySelection& sel, const std::vector<int>& verts, float threshold, std::string* msg) {
  if (verts.size() < 2) return fail(msg, "Select at least two vertices to weld");
  std::vector<int> target(m.positions.size());
  for (size_t i = 0; i < target.size(); ++i) target[i] = static_cast<int>(i);
  std::vector<int> group(verts.size(), -1);
  int groups = 0;
  for (size_t i = 0; i < verts.size(); ++i) {
    if (group[i] >= 0) continue;
    group[i] = groups;
    std::vector<size_t> stack = {i};
    while (!stack.empty()) {
      size_t a = stack.back();
      stack.pop_back();
      for (size_t j = 0; j < verts.size(); ++j)
        if (group[j] < 0 && length(m.positions[verts[a]] - m.positions[verts[j]]) <= threshold) {
          group[j] = groups;
          stack.push_back(j);
        }
    }
    ++groups;
  }
  bool any = false;
  for (int g = 0; g < groups; ++g) {
    std::vector<int> members;
    for (size_t i = 0; i < verts.size(); ++i)
      if (group[i] == g) members.push_back(verts[i]);
    if (members.size() < 2) continue;
    any = true;
    Vec3 c;
    for (int v : members) c += m.positions[v];
    m.positions[members[0]] = c / static_cast<float>(members.size());
    for (int v : members) target[v] = members[0];
  }
  if (!any) return fail(msg, "No vertices within the weld threshold");
  mergeVerticesInto(m, sel, target);
  return true;
}

bool opTargetWeld(PolyMesh& m, PolySelection& sel, int from, int to, std::string* msg) {
  int n = static_cast<int>(m.positions.size());
  if (from < 0 || to < 0 || from >= n || to >= n || from == to) return fail(msg, "Pick two different vertices");
  std::vector<int> target(n);
  for (int i = 0; i < n; ++i) target[i] = i;
  target[from] = to;
  mergeVerticesInto(m, sel, target);
  return true;
}

bool opCollapse(PolyMesh& m, PolySelection& sel, std::string* msg) {
  std::vector<int> verts = selectedVertsAtLevel(m, sel);
  if (verts.empty()) return fail(msg, "Nothing selected to collapse");
  PolyTopology topo(m);
  std::vector<char> in(m.positions.size(), 0);
  for (int v : verts) in[v] = 1;
  std::vector<int> target(m.positions.size());
  for (size_t i = 0; i < target.size(); ++i) target[i] = static_cast<int>(i);
  std::vector<char> seen(m.positions.size(), 0);
  for (int v0 : verts) {
    if (seen[v0]) continue;
    std::vector<int> group, stack = {v0};
    seen[v0] = 1;
    while (!stack.empty()) {
      int v = stack.back();
      stack.pop_back();
      group.push_back(v);
      for (int e : topo.vertEdges[v]) {
        int o = topo.otherVertex(e, v);
        if (in[o] && !seen[o]) {
          seen[o] = 1;
          stack.push_back(o);
        }
      }
    }
    Vec3 c;
    for (int v : group) c += m.positions[v];
    m.positions[group[0]] = c / static_cast<float>(group.size());
    for (int v : group) target[v] = group[0];
  }
  sel.faces.clear();
  sel.edges.clear();
  mergeVerticesInto(m, sel, target);
  return true;
}

bool opRemoveEdges(PolyMesh& m, PolySelection& sel, bool removeVerts, std::string* msg) {
  if (sel.edges.empty()) return fail(msg, "Select edges to remove");
  std::set<int> endpoints;
  int merged = 0;
  for (uint64_t k : sel.edges) {
    int a = edgeKeyA(k), b = edgeKeyB(k);
    // Find the two faces using this edge in the current mesh.
    int fa = -1, fb = -1, ka = -1, kb = -1;
    for (size_t f = 0; f < m.faces.size(); ++f) {
      const auto& face = m.faces[f];
      size_t n = face.v.size();
      for (size_t c = 0; c < n; ++c) {
        int x = face.v[c], y = face.v[(c + 1) % n];
        if (x == a && y == b) { fa = static_cast<int>(f); ka = static_cast<int>(c); }
        if (x == b && y == a) { fb = static_cast<int>(f); kb = static_cast<int>(c); }
      }
    }
    if (fa < 0 || fb < 0 || fa == fb) continue;
    const auto& A = m.faces[fa];
    const auto& B = m.faces[fb];
    int na = static_cast<int>(A.v.size()), nb = static_cast<int>(B.v.size());
    bool uv = static_cast<int>(A.uv.size()) == na && static_cast<int>(B.uv.size()) == nb;
    PolyMesh::Face out;
    out.matId = A.matId;
    out.smoothGroups = A.smoothGroups;
    // A from b (index ka+1) around to a (index ka).
    for (int i = 0; i < na; ++i) {
      int c = (ka + 1 + i) % na;
      out.v.push_back(A.v[c]);
      if (uv) out.uv.push_back(A.uv[c]);
    }
    // B from after a (B has b->a at kb, so a is at kb+1) to before b.
    for (int i = 2; i < nb; ++i) {
      int c = (kb + i) % nb;
      out.v.push_back(B.v[c]);
      if (uv) out.uv.push_back(B.uv[c]);
    }
    m.faces[fa] = out;
    m.faces.erase(m.faces.begin() + fb);
    endpoints.insert(a);
    endpoints.insert(b);
    ++merged;
  }
  if (!merged) return fail(msg, "Only edges between two polygons can be removed");
  std::vector<int> faceMap;
  cleanAllFaces(m, &faceMap);
  if (removeVerts) {
    PolyTopology topo(m);
    for (int v : endpoints) {
      if (v >= static_cast<int>(topo.vertEdges.size()) || topo.vertEdges[v].size() != 2) continue;
      for (auto& f : m.faces) {
        int c = cornerOf(f, v);
        if (c < 0 || f.v.size() <= 3) continue;
        f.v.erase(f.v.begin() + c);
        if (!f.uv.empty()) f.uv.erase(f.uv.begin() + c);
      }
    }
  }
  sel.edges.clear();
  sel.faces.clear();
  std::vector<int> vmap = compactVertices(m);
  remapSelection(sel, vmap, {});
  return true;
}

bool opSplitEdges(PolyMesh& m, PolySelection& sel, std::string* msg) {
  if (sel.edges.empty()) return fail(msg, "Select edges to split");
  PolyTopology topo(m);
  std::set<int> cut = edgeSetFromKeys(topo, sel.edges);
  std::set<int> verts;
  for (int e : cut) {
    verts.insert(topo.edges[e].a);
    verts.insert(topo.edges[e].b);
  }
  for (int v : verts) {
    // Group the faces around v by connectivity through non-cut edges.
    const auto& fan = topo.vertFaces[v];
    std::map<int, int> group;
    int groups = 0;
    for (int f0 : fan) {
      if (group.count(f0)) continue;
      group[f0] = groups;
      std::vector<int> stack = {f0};
      while (!stack.empty()) {
        int f = stack.back();
        stack.pop_back();
        for (int e : topo.faceEdges[f]) {
          if (cut.count(e)) continue;
          if (topo.edges[e].a != v && topo.edges[e].b != v) continue;
          for (int g : topo.edges[e].faces)
            if (!group.count(g) && std::find(fan.begin(), fan.end(), g) != fan.end()) {
              group[g] = groups;
              stack.push_back(g);
            }
        }
      }
      ++groups;
    }
    if (groups < 2) continue;
    std::vector<int> copies(groups, v);
    for (int g = 1; g < groups; ++g) copies[g] = m.addVertex(m.positions[v]);
    for (int f : fan)
      for (int& c : m.faces[f].v)
        if (c == v) c = copies[group[f]];
  }
  return true;
}

bool opChamferVerts(PolyMesh& m, PolySelection& sel, float amount, bool open, std::string* msg) {
  if (sel.verts.empty()) return fail(msg, "Select vertices to chamfer");
  if (amount <= 0) return fail(msg, "Chamfer amount must be positive");
  std::set<int> vs(sel.verts.begin(), sel.verts.end());
  ChamferResult r = chamferCore(m, sel, vs, {}, amount, open);
  sel.verts = r.newVerts;
  sel.normalize();
  return true;
}

bool opChamferEdges(PolyMesh& m, PolySelection& sel, float amount, bool open, std::string* msg) {
  if (sel.edges.empty()) return fail(msg, "Select edges to chamfer");
  if (amount <= 0) return fail(msg, "Chamfer amount must be positive");
  PolyTopology topo(m);
  std::set<int> es = edgeSetFromKeys(topo, sel.edges);
  if (es.empty()) return fail(msg, "Select edges to chamfer");
  ChamferResult r = chamferCore(m, sel, {}, es, amount, open);
  // The new chamfer polygons stay selected for follow-up polygon tools.
  sel.faces = r.newFaces;
  sel.normalize();
  return true;
}

bool opExtrudeRegions(PolyMesh& m, PolySelection& sel, const std::vector<int>& faces, float height, int mode,
                      float outline, std::vector<int>* sidesOut = nullptr) {
  if (faces.empty()) return false;
  bool uvs = meshHasUvs(m);
  std::vector<std::vector<int>> regions;
  {
    PolyTopology topo(m);
    regions = facesToRegions(m, topo, faces, mode == 2);
  }
  for (const auto& region : regions) {
    PolyTopology topo(m);
    // Normals before lifting.
    Vec3 groupN;
    std::map<int, Vec3> vertN;
    for (int f : region) {
      Vec3 fn = m.faceNormal(m.faces[f]);
      groupN += fn;
      for (int v : m.faces[f].v) vertN[v] += normalize(fn);
    }
    groupN = normalize(groupN);
    std::vector<int> sides;
    std::map<int, int> newv = liftRegion(m, topo, region, &sides);
    if (sidesOut) sidesOut->insert(sidesOut->end(), sides.begin(), sides.end());
    for (const auto& [oldV, nv] : newv) {
      Vec3 dir = mode == 0 ? groupN : normalize(vertN[oldV]);
      m.positions[nv] += dir * height;
    }
    if (outline != 0.0f) {
      PolyTopology t2(m);
      auto boundary = regionBoundary(m, t2, region);
      auto in = insetOffsets(m, region, boundary);
      for (const auto& [v, d] : in) m.positions[v] -= d * outline;
    }
  }
  if (uvs) ensureFaceUvs(m);
  sel.faces = faces;
  sel.normalize();
  return true;
}

bool opInset(PolyMesh& m, PolySelection& sel, float amount, bool byPolygon, std::string* msg) {
  std::vector<int> faces = selectedFaces(m, sel);
  if (faces.empty()) return fail(msg, "Select polygons to inset");
  bool uvs = meshHasUvs(m);
  std::vector<std::vector<int>> regions;
  {
    PolyTopology topo(m);
    regions = facesToRegions(m, topo, faces, byPolygon);
  }
  for (const auto& region : regions) {
    PolyTopology topo(m);
    auto boundary = regionBoundary(m, topo, region);
    auto inward = insetOffsets(m, region, boundary);
    std::map<int, int> newv = liftRegion(m, topo, region, nullptr);
    for (const auto& [oldV, d] : inward) {
      int nv = newv[oldV];
      m.positions[nv] += d * amount;
    }
  }
  if (uvs) ensureFaceUvs(m);
  sel.faces = faces;
  sel.normalize();
  return true;
}

bool opOutline(PolyMesh& m, PolySelection& sel, float amount, std::string* msg) {
  std::vector<int> faces = selectedFaces(m, sel);
  if (faces.empty()) return fail(msg, "Select polygons to outline");
  PolyTopology topo(m);
  auto regions = facesToRegions(m, topo, faces, false);
  for (const auto& region : regions) {
    auto boundary = regionBoundary(m, topo, region);
    auto inward = insetOffsets(m, region, boundary);
    for (const auto& [v, d] : inward) m.positions[v] -= d * amount;
  }
  return true;
}

bool opBridgeLoops(PolyMesh& m, std::vector<int> A, std::vector<int> Brev, int segments) {
  // A in "virtual face" order; Brev is the other loop reversed. Align and connect.
  size_t n = A.size();
  if (n < 2 || Brev.size() != n) return false;
  float best = kInfinity;
  size_t shift = 0;
  for (size_t s = 0; s < n; ++s) {
    float d = 0;
    for (size_t i = 0; i < n; ++i) d += length(m.positions[A[i]] - m.positions[Brev[(i + s) % n]]);
    if (d < best) {
      best = d;
      shift = s;
    }
  }
  std::vector<int> B(n);
  for (size_t i = 0; i < n; ++i) B[i] = Brev[(i + shift) % n];
  segments = std::max(1, segments);
  std::vector<std::vector<int>> rings = {A};
  for (int s = 1; s < segments; ++s) {
    float t = static_cast<float>(s) / segments;
    std::vector<int> ring(n);
    for (size_t i = 0; i < n; ++i) ring[i] = m.addVertex(lerp(m.positions[A[i]], m.positions[B[i]], t));
    rings.push_back(ring);
  }
  rings.push_back(B);
  bool closedLoop = n > 2;
  size_t count = closedLoop ? n : n - 1;
  for (size_t r = 0; r + 1 < rings.size(); ++r)
    for (size_t i = 0; i < count; ++i) {
      size_t j = (i + 1) % n;
      PolyMesh::Face f;
      f.v = std::vector<int>{rings[r][i], rings[r][j], rings[r + 1][j], rings[r + 1][i]};
      f.smoothGroups = 0;
      m.faces.push_back(f);
    }
  return true;
}

// Bridging needs the vertex indices to survive face removal, so faces are removed before
// vertices are compacted.
bool bridgeFacesImpl(PolyMesh& m, PolySelection& sel, int segments, std::string* msg) {
  std::vector<int> faces = selectedFaces(m, sel);
  if (faces.size() != 2) return fail(msg, "Select exactly two polygons to bridge");
  std::vector<int> A = m.faces[faces[0]].v, B = m.faces[faces[1]].v;
  if (A.size() != B.size()) return fail(msg, "Bridged polygons need the same number of sides");
  std::reverse(B.begin(), B.end());
  bool uvs = meshHasUvs(m);
  std::sort(faces.begin(), faces.end());
  m.faces.erase(m.faces.begin() + faces[1]);
  m.faces.erase(m.faces.begin() + faces[0]);
  opBridgeLoops(m, A, B, segments);
  sel.faces.clear();
  std::vector<int> vmap = compactVertices(m);
  remapSelection(sel, vmap, {});
  if (uvs) ensureFaceUvs(m);
  return true;
}

bool opBridgeBorders(PolyMesh& m, PolySelection& sel, int segments, std::string* msg) {
  auto loops = openBorderLoops(m);
  std::vector<std::vector<int>> picked;
  for (auto& loop : loops) {
    bool any = sel.edges.empty();
    for (size_t i = 0; i < loop.size() && !any; ++i) any = sel.hasEdge(edgeKey(loop[i], loop[(i + 1) % loop.size()]));
    if (any) picked.push_back(loop);
  }
  if (picked.size() != 2) return fail(msg, "Select edges on exactly two borders to bridge");
  if (picked[0].size() != picked[1].size()) return fail(msg, "Bridged borders need the same number of edges");
  std::vector<int> B = picked[1];
  std::reverse(B.begin(), B.end());
  bool uvs = meshHasUvs(m);
  opBridgeLoops(m, picked[0], B, segments);
  if (uvs) ensureFaceUvs(m);
  sel.edges.clear();
  return true;
}

bool opBridgeEdges(PolyMesh& m, PolySelection& sel, int segments, std::string* msg) {
  if (sel.edges.size() != 2) return fail(msg, "Select exactly two open edges to bridge");
  // Orient each edge as used by its (single) face.
  std::vector<std::pair<int, int>> dir;
  for (uint64_t k : sel.edges) {
    int a = edgeKeyA(k), b = edgeKeyB(k);
    bool found = false;
    for (const auto& f : m.faces) {
      for (size_t c = 0; c < f.v.size(); ++c) {
        int x = f.v[c], y = f.v[(c + 1) % f.v.size()];
        if ((x == a && y == b) || (x == b && y == a)) {
          dir.push_back({x, y});
          found = true;
          break;
        }
      }
      if (found) break;
    }
  }
  if (dir.size() != 2) return fail(msg, "Bridged edges must be open edges");
  // Quad [b, a, d, c] contains b->a and d->c (opposite to their faces).
  int a = dir[0].first, b = dir[0].second, c = dir[1].first, d = dir[1].second;
  segments = std::max(1, segments);
  std::vector<int> left = {b}, right = {a};
  for (int s = 1; s < segments; ++s) {
    float t = static_cast<float>(s) / segments;
    left.push_back(m.addVertex(lerp(m.positions[b], m.positions[c], t)));
    right.push_back(m.addVertex(lerp(m.positions[a], m.positions[d], t)));
  }
  left.push_back(c);
  right.push_back(d);
  for (int s = 0; s < segments; ++s) {
    PolyMesh::Face f;
    f.v = std::vector<int>{left[s], right[s], right[s + 1], left[s + 1]};
    f.smoothGroups = 0;
    m.faces.push_back(f);
  }
  ensureFaceUvs(m);
  sel.edges.clear();
  return true;
}

bool opConnectEdges(PolyMesh& m, PolySelection& sel, int segments, float pinch, float slide, std::string* msg) {
  if (sel.edges.empty()) return fail(msg, "Select edges to connect");
  segments = std::max(1, segments);
  // Insert points on each selected edge, ordered from the edge's lower to higher vertex index.
  std::map<uint64_t, std::vector<int>> pts;
  for (uint64_t k : sel.edges) {
    int a = edgeKeyA(k), b = edgeKeyB(k);
    std::vector<int> list;
    int cur = a;
    float done = 0.0f;
    for (int s = 1; s <= segments; ++s) {
      float t = static_cast<float>(s) / (segments + 1);
      // Pinch pulls points together around the middle; slide shifts them along the edge.
      t = 0.5f + (t - 0.5f) * (1.0f - pinch * 0.01f) + slide * 0.005f;
      t = clampf(t, 0.01f, 0.99f);
      float local = (t - done) / (1.0f - done);
      int nv = insertVertexOnEdge(m, cur, b, local);
      if (nv < 0) break;
      list.push_back(nv);
      cur = nv;
      done = t;
    }
    pts[k] = list;
  }
  std::vector<uint64_t> newEdges;
  // For each face, connect inserted point runs of its selected edges in pairs.
  size_t nf = m.faces.size();
  for (size_t f = 0; f < nf; ++f) {
    std::vector<std::vector<int>> runs;
    {
      const auto& face = m.faces[f];
      for (const auto& [k, list] : pts) {
        if (list.empty()) continue;
        if (cornerOf(face, list[0]) < 0) continue;
        // Order the run along the face winding.
        std::vector<int> run = list;
        if (run.size() > 1) {
          int c0 = cornerOf(face, run[0]), c1 = cornerOf(face, run[1]);
          if ((c0 + 1) % static_cast<int>(face.v.size()) != c1) std::reverse(run.begin(), run.end());
        }
        runs.push_back(run);
      }
    }
    if (runs.size() < 2) continue;
    // Sort runs by position along the face winding.
    std::sort(runs.begin(), runs.end(), [&](const std::vector<int>& x, const std::vector<int>& y) {
      return cornerOf(m.faces[f], x[0]) < cornerOf(m.faces[f], y[0]);
    });
    for (size_t r = 0; r + 1 < runs.size(); r += 2) {
      const auto& p = runs[r];
      const auto& q = runs[r + 1];
      size_t cnt = std::min(p.size(), q.size());
      for (size_t i = 0; i < cnt; ++i) {
        int va = p[i], vb = q[q.size() - 1 - i];
        // The face containing both may have changed after earlier splits.
        for (size_t g = 0; g < m.faces.size(); ++g)
          if (cornerOf(m.faces[g], va) >= 0 && cornerOf(m.faces[g], vb) >= 0) {
            if (splitFace(m, static_cast<int>(g), va, vb)) newEdges.push_back(edgeKey(va, vb));
            break;
          }
      }
    }
  }
  sel.edges = newEdges;
  sel.normalize();
  return true;
}

bool opConnectVerts(PolyMesh& m, PolySelection& sel, std::string* msg) {
  if (sel.verts.size() < 2) return fail(msg, "Select vertices that share a polygon");
  std::vector<uint64_t> made;
  bool any = true;
  while (any) {
    any = false;
    for (size_t f = 0; f < m.faces.size() && !any; ++f) {
      const auto& face = m.faces[f];
      std::vector<int> corners;
      for (size_t k = 0; k < face.v.size(); ++k)
        if (sel.hasVert(face.v[k])) corners.push_back(static_cast<int>(k));
      int n = static_cast<int>(face.v.size());
      for (size_t i = 0; i < corners.size() && !any; ++i)
        for (size_t j = i + 1; j < corners.size() && !any; ++j) {
          int ci = corners[i], cj = corners[j];
          if ((ci + 1) % n == cj || (cj + 1) % n == ci) continue;
          int va = face.v[ci], vb = face.v[cj];
          if (splitFace(m, static_cast<int>(f), va, vb)) {
            made.push_back(edgeKey(va, vb));
            any = true;
          }
        }
    }
  }
  if (made.empty()) return fail(msg, "Selected vertices share no polygon");
  return true;
}

bool opExtrudeBorders(PolyMesh& m, PolySelection& sel, float height, float width, std::string* msg) {
  PolyTopology topo(m);
  auto loops = openBorderLoops(m);
  bool any = false;
  std::vector<uint64_t> newSel;
  for (auto& loop : loops) {
    bool use = false;
    for (size_t i = 0; i < loop.size(); ++i) use |= sel.hasEdge(edgeKey(loop[i], loop[(i + 1) % loop.size()]));
    if (!use) continue;
    any = true;
    // Average normal of the faces along the border.
    Vec3 n;
    Vec3 center;
    for (int v : loop) {
      center += m.positions[v];
      for (int f : topo.vertFaces[v]) n += m.faceNormal(m.faces[f]);
    }
    n = normalize(n);
    center /= static_cast<float>(loop.size());
    std::vector<int> copy(loop.size());
    for (size_t i = 0; i < loop.size(); ++i) {
      Vec3 p = m.positions[loop[i]];
      Vec3 out = p - center;
      out -= n * dot(out, n);
      Vec3 dir = length2(out) > 1e-12f ? normalize(out) : Vec3();
      copy[i] = m.addVertex(p + n * height + dir * width);
    }
    // The hole runs v -> u where the border face has u -> v; the side quad [v, u, u', v'] closes it.
    for (size_t i = 0; i < loop.size(); ++i) {
      int v = loop[i], u = loop[(i + 1) % loop.size()];
      int vv = copy[i], uu = copy[(i + 1) % loop.size()];
      PolyMesh::Face f;
      f.v = std::vector<int>{v, u, uu, vv};
      f.smoothGroups = 0;
      m.faces.push_back(f);
      newSel.push_back(edgeKey(vv, uu));
    }
  }
  if (!any) return fail(msg, "Select border edges to extrude");
  ensureFaceUvs(m);
  sel.edges = newSel;
  sel.normalize();
  return true;
}

bool opCapBorders(PolyMesh& m, PolySelection& sel, std::string* msg) {
  auto loops = openBorderLoops(m);
  int capped = 0;
  for (auto& loop : loops) {
    bool use = sel.edges.empty();
    for (size_t i = 0; i < loop.size() && !use; ++i) use = sel.hasEdge(edgeKey(loop[i], loop[(i + 1) % loop.size()]));
    if (!use) continue;
    PolyMesh::Face f;
    f.v = loop;
    f.smoothGroups = 0;
    m.faces.push_back(f);
    ++capped;
  }
  if (!capped) return fail(msg, "No open borders selected");
  ensureFaceUvs(m);
  sel.edges.clear();
  return true;
}

bool opHinge(PolyMesh& m, PolySelection& sel, float angleDeg, int segments, int ha, int hb, std::string* msg) {
  std::vector<int> faces = selectedFaces(m, sel);
  if (faces.empty()) return fail(msg, "Select polygons to hinge");
  segments = std::max(1, segments);
  for (int f : faces) {
    int a = ha, b = hb;
    const auto& face = m.faces[f];
    if (a < 0 || b < 0 || cornerOf(face, a) < 0 || cornerOf(face, b) < 0) {
      a = face.v[0];
      b = face.v[1];
    }
    Vec3 pa = m.positions[a], pb = m.positions[b];
    Vec3 axis = normalize(pb - pa);
    std::vector<int> current = {f};
    for (int s = 1; s <= segments; ++s) {
      PolySelection tmp;
      tmp.faces = current;
      opExtrudeRegions(m, tmp, current, 0.0f, 2, 0.0f);
      // Rotate the moved polygon's vertices about the hinge.
      Mat4 r = Mat4::translate(pa) * Mat4::rotateAxis(axis, radians(angleDeg) / segments) * Mat4::translate(-pa);
      for (int& v : m.faces[f].v) m.positions[v] = r.transformPoint(m.positions[v]);
      // The hinge edge stays attached: weld its moved copies back.
      std::vector<int> target(m.positions.size());
      for (size_t i = 0; i < target.size(); ++i) target[i] = static_cast<int>(i);
      for (int v : m.faces[f].v) {
        if (length(m.positions[v] - pa) < 1e-4f) target[v] = a;
        else if (length(m.positions[v] - pb) < 1e-4f) target[v] = b;
      }
      for (auto& fc : m.faces)
        for (int& v : fc.v) v = target[v];
      std::vector<int> fmap;
      cleanAllFaces(m, &fmap);
      f = fmap[f];
      if (f < 0) return fail(msg, "Hinge collapsed the polygon");
      current = std::vector<int>{f};
    }
  }
  std::vector<int> vmap = compactVertices(m);
  remapSelection(sel, vmap, {});
  sel.faces.clear();
  ensureFaceUvs(m);
  return true;
}

bool opTriangulate(PolyMesh& m, PolySelection& sel, std::string* msg) {
  std::vector<int> faces = selectedFaces(m, sel);
  if (faces.empty()) {
    for (size_t f = 0; f < m.faces.size(); ++f) faces.push_back(static_cast<int>(f));
  }
  std::vector<int> newSel;
  for (int f : faces) {
    PolyMesh::Face face = m.faces[f];
    if (face.v.size() <= 3) {
      newSel.push_back(f);
      continue;
    }
    std::vector<int> tris = earClipFace(m, face);
    bool hasUv = face.uv.size() == face.v.size();
    for (size_t t = 0; t + 2 < tris.size(); t += 3) {
      PolyMesh::Face nf;
      nf.matId = face.matId;
      nf.smoothGroups = face.smoothGroups;
      for (int c = 0; c < 3; ++c) {
        nf.v.push_back(face.v[tris[t + c]]);
        if (hasUv) nf.uv.push_back(face.uv[tris[t + c]]);
      }
      if (t == 0) {
        m.faces[f] = nf;
        newSel.push_back(f);
      } else {
        m.faces.push_back(nf);
        newSel.push_back(static_cast<int>(m.faces.size()) - 1);
      }
    }
  }
  sel.faces = newSel;
  sel.normalize();
  return true;
}

bool opTessellate(PolyMesh& m, PolySelection& sel, std::string* msg) {
  std::vector<int> faces = selectedFaces(m, sel);
  if (faces.empty())
    for (size_t f = 0; f < m.faces.size(); ++f) faces.push_back(static_cast<int>(f));
  // Midpoints on all edges of the faces.
  std::set<uint64_t> edges;
  for (int f : faces) {
    const auto& face = m.faces[f];
    for (size_t k = 0; k < face.v.size(); ++k) edges.insert(edgeKey(face.v[k], face.v[(k + 1) % face.v.size()]));
  }
  std::set<int> mids;
  for (uint64_t k : edges) {
    int nv = insertVertexOnEdge(m, edgeKeyA(k), edgeKeyB(k), 0.5f);
    if (nv >= 0) mids.insert(nv);
  }
  std::vector<int> newSel;
  for (int f : faces) {
    PolyMesh::Face face = m.faces[f];
    size_t n = face.v.size();
    bool hasUv = face.uv.size() == n;
    Vec3 c = faceCenter(m, face);
    int cv = m.addVertex(c);
    int cuv = -1;
    if (hasUv) {
      Vec2 u;
      for (int t : face.uv) u += m.uvs[t];
      cuv = m.addUv(u / static_cast<float>(n));
    }
    bool first = true;
    for (size_t k = 0; k < n; ++k) {
      if (mids.count(face.v[k])) continue;  // corners only
      size_t kp = (k + n - 1) % n, kn = (k + 1) % n;
      PolyMesh::Face q;
      q.matId = face.matId;
      q.smoothGroups = face.smoothGroups;
      q.v = std::vector<int>{face.v[k], face.v[kn], cv, face.v[kp]};
      if (hasUv) q.uv = std::vector<int>{face.uv[k], face.uv[kn], cuv, face.uv[kp]};
      if (first) {
        m.faces[f] = q;
        newSel.push_back(f);
        first = false;
      } else {
        m.faces.push_back(q);
        newSel.push_back(static_cast<int>(m.faces.size()) - 1);
      }
    }
  }
  sel.faces = newSel;
  sel.normalize();
  return true;
}

bool opMakePlanar(PolyMesh& m, PolySelection& sel, int axis, const json& op, std::string* msg) {
  std::vector<int> verts = selectedVertsAtLevel(m, sel);
  if (verts.size() < 3 && axis < 0 && !op.contains("normal")) return fail(msg, "Select at least three vertices");
  if (verts.empty()) return fail(msg, "Nothing selected");
  Vec3 c;
  for (int v : verts) c += m.positions[v];
  c /= static_cast<float>(verts.size());
  if (op.contains("point")) c = jsonVec3(op, "point", c);
  Vec3 n;
  if (op.contains("normal")) {
    n = normalize(jsonVec3(op, "normal", Vec3(0, 0, 1)));
  } else if (axis >= 0) {
    n[axis] = 1.0f;
  } else {
    for (int f : facesFromSelection(m, sel)) n += m.faceNormal(m.faces[f]);
    if (length2(n) < 1e-20f) n = Vec3(0, 0, 1);
    n = normalize(n);
  }
  for (int v : verts) m.positions[v] -= n * dot(m.positions[v] - c, n);
  return true;
}

bool opRelax(PolyMesh& m, PolySelection& sel, float amount, int iterations, bool keepBoundary, std::string* msg) {
  std::vector<int> verts = selectedVertsAtLevel(m, sel);
  if (verts.empty())
    for (size_t v = 0; v < m.positions.size(); ++v) verts.push_back(static_cast<int>(v));
  PolyTopology topo(m);
  for (int it = 0; it < std::max(1, iterations); ++it) {
    std::vector<Vec3> next = m.positions;
    for (int v : verts) {
      const auto& es = topo.vertEdges[v];
      if (es.empty()) continue;
      bool boundary = false;
      for (int e : es) boundary |= topo.isBoundary(e);
      if (boundary && keepBoundary) continue;
      Vec3 avg;
      for (int e : es) avg += m.positions[topo.otherVertex(e, v)];
      avg /= static_cast<float>(es.size());
      next[v] = lerp(m.positions[v], avg, amount);
    }
    m.positions = next;
  }
  return true;
}

bool opInsertLoop(PolyMesh& m, PolySelection& sel, int a, int b, float t, std::string* msg) {
  PolyTopology topo(m);
  int e0 = topo.edgeOf(a, b);
  if (e0 < 0) return fail(msg, "Pick an edge for the new loop");
  // Walk the quad ring in both directions, tracking a consistent orientation.
  struct RingEdge {
    int from, to;
  };
  std::vector<RingEdge> ring = {{a, b}};
  std::set<int> seenEdges = {e0};
  std::set<int> faces;
  for (int dir = 0; dir < 2; ++dir) {
    RingEdge cur = {a, b};
    int e = e0;
    int lastFace = -1;
    for (size_t guard = 0; guard < topo.edges.size(); ++guard) {
      int f = -1;
      for (int g : topo.edges[e].faces)
        if (g != lastFace && topo.faceEdges[g].size() == 4) {
          if (dir == 1 && lastFace < 0 && !topo.edges[e].faces.empty() && g == topo.edges[e].faces[0] &&
              topo.edges[e].faces.size() > 1)
            continue;
          f = g;
          break;
        }
      if (dir == 0 && lastFace < 0 && !topo.edges[e].faces.empty()) f = topo.edges[e].faces[0];
      if (f < 0 || topo.faceEdges[f].size() != 4) break;
      faces.insert(f);
      const auto& fv = m.faces[f].v;
      int k = -1;
      for (int i = 0; i < 4; ++i)
        if (topo.faceEdges[f][i] == e) k = i;
      if (k < 0) break;
      RingEdge next;
      if (fv[k] == cur.from) next = {fv[(k + 3) % 4], fv[(k + 2) % 4]};
      else next = {fv[(k + 2) % 4], fv[(k + 3) % 4]};
      int ne = topo.faceEdges[f][(k + 2) % 4];
      lastFace = f;
      if (seenEdges.count(ne)) break;
      seenEdges.insert(ne);
      ring.push_back(next);
      cur = next;
      e = ne;
    }
  }
  // Insert the points, then split each ring face between its two new points.
  std::map<uint64_t, int> point;
  for (const auto& re : ring) {
    int nv = insertVertexOnEdge(m, re.from, re.to, t);
    if (nv >= 0) point[edgeKey(re.from, re.to)] = nv;
  }
  std::vector<uint64_t> newEdges;
  std::vector<int> pts;
  for (const auto& [k, v] : point) pts.push_back(v);
  bool any = true;
  std::set<std::pair<int, int>> done;
  while (any) {
    any = false;
    for (size_t f = 0; f < m.faces.size() && !any; ++f) {
      std::vector<int> mine;
      for (int v : pts)
        if (cornerOf(m.faces[f], v) >= 0) mine.push_back(v);
      if (mine.size() != 2) continue;
      std::pair<int, int> key = {std::min(mine[0], mine[1]), std::max(mine[0], mine[1])};
      if (done.count(key)) continue;
      if (splitFace(m, static_cast<int>(f), mine[0], mine[1])) {
        done.insert(key);
        newEdges.push_back(edgeKey(mine[0], mine[1]));
        any = true;
      }
    }
  }
  if (newEdges.empty()) return fail(msg, "The edge ring needs quads");
  sel.edges = newEdges;
  sel.normalize();
  return true;
}

bool opTurnEdge(PolyMesh& m, PolySelection& sel, int a, int b, std::string* msg) {
  int f1 = -1, f2 = -1, k1 = -1, k2 = -1;
  for (size_t f = 0; f < m.faces.size(); ++f) {
    const auto& face = m.faces[f];
    if (face.v.size() != 3) continue;
    for (int k = 0; k < 3; ++k) {
      if (face.v[k] == a && face.v[(k + 1) % 3] == b) { f1 = static_cast<int>(f); k1 = k; }
      if (face.v[k] == b && face.v[(k + 1) % 3] == a) { f2 = static_cast<int>(f); k2 = k; }
    }
  }
  if (f1 < 0 || f2 < 0) return fail(msg, "Turn needs an edge between two triangles");
  PolyMesh::Face A = m.faces[f1], B = m.faces[f2];
  int c = A.v[(k1 + 2) % 3], d = B.v[(k2 + 2) % 3];
  bool uv = A.uv.size() == 3 && B.uv.size() == 3;
  auto uvOf = [&](const PolyMesh::Face& f, int v) { return f.uv[cornerOf(f, v)]; };
  PolyMesh::Face t1 = A, t2 = A;
  t1.v = std::vector<int>{a, d, c};
  t2.v = std::vector<int>{d, b, c};
  if (uv) {
    t1.uv = std::vector<int>{uvOf(A, a), uvOf(B, d), uvOf(A, c)};
    t2.uv = std::vector<int>{uvOf(B, d), uvOf(A, b), uvOf(A, c)};
  } else {
    t1.uv.clear();
    t2.uv.clear();
  }
  m.faces[f1] = t1;
  m.faces[f2] = t2;
  sel.edges = {edgeKey(c, d)};
  return true;
}

bool opWeldPairs(PolyMesh& m, PolySelection& sel, const json& pairs, std::string* msg) {
  int n = static_cast<int>(m.positions.size());
  std::vector<int> target(n);
  for (int i = 0; i < n; ++i) target[i] = i;
  bool any = false;
  for (const auto& p : pairs) {
    if (!p.is_array() || p.size() != 2) continue;
    int from = p[0].get<int>(), to = p[1].get<int>();
    if (from < 0 || to < 0 || from >= n || to >= n || from == to) continue;
    target[from] = to;
    any = true;
  }
  if (!any) return fail(msg, "Nothing to weld");
  // Resolve chains (a -> b -> c).
  for (int i = 0; i < n; ++i)
    for (int guard = 0; target[target[i]] != target[i] && guard < n; ++guard) target[i] = target[target[i]];
  mergeVerticesInto(m, sel, target);
  return true;
}

bool opDetachElement(PolyMesh& m, PolySelection& sel, std::string* msg) {
  std::vector<int> faces = selectedFaces(m, sel);
  if (faces.empty()) return fail(msg, "Select polygons to detach");
  std::vector<char> in(m.faces.size(), 0);
  for (int f : faces) in[f] = 1;
  std::vector<char> usedOutside(m.positions.size(), 0);
  for (size_t f = 0; f < m.faces.size(); ++f)
    if (!in[f])
      for (int v : m.faces[f].v) usedOutside[v] = 1;
  std::map<int, int> copy;
  for (int f : faces)
    for (int& v : m.faces[f].v)
      if (usedOutside[v]) {
        auto it = copy.find(v);
        if (it == copy.end()) it = copy.emplace(v, m.addVertex(m.positions[v])).first;
        v = it->second;
      }
  return true;
}

bool opCreateFace(PolyMesh& m, PolySelection& sel, const json& verts, std::string* msg) {
  PolyMesh::Face f;
  int n = static_cast<int>(m.positions.size());
  for (const auto& v : verts) {
    int i = v.get<int>();
    if (i < 0 || i >= n) return fail(msg, "Invalid vertex");
    if (!f.v.empty() && f.v.back() == i) continue;
    f.v.push_back(i);
  }
  while (f.v.size() > 1 && f.v.front() == f.v.back()) f.v.pop_back();
  std::vector<int> u = f.v;
  sortUnique(u);
  if (u.size() < 3 || u.size() != f.v.size()) return fail(msg, "A polygon needs three or more different vertices");
  // Wind the new polygon opposite to neighbours that share its edges.
  std::set<std::pair<int, int>> existing;
  for (const auto& face : m.faces)
    for (size_t k = 0; k < face.v.size(); ++k) existing.insert({face.v[k], face.v[(k + 1) % face.v.size()]});
  int same = 0, opposite = 0;
  for (size_t k = 0; k < f.v.size(); ++k) {
    int a = f.v[k], b = f.v[(k + 1) % f.v.size()];
    if (existing.count({a, b})) ++same;
    if (existing.count({b, a})) ++opposite;
  }
  if (same > opposite) std::reverse(f.v.begin(), f.v.end());
  f.smoothGroups = 0;
  m.faces.push_back(f);
  ensureFaceUvs(m);
  sel.faces = {static_cast<int>(m.faces.size()) - 1};
  return true;
}

bool opInsertVertexInFace(PolyMesh& m, PolySelection& sel, int face, const Vec3& p, std::string* msg) {
  if (face < 0 || face >= static_cast<int>(m.faces.size())) return fail(msg, "Pick a polygon");
  PolyMesh::Face f = m.faces[face];
  int nv = m.addVertex(p);
  size_t n = f.v.size();
  bool uv = f.uv.size() == n;
  int cuv = -1;
  if (uv) {
    Vec2 c;
    for (int t : f.uv) c += m.uvs[t];
    cuv = m.addUv(c / static_cast<float>(n));
  }
  for (size_t k = 0; k < n; ++k) {
    PolyMesh::Face t = f;
    t.v = std::vector<int>{f.v[k], f.v[(k + 1) % n], nv};
    if (uv) t.uv = std::vector<int>{f.uv[k], f.uv[(k + 1) % n], cuv};
    if (k == 0) m.faces[face] = t;
    else m.faces.push_back(t);
  }
  sel.verts = {nv};
  return true;
}

// Resolves a cut point: {"v": vertex} or {"a", "b", "t"} on an edge (inserting a vertex).
int resolveCutPoint(PolyMesh& m, const json& p) {
  int n = static_cast<int>(m.positions.size());
  if (p.contains("v")) {
    int v = p["v"].get<int>();
    return v >= 0 && v < n ? v : -1;
  }
  int a = p.value("a", -1), b = p.value("b", -1);
  float t = p.value("t", 0.5f);
  if (a < 0 || b < 0 || a >= n || b >= n) return -1;
  if (t <= 1e-3f) return a;
  if (t >= 1.0f - 1e-3f) return b;
  return insertVertexOnEdge(m, a, b, t);
}

bool opCutSegment(PolyMesh& m, PolySelection& sel, const json& from, const json& to, std::string* msg) {
  int va = resolveCutPoint(m, from);
  int vb = resolveCutPoint(m, to);
  if (va < 0 || vb < 0) return fail(msg, "Cut points must be on edges or vertices");
  if (va == vb) return fail(msg, "Pick a different point");
  for (size_t f = 0; f < m.faces.size(); ++f) {
    const auto& face = m.faces[f];
    int ca = cornerOf(face, va), cb = cornerOf(face, vb);
    if (ca < 0 || cb < 0) continue;
    int n = static_cast<int>(face.v.size());
    if ((ca + 1) % n == cb || (cb + 1) % n == ca) {
      sel.edges.push_back(edgeKey(va, vb));
      return true;  // already connected
    }
    if (splitFace(m, static_cast<int>(f), va, vb)) {
      sel.edges.push_back(edgeKey(va, vb));
      return true;
    }
  }
  return fail(msg, "Both cut points must be on the same polygon");
}

bool opDeleteSelected(PolyMesh& m, PolySelection& sel, std::string* msg) {
  std::vector<int> faces;
  if (sel.level == PolyFace || sel.level == PolyElement) {
    faces = selectedFaces(m, sel);
  } else if (sel.level == PolyEdge || sel.level == PolyBorder) {
    for (size_t f = 0; f < m.faces.size(); ++f) {
      const auto& face = m.faces[f];
      for (size_t k = 0; k < face.v.size(); ++k)
        if (sel.hasEdge(edgeKey(face.v[k], face.v[(k + 1) % face.v.size()]))) {
          faces.push_back(static_cast<int>(f));
          break;
        }
    }
  } else if (sel.level == PolyVertex) {
    faces = facesFromSelection(m, sel);
  }
  if (faces.empty()) return fail(msg, "Nothing selected to delete");
  sel.clearLevel(sel.level);
  eraseFaces(m, sel, faces);
  return true;
}

}  // namespace

bool runPolyOp(PolyMesh& m, PolySelection& sel, const json& op, std::string* msg) {
  std::string name = op.value("op", std::string());
  auto f = [&](const char* key, float def) { return op.value(key, def); };
  auto i = [&](const char* key, int def) { return op.value(key, def); };
  auto b = [&](const char* key, bool def) { return op.value(key, def); };
  bool ok = false;
  if (name == "select") {
    PolySelection s;
    s.fromJson(op.value("selection", json::object()));
    s.hiddenFaces = sel.hiddenFaces;
    s.hiddenVerts = sel.hiddenVerts;
    sel = s;
    ok = true;
  } else if (name == "hide") {
    if (sel.level == PolyVertex) sel.hiddenVerts.insert(sel.hiddenVerts.end(), sel.verts.begin(), sel.verts.end());
    else {
      std::vector<int> faces = facesFromSelection(m, sel);
      sel.hiddenFaces.insert(sel.hiddenFaces.end(), faces.begin(), faces.end());
    }
    sel.clearLevel(sel.level);
    sel.normalize();
    ok = true;
  } else if (name == "unhideAll") {
    sel.hiddenFaces.clear();
    sel.hiddenVerts.clear();
    ok = true;
  } else if (name == "delete") {
    ok = opDeleteSelected(m, sel, msg);
  } else if (name == "removeVerts") {
    ok = opRemoveVerts(m, sel, msg);
  } else if (name == "breakVerts") {
    ok = opBreakVerts(m, sel, msg);
  } else if (name == "weldVerts") {
    ok = opWeld(m, sel, sel.verts, f("threshold", 0.1f), msg);
  } else if (name == "weldEdges") {
    ok = opWeld(m, sel, selectedVertsAtLevel(m, sel), f("threshold", 0.1f), msg);
  } else if (name == "targetWeld") {
    ok = opTargetWeld(m, sel, i("from", -1), i("to", -1), msg);
  } else if (name == "chamferVerts") {
    ok = opChamferVerts(m, sel, f("amount", 1.0f), b("open", false), msg);
  } else if (name == "extrudeVerts") {
    if (sel.verts.empty()) return fail(msg, "Select vertices to extrude");
    float width = std::max(f("width", 2.0f), 1e-3f);
    std::set<int> vs(sel.verts.begin(), sel.verts.end());
    std::vector<Vec3> normals(m.positions.size());
    for (const auto& face : m.faces)
      for (int v : face.v) normals[v] += m.faceNormal(face);
    std::map<int, Vec3> apexOf;
    for (int v : sel.verts) apexOf[v] = m.positions[v] + normalize(normals[v]) * f("height", 5.0f);
    ChamferResult r = chamferCore(m, sel, vs, {}, width, false);
    // Replace each new chamfer polygon with a fan to its apex.
    std::vector<int> apexes;
    for (int nf : r.newFaces) {
      PolyMesh::Face face = m.faces[nf];
      Vec3 c = faceCenter(m, face);
      Vec3 best;
      float bestD = kInfinity;
      for (const auto& [v, apex] : apexOf) {
        float d = length(apex - c);
        if (d < bestD) {
          bestD = d;
          best = apex;
        }
      }
      int ap = m.addVertex(best);
      apexes.push_back(ap);
      size_t n = face.v.size();
      for (size_t k = 0; k < n; ++k) {
        PolyMesh::Face t;
        t.v = std::vector<int>{face.v[k], face.v[(k + 1) % n], ap};
        t.smoothGroups = 0;
        if (k == 0) m.faces[nf] = t;
        else m.faces.push_back(t);
      }
    }
    ensureFaceUvs(m);
    sel.verts = apexes;
    sel.normalize();
    ok = true;
  } else if (name == "cutSegment") {
    ok = opCutSegment(m, sel, op.value("from", json::object()), op.value("to", json::object()), msg);
  } else if (name == "attach") {
    PolyMesh other = polyMeshFromJson(op.value("mesh", json::object()));
    if (other.faces.empty()) return fail(msg, "Nothing to attach");
    int base = static_cast<int>(m.faces.size());
    if (meshHasUvs(m) || meshHasUvs(other)) {
      ensureFaceUvs(m);
      ensureFaceUvs(other);
    }
    m.append(other);
    if (sel.level == PolyFace || sel.level == PolyElement) {
      sel.faces.clear();
      for (int f = base; f < static_cast<int>(m.faces.size()); ++f) sel.faces.push_back(f);
    }
    ensureFaceUvs(m);
    ok = true;
  } else if (name == "turnEdge") {
    ok = opTurnEdge(m, sel, i("a", -1), i("b", -1), msg);
  } else if (name == "weldPairs") {
    ok = opWeldPairs(m, sel, op.value("pairs", json::array()), msg);
  } else if (name == "detachElement") {
    ok = opDetachElement(m, sel, msg);
  } else if (name == "createFace") {
    ok = opCreateFace(m, sel, op.value("verts", json::array()), msg);
  } else if (name == "insertVertexInFace") {
    ok = opInsertVertexInFace(m, sel, i("face", -1), jsonVec3(op, "point", Vec3()), msg);
  } else if (name == "hideUnselected") {
    if (sel.level == PolyVertex) {
      for (size_t v = 0; v < m.positions.size(); ++v)
        if (!sel.hasVert(static_cast<int>(v))) sel.hiddenVerts.push_back(static_cast<int>(v));
    } else {
      std::vector<int> keep = facesFromSelection(m, sel);
      std::sort(keep.begin(), keep.end());
      for (size_t f = 0; f < m.faces.size(); ++f)
        if (!std::binary_search(keep.begin(), keep.end(), static_cast<int>(f))) sel.hiddenFaces.push_back(static_cast<int>(f));
    }
    sel.normalize();
    ok = true;
  } else if (name == "connectVerts") {
    ok = opConnectVerts(m, sel, msg);
  } else if (name == "removeIsolated") {
    std::vector<int> vmap = compactVertices(m);
    remapSelection(sel, vmap, {});
    ok = true;
  } else if (name == "insertVertex") {
    int nv = insertVertexOnEdge(m, i("a", -1), i("b", -1), f("t", 0.5f));
    ok = nv >= 0;
    if (ok) {
      sel.verts = std::vector<int>{nv};
    } else if (msg) {
      *msg = "Pick an edge";
    }
  } else if (name == "removeEdges") {
    ok = opRemoveEdges(m, sel, b("removeVerts", false), msg);
  } else if (name == "splitEdges") {
    ok = opSplitEdges(m, sel, msg);
  } else if (name == "extrudeEdges") {
    if (sel.edges.empty()) return fail(msg, "Select edges to extrude");
    PolyTopology topo(m);
    std::set<int> es = interiorEdges(topo, sel.edges);
    if (es.empty()) return fail(msg, "Extrude needs edges shared by two polygons");
    ChamferResult r = chamferCore(m, sel, {}, es, std::max(f("width", 1.0f), 1e-3f), false);
    PolySelection tmp;
    opExtrudeRegions(m, tmp, r.newFaces, f("height", 5.0f), 1, 0.0f);
    sel.faces = r.newFaces;
    sel.normalize();
    ok = true;
  } else if (name == "chamferEdges") {
    ok = opChamferEdges(m, sel, f("amount", 1.0f), b("open", false), msg);
  } else if (name == "bridgeEdges") {
    ok = opBridgeEdges(m, sel, i("segments", 1), msg);
  } else if (name == "connectEdges") {
    ok = opConnectEdges(m, sel, i("segments", 1), f("pinch", 0.0f), f("slide", 0.0f), msg);
  } else if (name == "capBorders") {
    ok = opCapBorders(m, sel, msg);
  } else if (name == "extrudeBorders") {
    ok = opExtrudeBorders(m, sel, f("height", 5.0f), f("width", 0.0f), msg);
  } else if (name == "bridgeBorders") {
    ok = opBridgeBorders(m, sel, i("segments", 1), msg);
  } else if (name == "extrudeFaces") {
    std::vector<int> faces = selectedFaces(m, sel);
    if (faces.empty()) return fail(msg, "Select polygons to extrude");
    ok = opExtrudeRegions(m, sel, faces, f("height", 10.0f), i("mode", 0), 0.0f);
  } else if (name == "bevelFaces") {
    std::vector<int> faces = selectedFaces(m, sel);
    if (faces.empty()) return fail(msg, "Select polygons to bevel");
    ok = opExtrudeRegions(m, sel, faces, f("height", 10.0f), i("mode", 0), f("outline", -2.0f));
  } else if (name == "insetFaces") {
    ok = opInset(m, sel, f("amount", 2.0f), b("byPolygon", false), msg);
  } else if (name == "outlineFaces") {
    ok = opOutline(m, sel, f("amount", 1.0f), msg);
  } else if (name == "bridgeFaces") {
    ok = bridgeFacesImpl(m, sel, i("segments", 1), msg);
  } else if (name == "flipFaces") {
    std::vector<int> faces = selectedFaces(m, sel);
    if (faces.empty()) return fail(msg, "Select polygons or elements to flip");
    for (int fi : faces) {
      std::reverse(m.faces[fi].v.begin(), m.faces[fi].v.end());
      std::reverse(m.faces[fi].uv.begin(), m.faces[fi].uv.end());
    }
    ok = true;
  } else if (name == "hingeFaces") {
    ok = opHinge(m, sel, f("angle", 90.0f), i("segments", 4), i("a", -1), i("b", -1), msg);
  } else if (name == "triangulateFaces") {
    ok = opTriangulate(m, sel, msg);
  } else if (name == "setMaterialId") {
    std::vector<int> faces = selectedFaces(m, sel);
    if (faces.empty()) return fail(msg, "Select polygons");
    for (int fi : faces) m.faces[fi].matId = std::max(0, i("id", 1) - 1);
    ok = true;
  } else if (name == "setSmoothing") {
    std::vector<int> faces = selectedFaces(m, sel);
    if (faces.empty()) return fail(msg, "Select polygons");
    int g = std::clamp(i("group", 1), 1, 32);
    for (int fi : faces) {
      if (b("on", true)) m.faces[fi].smoothGroups |= 1u << (g - 1);
      else m.faces[fi].smoothGroups &= ~(1u << (g - 1));
    }
    ok = true;
  } else if (name == "clearSmoothing") {
    std::vector<int> faces = selectedFaces(m, sel);
    if (faces.empty()) return fail(msg, "Select polygons");
    for (int fi : faces) m.faces[fi].smoothGroups = 0;
    ok = true;
  } else if (name == "autoSmooth") {
    std::vector<int> faces = selectedFaces(m, sel);
    autoSmooth(m, faces.empty() ? nullptr : &faces, f("angle", 45.0f));
    ok = true;
  } else if (name == "collapse") {
    ok = opCollapse(m, sel, msg);
  } else if (name == "tessellate") {
    int it = std::clamp(i("iterations", 1), 1, 4);
    ok = true;
    for (int k = 0; k < it && ok; ++k) ok = opTessellate(m, sel, msg);
  } else if (name == "msmooth") {
    int it = std::clamp(i("iterations", 1), 1, 4);
    for (int k = 0; k < it; ++k) m = subdivideCatmullClark(m);
    int level = sel.level;
    sel = PolySelection();
    sel.level = level;
    ok = true;
  } else if (name == "makePlanar") {
    ok = opMakePlanar(m, sel, i("axis", -1), op, msg);
  } else if (name == "relax") {
    ok = opRelax(m, sel, f("amount", 0.5f), i("iterations", 1), b("keepBoundary", true), msg);
  } else if (name == "slice") {
    Vec3 p = jsonVec3(op, "point", Vec3()), n = jsonVec3(op, "normal", Vec3(0, 0, 1));
    std::vector<int> faces = selectedFaces(m, sel);
    bool only = b("selectedOnly", false) && !faces.empty();
    int cuts = slicePlane(m, p, n, only ? &faces : nullptr, b("split", false));
    if (cuts == 0) return fail(msg, "The slice plane doesn't cross the mesh");
    ensureFaceUvs(m);
    ok = true;
  } else if (name == "cut") {
    int face = i("face", -1);
    int va = i("a", -1), vb = i("b", -1);
    if (face < 0) {
      for (size_t g = 0; g < m.faces.size() && face < 0; ++g)
        if (cornerOf(m.faces[g], va) >= 0 && cornerOf(m.faces[g], vb) >= 0) face = static_cast<int>(g);
    }
    ok = splitFace(m, face, va, vb);
    if (!ok && msg) *msg = "Both points must be on the same polygon";
    if (ok) sel.edges = {edgeKey(va, vb)};
  } else if (name == "insertLoop") {
    ok = opInsertLoop(m, sel, i("a", -1), i("b", -1), f("t", 0.5f), msg);
  } else if (name == "transform") {
    Mat4 mat;
    if (op.contains("matrix") && op["matrix"].size() == 16)
      for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) mat.m[r][c] = op["matrix"][r * 4 + c].get<float>();
    SoftSelection soft = sel.soft;
    if (op.contains("soft")) {
      soft.enabled = op["soft"].value("enabled", false);
      soft.falloff = op["soft"].value("falloff", 20.0f);
      soft.pinch = op["soft"].value("pinch", 0.0f);
      soft.bubble = op["soft"].value("bubble", 0.0f);
    }
    std::vector<float> w = softSelectionWeights(m, sel, soft);
    for (size_t v = 0; v < m.positions.size(); ++v)
      if (w[v] > 0.0f) m.positions[v] = lerp(m.positions[v], mat.transformPoint(m.positions[v]), w[v]);
    ok = true;
  } else if (name == "moveVerts") {
    const json& vs = op.value("verts", json::array());
    const json& ds = op.value("deltas", json::array());
    for (size_t k = 0; k < vs.size() && 3 * k + 2 < ds.size(); ++k) {
      int v = vs[k].get<int>();
      if (v < 0 || v >= static_cast<int>(m.positions.size())) continue;
      m.positions[v] += Vec3(ds[3 * k].get<float>(), ds[3 * k + 1].get<float>(), ds[3 * k + 2].get<float>());
    }
    ok = true;
  } else {
    return fail(msg, "Unknown operation");
  }
  if (ok) {
    compactUvs(m);
    sel.normalize();
  }
  return ok;
}

}  // namespace veraa
