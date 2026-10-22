#include "meshbuild.h"

namespace veraa {

Vec2 boxUv(const Vec3& p, const Vec3& n, float s) {
  Vec3 a = vabs(n);
  if (a.x >= a.y && a.x >= a.z) return Vec2((n.x > 0 ? p.y : -p.y) * s, p.z * s);
  if (a.y >= a.z) return Vec2((n.y > 0 ? -p.x : p.x) * s, p.z * s);
  return Vec2(p.x * s, (n.z > 0 ? p.y : -p.y) * s);
}

void MeshBuilder::faceBoxUv(const std::vector<int>& v, int matId, uint32_t smooth) {
  PolyMesh::Face f;
  f.v = v;
  Vec3 n = normalize(m_.faceNormal(f));
  std::vector<int> t;
  t.reserve(v.size());
  for (int vi : v) t.push_back(m_.addUv(boxUv(m_.positions[vi], n, uvScale_)));
  m_.addFace(v, std::move(t), smooth, matId);
}

void MeshBuilder::box(const Mat4& xf, const Vec3& lo, const Vec3& hi, int matId) {
  Vec3 c[8];
  for (int i = 0; i < 8; ++i)
    c[i] = xf.transformPoint(Vec3((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z));
  int b = static_cast<int>(m_.positions.size());
  for (const Vec3& p : c) m_.addVertex(p);
  // Faces counter-clockwise seen from outside (for a non-mirroring transform).
  static const int faces[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
  bool mirrored = xf.determinant3x3() < 0.0f;
  for (int f = 0; f < 6; ++f) {
    std::vector<int> v = {b + faces[f][0], b + faces[f][1], b + faces[f][2], b + faces[f][3]};
    if (mirrored) std::reverse(v.begin(), v.end());
    faceBoxUv(v, matId, 1u << (f % 6 + 8));
  }
}

void MeshBuilder::cylinder(const Vec3& a, const Vec3& b, float radius, int sides, bool caps, int matId) {
  sides = std::max(3, sides);
  Vec3 axis = b - a;
  float len = length(axis);
  if (len < 1e-6f || radius <= 0.0f) return;
  Vec3 d = axis / len, u, w;
  makeBasis(d, u, w);
  int base = static_cast<int>(m_.positions.size());
  for (int ring = 0; ring < 2; ++ring)
    for (int k = 0; k < sides; ++k) {
      float t = kTwoPi * k / sides;
      m_.addVertex((ring ? b : a) + (u * std::cos(t) + w * std::sin(t)) * radius);
    }
  int uvBase = static_cast<int>(m_.uvs.size());
  float circ = kTwoPi * radius * uvScale_;
  for (int ring = 0; ring < 2; ++ring)
    for (int k = 0; k <= sides; ++k) m_.addUv(Vec2(circ * k / sides, ring ? len * uvScale_ : 0.0f));
  for (int k = 0; k < sides; ++k) {
    int kn = (k + 1) % sides;
    m_.addFace({base + k, base + kn, base + sides + kn, base + sides + k},
               {uvBase + k, uvBase + k + 1, uvBase + sides + 1 + k + 1, uvBase + sides + 1 + k}, 1u, matId);
  }
  if (caps) {
    std::vector<int> bottom, top;
    for (int k = sides - 1; k >= 0; --k) bottom.push_back(base + k);
    for (int k = 0; k < sides; ++k) top.push_back(base + sides + k);
    faceBoxUv(bottom, matId, 0u);
    faceBoxUv(top, matId, 0u);
  }
}

void MeshBuilder::cone(const Vec3& a, const Vec3& b, float r0, float r1, int sides, int matId) {
  sides = std::max(3, sides);
  Vec3 axis = b - a;
  float len = length(axis);
  if (len < 1e-6f || (r0 <= 0.0f && r1 <= 0.0f)) return;
  Vec3 d = axis / len, u, w;
  makeBasis(d, u, w);
  int base = static_cast<int>(m_.positions.size());
  bool apex = r1 <= 1e-6f;
  for (int k = 0; k < sides; ++k) {
    float t = kTwoPi * k / sides;
    m_.addVertex(a + (u * std::cos(t) + w * std::sin(t)) * r0);
  }
  if (apex) {
    int tip = m_.addVertex(b);
    for (int k = 0; k < sides; ++k) faceBoxUv({base + k, base + (k + 1) % sides, tip}, matId, 1u);
  } else {
    for (int k = 0; k < sides; ++k) {
      float t = kTwoPi * k / sides;
      m_.addVertex(b + (u * std::cos(t) + w * std::sin(t)) * r1);
    }
    for (int k = 0; k < sides; ++k) {
      int kn = (k + 1) % sides;
      faceBoxUv({base + k, base + kn, base + sides + kn, base + sides + k}, matId, 1u);
    }
    std::vector<int> top;
    for (int k = 0; k < sides; ++k) top.push_back(base + sides + k);
    faceBoxUv(top, matId, 0u);
  }
  std::vector<int> bottom;
  for (int k = sides - 1; k >= 0; --k) bottom.push_back(base + k);
  faceBoxUv(bottom, matId, 0u);
}

void MeshBuilder::sphere(const Vec3& c, float radius, int segments, int matId) {
  int seg = std::max(4, segments), rings = std::max(2, seg / 2);
  int top = m_.addVertex(c + Vec3(0, 0, radius));
  int first = static_cast<int>(m_.positions.size());
  for (int r = 1; r < rings; ++r) {
    float phi = kPi * r / rings;
    for (int k = 0; k < seg; ++k) {
      float t = kTwoPi * k / seg;
      m_.addVertex(c + Vec3(std::sin(phi) * std::cos(t), std::sin(phi) * std::sin(t), std::cos(phi)) * radius);
    }
  }
  int bottom = m_.addVertex(c - Vec3(0, 0, radius));
  auto V = [&](int r, int k) { return first + (r - 1) * seg + (k % seg); };
  for (int k = 0; k < seg; ++k) faceBoxUv({top, V(1, k), V(1, k + 1)}, matId, 1u);
  for (int r = 1; r + 1 < rings; ++r)
    for (int k = 0; k < seg; ++k) faceBoxUv({V(r, k), V(r + 1, k), V(r + 1, k + 1), V(r, k + 1)}, matId, 1u);
  for (int k = 0; k < seg; ++k) faceBoxUv({V(rings - 1, k), bottom, V(rings - 1, k + 1)}, matId, 1u);
}

void MeshBuilder::quad2(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d, int matId) {
  int i0 = m_.addVertex(a), i1 = m_.addVertex(b), i2 = m_.addVertex(c), i3 = m_.addVertex(d);
  int t0 = m_.addUv(Vec2(0, 0)), t1 = m_.addUv(Vec2(1, 0)), t2 = m_.addUv(Vec2(1, 1)), t3 = m_.addUv(Vec2(0, 1));
  // Faceted: the two sides share vertices, so smoothing them together would cancel the normals.
  m_.addFace({i0, i1, i2, i3}, {t0, t1, t2, t3}, 0u, matId);
  m_.addFace({i3, i2, i1, i0}, {t3, t2, t1, t0}, 0u, matId);
}

void boxMapUvs(PolyMesh& m, float scale) {
  m.uvs.clear();
  for (auto& f : m.faces) {
    Vec3 n = m.faceNormal(f);
    n = length2(n) > 0 ? normalize(n) : Vec3(0, 0, 1);
    f.uv.clear();
    for (int v : f.v) f.uv.push_back(m.addUv(boxUv(m.positions[v], n, scale)));
  }
}

}  // namespace veraa
