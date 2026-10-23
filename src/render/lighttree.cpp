#include "lighttree.h"

#include <algorithm>

#include "environment.h"
#include "lights.h"

namespace veraa {
namespace render {

namespace {

constexpr int kMaxSplitDepth = 5;    // 2^5 = kMaxPicks clusters at most
constexpr float kSplitRatio = 0.5f;  // split clusters whose bounding sphere radius exceeds this fraction of the distance
constexpr int kBins = 12;

// Smallest cone containing two normal cones (both spreads and falloffs).
void uniteCone(Vec3& axis, float& thetaO, float& thetaE, Vec3 axisB, float thetaOB, float thetaEB) {
  thetaE = std::max(thetaE, thetaEB);
  if (thetaOB > thetaO) {
    std::swap(axis, axisB);
    std::swap(thetaO, thetaOB);
  }
  float thetaD = std::acos(clampf(dot(axis, axisB), -1.0f, 1.0f));
  if (std::min(thetaD + thetaOB, kPi) <= thetaO) return;
  float o = (thetaO + thetaD + thetaOB) * 0.5f;
  Vec3 w = cross(axis, axisB);
  float wl = length(w);
  if (o >= kPi || wl < 1e-6f) {
    thetaO = kPi;
    return;
  }
  // Rotate axis toward axisB so the new cone touches both.
  w = w / wl;
  float rot = o - thetaO, c = std::cos(rot), s = std::sin(rot);
  axis = normalize(axis * c + cross(w, axis) * s + w * (dot(w, axis) * (1.0f - c)));
  thetaO = o;
}

LightTree::Bounds unite(const LightTree::Bounds& a, const LightTree::Bounds& b) {
  LightTree::Bounds r = a;
  r.box.expand(b.box);
  uniteCone(r.axis, r.thetaO, r.thetaE, b.axis, b.thetaO, b.thetaE);
  r.energy += b.energy;
  return r;
}

// Orientation measure of a normal cone (Conty Estevez and Kulla 2018), for the build heuristic.
float orientationMeasure(float thetaO, float thetaE) {
  float thetaW = std::min(thetaO + thetaE, kPi);
  float s = std::sin(thetaO), c = std::cos(thetaO);
  return kTwoPi * (1.0f - c) + 0.5f * kPi * (2.0f * thetaW * s - std::cos(thetaO - 2.0f * thetaW) - 2.0f * thetaO * s + c);
}

float surfaceArea(const Aabb& b) {
  Vec3 e = b.extent();
  return 2.0f * (e.x * e.y + e.y * e.z + e.z * e.x);
}

}  // namespace

bool LightTree::lightBounds(const Light& l, const Environment* env, Bounds& b) {
  b = Bounds();
  switch (l.type) {
    case Light::Type::Rect:
    case Light::Type::Disc:
    case Light::Type::Portal:
      for (int i = 0; i < 4; ++i) b.box.expand(l.position + l.ex * ((i & 1) ? 1.0f : -1.0f) + l.ey * ((i & 2) ? 1.0f : -1.0f));
      b.axis = normalize(l.n);
      b.thetaO = l.doubleSided ? kPi : 0.0f;
      break;
    case Light::Type::Sphere:
    case Light::Type::Omni:
    case Light::Type::Ies:
      b.box.expand(l.position - Vec3(l.radius));
      b.box.expand(l.position + Vec3(l.radius));
      break;
    case Light::Type::Spot:
      b.box.expand(l.position - Vec3(l.radius));
      b.box.expand(l.position + Vec3(l.radius));
      b.axis = normalize(l.dir);
      b.thetaO = std::acos(clampf(l.cosFall, -1.0f, 1.0f));
      b.thetaE = 0.0f;
      break;
    case Light::Type::Mesh: {
      if (l.tri.empty()) return false;
      bool first = true;
      for (size_t t = 0; t + 2 < l.tri.size(); t += 3) {
        const Vec3 &p0 = l.tri[t], &p1 = l.tri[t + 1], &p2 = l.tri[t + 2];
        b.box.expand(p0);
        b.box.expand(p1);
        b.box.expand(p2);
        if (l.doubleSided) continue;
        Vec3 ng = cross(p1 - p0, p2 - p0);
        if (length2(ng) <= 0.0f) continue;
        ng = normalize(ng);
        if (first) {
          b.axis = ng;
          b.thetaO = 0.0f;
          first = false;
        } else if (b.thetaO < kPi) {
          uniteCone(b.axis, b.thetaO, b.thetaE, ng, 0.0f, kPi * 0.5f);
        }
      }
      if (l.doubleSided || first) b.thetaO = kPi;
      break;
    }
    case Light::Type::Sun: return false;
  }
  float power = l.type == Light::Type::Portal ? (env ? env->averageLuminance() : 0.0f) * l.area * kPi : l.power();
  b.energy = std::isfinite(power) ? std::max(power, 0.0f) : 0.0f;
  return b.box.valid();
}

void LightTree::clear() {
  nodes_.clear();
  leafOf_.clear();
}

void LightTree::build(const std::vector<Light>& lights, const std::vector<int>& indices, const Environment* env) {
  clear();
  leafOf_.assign(lights.size(), -1);
  std::vector<std::pair<int, Bounds>> items;
  for (int i : indices) {
    Bounds b;
    if (lightBounds(lights[i], env, b) && b.energy > 0.0f) items.push_back({i, b});
  }
  if (items.empty()) return;
  nodes_.reserve(items.size() * 2);
  buildRange(items, 0, static_cast<int>(items.size()), 0, -1);
}

int LightTree::buildRange(std::vector<std::pair<int, Bounds>>& items, int begin, int end, int depth, int parent) {
  int idx = static_cast<int>(nodes_.size());
  nodes_.emplace_back();
  Bounds b = items[begin].second;
  for (int i = begin + 1; i < end; ++i) b = unite(b, items[i].second);
  nodes_[idx].b = b;
  nodes_[idx].center = b.box.center();
  nodes_[idx].radius = b.box.radius();
  nodes_[idx].cosO = std::cos(b.thetaO);
  nodes_[idx].sinO = std::sin(b.thetaO);
  nodes_[idx].cosE = std::cos(b.thetaE);
  nodes_[idx].parent = parent;
  nodes_[idx].depth = depth;
  if (end - begin == 1) {
    nodes_[idx].light = items[begin].first;
    leafOf_[items[begin].first] = idx;
    return idx;
  }

  // Binned surface area orientation heuristic over the light centers.
  Aabb cb;
  for (int i = begin; i < end; ++i) cb.expand(items[i].second.box.center());
  Vec3 ext = cb.extent();
  float maxExt = maxComponent(ext);
  float areaEps = 1e-6f * std::max(length2(b.box.extent()), 1e-6f);
  int bestAxis = -1, bestSplit = -1;
  float bestCost = kInfinity;
  for (int a = 0; a < 3 && maxExt > 0.0f; ++a) {
    if (ext[a] <= 0.0f) continue;
    Bounds bins[kBins];
    int counts[kBins] = {};
    auto binOf = [&](const Bounds& lb) {
      int k = static_cast<int>((lb.box.center()[a] - cb.lo[a]) / ext[a] * kBins);
      return std::clamp(k, 0, kBins - 1);
    };
    for (int i = begin; i < end; ++i) {
      int k = binOf(items[i].second);
      bins[k] = counts[k] ? unite(bins[k], items[i].second) : items[i].second;
      ++counts[k];
    }
    for (int s = 1; s < kBins; ++s) {
      Bounds l, r;
      int nl = 0, nr = 0;
      for (int k = 0; k < kBins; ++k) {
        if (!counts[k]) continue;
        if (k < s) {
          l = nl ? unite(l, bins[k]) : bins[k];
          nl += counts[k];
        } else {
          r = nr ? unite(r, bins[k]) : bins[k];
          nr += counts[k];
        }
      }
      if (!nl || !nr) continue;
      float cost = (l.energy * (surfaceArea(l.box) + areaEps) * orientationMeasure(l.thetaO, l.thetaE) +
                    r.energy * (surfaceArea(r.box) + areaEps) * orientationMeasure(r.thetaO, r.thetaE)) *
                   (maxExt / ext[a]);
      if (cost < bestCost) {
        bestCost = cost;
        bestAxis = a;
        bestSplit = s;
      }
    }
  }
  int mid;
  if (bestAxis >= 0) {
    auto it = std::partition(items.begin() + begin, items.begin() + end, [&](const std::pair<int, Bounds>& e) {
      int k = static_cast<int>((e.second.box.center()[bestAxis] - cb.lo[bestAxis]) / ext[bestAxis] * kBins);
      return std::clamp(k, 0, kBins - 1) < bestSplit;
    });
    mid = static_cast<int>(it - items.begin());
  } else {
    mid = begin;
  }
  if (mid <= begin || mid >= end) mid = (begin + end) / 2;  // coincident centers: split the list
  int c0 = buildRange(items, begin, mid, depth + 1, idx);
  int c1 = buildRange(items, mid, end, depth + 1, idx);
  nodes_[idx].child[0] = c0;
  nodes_[idx].child[1] = c1;
  return idx;
}

float LightTree::importance(const Node& nd, const Vec3& p, const Vec3& n, bool twoSided) const {
  // Conservative estimate of the light the cluster delivers (Conty Estevez and Kulla 2018), in
  // the cosine domain: angles are subtracted with cos(a - b) = cos a cos b + sin a sin b.
  if (!(nd.b.energy > 0.0f)) return 0.0f;
  Vec3 v = p - nd.center;
  float d2 = length2(v);
  float r = nd.radius;
  // Half-angle the cluster's bounding sphere subtends (theta_u); inside it everything is possible.
  bool inside = d2 <= r * r;
  float sinU = inside ? 1.0f : r / std::sqrt(d2), cosU = inside ? -1.0f : std::sqrt(std::max(0.0f, 1.0f - sinU * sinU));
  Vec3 dir = d2 > 0.0f ? v / std::sqrt(d2) : nd.b.axis;  // from the cluster toward the receiver
  // Emission: theta' = max(0, theta - theta_o - theta_u) must stay within theta_e.
  float cosE = 1.0f;
  if (!inside) {
    float cosT = clampf(dot(nd.b.axis, dir), -1.0f, 1.0f), sinT = std::sqrt(std::max(0.0f, 1.0f - cosT * cosT));
    float cosX = 1.0f, sinX = 0.0f;  // theta - theta_o
    if (cosT < nd.cosO) {
      cosX = cosT * nd.cosO + sinT * nd.sinO;
      sinX = sinT * nd.cosO - cosT * nd.sinO;
    }
    if (cosX < cosU) cosE = cosX * cosU + sinX * sinU;  // minus theta_u
    if (cosE < nd.cosE - 1e-6f) return 0.0f;
    cosE = std::max(cosE, 0.0f);
  }
  float cosI = 1.0f;
  if (length2(n) > 0.0f && !inside) {
    float c = -dot(n, dir);
    if (twoSided) c = std::fabs(c);
    c = clampf(c, -1.0f, 1.0f);
    if (c < cosU) {
      float s = std::sqrt(std::max(0.0f, 1.0f - c * c));
      cosI = std::max(c * cosU + s * sinU, 0.0f);
    }
  }
  float dist2 = std::max(d2, 0.25f * r * r);
  if (!(dist2 > 0.0f)) dist2 = 1e-12f;
  return nd.b.energy * cosE * cosI / dist2;
}

bool LightTree::split(const Node& nd, const Vec3& p) const {
  if (nd.light >= 0 || nd.depth >= kMaxSplitDepth) return false;
  return nd.radius * nd.radius > kSplitRatio * kSplitRatio * length2(p - nd.center);
}

int LightTree::pick(const Vec3& p, const Vec3& n, bool twoSided, float u, LightPick* out) const {
  if (nodes_.empty()) return 0;
  struct Item {
    int node;
    float u, prob;
  };
  Item stack[2 * kMaxPicks + 2];
  int sp = 0, count = 0;
  stack[sp++] = {0, u, 1.0f};
  while (sp > 0) {
    Item it = stack[--sp];
    const Node& nd = nodes_[it.node];
    if (nd.light >= 0) {
      if (count < kMaxPicks) out[count++] = {nd.light, it.prob};
      continue;
    }
    if (split(nd, p)) {
      stack[sp++] = {nd.child[0], it.u, it.prob};
      stack[sp++] = {nd.child[1], it.u, it.prob};
      continue;
    }
    float i0 = importance(nodes_[nd.child[0]], p, n, twoSided);
    float i1 = importance(nodes_[nd.child[1]], p, n, twoSided);
    float s = i0 + i1;
    if (!(s > 0.0f) || !std::isfinite(s)) continue;
    float p0 = i0 / s;
    if (it.u < p0) stack[sp++] = {nd.child[0], std::min(it.u / p0, 0.99999994f), it.prob * p0};
    else stack[sp++] = {nd.child[1], std::min((it.u - p0) / (1.0f - p0), 0.99999994f), it.prob * (i1 / s)};
  }
  return count;
}

float LightTree::pmf(const Vec3& p, const Vec3& n, bool twoSided, int light) const {
  if (light < 0 || light >= static_cast<int>(leafOf_.size())) return 0.0f;
  int nd = leafOf_[light];
  if (nd < 0) return 0.0f;
  float prob = 1.0f;
  while (nodes_[nd].parent >= 0) {
    int par = nodes_[nd].parent;
    const Node& pn = nodes_[par];
    if (!split(pn, p)) {
      float i0 = importance(nodes_[pn.child[0]], p, n, twoSided);
      float i1 = importance(nodes_[pn.child[1]], p, n, twoSided);
      float s = i0 + i1;
      if (!(s > 0.0f) || !std::isfinite(s)) return 0.0f;
      prob *= (pn.child[0] == nd ? i0 : i1) / s;
    }
    nd = par;
  }
  return prob;
}

}  // namespace render
}  // namespace veraa
