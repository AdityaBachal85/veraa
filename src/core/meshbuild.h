// Small helpers for procedural mesh generators (shape modifiers, compound and AEC objects):
// faces with material IDs, smoothing groups and real-world box-mapped UVs.
#pragma once

#include <vector>

#include "mesh.h"

namespace veraa {

class MeshBuilder {
 public:
  explicit MeshBuilder(PolyMesh& mesh, float uvScale = 0.01f) : m_(mesh), uvScale_(uvScale) {}

  PolyMesh& mesh() { return m_; }
  int vertex(const Vec3& p) { return m_.addVertex(p); }
  int uv(const Vec2& t) { return m_.addUv(t); }

  // Face with explicit UVs (same count as v, or empty).
  void face(std::vector<int> v, std::vector<int> uvs, int matId, uint32_t smooth) {
    m_.addFace(std::move(v), std::move(uvs), smooth, matId);
  }
  // Face whose UVs are planar-projected from its own normal (box mapping, uvScale per unit).
  void faceBoxUv(const std::vector<int>& v, int matId, uint32_t smooth);

  // Axis-aligned box [lo, hi] transformed by xf, six faces with box-mapped UVs.
  void box(const Mat4& xf, const Vec3& lo, const Vec3& hi, int matId);
  // Box given by its center, size and transform.
  void boxCentered(const Mat4& xf, const Vec3& center, const Vec3& size, int matId) {
    box(xf, center - size * 0.5f, center + size * 0.5f, matId);
  }
  // Open-ended or capped cylinder between a and b.
  void cylinder(const Vec3& a, const Vec3& b, float radius, int sides, bool caps, int matId);
  // Capped cylinder whose radius goes from r0 at a to r1 at b (r1 = 0 makes a cone).
  void cone(const Vec3& a, const Vec3& b, float r0, float r1, int sides, int matId);
  // Closed UV sphere.
  void sphere(const Vec3& center, float radius, int segments, int matId);
  // Two-sided quad (front and back faces), for leaves and thin panels.
  void quad2(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d, int matId);

 private:
  PolyMesh& m_;
  float uvScale_;
};

// Planar UV of point p for a face with normal n (dominant-axis projection), scaled.
Vec2 boxUv(const Vec3& p, const Vec3& n, float scale);
// Replaces every face's UVs with box-mapped ones (real-world scale).
void boxMapUvs(PolyMesh& m, float scale);

}  // namespace veraa
