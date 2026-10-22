// Sub-object selection for Editable Poly / Edit Poly: vertices, edges, faces, per level.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "params.h"

namespace veraa {

// Sub-object levels, numbered like the 1-5 hotkeys.
enum PolyLevel : int { PolyObject = 0, PolyVertex = 1, PolyEdge = 2, PolyBorder = 3, PolyFace = 4, PolyElement = 5 };

inline uint64_t edgeKey(int a, int b) {
  if (a > b) std::swap(a, b);
  return (static_cast<uint64_t>(static_cast<uint32_t>(a)) << 32) | static_cast<uint32_t>(b);
}
inline int edgeKeyA(uint64_t k) { return static_cast<int>(k >> 32); }
inline int edgeKeyB(uint64_t k) { return static_cast<int>(k & 0xffffffffu); }

// Soft selection: vertices near the selection follow a transform partially.
struct SoftSelection {
  bool enabled = false;
  float falloff = 20.0f;
  float pinch = 0.0f;
  float bubble = 0.0f;
  bool operator==(const SoftSelection& o) const {
    return enabled == o.enabled && falloff == o.falloff && pinch == o.pinch && bubble == o.bubble;
  }
};

struct PolySelection {
  int level = PolyObject;
  std::vector<int> verts;        // sorted, unique
  std::vector<uint64_t> edges;   // sorted, unique edge keys (Edge and Border levels)
  std::vector<int> faces;        // sorted, unique (Polygon and Element levels)
  std::vector<int> hiddenFaces;  // sorted, unique
  std::vector<int> hiddenVerts;  // sorted, unique
  SoftSelection soft;            // soft selection settings used for transforms and the stack

  bool hasVert(int v) const { return std::binary_search(verts.begin(), verts.end(), v); }
  bool hasEdge(uint64_t k) const { return std::binary_search(edges.begin(), edges.end(), k); }
  bool hasFace(int f) const { return std::binary_search(faces.begin(), faces.end(), f); }
  bool isFaceHidden(int f) const { return std::binary_search(hiddenFaces.begin(), hiddenFaces.end(), f); }
  bool isVertHidden(int v) const { return std::binary_search(hiddenVerts.begin(), hiddenVerts.end(), v); }
  // Selection at the current level is empty.
  bool emptyAtLevel() const;
  void clearLevel(int lvl);
  void normalize();  // sort and unique all lists

  json toJson() const;
  void fromJson(const json& j);
};

template <class T>
void sortUnique(std::vector<T>& v) {
  std::sort(v.begin(), v.end());
  v.erase(std::unique(v.begin(), v.end()), v.end());
}

}  // namespace veraa
