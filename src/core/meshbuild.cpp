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

}  // namespace veraa
