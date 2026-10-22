#include "polymods.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "registry.h"

namespace veraa {

namespace {

// Drops hidden polygons from the mesh passed up the stack (vertex indices stay valid).
void dropHiddenFaces(PolyMesh& m, const PolySelection& sel) {
  if (sel.hiddenFaces.empty()) return;
  std::vector<PolyMesh::Face> kept;
  kept.reserve(m.faces.size());
  for (size_t f = 0; f < m.faces.size(); ++f)
    if (!sel.isFaceHidden(static_cast<int>(f))) kept.push_back(std::move(m.faces[f]));
  m.faces = std::move(kept);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Edit Poly

void EditPolyModifier::applyState(GeomState& st) const {
  if (st.isShape) return;
  PolySelection sel;
  for (const auto& op : ops) runPolyOp(st.mesh, sel, op);
  if (!preview_.is_null()) runPolyOp(st.mesh, sel, preview_);
  cachedMesh = st.mesh;
  cachedSel = sel;
  cacheValid = true;
  st.selWeights = stackSelectionWeights(st.mesh, sel);
  dropHiddenFaces(st.mesh, sel);
}

json EditPolyModifier::toJson() const {
  json j = Modifier::toJson();
  j["ops"] = ops;
  return j;
}

void EditPolyModifier::fromJson(const json& j) {
  Modifier::fromJson(j);
  ops = j.contains("ops") && j["ops"].is_array() ? j["ops"] : json::array();
  preview_ = json();
  cacheValid = false;
}

void EditPolyModifier::setSelection(const PolySelection& s) {
  json op = {{"op", "select"}, {"selection", s.toJson()}};
  if (!ops.empty() && ops.back().value("op", std::string()) == "select") ops.back() = op;
  else ops.push_back(op);
  PolySelection kept = s;
  kept.hiddenFaces = cachedSel.hiddenFaces;
  kept.hiddenVerts = cachedSel.hiddenVerts;
  cachedSel = kept;
  touch();
}

void EditPolyModifier::record(const json& op) {
  ops.push_back(op);
  touch();
}

void EditPolyModifier::setPreview(const json& op) {
  preview_ = op;
  touch();
}

void EditPolyModifier::clearPreview() {
  if (preview_.is_null()) return;
  preview_ = json();
  touch();
}

// ---------------------------------------------------------------------------------------------
// Poly Select / Mesh Select

void PolySelectModifier::applyState(GeomState& st) const {
  if (st.isShape) return;
  cachedMesh = st.mesh;
  cachedSel = sel;
  cacheValid = true;
  st.selWeights = stackSelectionWeights(st.mesh, sel);
  dropHiddenFaces(st.mesh, sel);
}

json PolySelectModifier::toJson() const {
  json j = Modifier::toJson();
  j["sel"] = sel.toJson();
  return j;
}

void PolySelectModifier::fromJson(const json& j) {
  Modifier::fromJson(j);
  sel = PolySelection();
  if (j.contains("sel")) sel.fromJson(j["sel"]);
  cacheValid = false;
}

void PolySelectModifier::setSelection(const PolySelection& s) {
  sel = s;
  cachedSel = s;
  touch();
}

namespace {

// ---------------------------------------------------------------------------------------------
// Vol. Select

class VolSelectModifier : public Modifier {
 public:
  VolSelectModifier() {
    params.beginGroup("Stack Selection Level");
    params.addEnum("level", "Level", {"Object", "Vertex", "Face"}, 1);
    params.beginGroup("Selection Method");
    params.addBool("invert", "Invert", false);
    params.addEnum("volume", "Volume", {"Box", "Sphere", "Cylinder"}, 0);
    params.beginGroup("Gizmo (relative to the object's bounds)");
    params.addFloat("centerX", "Center X %", 0.0, -1000, 1000, 1.0, 1);
    params.addFloat("centerY", "Center Y %", 0.0, -1000, 1000, 1.0, 1);
    params.addFloat("centerZ", "Center Z %", 0.0, -1000, 1000, 1.0, 1);
    params.addFloat("sizeX", "Size X %", 50.0, 0.0, 1000, 1.0, 1);
    params.addFloat("sizeY", "Size Y %", 50.0, 0.0, 1000, 1.0, 1);
    params.addFloat("sizeZ", "Size Z %", 50.0, 0.0, 1000, 1.0, 1);
    params.beginGroup("Soft Selection");
    params.addFloat("falloff", "Falloff", 0.0, 0.0, 1e7, 1.0);
  }
  std::string type() const override { return "VolSelect"; }
  std::unique_ptr<Modifier> clone() const override { return std::make_unique<VolSelectModifier>(*this); }

  // Signed-ish distance outside the volume (0 inside) in object units.
  float outside(const Vec3& p, const Vec3& c, const Vec3& half) const {
    Vec3 d = p - c;
    switch (params.getInt("volume")) {
      case 1: {  // sphere (ellipsoid scaled by half extents)
        Vec3 n(d.x / std::max(half.x, 1e-6f), d.y / std::max(half.y, 1e-6f), d.z / std::max(half.z, 1e-6f));
        float r = length(n);
        return r <= 1.0f ? 0.0f : (r - 1.0f) * std::min(half.x, std::min(half.y, half.z));
      }
      case 2: {  // cylinder along Z
        float rx = d.x / std::max(half.x, 1e-6f), ry = d.y / std::max(half.y, 1e-6f);
        float radial = std::sqrt(rx * rx + ry * ry);
        float outR = radial <= 1.0f ? 0.0f : (radial - 1.0f) * std::min(half.x, half.y);
        float outZ = std::max(0.0f, std::fabs(d.z) - half.z);
        return std::max(outR, outZ);
      }
      default: {
        Vec3 q = vmax(vabs(d) - half, Vec3(0.0f));
        return length(q);
      }
    }
  }

  void applyState(GeomState& st) const override {
    if (st.isShape) return;
    int level = params.getInt("level");
    if (level == 0) {
      st.selWeights.clear();
      return;
    }
    const PolyMesh& m = st.mesh;
    Aabb b = m.bounds();
    if (!b.valid()) return;
    Vec3 ext = b.extent();
    Vec3 c = b.center() + Vec3(ext.x * params.getFloat("centerX"), ext.y * params.getFloat("centerY"),
                               ext.z * params.getFloat("centerZ")) * 0.01f;
    Vec3 half = Vec3(ext.x * params.getFloat("sizeX"), ext.y * params.getFloat("sizeY"),
                     ext.z * params.getFloat("sizeZ")) * 0.005f;
    float falloff = params.getFloat("falloff");
    bool invert = params.getBool("invert");
    auto weightAt = [&](const Vec3& p) {
      float d = outside(p, c, half);
      float w = d <= 0.0f ? 1.0f : (falloff > 0.0f && d < falloff ? 0.5f * (1.0f + std::cos(kPi * d / falloff)) : 0.0f);
      return invert ? 1.0f - w : w;
    };
    std::vector<float> w(m.positions.size(), 0.0f);
    if (level == 1) {
      for (size_t i = 0; i < w.size(); ++i) w[i] = weightAt(m.positions[i]);
    } else {
      // Face level: faces whose center is inside select all their vertices.
      for (const auto& f : m.faces) {
        Vec3 fc;
        for (int v : f.v) fc += m.positions[v];
        fc /= static_cast<float>(std::max<size_t>(1, f.v.size()));
        float fw = weightAt(fc);
        for (int v : f.v) w[v] = std::max(w[v], fw);
      }
    }
    st.selWeights = std::move(w);
  }
};

// ---------------------------------------------------------------------------------------------
// Turn to Poly

class TurnToPolyModifier : public Modifier {
 public:
  TurnToPolyModifier() {
    params.beginGroup("Parameters");
    params.addBool("keepConvex", "Keep Polygons Convex", false);
    params.addBool("limitSize", "Limit Polygon Size", false);
    params.addInt("maxSize", "Max Size", 4, 3, 100);
    params.addBool("requirePlanar", "Require Planar Polygons", false);
    params.addFloat("threshold", "Threshold", 45.0, 0.0, 180.0, 1.0, 1);
    params.addBool("removeMidEdge", "Remove Mid-Edge Vertices", false);
  }
  std::string type() const override { return "TurnToPoly"; }
  std::unique_ptr<Modifier> clone() const override { return std::make_unique<TurnToPolyModifier>(*this); }

  static bool isConvex(const PolyMesh& m, const PolyMesh::Face& f) {
    Vec3 n = normalize(m.faceNormal(f));
    size_t k = f.v.size();
    for (size_t i = 0; i < k; ++i) {
      const Vec3& a = m.positions[f.v[(i + k - 1) % k]];
      const Vec3& b = m.positions[f.v[i]];
      const Vec3& c = m.positions[f.v[(i + 1) % k]];
      if (dot(cross(b - a, c - b), n) < -1e-6f) return false;
    }
    return true;
  }

  static float planarityAngle(const PolyMesh& m, const PolyMesh::Face& f) {
    std::vector<int> tris = faceTriangles(m, f);
    Vec3 n = normalize(m.faceNormal(f));
    float worst = 0.0f;
    for (size_t t = 0; t + 2 < tris.size(); t += 3) {
      Vec3 a = m.positions[f.v[tris[t]]], b = m.positions[f.v[tris[t + 1]]], c = m.positions[f.v[tris[t + 2]]];
      Vec3 tn = cross(b - a, c - a);
      if (length2(tn) < 1e-20f) continue;
      worst = std::max(worst, degrees(std::acos(clampf(dot(normalize(tn), n), -1.0f, 1.0f))));
    }
    return worst * 2.0f;  // angle between the most tilted triangles, approximately
  }

  void apply(PolyMesh& m) const override {
    if (params.getBool("removeMidEdge")) {
      PolyTopology topo(m);
      std::set<int> mid;
      for (size_t v = 0; v < m.positions.size(); ++v) {
        const auto& es = topo.vertEdges[v];
        if (es.size() != 2) continue;
        Vec3 a = m.positions[topo.otherVertex(es[0], static_cast<int>(v))] - m.positions[v];
        Vec3 b = m.positions[topo.otherVertex(es[1], static_cast<int>(v))] - m.positions[v];
        if (length2(a) < 1e-20f || length2(b) < 1e-20f) continue;
        if (dot(normalize(a), normalize(b)) < -0.9998f) mid.insert(static_cast<int>(v));
      }
      for (auto& f : m.faces) {
        PolyMesh::Face out = f;
        out.v.clear();
        out.uv.clear();
        bool uv = f.uv.size() == f.v.size();
        for (size_t k = 0; k < f.v.size(); ++k) {
          if (mid.count(f.v[k])) continue;
          out.v.push_back(f.v[k]);
          if (uv) out.uv.push_back(f.uv[k]);
        }
        if (out.v.size() >= 3) f = out;
      }
    }
    bool convex = params.getBool("keepConvex"), limit = params.getBool("limitSize"),
         planar = params.getBool("requirePlanar");
    int maxSize = params.getInt("maxSize");
    float threshold = params.getFloat("threshold");
    if (!convex && !limit && !planar) return;
    std::vector<PolyMesh::Face> out;
    out.reserve(m.faces.size());
    for (const auto& f : m.faces) {
      bool split = f.v.size() > 3 && ((limit && static_cast<int>(f.v.size()) > maxSize) || (convex && !isConvex(m, f)) ||
                                      (planar && planarityAngle(m, f) > threshold));
      if (!split) {
        out.push_back(f);
        continue;
      }
      std::vector<int> tris = faceTriangles(m, f);
      bool uv = f.uv.size() == f.v.size();
      for (size_t t = 0; t + 2 < tris.size(); t += 3) {
        PolyMesh::Face tf;
        tf.matId = f.matId;
        tf.smoothGroups = f.smoothGroups;
        for (int c = 0; c < 3; ++c) {
          tf.v.push_back(f.v[tris[t + c]]);
          if (uv) tf.uv.push_back(f.uv[tris[t + c]]);
        }
        out.push_back(std::move(tf));
      }
    }
    m.faces = std::move(out);
  }
};

}  // namespace

void registerPolyModifierTypes() {
  registerModifierType({"EditPoly", "Edit Poly", "Mesh Editing", true, false,
                        [] { return std::make_unique<EditPolyModifier>(); }});
  registerModifierType({"PolySelect", "Poly Select", "Selection Modifiers", true, false,
                        [] { return std::make_unique<PolySelectModifier>("PolySelect"); }});
  registerModifierType({"MeshSelect", "Mesh Select", "Selection Modifiers", true, false,
                        [] { return std::make_unique<PolySelectModifier>("MeshSelect"); }});
  registerModifierType({"VolSelect", "Vol. Select", "Selection Modifiers", true, false,
                        [] { return std::make_unique<VolSelectModifier>(); }});
  registerModifierType({"TurnToPoly", "Turn to Poly", "Conversion", true, false,
                        [] { return std::make_unique<TurnToPolyModifier>(); }});
}

}  // namespace veraa
