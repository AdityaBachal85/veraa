// Small vector/matrix library used everywhere in Veraa.
// World space is Z-up, like 3ds Max: the ground plane is XY.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace veraa {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;
constexpr float kInvPi = 1.0f / kPi;
constexpr float kInfinity = std::numeric_limits<float>::infinity();

inline float radians(float deg) { return deg * (kPi / 180.0f); }
inline float degrees(float rad) { return rad * (180.0f / kPi); }
inline float clampf(float v, float lo, float hi) { return std::min(std::max(v, lo), hi); }
inline float saturate(float v) { return clampf(v, 0.0f, 1.0f); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

struct Vec2 {
  float x = 0, y = 0;
  constexpr Vec2() = default;
  constexpr Vec2(float x_, float y_) : x(x_), y(y_) {}
  Vec2 operator+(const Vec2& o) const { return {x + o.x, y + o.y}; }
  Vec2 operator-(const Vec2& o) const { return {x - o.x, y - o.y}; }
  Vec2 operator*(float s) const { return {x * s, y * s}; }
  Vec2 operator/(float s) const { return {x / s, y / s}; }
  Vec2& operator+=(const Vec2& o) { x += o.x; y += o.y; return *this; }
  bool operator==(const Vec2& o) const { return x == o.x && y == o.y; }
};

struct Vec3 {
  float x = 0, y = 0, z = 0;
  constexpr Vec3() = default;
  constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
  constexpr explicit Vec3(float s) : x(s), y(s), z(s) {}

  float& operator[](int i) { return i == 0 ? x : (i == 1 ? y : z); }
  float operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }

  Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
  Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
  Vec3 operator*(const Vec3& o) const { return {x * o.x, y * o.y, z * o.z}; }
  Vec3 operator/(const Vec3& o) const { return {x / o.x, y / o.y, z / o.z}; }
  Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
  Vec3 operator/(float s) const { float inv = 1.0f / s; return {x * inv, y * inv, z * inv}; }
  Vec3 operator-() const { return {-x, -y, -z}; }
  Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
  Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
  Vec3& operator*=(const Vec3& o) { x *= o.x; y *= o.y; z *= o.z; return *this; }
  Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
  Vec3& operator/=(float s) { float inv = 1.0f / s; x *= inv; y *= inv; z *= inv; return *this; }
  bool operator==(const Vec3& o) const { return x == o.x && y == o.y && z == o.z; }
  bool operator!=(const Vec3& o) const { return !(*this == o); }
};

