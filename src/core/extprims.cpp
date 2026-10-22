// Teapot (Standard Primitives) and the Extended Primitives: Hedra, Torus Knot, ChamferCyl,
// OilTank, Capsule, Spindle, L-Ext, Gengon, C-Ext, RingWave, Hose and Prism.
//
// The teapot is built from the classic Newell teapot Bezier patch data (Martin Newell,
// University of Utah, 1975), which is in the public domain.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <tuple>

#include "object.h"
#include "polyops.h"
#include "registry.h"

namespace veraa {

namespace {

// ---------------------------------------------------------------------------------------------
// Builders

// Box-projected UVs for every face (used by shapes without a natural parameterization).
void boxMapAll(PolyMesh& m, float scale) {
  m.uvs.clear();
  for (auto& f : m.faces) {
    f.uv.clear();
    Vec3 n = vabs(normalize(m.faceNormal(f)));
    for (int v : f.v) {
      const Vec3& p = m.positions[v];
      Vec2 uv = n.x >= n.y && n.x >= n.z ? Vec2(p.y, p.z) : (n.y >= n.z ? Vec2(p.x, p.z) : Vec2(p.x, p.y));
      f.uv.push_back(m.addUv(uv / scale));
    }
  }
}

// Surface of revolution around Z. The profile runs from bottom to top as (radius, z); points
// with zero radius become poles. segSg[i] is the smoothing group of profile segment i.
PolyMesh lathe(const std::vector<Vec2>& prof, const std::vector<uint32_t>& segSg, int sides) {
  PolyMesh m;
  sides = std::max(3, sides);
  // Arc length for V coordinates.
  std::vector<float> acc(prof.size(), 0.0f);
  for (size_t i = 1; i < prof.size(); ++i) acc[i] = acc[i - 1] + length(Vec3(prof[i].x - prof[i - 1].x, 0, prof[i].y - prof[i - 1].y));
  float total = std::max(acc.back(), 1e-6f);
  std::vector<std::vector<int>> rings, uvRings;
  for (size_t i = 0; i < prof.size(); ++i) {
    std::vector<int> ring, uvRing;
    if (prof[i].x <= 1e-7f) {
      ring.push_back(m.addVertex(Vec3(0, 0, prof[i].y)));
    } else {
      for (int k = 0; k < sides; ++k) {
        float a = kTwoPi * k / sides;
        ring.push_back(m.addVertex(Vec3(prof[i].x * std::cos(a), prof[i].x * std::sin(a), prof[i].y)));
      }
    }
    for (int k = 0; k <= sides; ++k) uvRing.push_back(m.addUv(Vec2(static_cast<float>(k) / sides, acc[i] / total)));
    rings.push_back(ring);
    uvRings.push_back(uvRing);
  }
  for (size_t i = 0; i + 1 < prof.size(); ++i) {
    const auto& a = rings[i];
    const auto& b = rings[i + 1];
    uint32_t sg = i < segSg.size() ? segSg[i] : 1u;
    for (int k = 0; k < sides; ++k) {
      int kn = (k + 1) % sides;
      int ua = uvRings[i][k], ub = uvRings[i][k + 1], uc = uvRings[i + 1][k + 1], ud = uvRings[i + 1][k];
      if (a.size() == 1 && b.size() == 1) continue;
      if (a.size() == 1) m.addFace({a[0], b[kn], b[k]}, {ua, uc, ud}, sg);
      else if (b.size() == 1) m.addFace({a[k], a[kn], b[0]}, {ua, ub, uc}, sg);
      else m.addFace({a[k], a[kn], b[kn], b[k]}, {ua, ub, uc, ud}, sg);
    }
  }
  return m;
}

// Extrudes a counter-clockwise polygon along +Z. edgeSg[i] is the smoothing group of the side
// faces along polygon edge i.
PolyMesh extrudePolygon(const std::vector<Vec2>& poly, float height, int heightSegs, const std::vector<uint32_t>& edgeSg) {
  PolyMesh m;
  size_t n = poly.size();
  heightSegs = std::max(1, heightSegs);
  std::vector<std::vector<int>> rings(heightSegs + 1);
  for (int s = 0; s <= heightSegs; ++s)
    for (const auto& p : poly) rings[s].push_back(m.addVertex(Vec3(p.x, p.y, height * s / heightSegs)));
  for (int s = 0; s < heightSegs; ++s)
    for (size_t i = 0; i < n; ++i) {
      size_t j = (i + 1) % n;
      m.addFace({rings[s][i], rings[s][j], rings[s + 1][j], rings[s + 1][i]}, {}, i < edgeSg.size() ? edgeSg[i] : 1u);
    }
  std::vector<int> bottom(rings[0].rbegin(), rings[0].rend());
  m.addFace(bottom, {}, 0);
  m.addFace(rings[heightSegs], {}, 0);
  if (height < 0) m.flipFaces();
  return m;
}

// Quarter arc points from angle a0 to a1 (radians) around a center, excluding the start.
void arcPoints(std::vector<Vec2>& out, const Vec2& c, float r, float a0, float a1, int segs, bool includeStart) {
  for (int i = includeStart ? 0 : 1; i <= segs; ++i) {
    float a = a0 + (a1 - a0) * i / segs;
    out.push_back(Vec2(c.x + r * std::cos(a), c.y + r * std::sin(a)));
  }
}

// Polygon with rounded corners (fillet radius f, fsegs segments per corner). Corners must be
// convex for the fillet; concave corners are kept sharp.
std::vector<Vec2> filletPolygon(const std::vector<Vec2>& poly, float f, int fsegs, std::vector<char>* arcFlags = nullptr) {
  std::vector<Vec2> out;
  size_t n = poly.size();
  if (arcFlags) arcFlags->clear();
  for (size_t i = 0; i < n; ++i) {
    Vec2 p = poly[i], a = poly[(i + n - 1) % n], b = poly[(i + 1) % n];
    Vec2 da = a - p, db = b - p;
    float la = std::sqrt(da.x * da.x + da.y * da.y), lb = std::sqrt(db.x * db.x + db.y * db.y);
    float cross = (p.x - a.x) * (b.y - p.y) - (p.y - a.y) * (b.x - p.x);
    if (f <= 1e-6f || fsegs < 1 || la < 1e-6f || lb < 1e-6f || cross <= 0) {
      out.push_back(p);
      if (arcFlags) arcFlags->push_back(0);
      continue;
    }
    da = da / la;
    db = db / lb;
    float cosT = std::clamp(da.x * db.x + da.y * db.y, -1.0f, 1.0f);
    float half = std::acos(cosT) * 0.5f;
    float t = std::min({f / std::tan(half), la * 0.49f, lb * 0.49f});
    float r = t * std::tan(half);
    Vec2 p0 = p + da * t, p1 = p + db * t;
    Vec2 bis = da + db;
    float bl = std::sqrt(bis.x * bis.x + bis.y * bis.y);
    bis = bis / std::max(bl, 1e-9f);
    Vec2 c = p + bis * (r / std::sin(half));
    float a0 = std::atan2(p0.y - c.y, p0.x - c.x), a1 = std::atan2(p1.y - c.y, p1.x - c.x);
    while (a1 < a0) a1 += kTwoPi;
    if (a1 - a0 > kPi) a1 -= kTwoPi;
    for (int s = 0; s <= fsegs; ++s) {
      float a = a0 + (a1 - a0) * s / fsegs;
      out.push_back(Vec2(c.x + r * std::cos(a), c.y + r * std::sin(a)));
      if (arcFlags) arcFlags->push_back(s < fsegs ? 1 : 0);
    }
  }
  return out;
}

class Prim : public GeometryObject {};

// ---------------------------------------------------------------------------------------------
// Teapot

// Patch indices into kTeapotPoints; patches 0-5 are mirrored four ways, 6-9 two ways.
const int kTeapotPatches[10][16] = {
    {102, 103, 104, 105, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},            // rim
    {12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27},          // body
    {24, 25, 26, 27, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40},          // body
    {96, 96, 96, 96, 97, 98, 99, 100, 101, 101, 101, 101, 0, 1, 2, 3},         // lid
    {0, 1, 2, 3, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117},  // lid
    {118, 118, 118, 118, 124, 122, 119, 121, 123, 126, 125, 120, 40, 39, 38, 37},  // bottom
    {41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56},          // handle
    {53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 28, 65, 66, 67},          // handle
    {68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83},          // spout
    {80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95},          // spout
};

const float kTeapotPoints[127][3] = {
    {0.2f, 0, 2.7f},          {0.2f, -0.112f, 2.7f},       {0.112f, -0.2f, 2.7f},       {0, -0.2f, 2.7f},
    {1.3375f, 0, 2.53125f},   {1.3375f, -0.749f, 2.53125f}, {0.749f, -1.3375f, 2.53125f}, {0, -1.3375f, 2.53125f},
    {1.4375f, 0, 2.53125f},   {1.4375f, -0.805f, 2.53125f}, {0.805f, -1.4375f, 2.53125f}, {0, -1.4375f, 2.53125f},
    {1.5f, 0, 2.4f},          {1.5f, -0.84f, 2.4f},         {0.84f, -1.5f, 2.4f},         {0, -1.5f, 2.4f},
    {1.75f, 0, 1.875f},       {1.75f, -0.98f, 1.875f},      {0.98f, -1.75f, 1.875f},      {0, -1.75f, 1.875f},
    {2, 0, 1.35f},            {2, -1.12f, 1.35f},           {1.12f, -2, 1.35f},           {0, -2, 1.35f},
    {2, 0, 0.9f},             {2, -1.12f, 0.9f},            {1.12f, -2, 0.9f},            {0, -2, 0.9f},
    {-2, 0, 0.9f},            {2, 0, 0.45f},                {2, -1.12f, 0.45f},           {1.12f, -2, 0.45f},
    {0, -2, 0.45f},           {1.5f, 0, 0.225f},            {1.5f, -0.84f, 0.225f},       {0.84f, -1.5f, 0.225f},
    {0, -1.5f, 0.225f},       {1.5f, 0, 0.15f},             {1.5f, -0.84f, 0.15f},        {0.84f, -1.5f, 0.15f},
    {0, -1.5f, 0.15f},        {-1.6f, 0, 2.025f},           {-1.6f, -0.3f, 2.025f},       {-1.5f, -0.3f, 2.25f},
    {-1.5f, 0, 2.25f},        {-2.3f, 0, 2.025f},           {-2.3f, -0.3f, 2.025f},       {-2.5f, -0.3f, 2.25f},
    {-2.5f, 0, 2.25f},        {-2.7f, 0, 2.025f},           {-2.7f, -0.3f, 2.025f},       {-3, -0.3f, 2.25f},
    {-3, 0, 2.25f},           {-2.7f, 0, 1.8f},             {-2.7f, -0.3f, 1.8f},         {-3, -0.3f, 1.8f},
    {-3, 0, 1.8f},            {-2.7f, 0, 1.575f},           {-2.7f, -0.3f, 1.575f},       {-3, -0.3f, 1.35f},
    {-3, 0, 1.35f},           {-2.5f, 0, 1.125f},           {-2.5f, -0.3f, 1.125f},       {-2.65f, -0.3f, 0.9375f},
    {-2.65f, 0, 0.9375f},     {-2, -0.3f, 0.9f},            {-1.9f, -0.3f, 0.6f},         {-1.9f, 0, 0.6f},
    {1.7f, 0, 1.425f},        {1.7f, -0.66f, 1.425f},       {1.7f, -0.66f, 0.6f},         {1.7f, 0, 0.6f},
    {2.6f, 0, 1.425f},        {2.6f, -0.66f, 1.425f},       {3.1f, -0.66f, 0.825f},       {3.1f, 0, 0.825f},
    {2.3f, 0, 2.1f},          {2.3f, -0.25f, 2.1f},         {2.4f, -0.25f, 2.025f},       {2.4f, 0, 2.025f},
    {2.7f, 0, 2.4f},          {2.7f, -0.25f, 2.4f},         {3.3f, -0.25f, 2.4f},         {3.3f, 0, 2.4f},
    {2.8f, 0, 2.475f},        {2.8f, -0.25f, 2.475f},       {3.525f, -0.25f, 2.49375f},   {3.525f, 0, 2.49375f},
    {2.9f, 0, 2.475f},        {2.9f, -0.15f, 2.475f},       {3.45f, -0.15f, 2.5125f},     {3.45f, 0, 2.5125f},
    {2.8f, 0, 2.4f},          {2.8f, -0.15f, 2.4f},         {3.2f, -0.15f, 2.4f},         {3.2f, 0, 2.4f},
    {0, 0, 3.15f},            {0.8f, 0, 3.15f},             {0.8f, -0.45f, 3.15f},        {0.45f, -0.8f, 3.15f},
    {0, -0.8f, 3.15f},        {0, 0, 2.85f},                {1.4f, 0, 2.4f},              {1.4f, -0.784f, 2.4f},
    {0.784f, -1.4f, 2.4f},    {0, -1.4f, 2.4f},             {0.4f, 0, 2.55f},             {0.4f, -0.224f, 2.55f},
    {0.224f, -0.4f, 2.55f},   {0, -0.4f, 2.55f},            {1.3f, 0, 2.55f},             {1.3f, -0.728f, 2.55f},
    {0.728f, -1.3f, 2.55f},   {0, -1.3f, 2.55f},            {1.3f, 0, 2.4f},              {1.3f, -0.728f, 2.4f},
    {0.728f, -1.3f, 2.4f},    {0, -1.3f, 2.4f},             {0, 0, 0},                    {1.425f, -0.798f, 0},
    {1.5f, 0, 0.075f},        {1.425f, 0, 0},               {0.798f, -1.425f, 0},         {0, -1.5f, 0.075f},
    {0, -1.425f, 0},          {1.5f, -0.84f, 0.075f},       {0.84f, -1.5f, 0.075f},
};

Vec3 bezierPatch(const Vec3 cp[4][4], float u, float v, Vec3* du, Vec3* dv) {
  auto b = [](float t, float w[4], float d[4]) {
    float s = 1 - t;
    w[0] = s * s * s;
    w[1] = 3 * t * s * s;
    w[2] = 3 * t * t * s;
    w[3] = t * t * t;
    d[0] = -3 * s * s;
    d[1] = 3 * s * s - 6 * t * s;
    d[2] = 6 * t * s - 3 * t * t;
    d[3] = 3 * t * t;
  };
  float wu[4], du4[4], wv[4], dv4[4];
  b(u, wu, du4);
  b(v, wv, dv4);
  Vec3 p, pu, pv;
  for (int j = 0; j < 4; ++j)
    for (int k = 0; k < 4; ++k) {
      p += cp[j][k] * (wv[j] * wu[k]);
      pu += cp[j][k] * (wv[j] * du4[k]);
      pv += cp[j][k] * (dv4[j] * wu[k]);
    }
  if (du) *du = pu;
  if (dv) *dv = pv;
  return p;
}

class TeapotObject : public Prim {
 public:
  TeapotObject() {
    params.beginGroup("Parameters");
    params.addFloat("radius", "Radius", 25.0, 0.0, 1e7, 1.0);
    params.addInt("segments", "Segments", 4, 1, 64);
    params.addBool("smooth", "Smooth", true);
    params.beginGroup("Teapot Parts");
    params.addBool("body", "Body", true);
    params.addBool("handle", "Handle", true);
    params.addBool("spout", "Spout", true);
    params.addBool("lid", "Lid", true);
  }
  std::string type() const override { return "Teapot"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<TeapotObject>(*this); }

