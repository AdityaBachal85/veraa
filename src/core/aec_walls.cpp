// AEC Extended: Wall, Railing and Foliage, plus the shared prism helper and registration.
#include <algorithm>
#include <cmath>
#include <functional>

#include "aec.h"
#include "meshbuild.h"
#include "shape2d.h"
#include "shape_mesh.h"
#include "shapes.h"

namespace veraa {

void addPrism(PolyMesh& m, const std::vector<Vec2>& polygon, float z0, float z1, int sideMat, int topMat,
              int bottomMat, const Mat4& xf) {
  Contour2D c{shape2d::cleaned(polygon), true};
  if (c.pts.size() < 3) return;
  if (shape2d::signedArea(c.pts) < 0) std::reverse(c.pts.begin(), c.pts.end());
  ExtrudeOptions o;
  o.capStartMatId = bottomMat;
  o.capEndMatId = topMat;
  o.sideMatId = sideMat;
  o.generateUVs = false;
  PolyMesh p = extrudeContours({c}, {{z0, 0.0f}, {z1, 0.0f}}, o);
  // capStart/capEnd follow the extrusion direction; keep top/bottom by height.
  if (z1 < z0)
    for (auto& f : p.faces) {
      if (f.matId == bottomMat && bottomMat != sideMat) f.matId = topMat;
      else if (f.matId == topMat && topMat != sideMat) f.matId = bottomMat;
    }
  p.transform(xf);
  boxMapUvs(p, 0.01f);
  m.append(p);
}

namespace {

constexpr float kUv = 0.01f;  // box-mapped UVs: one tile per 100 units

// Mitered offsets of a polyline: point i moved by d along its left normal.
std::vector<Vec2> offsetPolyline(const std::vector<Vec2>& pts, bool closed, float d) {
  size_t n = pts.size();
  std::vector<Vec2> out(n);
  auto leftN = [&](size_t i) {  // left normal of segment i -> i+1
    Vec2 s = pts[(i + 1) % n] - pts[i];
    float l = std::sqrt(s.x * s.x + s.y * s.y);
    return l > 1e-12f ? Vec2(-s.y / l, s.x / l) : Vec2(0, 1);
  };
  for (size_t i = 0; i < n; ++i) {
    bool hasPrev = closed || i > 0, hasNext = closed || i + 1 < n;
    Vec2 a = hasPrev ? leftN((i + n - 1) % n) : leftN(i);
    Vec2 b = hasNext ? leftN(i) : a;
    Vec2 mvec = a + b;
    float ml = std::sqrt(mvec.x * mvec.x + mvec.y * mvec.y);
    mvec = ml > 1e-12f ? mvec / ml : b;
    float cosA = mvec.x * b.x + mvec.y * b.y;
    out[i] = pts[i] + mvec * (d / std::max(0.2f, cosA));
  }
  return out;
}

// Point and unit tangent at distance s along a path polyline.
void pointAt(const PathFrames& F, float s, Vec3& p, Vec3& t) {
  int M = static_cast<int>(F.P.size());
  int segs = F.segments();
  for (int i = 0; i < segs; ++i) {
    int j = (i + 1) % M;
    float a = F.dist[i], b = i + 1 < M ? F.dist[i + 1] : F.length;
    if (s <= b || i == segs - 1) {
      float u = b > a ? clampf((s - a) / (b - a), 0.0f, 1.0f) : 0.0f;
      p = lerp(F.P[i], F.P[j], u);
      t = normalize(F.P[j] - F.P[i]);
      return;
    }
  }
  p = F.P.empty() ? Vec3() : F.P.back();
  t = Vec3(1, 0, 0);
}

// Upright frame at a path point (X along the path, Z up).
Mat4 uprightFrame(const Vec3& p, const Vec3& tangent) {
  Vec3 t(tangent.x, tangent.y, 0);
  if (length2(t) < 1e-12f) t = Vec3(1, 0, 0);
  t = normalize(t);
  Mat4 r;
  r.m[0][0] = t.x;
  r.m[1][0] = t.y;
  r.m[0][1] = -t.y;
  r.m[1][1] = t.x;
  return Mat4::translate(p) * r;
}

BezierShape sectionShape(int profile, float width, float depth) {
  BezierShape s;
  if (profile == 2) s.splines.push_back(makeCircleSpline(std::max(0.01f, width * 0.5f)));
  else s.splines.push_back(makeRectangleSpline(std::max(0.01f, width), std::max(0.01f, depth), 0));
  return s;
}

// Sweeps a rail section along the path at height z (section center).
void addRail(PolyMesh& m, const BezierShape& path, int profile, float width, float depth, float z, int matId) {
  if (profile == 0) return;
  SweepOptions o;
  o.banking = false;
  o.offset = Vec2(0, z);
  o.generateUVs = false;
  PolyMesh r = sweepSection(path, 6, true, shapeContours(sectionShape(profile, width, depth), 4, true), o);
  for (auto& f : r.faces) f.matId = matId;
  boxMapUvs(r, kUv);
  m.append(r);
}

// Upright post (box or cylinder) from z0 to z1 at a path point.
void addPost(PolyMesh& m, const Vec3& p, const Vec3& tangent, int profile, float width, float depth, float z0, float z1,
             int matId) {
  if (profile == 0 || z1 <= z0) return;
  MeshBuilder b(m, kUv);
  if (profile == 2) {
    b.cylinder(p + Vec3(0, 0, z0), p + Vec3(0, 0, z1), std::max(0.01f, width * 0.5f), 12, true, matId);
  } else {
    b.box(uprightFrame(p, tangent), Vec3(-depth * 0.5f, -width * 0.5f, z0), Vec3(depth * 0.5f, width * 0.5f, z1), matId);
  }
}

// ---- Foliage helpers ----

struct LeafRng {
  uint32_t s;
  explicit LeafRng(uint32_t seed) : s(seed * 747796405u + 2891336453u) {}
  float next() {
    s = s * 1664525u + 1013904223u;
    return static_cast<float>((s >> 8) & 0xFFFFFF) / 16777216.0f;
  }
  float range(float a, float b) { return a + (b - a) * next(); }
};

}  // namespace

// ---------------------------------------------------------------------------------------------
// Wall

WallObject::WallObject() {
  params.beginGroup("Parameters");
  params.addFloat("width", "Width", 20, 0.01, 1e7, 1.0);
  params.addFloat("height", "Height", 250, 0.0, 1e7, 1.0);
  params.addEnum("justify", "Justification", {"Left", "Center", "Right"}, 1);
  params.addFloat("length", "Default Length", 300, 0.01, 1e7, 1.0).tooltip =
      "Length of a straight wall created without clicking points";
}

PolyMesh WallObject::build() const {
  std::vector<Vec2> pts = points;
  bool isClosed = closed && pts.size() >= 3;
  if (pts.size() < 2) pts = {Vec2(0, 0), Vec2(params.getFloat("length"), 0)};
  float w = params.getFloat("width"), h = params.getFloat("height");
  if (h <= 1e-6f) return PolyMesh();
  int j = params.getInt("justify");
  float dl = j == 0 ? 0.0f : j == 1 ? w * 0.5f : w;  // left face offset
  float dr = dl - w;                                  // right face offset
  std::vector<Vec2> left = offsetPolyline(pts, isClosed, dl), right = offsetPolyline(pts, isClosed, dr);
  // Contours and the material of each of their edges.
  std::vector<Contour2D> contours;
  std::vector<std::vector<int>> edgeMat;
  if (!isClosed) {
    size_t n = pts.size();
    Contour2D c;
    c.closed = true;
    std::vector<int> mats;
    for (size_t i = 0; i < n; ++i) c.pts.push_back(right[i]);
    for (size_t i = n; i-- > 0;) c.pts.push_back(left[i]);
    for (size_t i = 0; i + 1 < n; ++i) mats.push_back(1);  // outside (right side)
    mats.push_back(0);                                      // end
    for (size_t i = 0; i + 1 < n; ++i) mats.push_back(2);  // inside (left side)
    mats.push_back(0);                                      // start
    if (shape2d::signedArea(c.pts) < 0) {
      std::reverse(c.pts.begin(), c.pts.end());
      // Reversing the points reverses the edge order (edge i becomes edge n-2-i, cyclically).
      std::vector<int> r(mats.size());
      size_t E = mats.size();
      for (size_t i = 0; i < E; ++i) r[(2 * E - 2 - i) % E] = mats[i];
      mats = r;
    }
    contours.push_back(std::move(c));
    edgeMat.push_back(std::move(mats));
  } else {
    bool ccw = shape2d::signedArea(pts) > 0;
    // Drawn counter-clockwise, the right side is the outside.
    Contour2D outer{ccw ? right : left, true}, inner{ccw ? left : right, true};
    if (shape2d::signedArea(outer.pts) < 0) std::reverse(outer.pts.begin(), outer.pts.end());
    if (shape2d::signedArea(inner.pts) > 0) std::reverse(inner.pts.begin(), inner.pts.end());
    edgeMat.push_back(std::vector<int>(outer.pts.size(), 1));
    edgeMat.push_back(std::vector<int>(inner.pts.size(), 2));
    contours.push_back(std::move(outer));
    contours.push_back(std::move(inner));
  }
  ExtrudeOptions o;
  o.capStartMatId = 4;
  o.capEndMatId = 3;
  o.sideMatId = 1;
  o.generateUVs = false;
  o.sharpAngleDeg = 20.0f;
  PolyMesh m = extrudeContours(contours, {{0.0f, 0.0f}, {h, 0.0f}}, o);
  // Side faces come first, contour by contour, one per edge.
  size_t f = 0;
  for (size_t c = 0; c < contours.size(); ++c)
    for (size_t e = 0; e < edgeMat[c].size() && f < m.faces.size(); ++e, ++f) m.faces[f].matId = edgeMat[c][e];
  boxMapUvs(m, kUv);
  return m;
}

json WallObject::toJson() const {
  json j = SceneObject::toJson();
  json pts = json::array();
  for (const auto& p : points) pts.push_back({p.x, p.y});
  j["points"] = pts;
  j["closed"] = closed;
  return j;
}

void WallObject::fromJson(const json& j) {
  SceneObject::fromJson(j);
  points.clear();
  if (j.contains("points"))
    for (const auto& p : j["points"])
      if (p.is_array() && p.size() >= 2) points.emplace_back(p[0].get<float>(), p[1].get<float>());
  closed = j.value("closed", false);
}

// ---------------------------------------------------------------------------------------------
// Railing

RailingObject::RailingObject() {
  const std::vector<std::string> profiles = {"None", "Square", "Round"};
  params.beginGroup("Railing");
  params.addFloat("length", "Length", 300, 0.01, 1e7, 1.0);
  params.addInt("segments", "Segments", 3, 1, 1000);
  params.addBool("respectCorners", "Respect Corners", true);
  params.beginGroup("Top Rail");
  params.addEnum("topProfile", "Profile", profiles, 1);
  params.addFloat("topDepth", "Depth", 6, 0.01, 1e7, 0.5);
  params.addFloat("topWidth", "Width", 6, 0.01, 1e7, 0.5);
  params.addFloat("height", "Height", 100, 0.0, 1e7, 1.0);
  params.beginGroup("Lower Rails");
  params.addEnum("lowerProfile", "Profile", profiles, 1);
  params.addFloat("lowerDepth", "Depth", 3, 0.01, 1e7, 0.5);
  params.addFloat("lowerWidth", "Width", 3, 0.01, 1e7, 0.5);
  params.addInt("lowerCount", "Count", 1, 0, 100);
  params.beginGroup("Posts");
  params.addEnum("postProfile", "Profile", profiles, 1);
  params.addFloat("postDepth", "Depth", 6, 0.01, 1e7, 0.5);
  params.addFloat("postWidth", "Width", 6, 0.01, 1e7, 0.5);
  params.addFloat("postExtension", "Extension", 0, 0.0, 1e7, 0.5);
  params.beginGroup("Fencing");
  params.addEnum("fencing", "Type", {"None", "Pickets", "Solid Fill"}, 1);
  params.addEnum("picketProfile", "Picket Profile", {"Square", "Round"}, 0);
  params.addFloat("picketDepth", "Picket Depth", 2, 0.01, 1e7, 0.5);
  params.addFloat("picketWidth", "Picket Width", 2, 0.01, 1e7, 0.5);
  params.addInt("picketCount", "Pickets per Segment", 6, 0, 1000);
  params.addFloat("bottomOffset", "Bottom Offset", 5, 0.0, 1e7, 0.5);
  params.addFloat("topOffset", "Top Offset (Solid)", 5, 0.0, 1e7, 0.5);
  params.addFloat("thickness", "Thickness (Solid)", 1, 0.01, 1e7, 0.1);
}

PolyMesh RailingObject::build() const {
  PolyMesh m;
  BezierShape p = path;
  if (p.empty()) {
    Spline s = makePolylineSpline({Vec2(0, 0), Vec2(params.getFloat("length"), 0)}, false);
    p.splines.push_back(s);
  }
  float height = params.getFloat("height");
  int topProfile = params.getInt("topProfile");
  float topDepth = params.getFloat("topDepth");
  for (const Spline& sp : p.splines) {
    BezierShape one;
    one.splines.push_back(sp);
    PathFrames F = pathFrames(sp.polyline(6, true), sp.closed, false);
    if (F.P.size() < 2) continue;
    // Top and lower rails.
    addRail(m, one, topProfile, params.getFloat("topWidth"), topDepth, height - topDepth * 0.5f, 0);
    int lowerCount = params.getInt("lowerCount");
    for (int k = 1; k <= lowerCount; ++k)
      addRail(m, one, params.getInt("lowerProfile"), params.getFloat("lowerWidth"), params.getFloat("lowerDepth"),
              height * static_cast<float>(k) / (lowerCount + 1), 1);
    // Post positions: evenly spaced, plus the path's corners.
    std::vector<float> at;
    int segs = params.getInt("segments");
    for (int i = 0; i <= segs; ++i) at.push_back(F.length * static_cast<float>(i) / segs);
    if (F.closed) at.pop_back();
    if (params.getBool("respectCorners"))
      for (size_t i = 1; i + 1 < sp.knots.size() + (sp.closed ? 1 : 0); ++i) {
        // Distance of knot i along the polyline: nearest path point.
        Vec3 kp = sp.knots[i % sp.knots.size()].p;
        float best = kInfinity, d = 0;
        for (size_t q = 0; q < F.P.size(); ++q)
          if (length2(F.P[q] - kp) < best) {
            best = length2(F.P[q] - kp);
            d = F.dist[q];
          }
        at.push_back(d);
      }
    std::sort(at.begin(), at.end());
    at.erase(std::unique(at.begin(), at.end(), [&](float a, float b) { return std::fabs(a - b) < 1e-3f * (1 + F.length); }),
             at.end());
    float topZ = height + params.getFloat("postExtension");
    for (float s : at) {
      Vec3 pt, tg;
      pointAt(F, s, pt, tg);
      addPost(m, pt, tg, params.getInt("postProfile"), params.getFloat("postWidth"), params.getFloat("postDepth"), 0.0f,
              topZ, 2);
    }
    // Fencing between posts.
    int fencing = params.getInt("fencing");
    float bottom = params.getFloat("bottomOffset");
    float railBottom = topProfile == 0 ? height : height - topDepth;
    if (fencing == 1) {
      int count = params.getInt("picketCount");
      std::vector<float> stops = at;
      if (F.closed) stops.push_back(F.length + (at.empty() ? 0.0f : at.front()));
      for (size_t i = 0; i + 1 < stops.size(); ++i)
        for (int k = 1; k <= count; ++k) {
          float s = stops[i] + (stops[i + 1] - stops[i]) * static_cast<float>(k) / (count + 1);
          if (s > F.length) s -= F.length;
          Vec3 pt, tg;
          pointAt(F, s, pt, tg);
          addPost(m, pt, tg, params.getInt("picketProfile") + 1, params.getFloat("picketWidth"),
                  params.getFloat("picketDepth"), bottom, railBottom, 3);
        }
    } else if (fencing == 2) {
      float top = railBottom - params.getFloat("topOffset");
      if (top > bottom) {
        SweepOptions o;
        o.banking = false;
        o.offset = Vec2(0, (top + bottom) * 0.5f);
        o.generateUVs = false;
        BezierShape panel;
        panel.splines.push_back(makeRectangleSpline(params.getFloat("thickness"), top - bottom, 0));
        PolyMesh r = sweepSection(one, 6, true, shapeContours(panel, 0, true), o);
        for (auto& f : r.faces) f.matId = 3;
        boxMapUvs(r, kUv);
        m.append(r);
      }
    }
  }
  return m;
}

json RailingObject::toJson() const {
  json j = SceneObject::toJson();
  if (!path.empty()) {
    j["path"] = bezierShapeToJson(path);
    j["pathName"] = pathName;
  }
  return j;
}

void RailingObject::fromJson(const json& j) {
  SceneObject::fromJson(j);
  path = j.contains("path") ? bezierShapeFromJson(j["path"]) : BezierShape();
  pathName = j.value("pathName", std::string());
}

// ---------------------------------------------------------------------------------------------
// Foliage

namespace {

class FoliageObject : public GeometryObject {
 public:
  FoliageObject() {
    params.beginGroup("Parameters");
    params.addEnum("plant", "Plant", {"Broadleaf Tree", "Conifer", "Palm", "Shrub"}, 0);
    params.addFloat("height", "Height", 400, 1.0, 1e7, 10.0);
    params.addFloat("density", "Density", 1, 0.0, 1.0, 0.05);
    params.addFloat("pruning", "Pruning", 0, 0.0, 1.0, 0.05);
    params.addInt("seed", "Seed", 1, 0, 99999999);
    params.beginGroup("Show");
    params.addBool("leaves", "Leaves", true);
    params.addBool("trunk", "Trunk", true);
    params.addBool("branches", "Branches", true);
    params.addBool("fruit", "Fruit", true);
    params.addBool("flowers", "Flowers", true);
    params.beginGroup("Level-of-Detail");
    params.addEnum("lod", "Level of Detail", {"Low", "Medium", "High"}, 1);
  }
  std::string type() const override { return "Foliage"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<FoliageObject>(*this); }
  PolyMesh build() const override;
};

PolyMesh FoliageObject::build() const {
  PolyMesh m;
  MeshBuilder b(m, kUv);
  LeafRng rng(static_cast<uint32_t>(params.getInt("seed")) + 7u);
  float H = params.getFloat("height");
  float density = params.getFloat("density"), pruning = params.getFloat("pruning");
  int lod = params.getInt("lod");
  int sides = lod == 0 ? 5 : lod == 1 ? 7 : 10;
  bool showLeaves = params.getBool("leaves"), showTrunk = params.getBool("trunk"),
       showBranches = params.getBool("branches"), showFruit = params.getBool("fruit"),
       showFlowers = params.getBool("flowers");
  int plant = params.getInt("plant");
  auto leaf = [&](const Vec3& at, const Vec3& dirIn, float size, int matId) {
    Vec3 dir = length2(dirIn) > 1e-12f ? normalize(dirIn) : Vec3(0, 0, 1);
    Vec3 side = cross(dir, Vec3(0, 0, 1));
    if (length2(side) < 1e-8f) side = Vec3(1, 0, 0);
    side = normalize(side);
    Vec3 tip = at + dir * size;
    Vec3 mid = at + dir * (size * 0.5f);
    b.quad2(at, mid + side * (size * 0.3f), tip, mid - side * (size * 0.3f), matId);
  };
  // Recursive branches with leaves at the tips.
  std::function<void(const Vec3&, const Vec3&, float, float, int)> branch = [&](const Vec3& a, const Vec3& dir,
                                                                                 float len, float r, int depth) {
    Vec3 e = a + dir * len;
    if (showBranches) b.cone(a, e, r, r * 0.6f, std::max(3, sides - 2), 1);
    if (depth == 0) {
      if (showLeaves) {
        int leaves = static_cast<int>(std::round((lod + 3) * density * 2));
        for (int i = 0; i < leaves; ++i) {
          Vec3 d = normalize(dir + Vec3(rng.range(-1, 1), rng.range(-1, 1), rng.range(-0.3f, 1)));
          leaf(e + Vec3(rng.range(-1, 1), rng.range(-1, 1), rng.range(-1, 1)) * (len * 0.3f), d, H * 0.04f, 2);
        }
      }
      if (showFlowers && plant == 3 && rng.next() < 0.5f) b.sphere(e, H * 0.025f, 6, 4);
      if (showFruit && plant == 0 && rng.next() < 0.15f) b.sphere(e - Vec3(0, 0, H * 0.02f), H * 0.012f, 6, 3);
      return;
    }
    int kids = 2 + static_cast<int>(rng.next() * 2);
    for (int k = 0; k < kids; ++k) {
      Vec3 d = normalize(dir * 1.2f + Vec3(rng.range(-1, 1), rng.range(-1, 1), rng.range(-0.2f, 0.8f)));
      branch(e, d, len * rng.range(0.55f, 0.75f), r * 0.6f, depth - 1);
    }
  };
  int depth = lod == 0 ? 1 : lod == 1 ? 2 : 3;
  switch (plant) {
    case 1: {  // Conifer: straight trunk with tiers of needles (cones).
      float trunkR = H * 0.025f;
      if (showTrunk) b.cone(Vec3(), Vec3(0, 0, H), trunkR, trunkR * 0.2f, sides, 0);
      int tiers = 4 + lod * 2;
      float start = H * (0.15f + 0.4f * pruning);
      for (int t = 0; t < tiers; ++t) {
        float f = static_cast<float>(t) / tiers;
        float z = start + (H - start) * f;
        float r = H * 0.28f * (1.0f - f) * (0.6f + 0.4f * density);
        float h = (H - start) / tiers * 1.8f;
        if (showLeaves && r > 0) b.cone(Vec3(0, 0, z), Vec3(0, 0, std::min(H, z + h)), r, 0.0f, sides + 3, 2);
        if (showBranches && r > 0)
          for (int k = 0; k < 4; ++k) {
            float a = kTwoPi * (k + rng.next() * 0.5f) / 4;
            b.cone(Vec3(0, 0, z + h * 0.2f), Vec3(std::cos(a) * r * 0.9f, std::sin(a) * r * 0.9f, z), trunkR * 0.3f,
                   trunkR * 0.1f, 4, 1);
          }
      }
      break;
    }
    case 2: {  // Palm: curved trunk and arching fronds.
      int segs = 6 + lod * 2;
      float lean = rng.range(0.05f, 0.15f) * H;
      Vec3 prev;
      float r0 = H * 0.035f;
      Vec3 top;
      for (int s = 1; s <= segs; ++s) {
        float f = static_cast<float>(s) / segs;
        Vec3 p(lean * f * f, 0, H * 0.85f * f);
        if (showTrunk) b.cone(prev, p, r0 * (1.0f - 0.35f * (f - 1.0f / segs)), r0 * (1.0f - 0.35f * f), sides, 0);
        prev = p;
        top = p;
      }
      int fronds = static_cast<int>(6 + 6 * density);
      for (int k = 0; k < fronds && showLeaves; ++k) {
        float a = kTwoPi * k / fronds + rng.range(-0.1f, 0.1f);
        Vec3 out(std::cos(a), std::sin(a), 0);
        Vec3 side(-out.y, out.x, 0);
        int pieces = 3 + lod * 2;
        float len = H * 0.4f;
        Vec3 p0 = top;
        for (int q = 0; q < pieces; ++q) {
          float t1 = static_cast<float>(q + 1) / pieces;
          Vec3 p1 = top + out * (len * t1) + Vec3(0, 0, len * (0.35f * t1 - 0.7f * t1 * t1));
          float w0 = H * 0.06f * std::sin(kPi * (static_cast<float>(q) / pieces * 0.8f + 0.1f));
          float w1 = H * 0.06f * std::sin(kPi * (t1 * 0.8f + 0.1f));
          b.quad2(p0 - side * w0, p0 + side * w0, p1 + side * w1, p1 - side * w1, 2);
          p0 = p1;
        }
      }
      if (showFruit)
        for (int k = 0; k < 5; ++k) {
          float a = kTwoPi * k / 5;
          b.sphere(top + Vec3(std::cos(a), std::sin(a), -0.6f) * (H * 0.035f), H * 0.025f, 8, 3);
        }
      break;
    }
    case 3: {  // Shrub: many stems from the ground.
      int stems = static_cast<int>(5 + 6 * density);
      for (int k = 0; k < stems; ++k) {
        float a = kTwoPi * (k + rng.next()) / stems;
        Vec3 d = normalize(Vec3(std::cos(a) * 0.6f, std::sin(a) * 0.6f, 1));
        branch(Vec3(), d, H * 0.45f, H * 0.02f, std::max(1, depth - 1));
      }
      break;
    }
    default: {  // Broadleaf tree: trunk then a branching crown.
      float trunkH = H * (0.35f + 0.2f * pruning);
      float trunkR = H * 0.035f;
      if (showTrunk) b.cone(Vec3(), Vec3(0, 0, trunkH), trunkR, trunkR * 0.7f, sides, 0);
      int mains = static_cast<int>(4 + 3 * density);
      for (int k = 0; k < mains; ++k) {
        float a = kTwoPi * (k + rng.next() * 0.5f) / mains;
        Vec3 d = normalize(Vec3(std::cos(a) * 0.7f, std::sin(a) * 0.7f, 1));
        branch(Vec3(0, 0, trunkH * rng.range(0.85f, 1.0f)), d, (H - trunkH) * 0.45f, trunkR * 0.55f, depth);
      }
      break;
    }
  }
  return m;
}

}  // namespace

void registerAecTypes() {
  registerDoorWindowTypes();
  const char* geo = "Geometry";
  const char* ext = "AEC Extended";
  registerObjectType({"Foliage", "Foliage", geo, ext, [] { return std::make_unique<FoliageObject>(); }});
  registerObjectType({"Railing", "Railing", geo, ext, [] { return std::make_unique<RailingObject>(); }});
  registerObjectType({"Wall", "Wall", geo, ext, [] { return std::make_unique<WallObject>(); }});
  registerStairTypes();
}

}  // namespace veraa
