#include "vmath.h"

namespace veraa {

Mat4 Mat4::inverse() const {
  const float* a = &m[0][0];
  float inv[16];
  inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] +
           a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
  inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] -
           a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
  inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] +
           a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
  inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] -
            a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
  inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] -
           a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
  inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] +
           a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
  inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] -
           a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
  inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] +
            a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
  inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] +
           a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
  inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] -
           a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
  inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] +
            a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
  inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] -
            a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
  inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] -
           a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
  inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] +
           a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
  inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] -
            a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
  inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] +
            a[8] * a[1] * a[6] - a[8] * a[2] * a[5];

  float det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
  Mat4 r;
  if (std::fabs(det) < 1e-30f) return r;
  float invDet = 1.0f / det;
  float* o = &r.m[0][0];
  for (int i = 0; i < 16; ++i) o[i] = inv[i] * invDet;
  return r;
}

Vec3 eulerDegFromMatrix(const Mat4& in) {
  // Remove any scale so only rotation remains.
  Vec3 c0 = normalize(in.column(0)), c1 = normalize(in.column(1)), c2 = normalize(in.column(2));
  float r00 = c0.x, r10 = c0.y, r20 = c0.z;
  float r11 = c1.y, r21 = c1.z;
  float r22 = c2.z;
  float x, y, z;
  float sy = -r20;
  // Gimbal lock: only x - z (or x + z) is defined, so put all of it into x.
  if (sy > 0.99999f) {
    y = kPi * 0.5f; z = 0.0f; x = std::atan2(c1.x, r11);
  } else if (sy < -0.99999f) {
    y = -kPi * 0.5f; z = 0.0f; x = std::atan2(-c1.x, r11);
  } else {
    y = std::asin(sy);
    x = std::atan2(r21, r22);
    z = std::atan2(r10, r00);
  }
  return {degrees(x), degrees(y), degrees(z)};
}

Aabb Aabb::transformed(const Mat4& mat) const {
  Aabb r;
  if (!valid()) return r;
  for (int i = 0; i < 8; ++i) {
    Vec3 p((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z);
    r.expand(mat.transformPoint(p));
  }
  return r;
}

bool Aabb::intersect(const Ray& r, float tMin, float tMax, float* tHit) const {
  for (int a = 0; a < 3; ++a) {
    float invD = 1.0f / r.d[a];
    float t0 = (lo[a] - r.o[a]) * invD;
    float t1 = (hi[a] - r.o[a]) * invD;
    if (invD < 0.0f) std::swap(t0, t1);
    tMin = t0 > tMin ? t0 : tMin;
    tMax = t1 < tMax ? t1 : tMax;
    if (tMax < tMin) return false;
  }
  if (tHit) *tHit = tMin;
  return true;
}

}  // namespace veraa