  PolyMesh build() const override {
    float scale = params.getFloat("radius") / 2.0f;
    int segs = params.getInt("segments") * 2;
    uint32_t sg = params.getBool("smooth") ? 1u : 0u;
    bool parts[4] = {params.getBool("body"), params.getBool("lid"), params.getBool("handle"), params.getBool("spout")};
    PolyMesh m;
    std::map<std::tuple<int64_t, int64_t, int64_t>, int> weld;
    float tol = std::max(1e-6f, scale * 1e-4f);
    auto vertex = [&](const Vec3& p) {
      auto key = std::make_tuple(std::llround(p.x / tol), std::llround(p.y / tol), std::llround(p.z / tol));
      auto it = weld.find(key);
      if (it != weld.end()) return it->second;
      int v = m.addVertex(p);
      weld.emplace(key, v);
      return v;
    };
    for (int pi = 0; pi < 10; ++pi) {
      int part = pi <= 2 || pi == 5 ? 0 : (pi <= 4 ? 1 : (pi <= 7 ? 2 : 3));
      if (!parts[part]) continue;
      Vec3 base[4][4];
      for (int j = 0; j < 4; ++j)
        for (int k = 0; k < 4; ++k) {
          const float* c = kTeapotPoints[kTeapotPatches[pi][j * 4 + k]];
          base[j][k] = Vec3(c[0], c[1], c[2]);
        }
      bool flip = patchNeedsFlip(base, pi);
      int copies = pi < 6 ? 4 : 2;
      for (int c = 0; c < copies; ++c) {
        // Mirror copies: c=1 mirrors Y, c=2 mirrors X, c=3 mirrors both. Single mirrors also
        // reverse the column order so every copy keeps the same winding.
        Vec3 cp[4][4];
        for (int j = 0; j < 4; ++j)
          for (int k = 0; k < 4; ++k) {
            bool reverse = c == 1 || c == 2;
            Vec3 p = base[j][reverse ? 3 - k : k];
            if (c == 1 || c == 3) p.y = -p.y;
            if (c == 2 || c == 3) p.x = -p.x;
            cp[j][k] = p * scale;
          }
        std::vector<int> grid((segs + 1) * (segs + 1));
        int uvBase = static_cast<int>(m.uvs.size());
        for (int j = 0; j <= segs; ++j)
          for (int k = 0; k <= segs; ++k) {
            float u = static_cast<float>(k) / segs, v = static_cast<float>(j) / segs;
            grid[j * (segs + 1) + k] = vertex(bezierPatch(cp, u, v, nullptr, nullptr));
            m.addUv(Vec2(u, v));
          }
        for (int j = 0; j < segs; ++j)
          for (int k = 0; k < segs; ++k) {
            int i0 = j * (segs + 1) + k, i1 = i0 + 1, i2 = i0 + segs + 2, i3 = i0 + segs + 1;
            std::vector<int> v = {grid[i0], grid[i1], grid[i2], grid[i3]};
            std::vector<int> t = {uvBase + i0, uvBase + i1, uvBase + i2, uvBase + i3};
            if (flip) {
              std::reverse(v.begin(), v.end());
              std::reverse(t.begin(), t.end());
            }
            // Collapse corners that welded together (patch poles).
            std::vector<int> fv, ft;
            for (size_t q = 0; q < v.size(); ++q) {
              if (!fv.empty() && fv.back() == v[q]) continue;
              fv.push_back(v[q]);
              ft.push_back(t[q]);
            }
            while (fv.size() > 1 && fv.front() == fv.back()) {
              fv.pop_back();
              ft.pop_back();
            }
            if (fv.size() >= 3) m.addFace(fv, ft, sg);
          }
      }
    }
    return m;
  }

