#include "guiding.h"

#include <algorithm>

namespace veraa {
namespace render {

namespace {

constexpr float kInv4Pi = 1.0f / (4.0f * kPi);
constexpr float kDirThreshold = 0.01f;    // energy fraction above which a quadrant is subdivided
constexpr int kDirMaxDepth = 20;
constexpr size_t kDirMaxNodes = 1 << 15;
constexpr float kSpatialThreshold = 12000.0f;  // records before a region splits (grows with sqrt(2^k))
constexpr int kSpatialMaxDepth = 48;
constexpr size_t kMaxRegions = 1 << 14;
constexpr uint32_t kMinRecords = 64;      // a region needs this many records to learn a distribution
constexpr float kUniformPrior = 0.1f;     // share of a learned distribution spread uniformly

inline void atomicAdd(std::atomic<float>& a, float v) {
  float old = a.load(std::memory_order_relaxed);
  while (!a.compare_exchange_weak(old, old + v, std::memory_order_relaxed)) {
  }
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// DirectionTree

Vec2 DirectionTree::toSquare(const Vec3& dir) {
  constexpr float kBelowOne = 0.99999994f;
  float cosT = clampf(dir.z, -1.0f, 1.0f);
  float phi = std::atan2(dir.y, dir.x);
  if (phi < 0.0f) phi += kTwoPi;
  return {clampf((cosT + 1.0f) * 0.5f, 0.0f, kBelowOne), clampf(phi / kTwoPi, 0.0f, kBelowOne)};
}

Vec3 DirectionTree::fromSquare(const Vec2& p) {
  float cosT = 2.0f * p.x - 1.0f;
  float sinT = std::sqrt(std::max(0.0f, 1.0f - cosT * cosT));
  float phi = kTwoPi * p.y;
  return {sinT * std::cos(phi), sinT * std::sin(phi), cosT};
}

float DirectionTree::pdf(const Vec3& dir) const {
  if (!(total_ > 0.0f)) return 0.0f;
  Vec2 p = toSquare(dir);
  uint32_t n = 0;
  float pdf = 1.0f;
  for (;;) {
    const Node& nd = nodes_[n];
    int qx = p.x >= 0.5f ? 1 : 0, qy = p.y >= 0.5f ? 1 : 0;
    int q = qx + 2 * qy;
    float s = nd.sum[0] + nd.sum[1] + nd.sum[2] + nd.sum[3];
    if (!(s > 0.0f) || !(nd.sum[q] > 0.0f)) return 0.0f;
    pdf *= 4.0f * nd.sum[q] / s;
    if (nd.child[q] == 0) break;
    p.x = 2.0f * p.x - static_cast<float>(qx);
    p.y = 2.0f * p.y - static_cast<float>(qy);
    n = nd.child[q];
  }
  return pdf * kInv4Pi;
}

Vec3 DirectionTree::sample(Rng& rng, float& pdf) const {
  uint32_t n = 0;
  Vec2 origin(0.0f, 0.0f);
  float size = 1.0f, pdfSquare = 1.0f;
  for (;;) {
    const Node& nd = nodes_[n];
    float s = nd.sum[0] + nd.sum[1] + nd.sum[2] + nd.sum[3];
    float u = rng.next() * s;
    int q = 3;
    for (int i = 0; i < 3; ++i) {
      if (u < nd.sum[i]) {
        q = i;
        break;
      }
      u -= nd.sum[i];
    }
    while (q > 0 && !(nd.sum[q] > 0.0f)) --q;  // rounding at the end of the range
    pdfSquare *= 4.0f * nd.sum[q] / s;
    size *= 0.5f;
    origin.x += static_cast<float>(q & 1) * size;
    origin.y += static_cast<float>(q >> 1) * size;
    if (nd.child[q] == 0) break;
    n = nd.child[q];
  }
  Vec2 p(origin.x + rng.next() * size, origin.y + rng.next() * size);
  pdf = pdfSquare * kInv4Pi;
  return fromSquare(p);
}

uint32_t DirectionTree::leafSlot(Vec2 p) const {
  uint32_t n = 0;
  for (;;) {
    const Node& nd = nodes_[n];
    int qx = p.x >= 0.5f ? 1 : 0, qy = p.y >= 0.5f ? 1 : 0;
    int q = qx + 2 * qy;
    if (nd.child[q] == 0) return n * 4 + static_cast<uint32_t>(q);
    p.x = 2.0f * p.x - static_cast<float>(qx);
    p.y = 2.0f * p.y - static_cast<float>(qy);
    n = nd.child[q];
  }
}

float DirectionTree::sumUp(uint32_t node, int depth) {
  float s = 0.0f;
  for (int q = 0; q < 4; ++q) {
    uint32_t c = nodes_[node].child[q];
    if (c != 0 && depth < kDirMaxDepth + 1) nodes_[node].sum[q] = sumUp(c, depth + 1);
    s += nodes_[node].sum[q];
  }
  return s;
}

void DirectionTree::setLeafSums(const float* slots, float prior) {
  for (size_t n = 0; n < nodes_.size(); ++n)
    for (int q = 0; q < 4; ++q) nodes_[n].sum[q] = nodes_[n].child[q] == 0 ? std::max(slots[n * 4 + q], 0.0f) : 0.0f;
  total_ = sumUp(0, 0);
  if (!std::isfinite(total_) || !(total_ > 0.0f)) {
    total_ = 0.0f;
    return;
  }
  if (prior <= 0.0f) return;
  // A share of the energy spread uniformly over the sphere: directions the sparse records missed
  // keep a density, so the learned distribution cannot starve them.
  float spread = total_ * prior;
  std::vector<std::pair<uint32_t, float>> stack{{0u, spread}};
  while (!stack.empty()) {
    auto [n, e] = stack.back();
    stack.pop_back();
    for (int q = 0; q < 4; ++q) {
      if (nodes_[n].child[q] == 0) nodes_[n].sum[q] += e * 0.25f;
      else stack.push_back({nodes_[n].child[q], e * 0.25f});
    }
  }
  total_ = sumUp(0, 0);
}

void DirectionTree::refineFrom(const DirectionTree& prev, float threshold, int maxDepth) {
  nodes_.assign(1, Node());
  depth_ = 1;
  float total = prev.total_;
  struct Item {
    uint32_t dst;
    int src;       // node of prev covering this one, or -1 below prev's leaves
    float energy;  // energy of this node in prev
    int depth;
  };
  std::vector<Item> stack;
  stack.push_back({0, 0, total, 1});
  while (!stack.empty()) {
    Item it = stack.back();
    stack.pop_back();
    depth_ = std::max(depth_, it.depth);
    for (int q = 0; q < 4; ++q) {
      float e = it.src >= 0 ? prev.nodes_[it.src].sum[q] : it.energy * 0.25f;
      float fraction = total > 0.0f ? e / total : std::pow(0.25f, static_cast<float>(it.depth));
      if (it.depth >= maxDepth || !(fraction > threshold) || nodes_.size() >= kDirMaxNodes) continue;
      uint32_t c = static_cast<uint32_t>(nodes_.size());
      nodes_.emplace_back();
      nodes_[it.dst].child[q] = c;
      int src = it.src >= 0 && prev.nodes_[it.src].child[q] != 0 ? static_cast<int>(prev.nodes_[it.src].child[q]) : -1;
      stack.push_back({c, src, e, it.depth + 1});
    }
  }
  total_ = 0.0f;
}

// ---------------------------------------------------------------------------------------------
// PathGuide

void PathGuide::Region::resetEnergy() {
  size_t n = building.nodeCount() * 4;
  energy.reset(new std::atomic<float>[n]);
  for (size_t i = 0; i < n; ++i) energy[i].store(0.0f, std::memory_order_relaxed);
  samples.store(0, std::memory_order_relaxed);
}

PathGuide::PathGuide() = default;
PathGuide::~PathGuide() = default;

void PathGuide::addSelectionGradient(const Region* r, float product, float q, float guidePdf, float bsdfPdf,
                                     float mass) {
  if (!r || !(q > 0.0f) || !std::isfinite(product) || product < 0.0f) return;
  // d/ds KL(p* || q) = -E_q[(p* / q) (dq/ds) / q] with dq/ds = mass (guidePdf - bsdfPdf); p* is the
  // product up to its normalization, which the summed weights estimate.
  float w = product / q;
  float g = -w * mass * (guidePdf - bsdfPdf) / q;
  if (!std::isfinite(g) || !std::isfinite(w)) return;
  atomicAdd(r->gradSum, g);
  atomicAdd(r->gradWeight, w);
}

void PathGuide::adaptSelection() {
  constexpr float kRate = 0.15f, kBeta1 = 0.9f, kBeta2 = 0.999f;
  constexpr float kMinLogit = -4.0f, kMaxLogit = 3.0f;  // probability 0.018 .. 0.95
  for (auto& rp : regions_) {
    Region& r = *rp;
    float g = r.gradSum.exchange(0.0f, std::memory_order_relaxed);
    float w = r.gradWeight.exchange(0.0f, std::memory_order_relaxed);
    if (!(w > 0.0f)) continue;
    // Gradient with respect to the logit (s = sigmoid(logit)).
    float grad = g / w * r.selectProb * (1.0f - r.selectProb);
    ++r.adamSteps;
    r.adamM = kBeta1 * r.adamM + (1.0f - kBeta1) * grad;
    r.adamV = kBeta2 * r.adamV + (1.0f - kBeta2) * grad * grad;
    float mHat = r.adamM / (1.0f - std::pow(kBeta1, static_cast<float>(r.adamSteps)));
    float vHat = r.adamV / (1.0f - std::pow(kBeta2, static_cast<float>(r.adamSteps)));
    r.logit = clampf(r.logit - kRate * mHat / (std::sqrt(vHat) + 1e-8f), kMinLogit, kMaxLogit);
    r.selectProb = 1.0f / (1.0f + std::exp(-r.logit));
  }
}

void PathGuide::reset(const Aabb& bounds, float selectProb) {
  initialSelect_ = clampf(selectProb, 0.02f, 0.95f);
  Vec3 lo(-1000.0f), hi(1000.0f);
  if (bounds.valid()) {
    lo = bounds.lo;
    hi = bounds.hi;
  }
  Vec3 c = (lo + hi) * 0.5f;
  float half = std::max(maxComponent(hi - lo) * 0.5f * 1.01f, 1e-3f);
  origin_ = c - Vec3(half);
  extent_ = Vec3(2.0f * half);
  nodes_.assign(1, SpatialNode());
  nodes_[0].region = 0;
  regions_.clear();
  regions_.push_back(std::make_unique<Region>());
  Region& r = *regions_[0];
  r.building.refineFrom(r.sampling, kDirThreshold, kDirMaxDepth);
  r.resetEnergy();
  r.selectProb = initialSelect_;
  r.logit = std::log(initialSelect_ / (1.0f - initialSelect_));
  iteration_ = 0;
}

int PathGuide::leafIndex(Vec3 p, Vec3* size) const {
  constexpr float kBelowOne = 0.99999994f;
  for (int a = 0; a < 3; ++a) p[a] = clampf((p[a] - origin_[a]) / extent_[a], 0.0f, kBelowOne);
  if (size) *size = extent_;
  int n = 0;
  while (nodes_[n].region < 0) {
    const SpatialNode& nd = nodes_[n];
    int a = nd.axis;
    int c = p[a] >= 0.5f ? 1 : 0;
    p[a] = std::min(2.0f * p[a] - static_cast<float>(c), kBelowOne);
    if (size) (*size)[a] *= 0.5f;
    n = nd.child[c];
  }
  return n;
}

const PathGuide::Region* PathGuide::region(const Vec3& p) const {
  if (nodes_.empty()) return nullptr;
  return regions_[nodes_[leafIndex(p, nullptr)].region].get();
}

void PathGuide::record(const Vec3& p, const Vec3& dir, float value, Rng& rng) {
  if (nodes_.empty() || !std::isfinite(value)) return;
  // Stochastic box filter: the record lands in the cell under a point jittered by one cell size.
  Vec3 size;
  leafIndex(p, &size);
  Vec3 q = p + Vec3((rng.next() - 0.5f) * size.x, (rng.next() - 0.5f) * size.y, (rng.next() - 0.5f) * size.z);
  Region& r = *regions_[nodes_[leafIndex(q, nullptr)].region];
  // Relaxed read-modify-writes without locking: concurrent records may rarely lose an update,
  // which only perturbs the learned distribution (never the image), and avoids contention.
  if (value > 0.0f) {
    std::atomic<float>& e = r.energy[r.building.leafSlot(DirectionTree::toSquare(dir))];
    e.store(e.load(std::memory_order_relaxed) + value, std::memory_order_relaxed);
  }
  r.samples.store(r.samples.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

void PathGuide::splitLeaf(int node, uint32_t threshold, int depth) {
  int ri = nodes_[node].region;
  if (ri < 0) {
    for (int c = 0; c < 2; ++c) splitLeaf(nodes_[node].child[c], threshold, depth + 1);
    return;
  }
  uint32_t n = regions_[ri]->samples.load(std::memory_order_relaxed);
  if (n < threshold || depth >= kSpatialMaxDepth || regions_.size() >= kMaxRegions) return;
  // The region keeps the first half of the cell; a copy takes the second.
  auto copy = std::make_unique<Region>();
  copy->sampling = regions_[ri]->sampling;
  copy->building = regions_[ri]->building;
  copy->trainedWith = regions_[ri]->trainedWith / 2;
  copy->selectProb = regions_[ri]->selectProb;
  copy->logit = regions_[ri]->logit;
  copy->adamM = regions_[ri]->adamM;
  copy->adamV = regions_[ri]->adamV;
  copy->adamSteps = regions_[ri]->adamSteps;
  copy->samples.store(n / 2, std::memory_order_relaxed);
  regions_[ri]->samples.store(n / 2, std::memory_order_relaxed);
  regions_[ri]->trainedWith /= 2;
  int ci = static_cast<int>(regions_.size());
  regions_.push_back(std::move(copy));
  SpatialNode a, b;
  a.region = ri;
  b.region = ci;
  int ia = static_cast<int>(nodes_.size());
  nodes_.push_back(a);
  nodes_.push_back(b);
  nodes_[node].region = -1;
  nodes_[node].axis = static_cast<uint8_t>(depth % 3);
  nodes_[node].child[0] = ia;
  nodes_[node].child[1] = ia + 1;
  splitLeaf(ia, threshold, depth + 1);
  splitLeaf(ia + 1, threshold, depth + 1);
}

void PathGuide::update() {
  if (nodes_.empty()) return;
  // The collected energy becomes the sampling distribution (regions that saw too few records keep
  // what they had learned before).
  std::vector<float> slots;
  for (auto& rp : regions_) {
    Region& r = *rp;
    uint32_t n = r.samples.load(std::memory_order_relaxed);
    if (n < kMinRecords) continue;
    size_t count = r.building.nodeCount() * 4;
    slots.resize(count);
    for (size_t i = 0; i < count; ++i) slots[i] = r.energy[i].load(std::memory_order_relaxed);
    DirectionTree learned = r.building;
    learned.setLeafSums(slots.data(), kUniformPrior);
    if (learned.valid()) {
      r.sampling = std::move(learned);
      r.trainedWith = n;
    }
  }
  // Split regions that received many records; the threshold grows with the iteration length.
  float iterationLength = std::pow(2.0f, static_cast<float>(std::min(iteration_, 30)));
  uint32_t threshold = static_cast<uint32_t>(kSpatialThreshold * std::sqrt(iterationLength));
  splitLeaf(0, threshold, 0);
  // Directions are refined where the learned distribution has its energy.
  for (auto& rp : regions_) {
    rp->building.refineFrom(rp->sampling, kDirThreshold, kDirMaxDepth);
    rp->resetEnergy();
  }
  ++iteration_;
}

}  // namespace render
}  // namespace veraa
