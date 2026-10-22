// Editable Poly operations on PolyMesh: topology queries, sub-object selection tools and
// the editing operations (extrude, bevel, inset, chamfer, bridge, connect, cut, slice, ...).
//
// Every operation is also reachable through runPolyOp() with a JSON description, so the
// Edit Poly modifier can record operations and replay them on its input mesh.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "mesh.h"
#include "polysel.h"

namespace veraa {

// Edge/face adjacency of a PolyMesh. Rebuild after topology changes.
struct PolyTopology {
  struct Edge {
    int a = 0, b = 0;           // a < b
    std::vector<int> faces;     // faces using the edge
  };
  std::vector<Edge> edges;
  std::unordered_map<uint64_t, int> edgeIndex;
  std::vector<std::vector<int>> faceEdges;  // per face: edge of corner k -> k+1
  std::vector<std::vector<int>> vertFaces;  // per vertex: faces using it
  std::vector<std::vector<int>> vertEdges;  // per vertex: incident edges

  explicit PolyTopology(const PolyMesh& m);
  int edgeOf(int a, int b) const;  // -1 if none
  bool isBoundary(int e) const { return edges[e].faces.size() == 1; }
  int otherVertex(int e, int v) const { return edges[e].a == v ? edges[e].b : edges[e].a; }
};

// ---- Selection helpers ----

// Vertices touched by the selection at its level (vertex/edge/border/face/element).
std::vector<int> selectedVertsAtLevel(const PolyMesh& m, const PolySelection& sel);
// Faces touched by the selection (faces of the selected elements/polygons, or using selected
// vertices/edges).
std::vector<int> facesFromSelection(const PolyMesh& m, const PolySelection& sel);
// Converts the current level's selection into `toLevel` (Ctrl+click on a level button).
void convertSelection(const PolyMesh& m, PolySelection& sel, int toLevel);
void growSelection(const PolyMesh& m, PolySelection& sel);
void shrinkSelection(const PolyMesh& m, PolySelection& sel);
void selectLoop(const PolyMesh& m, PolySelection& sel);  // edge loops through selected edges
void selectRing(const PolyMesh& m, PolySelection& sel);  // edge rings through selected edges
// Faces connected to `face` across edges whose normals differ by less than angleDeg.
std::vector<int> facesByAngle(const PolyMesh& m, int face, float angleDeg);
// Faces of the connected element containing `face` (connected through shared edges).
std::vector<int> elementOf(const PolyMesh& m, int face);
// All edges of the open border loop containing edge (a, b); empty if the edge is not open.
std::vector<uint64_t> borderLoopOf(const PolyMesh& m, int a, int b);
// Faces whose material ID equals id.
std::vector<int> facesWithMaterial(const PolyMesh& m, int id);
std::vector<int> facesWithSmoothingGroup(const PolyMesh& m, int group);  // group 1..32

// Soft selection weights per vertex (1 for selected, falling off to 0 at `falloff`).
std::vector<float> softSelectionWeights(const PolyMesh& m, const PolySelection& sel, const SoftSelection& soft);

// ---- Editing operations ----
// Each returns false (and sets *msg) when nothing could be done. Operations keep face indices of
// untouched faces where possible and update `sel` to the result (like 3ds Max).

// Generic op runner. op = {"op": name, ...params}. Names:
//   select (verts/edges/faces/level), hide, unhideAll, delete,
//   removeVerts, breakVerts, weldVerts {threshold}, targetWeld {from, to}, chamferVerts {amount, open},
//   extrudeVerts {height, width}, connectVerts, removeIsolated,
//   insertVertex {a, b, t}, removeEdges {removeVerts}, splitEdges, extrudeEdges {height},
//   weldEdges {threshold}, chamferEdges {amount, segments, open}, bridgeEdges {segments},
//   connectEdges {segments, pinch, slide}, capBorders, extrudeBorders {height, width},
//   bridgeBorders {segments}, extrudeFaces {height, mode: 0 group / 1 local / 2 by polygon},
//   insetFaces {amount, byPolygon}, outlineFaces {amount}, bevelFaces {height, outline, mode},
//   bridgeFaces {segments}, flipFaces, hingeFaces {angle, segments, a, b}, triangulateFaces,
//   setMaterialId {id}, setSmoothing {group, on}, clearSmoothing, autoSmooth {angle},
//   collapse, tessellate, msmooth {iterations}, makePlanar {axis: -1 best fit, 0..2},
//   relax {amount, iterations, keepBoundary}, slice {point[3], normal[3], selectedOnly, split},
//   cut {face, a, b} (a and b are vertex indices of the face), insertLoop {a, b, t},
//   transform {matrix[16] (row-major object-space), soft {enabled, falloff, pinch, bubble}},
//   moveVerts {verts[], deltas[] (xyz triplets)}, turnEdge {a, b}, weldPairs {pairs: [[from, to], ...]},
//   detachElement, createFace {verts[]}, insertVertexInFace {face, point[3]}, hideUnselected
bool runPolyOp(PolyMesh& m, PolySelection& sel, const json& op, std::string* msg = nullptr);

// Building blocks (also used directly by modifiers and tools).
int insertVertexOnEdge(PolyMesh& m, int a, int b, float t);  // returns new vertex (or -1)
bool splitFace(PolyMesh& m, int face, int va, int vb);         // va, vb must be non-adjacent corners
void removeFaces(PolyMesh& m, std::vector<int> faces);         // also drops unused vertices
// Drops vertices no face uses; returns old->new index map (-1 for removed).
std::vector<int> compactVertices(PolyMesh& m);
void remapSelection(PolySelection& sel, const std::vector<int>& vertMap, const std::vector<int>& faceMap);
std::vector<std::vector<int>> openBorderLoops(const PolyMesh& m);  // hole loops (fill orientation)
// Fills every open border with a polygon (Cap Holes).
int capAllHoles(PolyMesh& m);
// Splits all faces crossing the plane (point p, normal n). Returns number of faces cut.
int slicePlane(PolyMesh& m, const Vec3& p, const Vec3& n, const std::vector<int>* onlyFaces, bool split);
// Extracts faces into a new mesh (only the vertices they use).
PolyMesh extractFaces(const PolyMesh& m, const std::vector<int>& faces);
// Gives faces without UVs box-projected UVs when the mesh uses UVs elsewhere.
void ensureFaceUvs(PolyMesh& m);
// Polylines (in mesh space) for selected edges, chained into continuous runs.
std::vector<std::vector<Vec3>> edgeChains(const PolyMesh& m, const std::vector<uint64_t>& edges,
                                          std::vector<bool>* closedOut = nullptr);
// Assigns smoothing groups so faces meeting at less than angleDeg are smoothed together.
void autoSmooth(PolyMesh& m, const std::vector<int>* faces, float angleDeg);
// Triangulation of one polygon as corner indices (3 per triangle; handles concave polygons).
std::vector<int> faceTriangles(const PolyMesh& m, const PolyMesh::Face& f);
// Signed volume (for closed meshes) via triangulation.
double meshVolume(const PolyMesh& m);
// Per-vertex weights a sub-object selection passes up the modifier stack: empty at object
// level (everything is affected), otherwise the (soft) selection weights.
std::vector<float> stackSelectionWeights(const PolyMesh& m, const PolySelection& sel);
bool isClosedManifold(const PolyMesh& m);

}  // namespace veraa