 private:
  // Picks the winding so the patch faces outward: rotational parts face away from the Z axis
  // (or up for the rim), handle and spout face away from their tube's center line.
  static bool patchNeedsFlip(const Vec3 cp[4][4], int pi) {
    double score = 0;
    for (int a = 1; a <= 3; ++a)
      for (int b = 1; b <= 3; ++b) {
        float u = a / 4.0f, v = b / 4.0f;
        Vec3 du, dv;
        Vec3 p = bezierPatch(cp, u, v, &du, &dv);
        Vec3 n = cross(du, dv);
        Vec3 ref;
        if (pi >= 6) {
          Vec3 m0 = bezierPatch(cp, 0.0f, v, nullptr, nullptr), m1 = bezierPatch(cp, 1.0f, v, nullptr, nullptr);
          ref = p - (m0 + m1) * 0.5f;
        } else if (pi == 0) {
          ref = Vec3(p.x, p.y, 0) + Vec3(0, 0, 1.5f);
        } else if (pi == 5) {
          ref = Vec3(p.x, p.y, 0) * 0.2f + Vec3(0, 0, -1);
        } else if (pi == 3 || pi == 4) {
          ref = Vec3(p.x, p.y, 0) + Vec3(0, 0, 0.5f);
        } else {
          ref = Vec3(p.x, p.y, 0);
        }
        score += dot(normalize(n), normalize(ref));
      }
    return score < 0;
  }
};

// ---------------------------------------------------------------------------------------------
// Hedra

class HedraObject : public Prim {
 public:
  HedraObject() {
    params.beginGroup("Family");
    params.addEnum("family", "Family", {"Tetra", "Cube", "Octa", "Dodeca", "Icosa", "Star1", "Star2"}, 0);
    params.beginGroup("Family Parameters");
    params.addFloat("p", "P (truncation)", 0.0, 0.0, 1.0, 0.05);
    params.addFloat("q", "Q (star height)", 0.5, 0.0, 2.0, 0.05);
    params.beginGroup("Parameters");
    params.addFloat("radius", "Radius", 50.0, 0.0, 1e7, 1.0);
  }
  std::string type() const override { return "Hedra"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<HedraObject>(*this); }

