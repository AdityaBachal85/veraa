// Sub-object selection: topology, level conversion, grow/shrink, loop/ring, soft selection.
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <queue>
#include <set>

#include "polyops.h"

namespace veraa {

// ---------------------------------------------------------------------------------------------
// PolySelection

bool PolySelection::emptyAtLevel() const {
  switch (level) {
    case PolyVertex: return verts.empty();
    case PolyEdge:
    case PolyBorder: return edges.empty();
    case PolyFace:
    case PolyElement: return faces.empty();
    default: return true;
  }
}

void PolySelection::clearLevel(int lvl) {
  if (lvl == PolyVertex) verts.clear();
  else if (lvl == PolyEdge || lvl == PolyBorder) edges.clear();
  else if (lvl == PolyFace || lvl == PolyElement) faces.clear();
}

void PolySelection::normalize() {
  sortUnique(verts);
  sortUnique(edges);
  sortUnique(faces);
  sortUnique(hiddenFaces);
  sortUnique(hiddenVerts);
}

json PolySelection::toJson() const {
  json e = json::array();
  for (uint64_t k : edges) e.push_back(json::array({edgeKeyA(k), edgeKeyB(k)}));
  json j{{"level", level}, {"verts", verts}, {"edges", std::move(e)}, {"faces", faces}};
  if (!hiddenFaces.empty()) j["hiddenFaces"] = hiddenFaces;
  if (!hiddenVerts.empty()) j["hiddenVerts"] = hiddenVerts;
  if (!(soft == SoftSelection()))
    j["soft"] = {{"enabled", soft.enabled}, {"falloff", soft.falloff}, {"pinch", soft.pinch}, {"bubble", soft.bubble}};
  return j;
}

void PolySelection::fromJson(const json& j) {
  *this = PolySelection();
  if (!j.is_object()) return;
  level = std::clamp(j.value("level", 0), 0, 5);
  verts = j.value("verts", std::vector<int>{});
  faces = j.value("faces", std::vector<int>{});
  hiddenFaces = j.value("hiddenFaces", std::vector<int>{});
  hiddenVerts = j.value("hiddenVerts", std::vector<int>{});
  if (j.contains("edges"))
    for (const auto& e : j["edges"])
      if (e.is_array() && e.size() == 2) edges.push_back(edgeKey(e[0].get<int>(), e[1].get<int>()));
  if (j.contains("soft") && j["soft"].is_object()) {
    const json& s = j["soft"];
    soft.enabled = s.value("enabled", false);
    soft.falloff = s.value("falloff", 20.0f);
    soft.pinch = s.value("pinch", 0.0f);
    soft.bubble = s.value("bubble", 0.0f);
  }
  normalize();
}

// ---------------------------------------------------------------------------------------------
// Topology

PolyTopology::PolyTopology(const PolyMesh& m) {
  faceEdges.resize(m.faces.size());
  vertFaces.resize(m.positions.size());
  vertEdges.resize(m.positions.size());
  edgeIndex.reserve(m.faces.size() * 4);
  for (size_t f = 0; f < m.faces.size(); ++f) {
    const auto& face = m.faces[f];
    size_t n = face.v.size();
    faceEdges[f].resize(n);
    for (size_t k = 0; k < n; ++k) {
      int a = face.v[k], b = face.v[(k + 1) % n];
      vertFaces[a].push_back(static_cast<int>(f));
      uint64_t key = edgeKey(a, b);
      auto it = edgeIndex.find(key);
      int e;
      if (it == edgeIndex.end()) {
        e = static_cast<int>(edges.size());
        Edge ed;
        ed.a = std::min(a, b);
        ed.b = std::max(a, b);
        edges.push_back(ed);
        edgeIndex.emplace(key, e);
        vertEdges[a].push_back(e);
        if (b != a) vertEdges[b].push_back(e);
      } else {
        e = it->second;
      }
      edges[e].faces.push_back(static_cast<int>(f));
      faceEdges[f][k] = e;
    }
  }
  for (auto& vf : vertFaces) sortUnique(vf);
}

int PolyTopology::edgeOf(int a, int b) const {
  auto it = edgeIndex.find(edgeKey(a, b));
  return it == edgeIndex.end() ? -1 : it->second;
}

// ---------------------------------------------------------------------------------------------
// Level conversion

std::vector<int> selectedVertsAtLevel(const PolyMesh& m, const PolySelection& sel) {
  std::vector<int> out;
  switch (sel.level) {
    case PolyVertex: out = sel.verts; break;
    case PolyEdge:
    case PolyBorder:
      for (uint64_t k : sel.edges) {
        out.push_back(edgeKeyA(k));
        out.push_back(edgeKeyB(k));
      }
      break;
    case PolyFace:
    case PolyElement:
      for (int f : sel.faces)
        if (f >= 0 && f < static_cast<int>(m.faces.size()))
          for (int v : m.faces[f].v) out.push_back(v);
      break;
    default: break;
  }
  sortUnique(out);
  out.erase(std::remove_if(out.begin(), out.end(),
                           [&](int v) { return v < 0 || v >= static_cast<int>(m.positions.size()); }),
            out.end());
  return out;
}

std::vector<int> facesFromSelection(const PolyMesh& m, const PolySelection& sel) {
  std::vector<int> out;
  if (sel.level == PolyFace || sel.level == PolyElement) {
    for (int f : sel.faces)
      if (f >= 0 && f < static_cast<int>(m.faces.size())) out.push_back(f);
    return out;
  }
  std::vector<int> verts = selectedVertsAtLevel(m, sel);
  std::vector<char> vsel(m.positions.size(), 0);
  for (int v : verts) vsel[v] = 1;
  for (size_t f = 0; f < m.faces.size(); ++f) {
    bool any = false;
    for (int v : m.faces[f].v) any |= vsel[v] != 0;
    if (any) out.push_back(static_cast<int>(f));
  }
  return out;
}

void convertSelection(const PolyMesh& m, PolySelection& sel, int toLevel) {
  std::vector<int> verts = selectedVertsAtLevel(m, sel);
  std::vector<char> vsel(m.positions.size(), 0);
  for (int v : verts) vsel[v] = 1;
  PolyTopology topo(m);
  if (toLevel == PolyVertex) {
    sel.verts = verts;
  } else if (toLevel == PolyEdge || toLevel == PolyBorder) {
    sel.edges.clear();
    bool fromFaces = sel.level == PolyFace || sel.level == PolyElement;
    for (size_t e = 0; e < topo.edges.size(); ++e) {
      const auto& ed = topo.edges[e];
      bool ok = vsel[ed.a] && vsel[ed.b];
      if (fromFaces) {
        ok = false;
        for (int f : ed.faces) ok |= sel.hasFace(f);
      }
      if (toLevel == PolyBorder) ok = ok && ed.faces.size() == 1;
      if (ok) sel.edges.push_back(edgeKey(ed.a, ed.b));
    }
  } else if (toLevel == PolyFace || toLevel == PolyElement) {
    sel.faces.clear();
    for (size_t f = 0; f < m.faces.size(); ++f) {
      bool all = !m.faces[f].v.empty();
      for (int v : m.faces[f].v) all &= vsel[v] != 0;
      if (all) sel.faces.push_back(static_cast<int>(f));
    }
    if (toLevel == PolyElement) {
      std::vector<int> grown;
      for (int f : sel.faces)
        if (!std::binary_search(grown.begin(), grown.end(), f)) {
          std::vector<int> el = elementOf(m, f);
          grown.insert(grown.end(), el.begin(), el.end());
          sortUnique(grown);
        }
      sel.faces = grown;
    }
  }
  sel.level = toLevel;
  sel.normalize();
}

// ---------------------------------------------------------------------------------------------
// Grow / shrink

void growSelection(const PolyMesh& m, PolySelection& sel) {
  PolyTopology topo(m);
  switch (sel.level) {
    case PolyVertex: {
      std::vector<int> out = sel.verts;
      for (int v : sel.verts)
        if (v < static_cast<int>(topo.vertEdges.size()))
          for (int e : topo.vertEdges[v]) out.push_back(topo.otherVertex(e, v));
      sel.verts = out;
      break;
    }
    case PolyEdge:
    case PolyBorder: {
      std::vector<uint64_t> out = sel.edges;
      for (uint64_t k : sel.edges)
        for (int v : {edgeKeyA(k), edgeKeyB(k)})
          if (v < static_cast<int>(topo.vertEdges.size()))
            for (int e : topo.vertEdges[v]) {
              if (sel.level == PolyBorder && !topo.isBoundary(e)) continue;
              out.push_back(edgeKey(topo.edges[e].a, topo.edges[e].b));
            }
      sel.edges = out;
      break;
    }
    case PolyFace:
    case PolyElement: {
      std::vector<int> out = sel.faces;
      for (int f : sel.faces)
        if (f < static_cast<int>(m.faces.size()))
          for (int v : m.faces[f].v)
            for (int g : topo.vertFaces[v]) out.push_back(g);
      sel.faces = out;
      break;
    }
    default: break;
  }
  sel.normalize();
}

void shrinkSelection(const PolyMesh& m, PolySelection& sel) {
  PolyTopology topo(m);
  switch (sel.level) {
    case PolyVertex: {
      std::vector<int> out;
      for (int v : sel.verts) {
        if (v >= static_cast<int>(topo.vertEdges.size())) continue;
        bool keep = true;
        for (int e : topo.vertEdges[v]) keep &= sel.hasVert(topo.otherVertex(e, v));
        if (keep) out.push_back(v);
      }
      sel.verts = out;
      break;
    }
    case PolyEdge:
    case PolyBorder: {
      // An edge stays if both endpoints touch only selected edges.
      std::vector<uint64_t> out;
      for (uint64_t k : sel.edges) {
        bool keep = true;
        for (int v : {edgeKeyA(k), edgeKeyB(k)}) {
          if (v >= static_cast<int>(topo.vertEdges.size())) continue;
          int count = 0;
          for (int e : topo.vertEdges[v])
            if (sel.hasEdge(edgeKey(topo.edges[e].a, topo.edges[e].b))) ++count;
          keep &= count >= 2;
        }
        if (keep) out.push_back(k);
      }
      sel.edges = out;
      break;
    }
    case PolyFace:
    case PolyElement: {
      std::vector<int> out;
      for (int f : sel.faces) {
        if (f >= static_cast<int>(m.faces.size())) continue;
        bool keep = true;
        for (int v : m.faces[f].v)
          for (int g : topo.vertFaces[v]) keep &= sel.hasFace(g);
        if (keep) out.push_back(f);
      }
      sel.faces = out;
      break;
    }
    default: break;
  }
  sel.normalize();
}

// ---------------------------------------------------------------------------------------------
// Loop / ring

namespace {

// Next edge of a loop at vertex v coming from edge e: the unique edge at v sharing no face with e
// (valence-4 interior vertices and valence-3 border vertices).
int loopNext(const PolyTopology& topo, int e, int v) {
  int found = -1;
  for (int c : topo.vertEdges[v]) {
    if (c == e) continue;
    bool shares = false;
    for (int f : topo.edges[c].faces)
      for (int g : topo.edges[e].faces) shares |= f == g;
    if (shares) continue;
    if (found >= 0) return -1;  // ambiguous
    found = c;
  }
  size_t valence = topo.vertEdges[v].size();
  bool border = topo.isBoundary(e);
  if (!border && valence != 4) return -1;
  if (border && found >= 0 && !topo.isBoundary(found)) return -1;
  return found;
}

}  // namespace

void selectLoop(const PolyMesh& m, PolySelection& sel) {
  PolyTopology topo(m);
  std::vector<uint64_t> out = sel.edges;
  for (uint64_t k : sel.edges) {
    int e0 = topo.edgeOf(edgeKeyA(k), edgeKeyB(k));
    if (e0 < 0) continue;
    for (int dir = 0; dir < 2; ++dir) {
      int e = e0;
      int v = dir == 0 ? topo.edges[e].b : topo.edges[e].a;
      for (size_t guard = 0; guard < topo.edges.size(); ++guard) {
        int next = loopNext(topo, e, v);
        if (next < 0 || next == e0) break;
        uint64_t nk = edgeKey(topo.edges[next].a, topo.edges[next].b);
        out.push_back(nk);
        v = topo.otherVertex(next, v);
        e = next;
      }
    }
  }
  sel.edges = out;
  sel.normalize();
}

void selectRing(const PolyMesh& m, PolySelection& sel) {
  PolyTopology topo(m);
  std::vector<uint64_t> out = sel.edges;
  std::set<int> seen;
  for (uint64_t k : sel.edges) {
    int e0 = topo.edgeOf(edgeKeyA(k), edgeKeyB(k));
    if (e0 < 0) continue;
    std::vector<int> stack = {e0};
    while (!stack.empty()) {
      int e = stack.back();
      stack.pop_back();
      if (!seen.insert(e).second) continue;
      out.push_back(edgeKey(topo.edges[e].a, topo.edges[e].b));
      for (int f : topo.edges[e].faces) {
        const auto& fe = topo.faceEdges[f];
        if (fe.size() != 4) continue;
        for (size_t i = 0; i < 4; ++i)
          if (fe[i] == e) stack.push_back(fe[(i + 2) % 4]);
      }
    }
  }
  sel.edges = out;
  sel.normalize();
}

std::vector<int> facesByAngle(const PolyMesh& m, int face, float angleDeg) {
  std::vector<int> out;
  if (face < 0 || face >= static_cast<int>(m.faces.size())) return out;
  PolyTopology topo(m);
  float cosLimit = std::cos(radians(angleDeg));
  std::vector<char> visited(m.faces.size(), 0);
  std::vector<int> stack = {face};
  visited[face] = 1;
  while (!stack.empty()) {
    int f = stack.back();
    stack.pop_back();
    out.push_back(f);
    Vec3 nf = normalize(m.faceNormal(m.faces[f]));
    for (int e : topo.faceEdges[f])
      for (int g : topo.edges[e].faces) {
        if (visited[g]) continue;
        if (dot(nf, normalize(m.faceNormal(m.faces[g]))) < cosLimit) continue;
        visited[g] = 1;
        stack.push_back(g);
      }
  }
  sortUnique(out);
  return out;
}

std::vector<int> elementOf(const PolyMesh& m, int face) {
  std::vector<int> out;
  if (face < 0 || face >= static_cast<int>(m.faces.size())) return out;
  PolyTopology topo(m);
  std::vector<char> visited(m.faces.size(), 0);
  std::vector<int> stack = {face};
  visited[face] = 1;
  while (!stack.empty()) {
    int f = stack.back();
    stack.pop_back();
    out.push_back(f);
    for (int e : topo.faceEdges[f])
      for (int g : topo.edges[e].faces)
        if (!visited[g]) {
          visited[g] = 1;
          stack.push_back(g);
        }
  }
  sortUnique(out);
  return out;
}

std::vector<uint64_t> borderLoopOf(const PolyMesh& m, int a, int b) {
  PolyTopology topo(m);
  std::vector<uint64_t> out;
  int e0 = topo.edgeOf(a, b);
  if (e0 < 0 || !topo.isBoundary(e0)) return out;
  std::vector<int> stack = {e0};
  std::set<int> seen;
  while (!stack.empty()) {
    int e = stack.back();
    stack.pop_back();
    if (!seen.insert(e).second) continue;
    out.push_back(edgeKey(topo.edges[e].a, topo.edges[e].b));
    for (int v : {topo.edges[e].a, topo.edges[e].b})
      for (int c : topo.vertEdges[v])
        if (topo.isBoundary(c)) stack.push_back(c);
  }
  sortUnique(out);
  return out;
}

std::vector<int> facesWithMaterial(const PolyMesh& m, int id) {
  std::vector<int> out;
  for (size_t f = 0; f < m.faces.size(); ++f)
    if (m.faces[f].matId == id) out.push_back(static_cast<int>(f));
  return out;
}

std::vector<int> facesWithSmoothingGroup(const PolyMesh& m, int group) {
  std::vector<int> out;
  if (group < 1 || group > 32) return out;
  uint32_t bit = 1u << (group - 1);
  for (size_t f = 0; f < m.faces.size(); ++f)
    if (m.faces[f].smoothGroups & bit) out.push_back(static_cast<int>(f));
  return out;
}

// ---------------------------------------------------------------------------------------------
// Soft selection

std::vector<float> softSelectionWeights(const PolyMesh& m, const PolySelection& sel, const SoftSelection& soft) {
  std::vector<float> w(m.positions.size(), 0.0f);
  std::vector<int> verts = selectedVertsAtLevel(m, sel);
  for (int v : verts)
    if (v >= 0 && v < static_cast<int>(w.size())) w[v] = 1.0f;
  if (!soft.enabled || soft.falloff <= 0.0f || verts.empty()) return w;
  // Distance to the nearest selected vertex, using a hash grid with falloff-sized cells.
  float cell = soft.falloff;
  auto key = [cell](const Vec3& p) {
    auto q = [cell](float x) { return static_cast<int64_t>(std::floor(x / cell)); };
    return std::array<int64_t, 3>{q(p.x), q(p.y), q(p.z)};
  };
  std::map<std::array<int64_t, 3>, std::vector<int>> grid;
  for (int v : verts) grid[key(m.positions[v])].push_back(v);
  for (size_t i = 0; i < m.positions.size(); ++i) {
    if (w[i] >= 1.0f) continue;
    float best = kInfinity;
    auto c = key(m.positions[i]);
    for (int64_t dx = -1; dx <= 1; ++dx)
      for (int64_t dy = -1; dy <= 1; ++dy)
        for (int64_t dz = -1; dz <= 1; ++dz) {
          auto it = grid.find({c[0] + dx, c[1] + dy, c[2] + dz});
          if (it == grid.end()) continue;
          for (int v : it->second) best = std::min(best, length(m.positions[i] - m.positions[v]));
        }
    if (best >= soft.falloff) continue;
    float t = best / soft.falloff;                   // 0 at the selection, 1 at the falloff radius
    float base = 0.5f * (1.0f + std::cos(kPi * t));  // smooth bell
    // Pinch sharpens the peak; bubble swells the shoulder.
    float y = std::pow(base, 1.0f + std::max(0.0f, soft.pinch) * 2.0f);
    if (soft.pinch < 0.0f) y = std::pow(base, 1.0f / (1.0f - soft.pinch * 2.0f));
    y += soft.bubble * 0.5f * std::sin(kPi * (1.0f - t)) * (1.0f - y);
    w[i] = clampf(y, 0.0f, 1.0f);
  }
  return w;
}

std::vector<float> stackSelectionWeights(const PolyMesh& m, const PolySelection& sel) {
  if (sel.level == PolyObject) return {};
  return softSelectionWeights(m, sel, sel.soft);
}

}  // namespace veraa
