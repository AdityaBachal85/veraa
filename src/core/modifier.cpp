#include "modifier.h"

#include <algorithm>
#include <atomic>
#include <unordered_map>

#include "registry.h"

namespace veraa {

void Modifier::applyWithSelection(GeomState& st) const {
  if (st.selWeights.empty() || st.selWeights.size() != st.mesh.positions.size()) {
    st.selWeights.clear();
    apply(st.mesh);
    return;
  }
  std::vector<Vec3> before = st.mesh.positions;
  size_t faces = st.mesh.faces.size();
  apply(st.mesh);
  if (st.mesh.positions.size() != before.size() || st.mesh.faces.size() != faces) {
    st.selWeights.clear();
    return;
  }
  for (size_t i = 0; i < before.size(); ++i) {
    float w = st.selWeights[i];
    if (w <= 0.0f) st.mesh.positions[i] = before[i];
    else if (w < 1.0f) st.mesh.positions[i] = lerp(before[i], st.mesh.positions[i], w);
  }
}

json Modifier::toJson() const {
  json j{{"type", type()}, {"enabled", enabled}, {"params", params.toJson()}};
  if (!displayName.empty()) j["name"] = displayName;
  return j;
}

void Modifier::fromJson(const json& j) {
  enabled = j.value("enabled", true);
  displayName = j.value("name", std::string());
  if (j.contains("params")) params.fromJson(j["params"]);
}

void Modifier::touch() {
  static std::atomic<uint64_t> counter{1};
  revision_ = ++counter;
}

namespace {

const std::vector<std::string> kAxes = {"X", "Y", "Z"};

// Axis a is the modifier's main axis; (b, c) span the perpendicular plane.
void axisFrame(int a, int& b, int& c) {
  b = (a + 1) % 3;
  c = (a + 2) % 3;
  if (a == 2) { b = 0; c = 1; }
}

void axisRange(const PolyMesh& m, int a, float& lo, float& hi) {
  Aabb box = m.bounds();
  lo = box.valid() ? box.lo[a] : 0.0f;
  hi = box.valid() ? box.hi[a] : 0.0f;
}

template <class Derived>
class ModifierBase : public Modifier {
 public:
  std::unique_ptr<Modifier> clone() const override { return std::make_unique<Derived>(static_cast<const Derived&>(*this)); }
};

class BendModifier : public ModifierBase<BendModifier> {
 public:
  BendModifier() {
    params.beginGroup("Bend");
    params.addFloat("angle", "Angle", 90.0, -36000, 36000, 1.0, 1);
    params.addFloat("direction", "Direction", 0.0, -36000, 36000, 1.0, 1);
    params.beginGroup("Bend Axis");
    params.addEnum("axis", "Axis", kAxes, 2);
  }
  std::string type() const override { return "Bend"; }
  void apply(PolyMesh& m) const override {
    float angle = radians(params.getFloat("angle"));
    if (std::fabs(angle) < 1e-6f || m.positions.empty()) return;
    int a = params.getInt("axis"), b, c;
    axisFrame(a, b, c);
    float lo, hi;
    axisRange(m, a, lo, hi);
    float len = std::max(hi - lo, 1e-6f);
    float radius = len / angle;
    float dir = radians(params.getFloat("direction"));
    float cd = std::cos(dir), sd = std::sin(dir);
    for (auto& p : m.positions) {
      // Rotate so the bend happens in the (b, a) plane.
      float x = cd * p[b] + sd * p[c], y = -sd * p[b] + cd * p[c], z = p[a];
      float phi = z / radius;
      float nx = radius - (radius - x) * std::cos(phi);
      float nz = (radius - x) * std::sin(phi);
      p[b] = cd * nx - sd * y;
      p[c] = sd * nx + cd * y;
      p[a] = nz;
    }
  }
};

class TaperModifier : public ModifierBase<TaperModifier> {
 public:
  TaperModifier() {
    params.beginGroup("Taper");
    params.addFloat("amount", "Amount", -0.5, -10.0, 10.0, 0.05, 2);
    params.addFloat("curve", "Curve", 0.0, -10.0, 10.0, 0.05, 2);
    params.beginGroup("Taper Axis");
    params.addEnum("axis", "Primary", kAxes, 2);
  }
  std::string type() const override { return "Taper"; }
  void apply(PolyMesh& m) const override {
    float amount = params.getFloat("amount"), curve = params.getFloat("curve");
    if (m.positions.empty()) return;
    int a = params.getInt("axis"), b, c;
    axisFrame(a, b, c);
    float lo, hi;
    axisRange(m, a, lo, hi);
    float len = std::max(hi - lo, 1e-6f);
    for (auto& p : m.positions) {
      float t = (p[a] - lo) / len;
      float s = 1.0f + amount * t + curve * 4.0f * t * (1.0f - t);
      p[b] *= s;
      p[c] *= s;
    }
  }
};

class TwistModifier : public ModifierBase<TwistModifier> {
 public:
  TwistModifier() {
    params.beginGroup("Twist");
    params.addFloat("angle", "Angle", 90.0, -36000, 36000, 1.0, 1);
    params.addFloat("bias", "Bias", 0.0, -100.0, 100.0, 1.0, 1);
    params.beginGroup("Twist Axis");
    params.addEnum("axis", "Axis", kAxes, 2);
  }
  std::string type() const override { return "Twist"; }
  void apply(PolyMesh& m) const override {
    float angle = radians(params.getFloat("angle"));
    if (std::fabs(angle) < 1e-6f || m.positions.empty()) return;
    int a = params.getInt("axis"), b, c;
    axisFrame(a, b, c);
    float lo, hi;
    axisRange(m, a, lo, hi);
    float len = std::max(hi - lo, 1e-6f);
    float expo = std::pow(2.0f, -params.getFloat("bias") / 50.0f);
    for (auto& p : m.positions) {
      float t = saturate((p[a] - lo) / len);
      float r = angle * std::pow(t, expo);
      float cr = std::cos(r), sr = std::sin(r);
      float x = p[b], y = p[c];
      p[b] = cr * x - sr * y;
      p[c] = sr * x + cr * y;
    }
  }
};

class NoiseModifier : public ModifierBase<NoiseModifier> {
 public:
  NoiseModifier() {
    params.beginGroup("Noise");
    params.addInt("seed", "Seed", 0, 0, 100000);
    params.addFloat("scale", "Scale", 100.0, 0.001, 1e7, 1.0);
    params.addBool("fractal", "Fractal", false);
    params.addFloat("roughness", "Roughness", 0.0, 0.0, 1.0, 0.05);
    params.addFloat("iterations", "Iterations", 6.0, 1.0, 10.0, 0.5, 1);
    params.beginGroup("Strength");
    params.addFloat("strengthX", "X", 0.0, -1e7, 1e7, 0.5);
    params.addFloat("strengthY", "Y", 0.0, -1e7, 1e7, 0.5);
    params.addFloat("strengthZ", "Z", 10.0, -1e7, 1e7, 0.5);
  }
  std::string type() const override { return "Noise"; }
  float sample(const Vec3& p) const {
    if (!params.getBool("fractal")) return perlinNoise(p.x, p.y, p.z);
    float sum = 0, amp = 1, freq = 1, norm = 0;
    float gain = 0.25f + 0.5f * params.getFloat("roughness");
    int octaves = std::max(1, static_cast<int>(params.getFloat("iterations")));
    for (int o = 0; o < octaves; ++o) {
      sum += amp * perlinNoise(p.x * freq, p.y * freq, p.z * freq);
      norm += amp;
      amp *= gain;
      freq *= 2.0f;
    }
    return sum / norm;
  }
  void apply(PolyMesh& m) const override {
    Vec3 strength(params.getFloat("strengthX"), params.getFloat("strengthY"), params.getFloat("strengthZ"));
    if (isBlack(vabs(strength))) return;
    float inv = 1.0f / params.getFloat("scale");
    float s = static_cast<float>(params.getInt("seed")) * 17.31f;
    for (auto& p : m.positions) {
      Vec3 q = p * inv;
      Vec3 d(sample(q + Vec3(s, 0.0f, 0.0f)), sample(q + Vec3(0.0f, s + 31.7f, 0.0f)), sample(q + Vec3(0.0f, 0.0f, s + 73.1f)));
      p += d * strength;
    }
  }
};

class SmoothModifier : public ModifierBase<SmoothModifier> {
 public:
  SmoothModifier() {
    params.beginGroup("Main");
    params.addInt("iterations", "Iterations", 1, 0, 5);
  }
  std::string type() const override { return "Smooth"; }
  void apply(PolyMesh& m) const override {
    int it = params.getInt("iterations");
    for (int i = 0; i < it; ++i) m = subdivideCatmullClark(m);
  }
};

class ShellModifier : public ModifierBase<ShellModifier> {
 public:
  ShellModifier() {
    params.beginGroup("Parameters");
    params.addFloat("inner", "Inner Amount", 0.0, 0.0, 1e7, 0.1);
    params.addFloat("outer", "Outer Amount", 1.0, 0.0, 1e7, 0.1);
  }
  std::string type() const override { return "Shell"; }
  void apply(PolyMesh& m) const override {
    float inner = params.getFloat("inner"), outer = params.getFloat("outer");
    if (m.faces.empty() || inner + outer <= 0.0f) return;
    std::vector<Vec3> n = m.vertexNormals();
    int nv = static_cast<int>(m.positions.size());

    // Count edge usage to find the open boundary.
    std::unordered_map<uint64_t, int> edgeCount;
    auto key = [](int a, int b) {
      if (a > b) std::swap(a, b);
      return (static_cast<uint64_t>(a) << 32) | static_cast<uint32_t>(b);
    };
    for (const auto& f : m.faces)
      for (size_t k = 0; k < f.v.size(); ++k) ++edgeCount[key(f.v[k], f.v[(k + 1) % f.v.size()])];

    PolyMesh out;
    out.uvs = m.uvs;
    for (int i = 0; i < nv; ++i) out.addVertex(m.positions[i] + n[i] * outer);
    for (int i = 0; i < nv; ++i) out.addVertex(m.positions[i] - n[i] * inner);
    for (const auto& f : m.faces) {
      out.faces.push_back(f);
      PolyMesh::Face back = f;
      for (int& v : back.v) v += nv;
      std::reverse(back.v.begin(), back.v.end());
      std::reverse(back.uv.begin(), back.uv.end());
      out.faces.push_back(std::move(back));
    }
    int u0 = out.addUv({0, 0}), u1 = out.addUv({0, 1}), u2 = out.addUv({1, 1}), u3 = out.addUv({1, 0});
    for (const auto& f : m.faces)
      for (size_t k = 0; k < f.v.size(); ++k) {
        int a = f.v[k], b = f.v[(k + 1) % f.v.size()];
        if (edgeCount[key(a, b)] != 1) continue;
        out.addFace({a, a + nv, b + nv, b}, {u1, u0, u3, u2}, 0u, f.matId);
      }
    m = std::move(out);
  }
};

class PushModifier : public ModifierBase<PushModifier> {
 public:
  PushModifier() { params.addFloat("amount", "Push Value", 1.0, -1e7, 1e7, 0.1); }
  std::string type() const override { return "Push"; }
  void apply(PolyMesh& m) const override {
    float amount = params.getFloat("amount");
    if (amount == 0.0f) return;
    std::vector<Vec3> n = m.vertexNormals();
    for (size_t i = 0; i < m.positions.size(); ++i) m.positions[i] += n[i] * amount;
  }
};

class UvwMapModifier : public ModifierBase<UvwMapModifier> {
 public:
  UvwMapModifier() {
    params.beginGroup("Mapping");
    params.addEnum("mapping", "Mapping", {"Planar", "Cylindrical", "Spherical", "Box"}, 3);
    params.addFloat("length", "Length", 100.0, 0.001, 1e7, 1.0);
    params.addFloat("width", "Width", 100.0, 0.001, 1e7, 1.0);
    params.addFloat("height", "Height", 100.0, 0.001, 1e7, 1.0);
    params.addFloat("uTile", "U Tile", 1.0, -1e4, 1e4, 0.1);
    params.addFloat("vTile", "V Tile", 1.0, -1e4, 1e4, 0.1);
  }
  std::string type() const override { return "UVWMap"; }
  void apply(PolyMesh& m) const override {
    int mode = params.getInt("mapping");
    float L = params.getFloat("length"), W = params.getFloat("width"), H = params.getFloat("height");
    float ut = params.getFloat("uTile"), vt = params.getFloat("vTile");
    m.uvs.clear();
    for (auto& f : m.faces) {
      Vec3 fn = normalize(m.faceNormal(f));
      Vec3 an = vabs(fn);
      std::vector<Vec2> uvs;
      for (int vi : f.v) {
        const Vec3& p = m.positions[vi];
        Vec2 uv;
        switch (mode) {
          case 0: uv = {p.x / W + 0.5f, p.y / L + 0.5f}; break;
          case 1: uv = {0.5f + std::atan2(p.y, p.x) / kTwoPi, p.z / H}; break;
          case 2: {
            Vec3 d = normalize(p);
            uv = {0.5f + std::atan2(d.y, d.x) / kTwoPi, 0.5f + std::asin(clampf(d.z, -1, 1)) / kPi};
            break;
          }
          default:
            if (an.x >= an.y && an.x >= an.z) uv = {(fn.x > 0 ? p.y : -p.y) / L + 0.5f, p.z / H};
            else if (an.y >= an.z) uv = {(fn.y > 0 ? -p.x : p.x) / W + 0.5f, p.z / H};
            else uv = {p.x / W + 0.5f, (fn.z > 0 ? p.y : -p.y) / L + 0.5f};
            break;
        }
        uvs.push_back(uv);
      }
      if (mode == 1 || mode == 2) {  // fix the wrap-around seam inside a face
        float umin = 1e9f, umax = -1e9f;
        for (auto& u : uvs) { umin = std::min(umin, u.x); umax = std::max(umax, u.x); }
        if (umax - umin > 0.5f)
          for (auto& u : uvs)
            if (u.x < 0.5f) u.x += 1.0f;
      }
      f.uv.clear();
      for (auto& u : uvs) f.uv.push_back(m.addUv(Vec2(u.x * ut, u.y * vt)));
    }
  }
};

class MirrorModifier : public ModifierBase<MirrorModifier> {
 public:
  MirrorModifier() {
    params.beginGroup("Mirror Axis");
    params.addEnum("axis", "Axis", kAxes, 0);
    params.addFloat("offset", "Offset", 0.0, -1e7, 1e7, 1.0);
    params.addBool("copy", "Copy", false);
  }
  std::string type() const override { return "Mirror"; }
  void apply(PolyMesh& m) const override {
    int a = params.getInt("axis");
    float off = params.getFloat("offset");
    PolyMesh mirrored = m;
    for (auto& p : mirrored.positions) p[a] = 2.0f * off - p[a];
    mirrored.flipFaces();
    if (params.getBool("copy")) m.append(mirrored);
    else m = std::move(mirrored);
  }
};

class NormalModifier : public ModifierBase<NormalModifier> {
 public:
  NormalModifier() { params.addBool("flip", "Flip Normals", true); }
  std::string type() const override { return "Normal"; }
  void apply(PolyMesh& m) const override {
    if (params.getBool("flip")) m.flipFaces();
  }
};

}  // namespace

void registerCoreModifierTypes() {
  auto add = [](const char* type, const char* name, const char* category, std::function<std::unique_ptr<Modifier>()> f) {
    registerModifierType({type, name, category, true, false, std::move(f)});
  };
  add("Bend", "Bend", "Parametric Deformers", [] { return std::make_unique<BendModifier>(); });
  add("Taper", "Taper", "Parametric Deformers", [] { return std::make_unique<TaperModifier>(); });
  add("Twist", "Twist", "Parametric Deformers", [] { return std::make_unique<TwistModifier>(); });
  add("Noise", "Noise", "Parametric Deformers", [] { return std::make_unique<NoiseModifier>(); });
  add("Push", "Push", "Parametric Deformers", [] { return std::make_unique<PushModifier>(); });
  add("Shell", "Shell", "Parametric Deformers", [] { return std::make_unique<ShellModifier>(); });
  add("Mirror", "Mirror", "Parametric Deformers", [] { return std::make_unique<MirrorModifier>(); });
  add("Smooth", "Smooth (Subdivide)", "Subdivision Surfaces", [] { return std::make_unique<SmoothModifier>(); });
  add("Normal", "Normal", "Mesh Editing", [] { return std::make_unique<NormalModifier>(); });
  add("UVWMap", "UVW Map", "UV Coordinates", [] { return std::make_unique<UvwMapModifier>(); });
}

namespace {

std::vector<ModifierTypeInfo>& modRegistry() {
  static std::vector<ModifierTypeInfo> types;
  return types;
}

void ensureBuiltinModifiers() {
  static bool done = false;
  if (done) return;
  done = true;
  registerBuiltinModifierTypes();
}

}  // namespace

void registerModifierType(ModifierTypeInfo info) {
  auto& reg = modRegistry();
  for (auto& t : reg)
    if (t.type == info.type) {
      t = std::move(info);
      return;
    }
  reg.push_back(std::move(info));
}

const std::vector<ModifierTypeInfo>& modifierTypeInfos() {
  ensureBuiltinModifiers();
  return modRegistry();
}

const ModifierTypeInfo* findModifierType(const std::string& type) {
  for (const auto& t : modifierTypeInfos())
    if (t.type == type) return &t;
  return nullptr;
}

std::unique_ptr<Modifier> createModifier(const std::string& type) {
  const ModifierTypeInfo* info = findModifierType(type);
  return info && info->create ? info->create() : nullptr;
}

const std::vector<std::string>& modifierTypes() {
  static std::vector<std::string> types;
  std::vector<const ModifierTypeInfo*> sorted;
  for (const auto& t : modifierTypeInfos()) sorted.push_back(&t);
  std::sort(sorted.begin(), sorted.end(),
            [](const ModifierTypeInfo* a, const ModifierTypeInfo* b) { return a->displayName < b->displayName; });
  types.clear();
  for (const auto* t : sorted) types.push_back(t->type);
  return types;
}

std::string modifierDisplayName(const std::string& type) {
  const ModifierTypeInfo* info = findModifierType(type);
  return info && !info->displayName.empty() ? info->displayName : type;
}

PolyMesh subdivideCatmullClark(const PolyMesh& in) {
  PolyMesh out;
  int nv = static_cast<int>(in.positions.size());
  int nf = static_cast<int>(in.faces.size());
  if (nf == 0) return in;

  struct Edge {
    int a, b;
    int faces = 0;
    Vec3 faceSum;
  };
  std::vector<Edge> edges;
  std::unordered_map<uint64_t, int> edgeIndex;
  auto edgeOf = [&](int a, int b) {
    int lo = std::min(a, b), hi = std::max(a, b);
    uint64_t k = (static_cast<uint64_t>(lo) << 32) | static_cast<uint32_t>(hi);
    auto it = edgeIndex.find(k);
    if (it != edgeIndex.end()) return it->second;
    edges.push_back(Edge{lo, hi, 0, Vec3()});
    int idx = static_cast<int>(edges.size()) - 1;
    edgeIndex.emplace(k, idx);
    return idx;
  };

  std::vector<Vec3> facePt(nf);
  std::vector<std::vector<int>> faceEdges(nf);
  for (int f = 0; f < nf; ++f) {
    const auto& face = in.faces[f];
    Vec3 c;
    for (int v : face.v) c += in.positions[v];
    facePt[f] = c / static_cast<float>(face.v.size());
    size_t n = face.v.size();
    for (size_t k = 0; k < n; ++k) {
      int e = edgeOf(face.v[k], face.v[(k + 1) % n]);
      edges[e].faces++;
      edges[e].faceSum += facePt[f];
      faceEdges[f].push_back(e);
    }
  }

  // Edge points.
  std::vector<Vec3> edgePt(edges.size());
  for (size_t e = 0; e < edges.size(); ++e) {
    const Edge& E = edges[e];
    Vec3 mid = (in.positions[E.a] + in.positions[E.b]) * 0.5f;
    edgePt[e] = E.faces == 2 ? (in.positions[E.a] + in.positions[E.b] + E.faceSum) * 0.25f : mid;
  }

  // Vertex points.
  std::vector<Vec3> faceAcc(nv), edgeMidAcc(nv), boundaryAcc(nv);
  std::vector<int> faceCount(nv, 0), edgeCount(nv, 0), boundaryCount(nv, 0);
  for (int f = 0; f < nf; ++f)
    for (int v : in.faces[f].v) { faceAcc[v] += facePt[f]; faceCount[v]++; }
  for (const auto& E : edges) {
    Vec3 mid = (in.positions[E.a] + in.positions[E.b]) * 0.5f;
    edgeMidAcc[E.a] += mid; edgeMidAcc[E.b] += mid;
    edgeCount[E.a]++; edgeCount[E.b]++;
    if (E.faces != 2) {
      boundaryAcc[E.a] += in.positions[E.b]; boundaryAcc[E.b] += in.positions[E.a];
      boundaryCount[E.a]++; boundaryCount[E.b]++;
    }
  }
  out.positions.resize(nv + edges.size() + nf);
  for (int v = 0; v < nv; ++v) {
    const Vec3& P = in.positions[v];
    if (boundaryCount[v] == 2) {
      out.positions[v] = P * 0.75f + boundaryAcc[v] * 0.125f;
    } else if (boundaryCount[v] > 0 || edgeCount[v] < 3 || faceCount[v] == 0) {
      out.positions[v] = P;  // corners and non-manifold vertices stay put
    } else {
      float n = static_cast<float>(edgeCount[v]);
      Vec3 F = faceAcc[v] / static_cast<float>(faceCount[v]);
      Vec3 R = edgeMidAcc[v] / n;
      out.positions[v] = (F + R * 2.0f + P * (n - 3.0f)) / n;
    }
  }
  int eBase = nv, fBase = nv + static_cast<int>(edges.size());
  for (size_t e = 0; e < edges.size(); ++e) out.positions[eBase + e] = edgePt[e];
  for (int f = 0; f < nf; ++f) out.positions[fBase + f] = facePt[f];

  // Faces (and linearly subdivided UVs).
  out.uvs = in.uvs;
  for (int f = 0; f < nf; ++f) {
    const auto& face = in.faces[f];
    size_t n = face.v.size();
    bool hasUv = face.uv.size() == n;
    int uvCenter = -1;
    std::vector<int> uvEdge(n, -1);
    if (hasUv) {
      Vec2 c;
      for (int t : face.uv) c += in.uvs[t];
      uvCenter = out.addUv(c / static_cast<float>(n));
      for (size_t k = 0; k < n; ++k)
        uvEdge[k] = out.addUv((in.uvs[face.uv[k]] + in.uvs[face.uv[(k + 1) % n]]) * 0.5f);
    }
    for (size_t k = 0; k < n; ++k) {
      size_t kp = (k + n - 1) % n;
      std::vector<int> v = {face.v[k], eBase + faceEdges[f][k], fBase + f, eBase + faceEdges[f][kp]};
      std::vector<int> t;
      if (hasUv) t = {face.uv[k], uvEdge[k], uvCenter, uvEdge[kp]};
      out.addFace(std::move(v), std::move(t), 1u, face.matId);
    }
  }
  return out;
}

namespace {

int perm(int i) {
  static int p[512];
  static bool init = false;
  if (!init) {
    static const int base[256] = {
        151, 160, 137, 91,  90,  15,  131, 13,  201, 95,  96,  53,  194, 233, 7,   225, 140, 36,  103, 30,  69,  142,
        8,   99,  37,  240, 21,  10,  23,  190, 6,   148, 247, 120, 234, 75,  0,   26,  197, 62,  94,  252, 219, 203,
        117, 35,  11,  32,  57,  177, 33,  88,  237, 149, 56,  87,  174, 20,  125, 136, 171, 168, 68,  175, 74,  165,
        71,  134, 139, 48,  27,  166, 77,  146, 158, 231, 83,  111, 229, 122, 60,  211, 133, 230, 220, 105, 92,  41,
        55,  46,  245, 40,  244, 102, 143, 54,  65,  25,  63,  161, 1,   216, 80,  73,  209, 76,  132, 187, 208, 89,
        18,  169, 200, 196, 135, 130, 116, 188, 159, 86,  164, 100, 109, 198, 173, 186, 3,   64,  52,  217, 226, 250,
        124, 123, 5,   202, 38,  147, 118, 126, 255, 82,  85,  212, 207, 206, 59,  227, 47,  16,  58,  17,  182, 189,
        28,  42,  223, 183, 170, 213, 119, 248, 152, 2,   44,  154, 163, 70,  221, 153, 101, 155, 167, 43,  172, 9,
        129, 22,  39,  253, 19,  98,  108, 110, 79,  113, 224, 232, 178, 185, 112, 104, 218, 246, 97,  228, 251, 34,
        242, 193, 238, 210, 144, 12,  191, 179, 162, 241, 81,  51,  145, 235, 249, 14,  239, 107, 49,  192, 214, 31,
        181, 199, 106, 157, 184, 84,  204, 176, 115, 121, 50,  45,  127, 4,   150, 254, 138, 236, 205, 93,  222, 114,
        67,  29,  24,  72,  243, 141, 128, 195, 78,  66,  215, 61,  156, 180};
    for (int k = 0; k < 512; ++k) p[k] = base[k & 255];
    init = true;
  }
  return p[i & 511];
}

float fade(float t) { return t * t * t * (t * (t * 6 - 15) + 10); }

float grad(int h, float x, float y, float z) {
  h &= 15;
  float u = h < 8 ? x : y;
  float v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
  return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
}

}  // namespace

float perlinNoise(float x, float y, float z) {
  int X = static_cast<int>(std::floor(x)) & 255, Y = static_cast<int>(std::floor(y)) & 255,
      Z = static_cast<int>(std::floor(z)) & 255;
  x -= std::floor(x); y -= std::floor(y); z -= std::floor(z);
  float u = fade(x), v = fade(y), w = fade(z);
  int A = perm(X) + Y, AA = perm(A) + Z, AB = perm(A + 1) + Z;
  int B = perm(X + 1) + Y, BA = perm(B) + Z, BB = perm(B + 1) + Z;
  return lerpf(lerpf(lerpf(grad(perm(AA), x, y, z), grad(perm(BA), x - 1, y, z), u),
                     lerpf(grad(perm(AB), x, y - 1, z), grad(perm(BB), x - 1, y - 1, z), u), v),
               lerpf(lerpf(grad(perm(AA + 1), x, y, z - 1), grad(perm(BA + 1), x - 1, y, z - 1), u),
                     lerpf(grad(perm(AB + 1), x, y - 1, z - 1), grad(perm(BB + 1), x - 1, y - 1, z - 1), u), v),
               w);
}

}  // namespace veraa
