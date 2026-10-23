// Many-light sampling with a light bounding volume hierarchy, after Conty Estevez and Kulla,
// "Importance Sampling of Many Lights with Adaptive Tree Splitting" (HPG 2018). Every node
// bounds its lights' positions (box), emission directions (normal cone with spread theta_o and
// falloff theta_e) and power. A shading point descends the tree choosing children in proportion
// to a conservative estimate of the light they deliver; clusters that are large as seen from the
// point are split, so nearby lights are sampled separately while far clusters cost one sample.
#pragma once

#include <vector>

#include "common.h"

namespace veraa {
namespace render {

struct Light;
class Environment;

struct LightPick {
  int light = -1;
  float prob = 0.0f;  // probability of this pick (the product of the choices on its path)
};

class LightTree {
 public:
  static constexpr int kMaxPicks = 32;  // splitting stops below depth 5

  // Builds the tree over lights[i] for i in `indices` (local lights; distant ones are sampled
  // separately). env gives portals their power.
  void build(const std::vector<Light>& lights, const std::vector<int>& indices, const Environment* env);
  void clear();
  bool empty() const { return nodes_.empty(); }

  // Picks lights for a receiver at p with geometric normal n (zero for points in a volume);
  // twoSided receivers also take light from behind. Returns the number of picks written.
  int pick(const Vec3& p, const Vec3& n, bool twoSided, float u, LightPick* out) const;
  // Probability that pick() returns `light` for this receiver (0 for lights not in the tree).
  float pmf(const Vec3& p, const Vec3& n, bool twoSided, int light) const;

  // Bounds of one light (also used by tests). Returns false for lights that cannot be bounded.
  struct Bounds {
    Aabb box;
    Vec3 axis{0, 0, 1};
    float thetaO = kPi;         // spread of the emission normals around axis
    float thetaE = kPi * 0.5f;  // emission falloff beyond the normals
    float energy = 0.0f;
  };
  static bool lightBounds(const Light& l, const Environment* env, Bounds& b);

 private:
  struct Node {
    Bounds b;
    // Precomputed for importance(): bounding sphere and the cone angles' cosines and sines.
    Vec3 center;
    float radius = 0.0f, cosO = -1.0f, sinO = 0.0f, cosE = 0.0f;
    int child[2] = {-1, -1};
    int light = -1;  // leaf
    int parent = -1;
    int depth = 0;
  };
  float importance(const Node& nd, const Vec3& p, const Vec3& n, bool twoSided) const;
  bool split(const Node& nd, const Vec3& p) const;
  int buildRange(std::vector<std::pair<int, Bounds>>& items, int begin, int end, int depth, int parent);

  std::vector<Node> nodes_;
  std::vector<int> leafOf_;  // light index -> leaf node (-1 when not in the tree)
};

}  // namespace render
}  // namespace veraa
