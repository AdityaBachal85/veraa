#include "ik.h"

namespace veraa {

bool solveFabrik(std::vector<Vec3>& j, const Vec3& goal, int iterations, float tolerance) {
  size_t n = j.size();
  if (n < 2) return false;
  std::vector<float> d(n - 1);
  float total = 0.0f;
  for (size_t i = 0; i + 1 < n; ++i) {
    d[i] = length(j[i + 1] - j[i]);
    total += d[i];
  }
  Vec3 root = j[0];
  if (length(goal - root) >= total) {
    // Out of reach: stretch towards the goal.
    for (size_t i = 0; i + 1 < n; ++i) j[i + 1] = j[i] + normalize(goal - j[i]) * d[i];
    return length(j.back() - goal) <= tolerance;
  }
  for (int it = 0; it < iterations; ++it) {
    if (length(j.back() - goal) <= tolerance) return true;
    j[n - 1] = goal;
    for (size_t i = n - 1; i-- > 0;) {
      Vec3 dir = j[i] - j[i + 1];
      if (length2(dir) < 1e-20f) dir = Vec3(1, 0, 0);
      j[i] = j[i + 1] + normalize(dir) * d[i];
    }
    j[0] = root;
    for (size_t i = 0; i + 1 < n; ++i) {
      Vec3 dir = j[i + 1] - j[i];
      if (length2(dir) < 1e-20f) dir = Vec3(1, 0, 0);
      j[i + 1] = j[i] + normalize(dir) * d[i];
    }
  }
  return length(j.back() - goal) <= tolerance;
}

bool solveTwoBone(const Vec3& root, Vec3& mid, Vec3& end, const Vec3& goal, const Vec3& poleHint) {
  float a = length(mid - root), b = length(end - mid);
  if (a < 1e-9f || b < 1e-9f) return false;
  Vec3 toGoal = goal - root;
  float c = clampf(length(toGoal), std::fabs(a - b) + 1e-5f, a + b - 1e-5f);
  Vec3 dir = length2(toGoal) > 1e-20f ? normalize(toGoal) : normalize(end - root);
  // Bend direction: the current mid joint's offset from the root-goal line.
  Vec3 bend = (mid - root) - dir * dot(mid - root, dir);
  if (length2(bend) < 1e-12f) bend = poleHint - dir * dot(poleHint, dir);
  if (length2(bend) < 1e-12f) bend = std::fabs(dir.z) < 0.9f ? cross(dir, Vec3(0, 0, 1)) : cross(dir, Vec3(1, 0, 0));
  bend = normalize(bend);
  float cosA = clampf((a * a + c * c - b * b) / (2 * a * c), -1.0f, 1.0f);
  float sinA = std::sqrt(std::max(0.0f, 1.0f - cosA * cosA));
  mid = root + dir * (a * cosA) + bend * (a * sinA);
  end = root + dir * c;
  return length(toGoal) <= a + b;
}

void applySwivel(std::vector<Vec3>& j, float deg) {
  if (j.size() < 3 || std::fabs(deg) < 1e-6f) return;
  Vec3 o = j.front();
  Vec3 axis = j.back() - o;
  if (length2(axis) < 1e-20f) return;
  Mat4 r = Mat4::rotateAxis(normalize(axis), radians(deg));
  for (size_t i = 1; i + 1 < j.size(); ++i) j[i] = o + r.transformDir(j[i] - o);
}

Mat4 rotationBetween(const Vec3& aIn, const Vec3& bIn) {
  Vec3 a = normalize(aIn), b = normalize(bIn);
  Vec3 axis = cross(a, b);
  float s = length(axis), c = dot(a, b);
  if (s < 1e-7f) {
    if (c > 0) return Mat4::identity();
    Vec3 perp = std::fabs(a.x) < 0.9f ? cross(a, Vec3(1, 0, 0)) : cross(a, Vec3(0, 1, 0));
    return Mat4::rotateAxis(normalize(perp), kPi);
  }
  return Mat4::rotateAxis(axis / s, std::atan2(s, c));
}

}  // namespace veraa