  static void icosahedron(std::vector<Vec3>& pts, std::vector<std::vector<int>>& faces) {
    float t = (1.0f + std::sqrt(5.0f)) * 0.5f;
    pts = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t},
           {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
    faces = {{0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4},
             {11, 10, 2}, {10, 7, 6}, {7, 1, 8}, {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8},
             {3, 8, 9}, {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}};
  }

  // Dual polyhedron: vertices at face centers, one face per original vertex.
  static void dual(const std::vector<Vec3>& pts, const std::vector<std::vector<int>>& faces, std::vector<Vec3>& dp,
                   std::vector<std::vector<int>>& df) {
    dp.clear();
    df.clear();
    for (const auto& f : faces) {
      Vec3 c;
      for (int v : f) c += pts[v];
      dp.push_back(c / static_cast<float>(f.size()));
    }
    for (size_t v = 0; v < pts.size(); ++v) {
      std::vector<int> around;
      for (size_t f = 0; f < faces.size(); ++f)
        if (std::find(faces[f].begin(), faces[f].end(), static_cast<int>(v)) != faces[f].end()) around.push_back(static_cast<int>(f));
      Vec3 n = normalize(pts[v]), u, w;
      makeBasis(n, u, w);
      std::sort(around.begin(), around.end(), [&](int a, int b) {
        Vec3 da = dp[a] - pts[v], db = dp[b] - pts[v];
        return std::atan2(dot(da, w), dot(da, u)) < std::atan2(dot(db, w), dot(db, u));
      });
      df.push_back(around);
    }
  }

  PolyMesh build() const override {
    std::vector<Vec3> pts;
    std::vector<std::vector<int>> faces;
    int fam = params.getInt("family");
    switch (fam) {
      case 0:
        pts = {{1, 1, 1}, {1, -1, -1}, {-1, 1, -1}, {-1, -1, 1}};
        faces = {{0, 1, 2}, {0, 3, 1}, {0, 2, 3}, {1, 3, 2}};
        break;
      case 1:
        pts = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1}, {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}};
        faces = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
        break;
      case 2:
      case 6:
        pts = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        faces = {{0, 2, 4}, {2, 1, 4}, {1, 3, 4}, {3, 0, 4}, {2, 0, 5}, {1, 2, 5}, {3, 1, 5}, {0, 3, 5}};
        break;
      case 3:
      case 5: {
        std::vector<Vec3> ip;
        std::vector<std::vector<int>> ifc;
        icosahedron(ip, ifc);
        dual(ip, ifc, pts, faces);
        break;
      }
      default: icosahedron(pts, faces); break;
    }
    PolyMesh m;
    for (const auto& p : pts) m.addVertex(normalize(p));
    for (auto f : faces) {
      // Wind outward.
      Vec3 c, n;
      for (int v : f) c += m.positions[v];
      PolyMesh::Face tmp;
      tmp.v = f;
      n = m.faceNormal(tmp);
      if (dot(n, c) < 0) std::reverse(f.begin(), f.end());
      m.addFace(f, {}, 0);
    }
    float p = params.getFloat("p");
    if (p > 0.0f) {
      // Truncate the corners.
      float edge = length(m.positions[m.faces[0].v[0]] - m.positions[m.faces[0].v[1]]);
      PolySelection sel;
      sel.level = PolyVertex;
      for (size_t v = 0; v < m.positions.size(); ++v) sel.verts.push_back(static_cast<int>(v));
      runPolyOp(m, sel, {{"op", "chamferVerts"}, {"amount", edge * 0.49f * p}});
    }
    if (fam >= 5) {
      // Stellate: a pyramid on every face.
      float q = params.getFloat("q");
      std::vector<PolyMesh::Face> out;
      for (const auto& f : m.faces) {
        Vec3 c;
        for (int v : f.v) c += m.positions[v];
        c /= static_cast<float>(f.v.size());
        float edge = length(m.positions[f.v[0]] - m.positions[f.v[1]]);
        int apex = m.addVertex(c + normalize(m.faceNormal(f)) * edge * q);
        for (size_t k = 0; k < f.v.size(); ++k) {
          PolyMesh::Face t;
          t.v = {f.v[k], f.v[(k + 1) % f.v.size()], apex};
          t.smoothGroups = 0;
          out.push_back(t);
        }
      }
      m.faces = out;
    }
    // Scale to the radius (farthest vertex).
    float far = 0;
    for (const auto& v : m.positions) far = std::max(far, length(v));
    float r = params.getFloat("radius");
    for (auto& v : m.positions) v = v * (far > 0 ? r / far : 0.0f);
    boxMapAll(m, std::max(r * 2.0f, 1e-3f));
    return m;
  }
};

// ---------------------------------------------------------------------------------------------
// Torus Knot

