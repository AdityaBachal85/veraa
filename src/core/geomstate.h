// The geometry flowing up a modifier stack, plus where and when it is being evaluated.
#pragma once

#include <cstdint>
#include <vector>

#include "mesh.h"
#include "shape.h"

namespace veraa {

class Scene;

// What flows up a modifier stack: a mesh, or splines (shapes). Modifiers like Extrude or
// Lathe turn splines into a mesh by filling `mesh` and setting isShape = false.
struct GeomState {
  PolyMesh mesh;
  BezierShape shape;
  bool isShape = false;
  double time = 0.0;              // scene time in frames (for animated modifiers)
  const Scene* scene = nullptr;   // scene being evaluated (for modifiers that read other nodes)
  uint32_t nodeId = 0;            // node being evaluated
  // Spline interpolation of the base shape (its "Steps" and "Optimize" parameters), used by
  // modifiers that turn splines into meshes.
  int shapeSteps = 6;
  bool shapeOptimize = true;
  // Sub-object selection passed up the stack (per mesh vertex, 0..1). Empty = whole mesh.
  // Set by Editable Poly sub-object levels and the selection modifiers; deformers only move
  // selected vertices (weighted for soft selections).
  std::vector<float> selWeights;
};

}  // namespace veraa
