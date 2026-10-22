// Motion blur hook: the animation system reports where a node is at any time (in frames), so
// the renderer can blur moving objects and cameras. Without a provider everything is static.
#pragma once

#include <functional>

#include "vmath.h"

namespace veraa {

class Scene;
class Node;

// Returns true and the node's world matrix (objectWorldMatrix() for geometry, worldMatrix() for
// cameras when wantPivot is true) at `frame`; false when the node does not move.
using NodeMatrixAtTime = std::function<bool(const Scene& scene, const Node& node, double frame, bool wantPivot, Mat4& out)>;

void setNodeMatrixAtTime(NodeMatrixAtTime provider);
bool nodeMatrixAtTime(const Scene& scene, const Node& node, double frame, bool wantPivot, Mat4& out);

}  // namespace veraa