class TorusKnotObject : public Prim {
 public:
  TorusKnotObject() {
    params.beginGroup("Base Curve");
    params.addFloat("radius", "Radius", 50.0, 0.0, 1e7, 1.0);
    params.addInt("segments", "Segments", 120, 3, 2000);
    params.addFloat("p", "P", 2.0, 1.0, 50.0, 1.0, 0);
    params.addFloat("q", "Q", 3.0, 1.0, 50.0, 1.0, 0);
    params.addFloat("eccentricity", "Eccentricity", 1.0, 0.1, 10.0, 0.05);
    params.beginGroup("Cross Section");
    params.addFloat("radius2", "Radius", 10.0, 0.0, 1e7, 0.5);
    params.addInt("sides", "Sides", 12, 3, 200);
    params.addFloat("twist", "Twist", 0.0, -1e4, 1e4, 1.0, 1);
    params.addFloat("lumps", "Lumps", 0.0, 0.0, 100.0, 1.0, 0);
    params.addFloat("lumpHeight", "Lump Height", 0.0, -10.0, 10.0, 0.05);
    params.beginGroup("Smoothing");
    params.addBool("smooth", "Smooth", true);
  }
  std::string type() const override { return "TorusKnot"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<TorusKnotObject>(*this); }

  Vec3 curve(float t) const {
    float R = params.getFloat("radius"), p = params.getFloat("p"), q = params.getFloat("q");
    float e = params.getFloat("eccentricity");
    float r = 0.5f * (2.0f + std::cos(q * t)) * R / 1.5f;
    return Vec3(r * std::cos(p * t), r * std::sin(p * t) * e, -std::sin(q * t) * R / 3.0f);
  }

  PolyMesh build() const override {
    int segs = params.getInt("segments"), sides = params.getInt("sides");
    float r2 = params.getFloat("radius2");
    float twist = radians(params.getFloat("twist"));
    float lumps = params.getFloat("lumps"), lumpH = params.getFloat("lumpHeight");
    uint32_t sg = params.getBool("smooth") ? 1u : 0u;
    // Parallel-transport frames around the closed curve, then spread the closing mismatch.
    std::vector<Vec3> pts(segs), tan(segs), nor(segs);
    for (int i = 0; i < segs; ++i) {
      float t = kTwoPi * i / segs;
      pts[i] = curve(t);
      tan[i] = normalize(curve(t + 1e-3f) - curve(t - 1e-3f));
    }
    Vec3 n0, b0;
    makeBasis(tan[0], n0, b0);
    nor[0] = n0;
    for (int i = 1; i < segs; ++i) {
      Vec3 n = nor[i - 1] - tan[i] * dot(nor[i - 1], tan[i]);
      nor[i] = normalize(n);
    }
    Vec3 back = normalize(nor[segs - 1] - tan[0] * dot(nor[segs - 1], tan[0]));
    float mismatch = std::atan2(dot(cross(back, nor[0]), tan[0]), dot(back, nor[0]));
    PolyMesh m;
    for (int i = 0; i < segs; ++i) {
      float corr = mismatch * i / segs + twist * i / segs;
      Vec3 n = nor[i], b = cross(tan[i], n);
      Vec3 rn = n * std::cos(corr) + b * std::sin(corr), rb = cross(tan[i], rn);
      float rr = r2 * (1.0f + lumpH * std::sin(lumps * kTwoPi * i / segs));
      for (int k = 0; k < sides; ++k) {
        float a = kTwoPi * k / sides;
        m.addVertex(pts[i] + (rn * std::cos(a) + rb * std::sin(a)) * rr);
      }
    }
    int uvBase = 0;
    for (int i = 0; i <= segs; ++i)
      for (int k = 0; k <= sides; ++k) m.addUv(Vec2(static_cast<float>(i) / segs, static_cast<float>(k) / sides));
    for (int i = 0; i < segs; ++i)
      for (int k = 0; k < sides; ++k) {
        int i1 = (i + 1) % segs, k1 = (k + 1) % sides;
        int a = i * sides + k, b = i * sides + k1, c = i1 * sides + k1, d = i1 * sides + k;
        int ua = uvBase + i * (sides + 1) + k, ub = ua + 1, uc = ua + sides + 2, ud = ua + sides + 1;
        m.addFace({a, d, c, b}, {ua, ud, uc, ub}, sg);
      }
    if (meshVolume(m) < 0) m.flipFaces();
    return m;
  }
};

// ---------------------------------------------------------------------------------------------
// Lathed extended primitives

class ChamferCylObject : public Prim {
 public:
  ChamferCylObject() {
    params.beginGroup("Parameters");
    params.addFloat("radius", "Radius", 50.0, 0.0, 1e7, 1.0);
    params.addFloat("height", "Height", 100.0, -1e7, 1e7, 1.0);
    params.addFloat("fillet", "Fillet", 5.0, 0.0, 1e7, 0.5);
    params.addInt("heightSegs", "Height Segs", 1, 1, 200);
    params.addInt("filletSegs", "Fillet Segs", 3, 1, 50);
    params.addInt("sides", "Sides", 24, 3, 500);
    params.addInt("capSegs", "Cap Segs", 1, 1, 100);
    params.addBool("smooth", "Smooth", true);
  }
  std::string type() const override { return "ChamferCyl"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<ChamferCylObject>(*this); }
  PolyMesh build() const override {
    float R = params.getFloat("radius"), H = params.getFloat("height");
    float h = std::fabs(H);
    float f = std::min({params.getFloat("fillet"), R * 0.999f, h * 0.499f});
    int fs = params.getInt("filletSegs"), hs = params.getInt("heightSegs"), cs = params.getInt("capSegs");
    bool smooth = params.getBool("smooth");
    std::vector<Vec2> prof;
    std::vector<uint32_t> sg;
    prof.push_back(Vec2(0, 0));
    for (int i = 1; i <= cs; ++i) {
      prof.push_back(Vec2((R - f) * i / cs, 0));
      sg.push_back(2);
    }
    if (f > 1e-6f) {
      arcPoints(prof, Vec2(R - f, f), f, -kPi * 0.5f, 0.0f, fs, false);
      for (int i = 0; i < fs; ++i) sg.push_back(smooth ? 1u : 0u);
    }
    for (int i = 1; i <= hs; ++i) {
      prof.push_back(Vec2(R, f + (h - 2 * f) * i / hs));
      sg.push_back(smooth ? 1u : 0u);
    }
    if (f > 1e-6f) {
      arcPoints(prof, Vec2(R - f, h - f), f, 0.0f, kPi * 0.5f, fs, false);
      for (int i = 0; i < fs; ++i) sg.push_back(smooth ? 1u : 0u);
    }
    for (int i = cs - 1; i >= 0; --i) {
      prof.push_back(Vec2((R - f) * i / cs, h));
      sg.push_back(4);
    }
    PolyMesh m = lathe(prof, sg, params.getInt("sides"));
    if (H < 0) {
      for (auto& p : m.positions) p.z = -p.z;
      m.flipFaces();
    }
    return m;
  }
};

