// Polygon meshes (what objects and modifiers produce) and triangle meshes (what the
// viewport and renderer draw).
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <vector>

#include "vmath.h"

namespace veraa {

// A std::vector<int>-like list that stores up to four indices inline, so triangles and quads
// (nearly every face of an imported model) need no heap allocation. Converts to and from
// std::vector<int>.
class IndexList {
 public:
  using value_type = int;
  using size_type = size_t;
  using iterator = int*;
  using const_iterator = const int*;
  using reverse_iterator = std::reverse_iterator<int*>;
  using const_reverse_iterator = std::reverse_iterator<const int*>;
  static constexpr uint32_t kInline = 4;

  IndexList() noexcept {}
  IndexList(std::initializer_list<int> l) { assign(l.begin(), l.end()); }
  IndexList(const std::vector<int>& v) { assign(v.data(), v.data() + v.size()); }  // NOLINT: implicit
  explicit IndexList(size_t n, int value = 0) { resize(n, value); }
  template <typename It>
  IndexList(It first, It last) {
    assign(first, last);
  }
  IndexList(const IndexList& o) { assign(o.begin(), o.end()); }
  IndexList(IndexList&& o) noexcept { moveFrom(o); }
  ~IndexList() { release(); }
  IndexList& operator=(const IndexList& o) {
    if (this != &o) assign(o.begin(), o.end());
    return *this;
  }
  IndexList& operator=(IndexList&& o) noexcept {
    if (this != &o) {
      release();
      moveFrom(o);
    }
    return *this;
  }
  IndexList& operator=(std::initializer_list<int> l) {
    assign(l.begin(), l.end());
    return *this;
  }
  IndexList& operator=(const std::vector<int>& v) {
    assign(v.data(), v.data() + v.size());
    return *this;
  }
  operator std::vector<int>() const { return std::vector<int>(begin(), end()); }  // NOLINT: implicit

  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  size_t capacity() const { return cap_; }
  int* data() { return cap_ > kInline ? heap_ : inline_; }
  const int* data() const { return cap_ > kInline ? heap_ : inline_; }
  int& operator[](size_t i) { return data()[i]; }
  const int& operator[](size_t i) const { return data()[i]; }
  int& at(size_t i) { return data()[i]; }
  const int& at(size_t i) const { return data()[i]; }
  int& front() { return data()[0]; }
  const int& front() const { return data()[0]; }
  int& back() { return data()[size_ - 1]; }
  const int& back() const { return data()[size_ - 1]; }
  int* begin() { return data(); }
  int* end() { return data() + size_; }
  const int* begin() const { return data(); }
  const int* end() const { return data() + size_; }
  const int* cbegin() const { return data(); }
  const int* cend() const { return data() + size_; }
  reverse_iterator rbegin() { return reverse_iterator(end()); }
  reverse_iterator rend() { return reverse_iterator(begin()); }
  const_reverse_iterator rbegin() const { return const_reverse_iterator(end()); }
  const_reverse_iterator rend() const { return const_reverse_iterator(begin()); }

  void clear() { size_ = 0; }
  void reserve(size_t n) {
    if (n > cap_) grow(n);
  }
  void push_back(int v) {
    if (size_ == cap_) grow(cap_ * 2);
    data()[size_++] = v;
  }
  void emplace_back(int v) { push_back(v); }
  void pop_back() { --size_; }
  void resize(size_t n, int value = 0) {
    reserve(n);
    for (size_t i = size_; i < n; ++i) data()[i] = value;
    size_ = static_cast<uint32_t>(n);
  }
  template <typename It>
  void assign(It first, It last) {
    size_t n = static_cast<size_t>(std::distance(first, last));
    clear();
    reserve(n);
    int* d = data();
    for (size_t i = 0; first != last; ++first, ++i) d[i] = static_cast<int>(*first);
    size_ = static_cast<uint32_t>(n);
  }
  void assign(size_t n, int value) {
    clear();
    resize(n, value);
  }
  int* insert(const int* pos, int value) {
    size_t at = static_cast<size_t>(pos - data());
    if (size_ == cap_) grow(cap_ * 2);
    int* d = data();
    std::memmove(d + at + 1, d + at, (size_ - at) * sizeof(int));
    d[at] = value;
    ++size_;
    return d + at;
  }
  template <typename It>
  int* insert(const int* pos, It first, It last) {
    size_t at = static_cast<size_t>(pos - data());
    size_t n = static_cast<size_t>(std::distance(first, last));
    reserve(size_ + n);
    int* d = data();
    std::memmove(d + at + n, d + at, (size_ - at) * sizeof(int));
    for (size_t i = 0; first != last; ++first, ++i) d[at + i] = static_cast<int>(*first);
    size_ += static_cast<uint32_t>(n);
    return d + at;
  }
  int* erase(const int* pos) { return erase(pos, pos + 1); }
  int* erase(const int* first, const int* last) {
    int* d = data();
    size_t a = static_cast<size_t>(first - d), b = static_cast<size_t>(last - d);
    std::memmove(d + a, d + b, (size_ - b) * sizeof(int));
    size_ -= static_cast<uint32_t>(b - a);
    return d + a;
  }
  void swap(IndexList& o) noexcept {
    IndexList t(std::move(o));
    o = std::move(*this);
    *this = std::move(t);
  }