inline Vec3 operator*(float s, const Vec3& v) { return v * s; }
inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length2(const Vec3& v) { return dot(v, v); }
inline float length(const Vec3& v) { return std::sqrt(dot(v, v)); }
inline Vec3 normalize(const Vec3& v) {
  float l = length(v);
  return l > 1e-20f ? v / l : Vec3(0, 0, 1);
}
inline Vec3 vmin(const Vec3& a, const Vec3& b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
inline Vec3 vmax(const Vec3& a, const Vec3& b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }
inline Vec3 vabs(const Vec3& a) { return {std::fabs(a.x), std::fabs(a.y), std::fabs(a.z)}; }
inline Vec3 lerp(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }
inline float maxComponent(const Vec3& v) { return std::max(v.x, std::max(v.y, v.z)); }
inline float minComponent(const Vec3& v) { return std::min(v.x, std::min(v.y, v.z)); }
inline float luminance(const Vec3& c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }
inline bool isBlack(const Vec3& c) { return c.x <= 0.0f && c.y <= 0.0f && c.z <= 0.0f; }
inline Vec3 clampColor(const Vec3& c, float lo, float hi) {
  return {clampf(c.x, lo, hi), clampf(c.y, lo, hi), clampf(c.z, lo, hi)};
}

// Builds an orthonormal basis (t, b) around unit vector n (Duff et al. 2017).
inline void makeBasis(const Vec3& n, Vec3& t, Vec3& b) {
  float sign = std::copysign(1.0f, n.z);
  float a = -1.0f / (sign + n.z);
  float bb = n.x * n.y * a;
  t = Vec3(1.0f + sign * n.x * n.x * a, sign * bb, -sign * n.x);
  b = Vec3(bb, sign + n.y * n.y * a, -n.y);
}

struct Vec4 {
  float x = 0, y = 0, z = 0, w = 0;
  constexpr Vec4() = default;
  constexpr Vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
  Vec3 xyz() const { return {x, y, z}; }
};

// Row-major 4x4 matrix: m[row][col]. Points are column vectors (p' = M * p).
struct Mat4 {
  float m[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};

  static Mat4 identity() { return Mat4(); }
  static Mat4 translate(const Vec3& t) {
    Mat4 r;
    r.m[0][3] = t.x; r.m[1][3] = t.y; r.m[2][3] = t.z;
    return r;
  }
  static Mat4 scale(const Vec3& s) {
    Mat4 r;
    r.m[0][0] = s.x; r.m[1][1] = s.y; r.m[2][2] = s.z;
    return r;
  }
  static Mat4 rotateX(float rad) {
    Mat4 r; float c = std::cos(rad), s = std::sin(rad);
    r.m[1][1] = c; r.m[1][2] = -s; r.m[2][1] = s; r.m[2][2] = c;
    return r;
  }
  static Mat4 rotateY(float rad) {
    Mat4 r; float c = std::cos(rad), s = std::sin(rad);
    r.m[0][0] = c; r.m[0][2] = s; r.m[2][0] = -s; r.m[2][2] = c;
    return r;
  }
  static Mat4 rotateZ(float rad) {
    Mat4 r; float c = std::cos(rad), s = std::sin(rad);
    r.m[0][0] = c; r.m[0][1] = -s; r.m[1][0] = s; r.m[1][1] = c;
    return r;
  }
  static Mat4 rotateAxis(const Vec3& axisIn, float rad) {
    Vec3 a = normalize(axisIn);
    float c = std::cos(rad), s = std::sin(rad), t = 1 - c;
    Mat4 r;
    r.m[0][0] = t * a.x * a.x + c;       r.m[0][1] = t * a.x * a.y - s * a.z; r.m[0][2] = t * a.x * a.z + s * a.y;
    r.m[1][0] = t * a.x * a.y + s * a.z; r.m[1][1] = t * a.y * a.y + c;       r.m[1][2] = t * a.y * a.z - s * a.x;
    r.m[2][0] = t * a.x * a.z - s * a.y; r.m[2][1] = t * a.y * a.z + s * a.x; r.m[2][2] = t * a.z * a.z + c;
    return r;
  }
  // Euler XYZ in degrees (3ds Max default order: X first, then Y, then Z).
  static Mat4 fromEulerDeg(const Vec3& deg) {
    return rotateZ(radians(deg.z)) * rotateY(radians(deg.y)) * rotateX(radians(deg.x));
  }
  static Mat4 perspective(float fovYRad, float aspect, float zNear, float zFar) {
    Mat4 r;
    float f = 1.0f / std::tan(fovYRad * 0.5f);
    r.m[0][0] = f / aspect; r.m[1][1] = f;
    r.m[2][2] = (zFar + zNear) / (zNear - zFar);
    r.m[2][3] = (2.0f * zFar * zNear) / (zNear - zFar);
    r.m[3][2] = -1.0f; r.m[3][3] = 0.0f;
    return r;
  }
  static Mat4 ortho(float l, float r_, float b, float t, float n, float f) {
    Mat4 r;
    r.m[0][0] = 2.0f / (r_ - l); r.m[1][1] = 2.0f / (t - b); r.m[2][2] = -2.0f / (f - n);
    r.m[0][3] = -(r_ + l) / (r_ - l); r.m[1][3] = -(t + b) / (t - b); r.m[2][3] = -(f + n) / (f - n);
    return r;
  }
  // View matrix: camera at eye looking at target, OpenGL convention (camera looks down -Z).
  static Mat4 lookAt(const Vec3& eye, const Vec3& target, const Vec3& upHint) {
    Vec3 f = normalize(target - eye);
    Vec3 s = cross(f, upHint);
    if (length2(s) < 1e-12f) s = cross(f, std::fabs(f.z) < 0.99f ? Vec3(0, 0, 1) : Vec3(0, 1, 0));
    s = normalize(s);
    Vec3 u = cross(s, f);
    Mat4 r;
    r.m[0][0] = s.x;  r.m[0][1] = s.y;  r.m[0][2] = s.z;  r.m[0][3] = -dot(s, eye);
    r.m[1][0] = u.x;  r.m[1][1] = u.y;  r.m[1][2] = u.z;  r.m[1][3] = -dot(u, eye);
    r.m[2][0] = -f.x; r.m[2][1] = -f.y; r.m[2][2] = -f.z; r.m[2][3] = dot(f, eye);
    return r;
  }

  Mat4 operator*(const Mat4& o) const {
    Mat4 r;
    for (int i = 0; i < 4; ++i)
      for (int j = 0; j < 4; ++j) {
        float s = 0;
        for (int k = 0; k < 4; ++k) s += m[i][k] * o.m[k][j];
        r.m[i][j] = s;
      }
    return r;
  }
  Vec3 transformPoint(const Vec3& p) const {
    return {m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + m[0][3],
            m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + m[1][3],
            m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + m[2][3]};
  }
  // Full projective transform with perspective divide.
  Vec3 transformPointProj(const Vec3& p) const {
    float w = m[3][0] * p.x + m[3][1] * p.y + m[3][2] * p.z + m[3][3];
    Vec3 r = transformPoint(p);
    return std::fabs(w) > 1e-20f ? r / w : r;
  }
  Vec4 transform(const Vec4& p) const {
    return {m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + m[0][3] * p.w,
            m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + m[1][3] * p.w,
            m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + m[2][3] * p.w,
            m[3][0] * p.x + m[3][1] * p.y + m[3][2] * p.z + m[3][3] * p.w};
  }
  Vec3 transformDir(const Vec3& d) const {
    return {m[0][0] * d.x + m[0][1] * d.y + m[0][2] * d.z,
            m[1][0] * d.x + m[1][1] * d.y + m[1][2] * d.z,
            m[2][0] * d.x + m[2][1] * d.y + m[2][2] * d.z};
  }
  // Transforms a normal with the inverse-transpose of the upper 3x3 (pass the inverse matrix).
  Vec3 transformNormalWithInverse(const Vec3& n) const {
    return {m[0][0] * n.x + m[1][0] * n.y + m[2][0] * n.z,
            m[0][1] * n.x + m[1][1] * n.y + m[2][1] * n.z,
            m[0][2] * n.x + m[1][2] * n.y + m[2][2] * n.z};
  }
  Vec3 column(int c) const { return {m[0][c], m[1][c], m[2][c]}; }
  Vec3 translation() const { return column(3); }

  Mat4 transposed() const {
    Mat4 r;
    for (int i = 0; i < 4; ++i)
      for (int j = 0; j < 4; ++j) r.m[i][j] = m[j][i];
    return r;
  }
  Mat4 inverse() const;  // general 4x4 inverse (returns identity if singular)
  float determinant3x3() const {
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
  }
};

// Recovers Euler XYZ angles (degrees) from a pure rotation matrix built with fromEulerDeg.
Vec3 eulerDegFromMatrix(const Mat4& r);

struct Ray {
  Vec3 o, d;
};

struct Aabb {
  Vec3 lo{kInfinity, kInfinity, kInfinity};
  Vec3 hi{-kInfinity, -kInfinity, -kInfinity};

  bool valid() const { return lo.x <= hi.x && lo.y <= hi.y && lo.z <= hi.z; }
  void expand(const Vec3& p) { lo = vmin(lo, p); hi = vmax(hi, p); }
  void expand(const Aabb& b) {
    if (!b.valid()) return;
    lo = vmin(lo, b.lo); hi = vmax(hi, b.hi);
  }
  Vec3 center() const { return (lo + hi) * 0.5f; }
  Vec3 extent() const { return valid() ? hi - lo : Vec3(0.0f); }
  float radius() const { return valid() ? length(hi - lo) * 0.5f : 0.0f; }
  Aabb transformed(const Mat4& m) const;
  // Slab test. Returns true and the entry distance when the ray hits the box within [tMin, tMax].
  bool intersect(const Ray& r, float tMin, float tMax, float* tHit = nullptr) const;
};

}  // namespace veraa