// Profile helpers for capsule-like shapes.
std::vector<Vec2> capProfile(float R, float H, float capH, int capSegs, int heightSegs, int kind,
                             std::vector<uint32_t>& sg) {
  // kind 0 = elliptical dome (oil tank, capsule), 1 = cone (spindle)
  std::vector<Vec2> prof;
  sg.clear();
  prof.push_back(Vec2(0, 0));
  for (int i = 1; i <= capSegs; ++i) {
    float t = static_cast<float>(i) / capSegs;
    if (kind == 1) prof.push_back(Vec2(R * t, capH * t));
    else prof.push_back(Vec2(R * std::sin(t * kPi * 0.5f), capH * (1.0f - std::cos(t * kPi * 0.5f))));
    sg.push_back(kind == 1 ? 2u : 1u);
  }
  for (int i = 1; i <= heightSegs; ++i) {
    prof.push_back(Vec2(R, capH + (H - 2 * capH) * i / heightSegs));
    sg.push_back(1);
  }
  for (int i = capSegs - 1; i >= 0; --i) {
    float t = static_cast<float>(i) / capSegs;
    if (kind == 1) prof.push_back(Vec2(R * t, H - capH * t));
    else prof.push_back(Vec2(R * std::sin(t * kPi * 0.5f), H - capH * (1.0f - std::cos(t * kPi * 0.5f))));
    sg.push_back(kind == 1 ? 4u : 1u);
  }
  return prof;
}

class OilTankObject : public Prim {
 public:
  OilTankObject() {
    params.beginGroup("Parameters");
    params.addFloat("radius", "Radius", 50.0, 0.0, 1e7, 1.0);
    params.addFloat("height", "Height", 150.0, 0.0, 1e7, 1.0);
    params.addFloat("capHeight", "Cap Height", 20.0, 0.0, 1e7, 1.0);
    params.addInt("sides", "Sides", 24, 3, 500);
    params.addInt("heightSegs", "Height Segs", 1, 1, 200);
    params.addInt("capSegs", "Cap Segs", 6, 1, 64);
  }
  std::string type() const override { return "OilTank"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<OilTankObject>(*this); }
  PolyMesh build() const override {
    float R = params.getFloat("radius"), H = params.getFloat("height");
    float cap = std::min(params.getFloat("capHeight"), H * 0.5f);
    std::vector<uint32_t> sg;
    auto prof = capProfile(R, std::max(H, 2 * cap), cap, params.getInt("capSegs"), params.getInt("heightSegs"), 0, sg);
    return lathe(prof, sg, params.getInt("sides"));
  }
};

class CapsuleObject : public Prim {
 public:
  CapsuleObject() {
    params.beginGroup("Parameters");
    params.addFloat("radius", "Radius", 25.0, 0.0, 1e7, 1.0);
    params.addFloat("height", "Height", 100.0, 0.0, 1e7, 1.0);
    params.addEnum("heightMode", "Height Measures", {"Overall", "Centers"}, 0);
    params.addInt("sides", "Sides", 24, 3, 500);
    params.addInt("heightSegs", "Height Segs", 1, 1, 200);
    params.addInt("capSegs", "Cap Segs", 8, 1, 64);
  }
  std::string type() const override { return "Capsule"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<CapsuleObject>(*this); }
  PolyMesh build() const override {
    float R = params.getFloat("radius"), H = params.getFloat("height");
    if (params.getInt("heightMode") == 1) H += 2 * R;
    H = std::max(H, 2 * R);
    std::vector<uint32_t> sg;
    auto prof = capProfile(R, H, R, params.getInt("capSegs"), params.getInt("heightSegs"), 0, sg);
    return lathe(prof, sg, params.getInt("sides"));
  }
};

class SpindleObject : public Prim {
 public:
  SpindleObject() {
    params.beginGroup("Parameters");
    params.addFloat("radius", "Radius", 40.0, 0.0, 1e7, 1.0);
    params.addFloat("height", "Height", 150.0, 0.0, 1e7, 1.0);
    params.addFloat("capHeight", "Cap Height", 40.0, 0.0, 1e7, 1.0);
    params.addInt("sides", "Sides", 24, 3, 500);
    params.addInt("heightSegs", "Height Segs", 1, 1, 200);
    params.addInt("capSegs", "Cap Segs", 1, 1, 64);
  }
  std::string type() const override { return "Spindle"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<SpindleObject>(*this); }
  PolyMesh build() const override {
    float R = params.getFloat("radius"), H = params.getFloat("height");
    float cap = std::min(params.getFloat("capHeight"), H * 0.5f);
    std::vector<uint32_t> sg;
    auto prof = capProfile(R, std::max(H, 2 * cap), cap, params.getInt("capSegs"), params.getInt("heightSegs"), 1, sg);
    return lathe(prof, sg, params.getInt("sides"));
  }
};

class HoseObject : public Prim {
 public:
  HoseObject() {
    params.beginGroup("Free Hose Parameters");
    params.addFloat("height", "Height", 100.0, 0.0, 1e7, 1.0);
    params.beginGroup("Common Hose Parameters");
    params.addInt("segments", "Segments", 45, 2, 1000);
    params.addBool("flex", "Flex Section Enable", true);
    params.addFloat("flexStart", "Starts %", 10.0, 0.0, 100.0, 1.0, 1);
    params.addFloat("flexStop", "Ends %", 90.0, 0.0, 100.0, 1.0, 1);
    params.addInt("cycles", "Cycles", 5, 0, 1000);
    params.addFloat("flexDiameter", "Diameter %", -20.0, -50.0, 500.0, 1.0, 1);
    params.beginGroup("Hose Shape");
    params.addFloat("diameter", "Diameter", 20.0, 0.0, 1e7, 1.0);
    params.addInt("sides", "Sides", 8, 3, 100);
    params.addBool("smooth", "Smooth", true);
  }
  std::string type() const override { return "Hose"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<HoseObject>(*this); }
  PolyMesh build() const override {
    float H = params.getFloat("height"), R = params.getFloat("diameter") * 0.5f;
    int segs = params.getInt("segments");
    bool flex = params.getBool("flex");
    float s0 = params.getFloat("flexStart") / 100.0f, s1 = params.getFloat("flexStop") / 100.0f;
    int cycles = params.getInt("cycles");
    float amp = params.getFloat("flexDiameter") / 100.0f;
    std::vector<Vec2> prof = {Vec2(0, 0)};
    std::vector<uint32_t> sg = {2};
    uint32_t side = params.getBool("smooth") ? 1u : 0u;
    for (int i = 0; i <= segs; ++i) {
      float t = static_cast<float>(i) / segs;
      float r = R;
      if (flex && t > s0 && t < s1 && s1 > s0) {
        float u = (t - s0) / (s1 - s0);
        r = R * (1.0f + amp * 0.5f * (1.0f - std::cos(kTwoPi * cycles * u)));
      }
      prof.push_back(Vec2(std::max(r, 1e-3f), H * t));
      if (i > 0) sg.push_back(side);
    }
    prof.push_back(Vec2(0, H));
    sg.push_back(4);
    return lathe(prof, sg, params.getInt("sides"));
  }
};