  friend bool operator==(const IndexList& a, const IndexList& b) {
    return a.size_ == b.size_ && std::memcmp(a.data(), b.data(), a.size_ * sizeof(int)) == 0;
  }
  friend bool operator!=(const IndexList& a, const IndexList& b) { return !(a == b); }
  friend bool operator<(const IndexList& a, const IndexList& b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end());
  }

 private:
  void grow(size_t n) {
    if (n < kInline * 2) n = kInline * 2;
    int* h = new int[n];
    if (size_) std::memcpy(h, data(), size_ * sizeof(int));
    release();
    heap_ = h;
    cap_ = static_cast<uint32_t>(n);
  }
  void release() {
    if (cap_ > kInline) delete[] heap_;
    cap_ = kInline;
  }
  void moveFrom(IndexList& o) {
    size_ = o.size_;
    cap_ = o.cap_;
    if (o.cap_ > kInline) heap_ = o.heap_;
    else std::memcpy(inline_, o.inline_, sizeof(inline_));
    o.cap_ = kInline;
    o.size_ = 0;
  }

  union {
    int inline_[kInline];
    int* heap_;
  };
  uint32_t size_ = 0;
  uint32_t cap_ = kInline;
};

// N-gon mesh with face-varying UVs, material IDs and smoothing groups, like 3ds Max's
// Editable Poly. Faces are wound counter-clockwise when seen from the front.
struct PolyMesh {
  struct Face {
    IndexList v;   // indices into positions
    IndexList uv;  // indices into uvs (same length as v, or empty)
    int matId = 0;        // material sub-ID (for multi-materials later)
    uint32_t smoothGroups = 1;  // bit mask; faces sharing a bit are smoothed together, 0 = faceted
  };

  std::vector<Vec3> positions;
  std::vector<Vec2> uvs;
  std::vector<Face> faces;

  bool empty() const { return faces.empty(); }
  void clear() { positions.clear(); uvs.clear(); faces.clear(); }
  Aabb bounds() const;
  size_t triangleCount() const;

  int addVertex(const Vec3& p) { positions.push_back(p); return static_cast<int>(positions.size()) - 1; }
  int addUv(const Vec2& t) { uvs.push_back(t); return static_cast<int>(uvs.size()) - 1; }
  void addFace(std::vector<int> v, std::vector<int> uv = {}, uint32_t smooth = 1, int matId = 0);
  // Appends another mesh (indices are offset).
  void append(const PolyMesh& other);
  void transform(const Mat4& m);
  void flipFaces();
  // Face normal via Newell's method (works for non-planar n-gons).
  Vec3 faceNormal(const Face& f) const;
  // Area-weighted vertex normals ignoring smoothing groups (used by Push/Shell/Noise).
  std::vector<Vec3> vertexNormals() const;
  // True when every face index is in range.
  bool isValid() const;
};

// Render-ready triangles. Vertices are split where normals or UVs differ.
struct TriMesh {
  std::vector<Vec3> positions;
  std::vector<Vec3> normals;
  std::vector<Vec2> uvs;
  std::vector<uint32_t> indices;    // 3 per triangle
  std::vector<int> triMatIds;       // one per triangle
  // Polygon edges for wireframe display (no triangulation diagonals).
  std::vector<Vec3> wirePositions;
  std::vector<uint32_t> wireIndices;  // 2 per edge
  Aabb bounds;

  size_t triangleCount() const { return indices.size() / 3; }
  bool empty() const { return indices.empty(); }
};

// Triangulates (ear clipping for concave polygons) and computes smoothing-group normals.
TriMesh triangulate(const PolyMesh& mesh);

// Ray/triangle-mesh test used for viewport picking. Returns nearest distance or -1.
float intersectTriMesh(const TriMesh& mesh, const Ray& ray);

}  // namespace veraa
