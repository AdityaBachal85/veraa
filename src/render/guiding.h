// Path guiding: learns the incident radiance field while rendering and samples directions
// from it at diffuse and rough glossy vertices.
//
// The field is stored in a spatio-directional tree (SD-tree) as described by Mueller, Gross and
// Novak, "Practical Path Guiding for Efficient Light-Transport Simulation" (EGSR 2017): a binary
// tree over space whose leaves hold quadtrees over the sphere of directions. Training happens in
// iterations of doubling length; after each one the distributions learned from the collected
// radiance become the sampling distributions and both trees are refined where they received
// enough energy. Records are spread over the neighboring cells with the stochastic box filter,
// and the probability of sampling the guide instead of the BSDF is learned per region by
// minimizing a KL divergence, both from Mueller's "Practical Path Guiding in Production"
// (SIGGRAPH 2019 course notes). Learned distributions keep a small uniform share so directions
// that sparse records missed are never starved.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "common.h"

namespace veraa {
namespace render {

// Distribution over the sphere: a quadtree on the cylindrical square (u = (cos theta + 1) / 2,
// v = phi / 2 pi), which maps equal areas to equal solid angles (the square covers 4 pi).
class DirectionTree {
 public:
  struct Node {
    float sum[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // energy of the quadrants (x = bit 0, y = bit 1)
    uint32_t child[4] = {0, 0, 0, 0};          // 0 = the quadrant is a leaf
  };

  DirectionTree() : nodes_(1) {}
  bool valid() const { return total_ > 0.0f; }
  float total() const { return total_; }
  size_t nodeCount() const { return nodes_.size(); }
  int depth() const { return depth_; }

  // Solid-angle density of sample() for a world direction (0 when invalid).
  float pdf(const Vec3& dir) const;
  // Direction proportional to the learned energy; pdf is the solid-angle density.
  Vec3 sample(Rng& rng, float& pdf) const;

  // Topology for collecting the next distribution: quadrants that hold more than `threshold`
  // of the energy of `prev` are subdivided (up to maxDepth levels). Sums are zero.
  void refineFrom(const DirectionTree& prev, float threshold, int maxDepth);
  // Leaf quadrant (node * 4 + quadrant) containing a point of the square.
  uint32_t leafSlot(Vec2 p) const;
  // Takes the leaf energies (node * 4 + quadrant), adds `prior` times their total spread
  // uniformly over the sphere and sums them up the tree.
  void setLeafSums(const float* slots, float prior = 0.0f);

  static Vec2 toSquare(const Vec3& dir);
  static Vec3 fromSquare(const Vec2& p);

 private:
  float sumUp(uint32_t node, int depth);
  std::vector<Node> nodes_;
  float total_ = 0.0f;
  int depth_ = 1;
};

class PathGuide {
 public:
  // A spatial leaf: the learned distribution and the one being collected.
  struct Region {
    DirectionTree sampling;  // read-only while rendering
    DirectionTree building;  // topology of the distribution being collected
    std::unique_ptr<std::atomic<float>[]> energy;  // building leaf sums, 4 per building node
    std::atomic<uint32_t> samples{0};
    uint32_t trainedWith = 0;  // records behind `sampling`
    // Probability of sampling the learned distribution instead of the BSDF. It is learned per
    // region by minimizing the KL divergence from the product of BSDF and incident radiance to
    // the mixture density (Adam on its logit), so regions where guiding does not pay off fall
    // back to BSDF sampling.
    float selectProb = 0.5f;
    float logit = 0.0f, adamM = 0.0f, adamV = 0.0f;
    int adamSteps = 0;
    mutable std::atomic<float> gradSum{0.0f}, gradWeight{0.0f};
    void resetEnergy();
  };

  PathGuide();
  ~PathGuide();

  // Starts empty over the scene bounds (made cubic and slightly enlarged); every region starts
  // sampling the guide with probability selectProb.
  void reset(const Aabb& bounds, float selectProb = 0.5f);
  // True once a trained distribution exists (after the first update()).
  bool ready() const { return iteration_ > 0; }
  int iteration() const { return iteration_; }
  size_t regionCount() const { return regions_.size(); }

  // Spatial leaf containing p (points outside the bounds use the nearest leaf).
  const Region* region(const Vec3& p) const;
  // Distribution of a region usable for sampling.
  static bool canSample(const Region* r) { return r && r->sampling.valid(); }
  static Vec3 sample(const Region* r, Rng& rng, float& pdf) { return r->sampling.sample(rng, pdf); }
  static float pdf(const Region* r, const Vec3& dir) { return r->sampling.pdf(dir); }

  // Adds one radiance sample: incident radiance from dir at p divided by the density the
  // direction was sampled with. Thread-safe; call only while training.
  void record(const Vec3& p, const Vec3& dir, float value, Rng& rng);
  // Adds the gradient of the selection probability from one vertex that sampled the mixture:
  // product = BSDF * incident radiance in the sampled direction, q = the mixture density,
  // guidePdf/bsdfPdf = the two strategies' densities, mass = the lobe share the probability was
  // scaled by. Thread-safe.
  static void addSelectionGradient(const Region* r, float product, float q, float guidePdf, float bsdfPdf, float mass);
  // One optimizer step of every region's selection probability (between training passes).
  void adaptSelection();

  // Ends a training iteration: the collected distributions become the sampling distributions,
  // then space and directions are refined for the next iteration. Not thread-safe (call
  // between passes).
  void update();

 private:
  struct SpatialNode {
    int child[2] = {-1, -1};
    int region = -1;  // leaf when >= 0
    uint8_t axis = 0;
  };
  int leafIndex(Vec3 p, Vec3* size) const;
  void splitLeaf(int node, uint32_t threshold, int depth);

  Vec3 origin_, extent_;
  std::vector<SpatialNode> nodes_;
  std::vector<std::unique_ptr<Region>> regions_;
  int iteration_ = 0;
  float initialSelect_ = 0.5f;
};

}  // namespace render
}  // namespace veraa