// ---------------------------------------------------------------------------------------------
// Extruded extended primitives

class GengonObject : public Prim {
 public:
  GengonObject() {
    params.beginGroup("Parameters");
    params.addInt("sides", "Sides", 5, 3, 100);
    params.addFloat("radius", "Radius", 50.0, 0.0, 1e7, 1.0);
    params.addFloat("fillet", "Fillet", 0.0, 0.0, 1e7, 0.5);
    params.addFloat("height", "Height", 100.0, -1e7, 1e7, 1.0);
    params.addInt("heightSegs", "Height Segs", 1, 1, 200);
    params.addInt("filletSegs", "Fillet Segs", 1, 1, 50);
    params.addBool("smooth", "Smooth", true);
  }
  std::string type() const override { return "Gengon"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<GengonObject>(*this); }
  PolyMesh build() const override {
    int n = params.getInt("sides");
    float R = params.getFloat("radius");
    std::vector<Vec2> poly;
    for (int i = 0; i < n; ++i) {
      float a = kTwoPi * i / n;
      poly.push_back(Vec2(R * std::cos(a), R * std::sin(a)));
    }
    std::vector<char> arcs;
    float f = params.getFloat("fillet");
    auto rounded = filletPolygon(poly, f, params.getInt("filletSegs"), &arcs);
    bool smooth = params.getBool("smooth");
    std::vector<uint32_t> sg;
    int flat = 0;
    for (size_t i = 0; i < rounded.size(); ++i) {
      if (arcs[i] && smooth) sg.push_back(1);
      else sg.push_back(f > 0 && smooth ? 1u : (1u << (1 + (flat++ % 30))));
    }
    PolyMesh m = extrudePolygon(rounded, params.getFloat("height"), params.getInt("heightSegs"), sg);
    boxMapAll(m, std::max(R * 2.0f, 1e-3f));
    return m;
  }
};

class LExtObject : public Prim {
 public:
  LExtObject() {
    params.beginGroup("Parameters");
    params.addFloat("sideLength", "Side Length", 100.0, -1e7, 1e7, 1.0);
    params.addFloat("frontLength", "Front Length", 100.0, -1e7, 1e7, 1.0);
    params.addFloat("sideWidth", "Side Width", 20.0, 0.0, 1e7, 1.0);
    params.addFloat("frontWidth", "Front Width", 20.0, 0.0, 1e7, 1.0);
    params.addFloat("height", "Height", 50.0, -1e7, 1e7, 1.0);
    params.addInt("heightSegs", "Height Segs", 1, 1, 200);
  }
  std::string type() const override { return "LExt"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<LExtObject>(*this); }
  PolyMesh build() const override {
    float sl = std::fabs(params.getFloat("sideLength")), fl = std::fabs(params.getFloat("frontLength"));
    float sw = std::min(params.getFloat("sideWidth"), fl * 0.99f), fw = std::min(params.getFloat("frontWidth"), sl * 0.99f);
    // L in the XY plane: front leg along +X, side leg along +Y, corner at the origin.
    std::vector<Vec2> poly = {{0, 0}, {fl, 0}, {fl, fw}, {sw, fw}, {sw, sl}, {0, sl}};
    std::vector<uint32_t> sg = {2, 4, 8, 16, 32, 64};
    PolyMesh m = extrudePolygon(poly, params.getFloat("height"), params.getInt("heightSegs"), sg);
    for (auto& p : m.positions) p = p - Vec3(fl * 0.5f, sl * 0.5f, 0);
    boxMapAll(m, std::max(std::max(sl, fl), 1e-3f));
    return m;
  }
};

class CExtObject : public Prim {
 public:
  CExtObject() {
    params.beginGroup("Parameters");
    params.addFloat("backLength", "Back Length", 100.0, -1e7, 1e7, 1.0);
    params.addFloat("sideLength", "Side Length", 60.0, -1e7, 1e7, 1.0);
    params.addFloat("frontLength", "Front Length", 100.0, -1e7, 1e7, 1.0);
    params.addFloat("backWidth", "Back Width", 15.0, 0.0, 1e7, 1.0);
    params.addFloat("sideWidth", "Side Width", 15.0, 0.0, 1e7, 1.0);
    params.addFloat("frontWidth", "Front Width", 15.0, 0.0, 1e7, 1.0);
    params.addFloat("height", "Height", 50.0, -1e7, 1e7, 1.0);
    params.addInt("heightSegs", "Height Segs", 1, 1, 200);
  }
  std::string type() const override { return "CExt"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<CExtObject>(*this); }
  PolyMesh build() const override {
    float bl = std::fabs(params.getFloat("backLength")), sl = std::fabs(params.getFloat("sideLength"));
    float fl = std::fabs(params.getFloat("frontLength"));
    float bw = std::min(params.getFloat("backWidth"), sl * 0.45f), fw = std::min(params.getFloat("frontWidth"), sl * 0.45f);
    float sw = std::min(params.getFloat("sideWidth"), std::min(bl, fl) * 0.99f);
    // C opening toward +X: back leg along the top (y = sl), front leg along the bottom.
    std::vector<Vec2> poly = {{0, 0}, {fl, 0}, {fl, fw}, {sw, fw}, {sw, sl - bw}, {bl, sl - bw}, {bl, sl}, {0, sl}};
    std::vector<uint32_t> sg = {2, 4, 8, 16, 32, 64, 128, 256};
    PolyMesh m = extrudePolygon(poly, params.getFloat("height"), params.getInt("heightSegs"), sg);
    float w = std::max(bl, fl);
    for (auto& p : m.positions) p = p - Vec3(w * 0.5f, sl * 0.5f, 0);
    boxMapAll(m, std::max(std::max(w, sl), 1e-3f));
    return m;
  }
};

