// Character rigging: bones, IK chain helpers, the Skin, Morpher and Path Deform modifiers.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "modifier.h"
#include "object.h"

namespace veraa {

class Scene;
class Node;

// A bone: a tapered box from the pivot along local +X ("length"), with optional fins.
// Child bones of a chain sit at the end of their parent.
class BoneObject : public GeometryObject {
 public:
  BoneObject();
  std::string type() const override { return "Bone"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<BoneObject>(*this); }
  PolyMesh build() const override;
};

// IK chain helper: the node's position is the goal of the chain from startBone to endBone
// (node ids). Solver 0 = HI (iterative, any length), 1 = Limb (analytic, two bones).
class IKChainObject : public HelperObject {
 public:
  IKChainObject();
  std::string type() const override { return "IKChain"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<IKChainObject>(*this); }
};

// Length of a bone-like node along its local X axis: the Bone length, the distance to its
// first child, or the size of its bounds.
float boneLength(const Scene& scene, const Node& n);

struct SkinBone {
  uint32_t nodeId = 0;
  Mat4 bindWorld;          // bone world matrix in the skin pose
  Vec3 start, end;         // envelope axis in bone space
  float innerRadius = 5.0f, outerRadius = 10.0f;
};

using VertexWeights = std::vector<std::pair<int, float>>;  // (bone index, weight), weights sum to 1

class SkinModifier : public Modifier {
 public:
  SkinModifier();
  std::string type() const override { return "Skin"; }
  std::unique_ptr<Modifier> clone() const override { return std::make_unique<SkinModifier>(*this); }
  void applyState(GeomState& state) const override;
  std::vector<uint32_t> referencedNodes() const override;
  json toJson() const override;
  void fromJson(const json& j) override;

  // Adds a bone using its current pose as the skin pose. meshNode is the skinned node.
  int addBone(const Scene& scene, uint32_t boneId, const Node& meshNode);
  void removeBone(int index);
  // Captures the current pose of the mesh and all bones as the skin pose.
  void resetSkinPose(const Scene& scene, const Node& meshNode);
  // Per-vertex weights (envelopes, then manual overrides).
  std::vector<VertexWeights> computeWeights(const PolyMesh& bindMesh) const;
  // Sets the weight of one bone on a vertex; other bones are scaled to keep the sum at 1.
  void setVertexWeight(const PolyMesh& bindMesh, int vertex, int bone, float weight);
  void clearOverrides() { overrides.clear(); }

  std::vector<SkinBone> bones;
  Mat4 meshBind;     // skinned node object-to-world in the skin pose
  bool bound = false;
  std::map<int, VertexWeights> overrides;
};

struct MorphChannel {
  std::string name;
  uint32_t sourceNode = 0;
  std::vector<Vec3> positions;  // captured target vertex positions (object space)
};

constexpr int kMorphChannels = 20;

// Blends the mesh towards captured targets with the channel weights ch1..ch20 (percent).
class MorpherModifier : public Modifier {
 public:
  MorpherModifier();
  std::string type() const override { return "Morpher"; }
  std::unique_ptr<Modifier> clone() const override { return std::make_unique<MorpherModifier>(*this); }
  void apply(PolyMesh& mesh) const override;
  json toJson() const override;
  void fromJson(const json& j) override;
  // Captures a target from another node's mesh into channel `index`.
  bool captureTarget(int index, const Node& target);

  std::vector<MorphChannel> channels = std::vector<MorphChannel>(kMorphChannels);
};

// Bends the mesh along a path spline. Parameters: path (shape node id), percent, stretch,
// rotation, twist, axis, flip, worldSpace (follow the path where it is placed in the scene).
class PathDeformModifier : public Modifier {
 public:
  PathDeformModifier();
  std::string type() const override { return "PathDeform"; }
  std::unique_ptr<Modifier> clone() const override { return std::make_unique<PathDeformModifier>(*this); }
  void applyState(GeomState& state) const override;
  std::vector<uint32_t> referencedNodes() const override;
};

// Deforms points along a polyline (shared with tests): coordinate `axis` of each point maps to
// the arc length, the other two to the path's normal frame.
void deformAlongPolyline(std::vector<Vec3>& points, const std::vector<Vec3>& path, int axis, float percent,
                         float stretch, float rotationDeg, float twistDeg, bool flip);

}  // namespace veraa
