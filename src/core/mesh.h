// Polygon meshes (what objects and modifiers produce) and triangle meshes (what the
// viewport and renderer draw).
#pragma once

#include <cstdint>
#include <vector>

#include "vmath.h"

namespace veraa {

// N-gon mesh with face-varying UVs, material IDs and smoothing groups, like 3ds Max's
// Editable Poly. Faces are wound counter-clockwise when seen from the front.
struct PolyMesh {
  struct Face {
    std::vector<int> v;   // indices into positions
    std::vector<int> uv;  // indices into uvs (same length as v, or empty)
    int matId = 0;        // material sub-ID (for multi-materials later)
    uint32_t smoothGroups = 1;  // bit mask; faces sharing a bit are smoothed together, 0 = faceted
  };

  std::vector<Vec3> positions;
  std::vector<Vec2> uvs;
  std::vector<Face> faces;

  bool empty() const { return faces.empty(); }
  void clear() { positions.clear(); uvs.clear(); faces.clear(); }
  Aabb bounds() const;
  size_t triangleCount() const;

  int addVertex(const Vec3& p) { positions.push_back(p); return static_cast<int>(positions.size()) - 1; }
  int addUv(const Vec2& t) { uvs.push_back(t); return static_cast<int>(uvs.size()) - 1; }
  void addFace(std::vector<int> v, std::vector<int> uv = {}, uint32_t smooth = 1, int matId = 0);
  // Appends another mesh (indices are offset).
  void append(const PolyMesh& other);
  void transform(const Mat4& m);
  void flipFaces();
  // Face normal via Newell's method (works for non-planar n-gons).
  Vec3 faceNormal(const Face& f) const;
  // Area-weighted vertex normals ignoring smoothing groups (used by Push/Shell/Noise).
  std::vector<Vec3> vertexNormals() const;
  // True when every face index is in range.
  bool isValid() const;
};

// Render-ready triangles. Vertices are split where normals or UVs differ.
struct TriMesh {
  std::vector<Vec3> positions;
  std::vector<Vec3> normals;
  std::vector<Vec2> uvs;
  std::vector<uint32_t> indices;    // 3 per triangle
  std::vector<int> triMatIds;       // one per triangle
  // Polygon edges for wireframe display (no triangulation diagonals).
  std::vector<Vec3> wirePositions;
  std::vector<uint32_t> wireIndices;  // 2 per edge
  Aabb bounds;

  size_t triangleCount() const { return indices.size() / 3; }
  bool empty() const { return indices.empty(); }
};

// Triangulates (ear clipping for concave polygons) and computes smoothing-group normals.
TriMesh triangulate(const PolyMesh& mesh);

// Ray/triangle-mesh test used for viewport picking. Returns nearest distance or -1.
float intersectTriMesh(const TriMesh& mesh, const Ray& ray);

}  // namespace veraa