class PrismObject : public Prim {
 public:
  PrismObject() {
    params.beginGroup("Parameters");
    params.addFloat("side1", "Side 1 Length", 100.0, 0.0, 1e7, 1.0);
    params.addFloat("side2", "Side 2 Length", 100.0, 0.0, 1e7, 1.0);
    params.addFloat("side3", "Side 3 Length", 100.0, 0.0, 1e7, 1.0);
    params.addFloat("height", "Height", 100.0, -1e7, 1e7, 1.0);
    params.addInt("heightSegs", "Height Segs", 1, 1, 200);
  }
  std::string type() const override { return "Prism"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<PrismObject>(*this); }
  PolyMesh build() const override {
    float a = params.getFloat("side1"), b = params.getFloat("side2"), c = params.getFloat("side3");
    // Keep the triangle valid.
    b = std::clamp(b, std::fabs(a - c) + 1e-3f, a + c - 1e-3f);
    // Vertices: (0,0), (a,0) and the apex from the law of cosines.
    float x = (a * a + c * c - b * b) / (2 * a);
    float y = std::sqrt(std::max(0.0f, c * c - x * x));
    std::vector<Vec2> poly = {{0, 0}, {a, 0}, {x, y}};
    PolyMesh m = extrudePolygon(poly, params.getFloat("height"), params.getInt("heightSegs"), {2, 4, 8});
    Vec3 center((a + x) / 3.0f, y / 3.0f, 0);
    for (auto& p : m.positions) p = p - center;
    boxMapAll(m, std::max(a, 1e-3f));
    return m;
  }
};

class RingWaveObject : public Prim {
 public:
  RingWaveObject() {
    params.beginGroup("Ring Wave Size");
    params.addFloat("radius", "Radius", 50.0, 0.0, 1e7, 1.0);
    params.addFloat("ringWidth", "Ring Width", 10.0, 0.0, 1e7, 0.5);
    params.addInt("sides", "Radial Segs (sides)", 128, 3, 2000);
    params.addFloat("height", "Height", 5.0, 0.0, 1e7, 0.5);
    params.beginGroup("Outer Edge Breakup");
    params.addBool("outerOn", "On", false);
    params.addFloat("outerCycles", "Major Cycles", 1.0, 0.0, 200.0, 1.0, 0);
    params.addFloat("outerAmount", "Width Flux %", 0.0, 0.0, 200.0, 1.0, 1);
    params.beginGroup("Inner Edge Breakup");
    params.addBool("innerOn", "On", true);
    params.addFloat("innerCycles", "Major Cycles", 11.0, 0.0, 200.0, 1.0, 0);
    params.addFloat("innerAmount", "Width Flux %", 25.0, 0.0, 200.0, 1.0, 1);
    params.addFloat("innerCrossCycles", "Minor Cycles", 31.0, 0.0, 400.0, 1.0, 0);
    params.addFloat("innerCrossAmount", "Minor Flux %", 10.0, 0.0, 200.0, 1.0, 1);
    params.addFloat("phase", "Phase", 0.0, -1e5, 1e5, 1.0, 1);
  }
  std::string type() const override { return "RingWave"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<RingWaveObject>(*this); }
  PolyMesh build() const override {
    float R = params.getFloat("radius"), w = std::min(params.getFloat("ringWidth"), R * 0.98f);
    int n = params.getInt("sides");
    float h = std::max(params.getFloat("height"), 0.01f);
    float ph = radians(params.getFloat("phase"));
    PolyMesh m;
    std::vector<int> ob, ot, it, ib;
    for (int i = 0; i < n; ++i) {
      float a = kTwoPi * i / n;
      float ro = R, ri = R - w;
      if (params.getBool("outerOn"))
        ro = R + w * params.getFloat("outerAmount") / 100.0f * 0.5f * std::sin(params.getFloat("outerCycles") * a + ph);
      if (params.getBool("innerOn")) {
        float fl = params.getFloat("innerAmount") / 100.0f * std::sin(params.getFloat("innerCycles") * a + ph) +
                   params.getFloat("innerCrossAmount") / 100.0f * std::sin(params.getFloat("innerCrossCycles") * a - ph);
        ri = R - w * (1.0f + 0.5f * fl);
      }
      ri = std::clamp(ri, 0.01f * R, ro - 1e-3f);
      Vec3 dir(std::cos(a), std::sin(a), 0);
      ob.push_back(m.addVertex(dir * ro));
      ot.push_back(m.addVertex(dir * ro + Vec3(0, 0, h)));
      it.push_back(m.addVertex(dir * ri + Vec3(0, 0, h)));
      ib.push_back(m.addVertex(dir * ri));
    }
    for (int i = 0; i < n; ++i) {
      int j = (i + 1) % n;
      m.addFace({ob[i], ob[j], ot[j], ot[i]}, {}, 1);  // outer wall
      m.addFace({ot[i], ot[j], it[j], it[i]}, {}, 2);  // top
      m.addFace({it[i], it[j], ib[j], ib[i]}, {}, 4);  // inner wall
      m.addFace({ib[i], ib[j], ob[j], ob[i]}, {}, 8);  // bottom
    }
    boxMapAll(m, std::max(R * 2.0f, 1e-3f));
    return m;
  }
};

}  // namespace

void registerExtendedPrimitiveTypes() {
  auto add = [](const char* type, const char* name, const char* sub, std::function<std::unique_ptr<SceneObject>()> f) {
    registerObjectType({type, name, "Geometry", sub, std::move(f)});
  };
  add("Teapot", "Teapot", "Standard Primitives", [] { return std::make_unique<TeapotObject>(); });
  const char* ext = "Extended Primitives";
  add("Hedra", "Hedra", ext, [] { return std::make_unique<HedraObject>(); });
  add("TorusKnot", "Torus Knot", ext, [] { return std::make_unique<TorusKnotObject>(); });
  add("ChamferCyl", "ChamferCyl", ext, [] { return std::make_unique<ChamferCylObject>(); });
  add("OilTank", "OilTank", ext, [] { return std::make_unique<OilTankObject>(); });
  add("Capsule", "Capsule", ext, [] { return std::make_unique<CapsuleObject>(); });
  add("Spindle", "Spindle", ext, [] { return std::make_unique<SpindleObject>(); });
  add("LExt", "L-Ext", ext, [] { return std::make_unique<LExtObject>(); });
  add("Gengon", "Gengon", ext, [] { return std::make_unique<GengonObject>(); });
  add("CExt", "C-Ext", ext, [] { return std::make_unique<CExtObject>(); });
  add("RingWave", "RingWave", ext, [] { return std::make_unique<RingWaveObject>(); });
  add("Hose", "Hose", ext, [] { return std::make_unique<HoseObject>(); });
  add("Prism", "Prism", ext, [] { return std::make_unique<PrismObject>(); });
}

}  // namespace veraa
