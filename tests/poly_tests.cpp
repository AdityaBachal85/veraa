// Poly modeling tests: Editable Poly operations, selection tools, mesh modifiers, Boolean,
// decimation and snapping math. Topology checks use closed-manifold and volume tests.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <string>

#include "modifier.h"
#include "object.h"
#include "csg.h"
#include "meshmods.h"
#include "polymods.h"
#include "polyops.h"
#include "scene.h"

using namespace veraa;

static int g_failures = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
      ++g_failures;                                                          \
    }                                                                        \
  } while (0)
#define CHECK_NEAR(a, b, tol)                                                                              \
  do {                                                                                                     \
    double va_ = (a), vb_ = (b);                                                                           \
    if (!(std::fabs(va_ - vb_) <= (tol))) {                                                                \
      std::printf("FAIL %s:%d: %s = %g, expected %g\n", __FILE__, __LINE__, #a, va_, vb_);                 \
      ++g_failures;                                                                                        \
    }                                                                                                      \
  } while (0)

static PolyMesh buildObject(const std::string& type, const json& params = json::object()) {
  auto obj = createObject(type);
  auto* g = dynamic_cast<GeometryObject*>(obj.get());
  if (!g) return {};
  for (auto it = params.begin(); it != params.end(); ++it) g->params.setNumber(it.key(), it.value().get<double>());
  return g->build();
}

static PolyMesh box100() { return buildObject("Box"); }

// Index of the face whose normal is closest to dir.
static int faceToward(const PolyMesh& m, const Vec3& dir) {
  int best = -1;
  float bestDot = -2;
  for (size_t f = 0; f < m.faces.size(); ++f) {
    float d = dot(normalize(m.faceNormal(m.faces[f])), dir);
    if (d > bestDot) {
      bestDot = d;
      best = static_cast<int>(f);
    }
  }
  return best;
}

static bool cornerOf3(const PolyMesh::Face& f, int v) { return std::find(f.v.begin(), f.v.end(), v) != f.v.end(); }

static bool run(PolyMesh& m, PolySelection& sel, const json& op) {
  std::string msg;
  bool ok = runPolyOp(m, sel, op, &msg);
  if (!ok) std::printf("  op %s failed: %s\n", op.value("op", std::string()).c_str(), msg.c_str());
  return ok && m.isValid();
}

static PolySelection faceSel(std::vector<int> faces) {
  PolySelection s;
  s.level = PolyFace;
  s.faces = std::move(faces);
  s.normalize();
  return s;
}

static PolySelection edgeSel(std::vector<uint64_t> edges) {
  PolySelection s;
  s.level = PolyEdge;
  s.edges = std::move(edges);
  s.normalize();
  return s;
}

// The edge of a box between the top face and the +X face.
static uint64_t topRightEdge(const PolyMesh& m) {
  int top = faceToward(m, Vec3(0, 0, 1)), right = faceToward(m, Vec3(1, 0, 0));
  const auto& a = m.faces[top].v;
  const auto& b = m.faces[right].v;
  for (size_t i = 0; i < a.size(); ++i) {
    int x = a[i], y = a[(i + 1) % a.size()];
    for (size_t j = 0; j < b.size(); ++j)
      if (b[j] == y && b[(j + 1) % b.size()] == x) return edgeKey(x, y);
  }
  return 0;
}

static void testExtrudeInsetBevel() {
  PolyMesh m = box100();
  CHECK(isClosedManifold(m));
  CHECK_NEAR(meshVolume(m), 1e6, 1.0);
  int top = faceToward(m, Vec3(0, 0, 1));
  PolySelection sel = faceSel({top});
  CHECK(run(m, sel, {{"op", "extrudeFaces"}, {"height", 10.0}}));
  CHECK(isClosedManifold(m));
  CHECK(m.faces.size() == 10);
  CHECK_NEAR(meshVolume(m), 1e6 + 1e5, 5.0);
  CHECK(sel.faces.size() == 1);
  CHECK_NEAR(m.bounds().hi.z, 110.0, 1e-3);

  // Inset keeps the volume and adds a ring of four faces.
  CHECK(run(m, sel, {{"op", "insetFaces"}, {"amount", 10.0}}));
  CHECK(isClosedManifold(m));
  CHECK(m.faces.size() == 14);
  CHECK_NEAR(meshVolume(m), 1.1e6, 5.0);
  // The inset face is 80x80 (the Newell normal's length is twice the area).
  int f = sel.faces[0];
  CHECK_NEAR(length(m.faceNormal(m.faces[f])), 2 * 6400.0, 1.0);

  // Bevel: extrude 20 and shrink by 10 -> a frustum of 80x80 -> 60x60, height 20.
  CHECK(run(m, sel, {{"op", "bevelFaces"}, {"height", 20.0}, {"outline", -10.0}}));
  CHECK(isClosedManifold(m));
  double frustum = 20.0 / 3.0 * (6400.0 + 3600.0 + std::sqrt(6400.0 * 3600.0));
  CHECK_NEAR(meshVolume(m), 1.1e6 + frustum, 20.0);

  // Outline grows the face back to 80x80 (a prism).
  CHECK(run(m, sel, {{"op", "outlineFaces"}, {"amount", 10.0}}));
  CHECK(isClosedManifold(m));
  CHECK_NEAR(meshVolume(m), 1.1e6 + 6400.0 * 20.0, 20.0);

  // Extrude by polygon: two faces extruded separately add eight side faces.
  PolyMesh b = box100();
  PolySelection s2 = faceSel({faceToward(b, Vec3(0, 0, 1)), faceToward(b, Vec3(1, 0, 0))});
  CHECK(run(b, s2, {{"op", "extrudeFaces"}, {"height", 10.0}, {"mode", 2}}));
  CHECK(isClosedManifold(b));
  CHECK(b.faces.size() == 14);
  CHECK_NEAR(meshVolume(b), 1.2e6, 5.0);
  // Group extrude of the same two faces shares the edge between them: 6 + 7 side faces.
  PolyMesh c = box100();
  PolySelection s3 = faceSel({faceToward(c, Vec3(0, 0, 1)), faceToward(c, Vec3(1, 0, 0))});
  CHECK(run(c, s3, {{"op", "extrudeFaces"}, {"height", 10.0}, {"mode", 0}}));
  CHECK(isClosedManifold(c));
  CHECK(c.faces.size() == 6 + 6);
  // Local normal extrude is closed too.
  PolyMesh d = box100();
  PolySelection s4 = faceSel({faceToward(d, Vec3(0, 0, 1)), faceToward(d, Vec3(1, 0, 0))});
  CHECK(run(d, s4, {{"op", "extrudeFaces"}, {"height", 10.0}, {"mode", 1}}));
  CHECK(isClosedManifold(d));
  CHECK(meshVolume(d) > 1.1e6);
}

static void testChamfer() {
  // One edge: removes a triangular prism of d^2/2 * 100.
  PolyMesh m = box100();
  PolySelection sel = edgeSel({topRightEdge(m)});
  CHECK(sel.edges[0] != 0);
  CHECK(run(m, sel, {{"op", "chamferEdges"}, {"amount", 10.0}}));
  CHECK(isClosedManifold(m));
  CHECK(m.faces.size() == 7);
  CHECK_NEAR(meshVolume(m), 1e6 - 50.0 * 100.0, 5.0);

  // All edges: 6 shrunken faces + 12 edge faces + 8 corner triangles.
  PolyMesh b = box100();
  PolySelection all;
  all.level = PolyEdge;
  PolyTopology topo(b);
  for (const auto& e : topo.edges) all.edges.push_back(edgeKey(e.a, e.b));
  all.normalize();
  CHECK(run(b, all, {{"op", "chamferEdges"}, {"amount", 10.0}}));
  CHECK(isClosedManifold(b));
  CHECK(b.faces.size() == 26);
  CHECK(meshVolume(b) < 1e6 && meshVolume(b) > 0.9e6);

  // Open chamfer leaves a hole where the chamfer face would be.
  PolyMesh o = box100();
  PolySelection os = edgeSel({topRightEdge(o)});
  CHECK(run(o, os, {{"op", "chamferEdges"}, {"amount", 10.0}, {"open", true}}));
  CHECK(!isClosedManifold(o));
  CHECK(o.faces.size() == 6);

  // Vertex chamfer cuts a corner tetrahedron of d^3/6.
  PolyMesh v = box100();
  PolySelection vs;
  vs.level = PolyVertex;
  vs.verts = {0};
  CHECK(run(v, vs, {{"op", "chamferVerts"}, {"amount", 10.0}}));
  CHECK(isClosedManifold(v));
  CHECK(v.faces.size() == 7);
  CHECK_NEAR(meshVolume(v), 1e6 - 1000.0 / 6.0, 1.0);

  // A loop of edges (the four top edges) on a box: closed, 6 + 4 + 4 corner patches merge away.
  PolyMesh l = box100();
  int top = faceToward(l, Vec3(0, 0, 1));
  PolySelection ls;
  ls.level = PolyEdge;
  const auto& tv = l.faces[top].v;
  for (size_t i = 0; i < tv.size(); ++i) ls.edges.push_back(edgeKey(tv[i], tv[(i + 1) % tv.size()]));
  ls.normalize();
  CHECK(run(l, ls, {{"op", "chamferEdges"}, {"amount", 5.0}}));
  CHECK(isClosedManifold(l));
  CHECK(l.faces.size() == 10);
}

static void testEdgeVertexOps() {
  // Extrude edge and vertex keep the mesh closed.
  PolyMesh m = box100();
  PolySelection sel = edgeSel({topRightEdge(m)});
  CHECK(run(m, sel, {{"op", "extrudeEdges"}, {"height", 10.0}, {"width", 5.0}}));
  CHECK(isClosedManifold(m));
  CHECK(m.bounds().hi.x > 50.0f);

  PolyMesh v = box100();
  PolySelection vs;
  vs.level = PolyVertex;
  vs.verts = {0};
  CHECK(run(v, vs, {{"op", "extrudeVerts"}, {"height", 10.0}, {"width", 5.0}}));
  CHECK(isClosedManifold(v));
  CHECK(vs.verts.size() == 1);

  // Remove an edge merges two faces.
  PolyMesh r = box100();
  PolySelection rs = edgeSel({topRightEdge(r)});
  CHECK(run(r, rs, {{"op", "removeEdges"}}));
  CHECK(isClosedManifold(r));
  CHECK(r.faces.size() == 5);

  // Connect the four vertical edges with two segments: two new edge loops.
  PolyMesh c = box100();
  PolySelection cs;
  cs.level = PolyEdge;
  PolyTopology topo(c);
  for (const auto& e : topo.edges)
    if (std::fabs(c.positions[e.a].z - c.positions[e.b].z) > 50.0f) cs.edges.push_back(edgeKey(e.a, e.b));
  cs.normalize();
  CHECK(cs.edges.size() == 4);
  CHECK(run(c, cs, {{"op", "connectEdges"}, {"segments", 2}}));
  CHECK(isClosedManifold(c));
  CHECK(c.faces.size() == 6 + 8);
  CHECK(cs.edges.size() == 8);
  CHECK_NEAR(meshVolume(c), 1e6, 1.0);

  // Insert a loop through a ring of quads (Swift Loop).
  PolyMesh q = box100();
  PolyTopology qt(q);
  int a = -1, b = -1;
  for (const auto& e : qt.edges)
    if (std::fabs(q.positions[e.a].z - q.positions[e.b].z) > 50.0f) {
      a = e.a;
      b = e.b;
      break;
    }
  PolySelection qs;
  CHECK(run(q, qs, {{"op", "insertLoop"}, {"a", a}, {"b", b}, {"t", 0.25}}));
  CHECK(isClosedManifold(q));
  CHECK(q.faces.size() == 10);

  // Weld: break a vertex into three, then weld back.
  PolyMesh w = box100();
  PolySelection ws;
  ws.level = PolyVertex;
  ws.verts = {3};
  CHECK(run(w, ws, {{"op", "breakVerts"}}));
  CHECK(w.positions.size() == 10);
  CHECK(!isClosedManifold(w));
  CHECK(ws.verts.size() == 3);
  CHECK(run(w, ws, {{"op", "weldVerts"}, {"threshold", 0.5}}));
  CHECK(w.positions.size() == 8);
  CHECK(isClosedManifold(w));

  // Target weld two ends of an edge: the box loses a vertex.
  PolyMesh t = box100();
  PolySelection ts;
  uint64_t k = topRightEdge(t);
  CHECK(run(t, ts, {{"op", "targetWeld"}, {"from", edgeKeyA(k)}, {"to", edgeKeyB(k)}}));
  CHECK(t.positions.size() == 7);
  CHECK(isClosedManifold(t));

  // Remove a vertex from a grid merges its four faces.
  PolyMesh g = buildObject("Plane");
  CHECK(g.faces.size() == 16);
  int center = -1;
  for (size_t i = 0; i < g.positions.size(); ++i)
    if (length(g.positions[i]) < 1e-3f) center = static_cast<int>(i);
  PolySelection gs;
  gs.level = PolyVertex;
  gs.verts = {center};
  CHECK(run(g, gs, {{"op", "removeVerts"}}));
  CHECK(g.faces.size() == 13);
  CHECK(g.positions.size() == 24);

  // Splitting the loop of top edges separates the lid from the rest.
  PolyMesh s = box100();
  PolySelection ss;
  ss.level = PolyEdge;
  const auto& sv = s.faces[faceToward(s, Vec3(0, 0, 1))].v;
  for (size_t i = 0; i < sv.size(); ++i) ss.edges.push_back(edgeKey(sv[i], sv[(i + 1) % sv.size()]));
  ss.normalize();
  CHECK(run(s, ss, {{"op", "splitEdges"}}));
  CHECK(!isClosedManifold(s));
  CHECK(s.positions.size() == 12);
  CHECK(elementOf(s, faceToward(s, Vec3(0, 0, 1))).size() == 1);

  // Collapse the top face to a point: a pyramid.
  PolyMesh p = box100();
  PolySelection ps = faceSel({faceToward(p, Vec3(0, 0, 1))});
  CHECK(run(p, ps, {{"op", "collapse"}}));
  CHECK(isClosedManifold(p));
  CHECK(p.faces.size() == 5);
  CHECK_NEAR(meshVolume(p), 1e6 / 3.0, 5.0);
}

static void testBordersBridgeCap() {
  PolyMesh m = box100();
  int top = faceToward(m, Vec3(0, 0, 1));
  PolySelection sel = faceSel({top});
  CHECK(run(m, sel, {{"op", "delete"}}));
  CHECK(m.faces.size() == 5);
  CHECK(!isClosedManifold(m));
  CHECK(openBorderLoops(m).size() == 1);
  // Extrude the border up by 20, then cap: a 100x100x120 box.
  PolySelection bs;
  bs.level = PolyBorder;
  bs.edges = borderLoopOf(m, openBorderLoops(m)[0][0], openBorderLoops(m)[0][1]);
  CHECK(bs.edges.size() == 4);
  CHECK(run(m, bs, {{"op", "extrudeBorders"}, {"height", 20.0}}));
  CHECK(m.faces.size() == 9);
  CHECK(run(m, bs, {{"op", "capBorders"}}));
  CHECK(isClosedManifold(m));
  CHECK_NEAR(meshVolume(m), 1.2e6, 5.0);

  // Bridge two facing polygons of two boxes 100 apart: one 300-long bar.
  PolyMesh a = box100(), b = box100();
  b.transform(Mat4::translate(Vec3(200, 0, 0)));
  int fa = faceToward(a, Vec3(1, 0, 0));
  int fb = faceToward(b, Vec3(-1, 0, 0)) + static_cast<int>(a.faces.size());
  a.append(b);
  PolySelection ab = faceSel({fa, fb});
  CHECK(run(a, ab, {{"op", "bridgeFaces"}}));
  CHECK(isClosedManifold(a));
  CHECK(a.faces.size() == 14);
  CHECK_NEAR(meshVolume(a), 3e6, 10.0);

  // Bridge two borders: delete the facing faces first, then bridge with 3 segments.
  PolyMesh c = box100(), d = box100();
  d.transform(Mat4::translate(Vec3(200, 0, 0)));
  int fc = faceToward(c, Vec3(1, 0, 0));
  int fd = faceToward(d, Vec3(-1, 0, 0)) + static_cast<int>(c.faces.size());
  c.append(d);
  PolySelection cd = faceSel({fc, fd});
  CHECK(run(c, cd, {{"op", "delete"}}));
  PolySelection border;
  border.level = PolyBorder;
  CHECK(run(c, border, {{"op", "bridgeBorders"}, {"segments", 3}}));
  CHECK(isClosedManifold(c));
  CHECK(c.faces.size() == 10 + 12);
  CHECK_NEAR(meshVolume(c), 3e6, 10.0);

  // Bridge two open edges.
  PolyMesh e = buildObject("Plane", {{"widthSegs", 1}, {"lengthSegs", 1}});
  PolyMesh e2 = e;
  e2.transform(Mat4::translate(Vec3(0, 0, 50)));
  e2.flipFaces();
  e.append(e2);
  // Pick edges at x = +50 on both planes.
  PolySelection es;
  es.level = PolyEdge;
  PolyTopology et(e);
  for (const auto& ed : et.edges)
    if (e.positions[ed.a].x > 49 && e.positions[ed.b].x > 49) es.edges.push_back(edgeKey(ed.a, ed.b));
  es.normalize();
  CHECK(es.edges.size() == 2);
  CHECK(run(e, es, {{"op", "bridgeEdges"}, {"segments", 2}}));
  CHECK(e.faces.size() == 4);
  // The bridge is wound consistently with both planes.
  {
    std::map<std::pair<int, int>, int> directed;
    for (const auto& f : e.faces)
      for (size_t k = 0; k < f.v.size(); ++k) directed[{f.v[k], f.v[(k + 1) % f.v.size()]}]++;
    bool ok = true;
    for (const auto& [key, n] : directed) ok = ok && n == 1;
    CHECK(ok);
  }

  // Cap Holes on a box with two holes.
  PolyMesh h = box100();
  PolySelection hs = faceSel({0, 1});
  CHECK(run(h, hs, {{"op", "delete"}}));
  CHECK(capAllHoles(h) == 2);
  CHECK(isClosedManifold(h));
}

static void testFaceOps() {
  // Hinge the top face 90 degrees about one of its edges.
  PolyMesh m = box100();
  int top = faceToward(m, Vec3(0, 0, 1));
  PolySelection sel = faceSel({top});
  CHECK(run(m, sel, {{"op", "hingeFaces"}, {"angle", 90.0}, {"segments", 4}}));
  CHECK(isClosedManifold(m));
  CHECK(meshVolume(m) > 1e6);

  // Flip makes the volume negative for a flipped element.
  PolyMesh f = box100();
  PolySelection fs = faceSel({0, 1, 2, 3, 4, 5});
  CHECK(run(f, fs, {{"op", "flipFaces"}}));
  CHECK_NEAR(meshVolume(f), -1e6, 1.0);

  // Triangulate, tessellate and MSmooth keep a closed manifold.
  PolyMesh t = box100();
  PolySelection ts;
  CHECK(run(t, ts, {{"op", "triangulateFaces"}}));
  CHECK(t.faces.size() == 12);
  CHECK(isClosedManifold(t));
  PolyMesh te = box100();
  PolySelection tes;
  CHECK(run(te, tes, {{"op", "tessellate"}}));
  CHECK(te.faces.size() == 24);
  CHECK(isClosedManifold(te));
  CHECK_NEAR(meshVolume(te), 1e6, 1.0);
  PolyMesh ms = box100();
  PolySelection mss;
  CHECK(run(ms, mss, {{"op", "msmooth"}, {"iterations", 2}}));
  CHECK(ms.faces.size() == 96);
  CHECK(isClosedManifold(ms));

  // Material IDs and smoothing groups.
  PolyMesh id = box100();
  PolySelection ids = faceSel({0, 2});
  CHECK(run(id, ids, {{"op", "setMaterialId"}, {"id", 3}}));
  CHECK(facesWithMaterial(id, 2).size() == 2);
  CHECK(run(id, ids, {{"op", "clearSmoothing"}}));
  CHECK(run(id, ids, {{"op", "setSmoothing"}, {"group", 20}, {"on", true}}));
  CHECK(facesWithSmoothingGroup(id, 20).size() == 2);
  PolySelection none;
  CHECK(run(id, none, {{"op", "autoSmooth"}, {"angle", 45.0}}));
  // Box faces meet at 90 degrees: every neighbour must be in a different group.
  PolyTopology topo(id);
  bool separated = true;
  for (const auto& e : topo.edges)
    if (e.faces.size() == 2) separated = separated && !(id.faces[e.faces[0]].smoothGroups & id.faces[e.faces[1]].smoothGroups);
  CHECK(separated);
  PolyMesh sph = buildObject("Sphere");
  CHECK(run(sph, none, {{"op", "autoSmooth"}, {"angle", 45.0}}));
  bool oneGroup = true;
  for (const auto& face : sph.faces) oneGroup = oneGroup && face.smoothGroups == sph.faces[0].smoothGroups;
  CHECK(oneGroup);

  // Make planar flattens selected vertices.
  PolyMesh p = box100();
  PolySelection ps;
  ps.level = PolyVertex;
  ps.verts = {0, 1, 2, 3, 4, 5, 6, 7};
  CHECK(run(p, ps, {{"op", "makePlanar"}, {"axis", 2}}));
  CHECK_NEAR(p.bounds().extent().z, 0.0, 1e-3);

  // Relax pulls a spike back toward its neighbours.
  PolyMesh r = buildObject("Plane");
  int center = -1;
  for (size_t i = 0; i < r.positions.size(); ++i)
    if (length(r.positions[i]) < 1e-3f) center = static_cast<int>(i);
  r.positions[center].z = 40.0f;
  PolySelection rs;
  rs.level = PolyVertex;
  rs.verts = {center};
  CHECK(run(r, rs, {{"op", "relax"}, {"amount", 1.0}, {"iterations", 1}}));
  CHECK(r.positions[center].z < 1.0f);

  // Slice a box at half height: 4 faces are cut.
  PolyMesh s = box100();
  PolySelection ss;
  CHECK(run(s, ss, {{"op", "slice"}, {"point", {0, 0, 50}}, {"normal", {0, 0, 1}}}));
  CHECK(s.faces.size() == 10);
  CHECK(isClosedManifold(s));
  CHECK_NEAR(meshVolume(s), 1e6, 1.0);
  // Split slice separates the halves into two open pieces.
  PolyMesh s2 = box100();
  CHECK(run(s2, ss, {{"op", "slice"}, {"point", {0, 0, 50}}, {"normal", {0, 0, 1}}, {"split", true}}));
  CHECK(!isClosedManifold(s2));
  CHECK(s2.positions.size() == 16);

  // Cut across the top face between two opposite corners.
  PolyMesh c = box100();
  int ct = faceToward(c, Vec3(0, 0, 1));
  int va = c.faces[ct].v[0], vb = c.faces[ct].v[2];
  PolySelection cs;
  CHECK(run(c, cs, {{"op", "cut"}, {"face", ct}, {"a", va}, {"b", vb}}));
  CHECK(c.faces.size() == 7);
  CHECK(isClosedManifold(c));

  // Turn the diagonal between two triangles.
  PolyMesh tr = buildObject("Plane", {{"widthSegs", 1}, {"lengthSegs", 1}});
  PolySelection trs;
  CHECK(run(tr, trs, {{"op", "triangulateFaces"}}));
  CHECK(tr.faces.size() == 2);
  int da = -1, db = -1;
  for (int k = 0; k < 3; ++k)
    if (cornerOf3(tr.faces[1], tr.faces[0].v[k]) && cornerOf3(tr.faces[1], tr.faces[0].v[(k + 1) % 3])) {
      da = tr.faces[0].v[k];
      db = tr.faces[0].v[(k + 1) % 3];
    }
  CHECK(run(tr, trs, {{"op", "turnEdge"}, {"a", da}, {"b", db}}));
  CHECK(!cornerOf3(tr.faces[0], da) || !cornerOf3(tr.faces[0], db));
  CHECK_NEAR(length(tr.faceNormal(tr.faces[0])) + length(tr.faceNormal(tr.faces[1])), 2 * 1e4, 1.0);

  // Create a polygon on the open top of a box, then insert a vertex inside it.
  PolyMesh cf = box100();
  int cfTop = faceToward(cf, Vec3(0, 0, 1));
  std::vector<int> ring = cf.faces[cfTop].v;
  PolySelection cfs = faceSel({cfTop});
  CHECK(run(cf, cfs, {{"op", "delete"}}));
  std::vector<int> loop = openBorderLoops(cf)[0];
  CHECK(run(cf, cfs, {{"op", "createFace"}, {"verts", loop}}));
  CHECK(isClosedManifold(cf));
  CHECK(run(cf, cfs, {{"op", "insertVertexInFace"}, {"face", static_cast<int>(cf.faces.size()) - 1}, {"point", {0, 0, 100}}}));
  CHECK(isClosedManifold(cf));
  CHECK(cf.faces.size() == 9);

  // Detach to element separates the polygon from its neighbours.
  PolyMesh de = box100();
  PolySelection des = faceSel({faceToward(de, Vec3(0, 0, 1))});
  CHECK(run(de, des, {{"op", "detachElement"}}));
  CHECK(elementOf(de, faceToward(de, Vec3(0, 0, 1))).size() == 1);
  CHECK(de.positions.size() == 12);

  // Weld pairs: weld the detached lid back.
  std::vector<std::vector<int>> pairs;
  for (int v = 8; v < 12; ++v)
    for (int u = 0; u < 8; ++u)
      if (length(de.positions[v] - de.positions[u]) < 1e-3f) pairs.push_back({v, u});
  CHECK(pairs.size() == 4);
  CHECK(run(de, des, {{"op", "weldPairs"}, {"pairs", pairs}}));
  CHECK(isClosedManifold(de));

  // Border chamfer adds a strip along the open edge.
  PolyMesh bc = box100();
  PolySelection bcs = faceSel({faceToward(bc, Vec3(0, 0, 1))});
  CHECK(run(bc, bcs, {{"op", "delete"}}));
  PolySelection border;
  border.level = PolyBorder;
  auto bl = openBorderLoops(bc)[0];
  border.edges = borderLoopOf(bc, bl[0], bl[1]);
  CHECK(run(bc, border, {{"op", "chamferEdges"}, {"amount", 10.0}}));
  CHECK(bc.faces.size() == 9);
  CHECK(openBorderLoops(bc).size() == 1);
  CHECK(openBorderLoops(bc)[0].size() == 4);

  // Hide / unhide and delete.
  PolyMesh h = box100();
  PolySelection hs = faceSel({1});
  CHECK(run(h, hs, {{"op", "hide"}}));
  CHECK(hs.hiddenFaces.size() == 1 && hs.faces.empty());
  CHECK(run(h, hs, {{"op", "unhideAll"}}));
  CHECK(hs.hiddenFaces.empty());
}

static void testSelectionTools() {
  PolyMesh g6 = buildObject("Plane", {{"widthSegs", 6}, {"lengthSegs", 6}});
  PolySelection s;
  s.level = PolyFace;
  float best = 1e9f;
  for (size_t f = 0; f < g6.faces.size(); ++f) {
    Vec3 c;
    for (int v : g6.faces[f].v) c += g6.positions[v] * 0.25f;
    if (length(c - Vec3(8.33f, 8.33f, 0)) < best) {
      best = length(c - Vec3(8.33f, 8.33f, 0));
      s.faces = {static_cast<int>(f)};
    }
  }
  growSelection(g6, s);
  CHECK(s.faces.size() == 9);  // 3x3 block (faces sharing a vertex)
  shrinkSelection(g6, s);
  CHECK(s.faces.size() == 1);

  PolyMesh g = buildObject("Plane");  // 4x4 grid

  // Loop: an interior horizontal edge extends across the grid (4 edges).
  PolyTopology topo(g);
  int e0 = -1;
  for (size_t e = 0; e < topo.edges.size(); ++e) {
    const auto& ed = topo.edges[e];
    Vec3 a = g.positions[ed.a], b = g.positions[ed.b];
    if (std::fabs(a.y) < 1e-3f && std::fabs(b.y) < 1e-3f && std::min(a.x, b.x) > -1e-3f && std::min(a.x, b.x) < 1.0f)
      e0 = static_cast<int>(e);
  }
  CHECK(e0 >= 0);
  PolySelection ls;
  ls.level = PolyEdge;
  ls.edges = {edgeKey(topo.edges[e0].a, topo.edges[e0].b)};
  selectLoop(g, ls);
  CHECK(ls.edges.size() == 4);
  PolySelection rs;
  rs.level = PolyEdge;
  rs.edges = {edgeKey(topo.edges[e0].a, topo.edges[e0].b)};
  selectRing(g, rs);
  CHECK(rs.edges.size() == 5);

  // Convert face selection to vertices and back.
  PolySelection cs;
  cs.level = PolyFace;
  cs.faces = {0};
  convertSelection(g, cs, PolyVertex);
  CHECK(cs.level == PolyVertex && cs.verts.size() == 4);
  convertSelection(g, cs, PolyEdge);
  CHECK(cs.edges.size() == 4);

  // Element, angle, material and border helpers.
  PolyMesh two = box100();
  PolyMesh other = box100();
  other.transform(Mat4::translate(Vec3(300, 0, 0)));
  two.append(other);
  CHECK(elementOf(two, 0).size() == 6);
  CHECK(elementOf(two, 7).size() == 6);
  CHECK(facesByAngle(two, 0, 45.0f).size() == 1);
  PolyMesh sph = buildObject("Sphere");
  CHECK(facesByAngle(sph, 0, 60.0f).size() == sph.faces.size());
  PolyMesh open = box100();
  PolySelection del = faceSel({faceToward(open, Vec3(0, 0, 1))});
  runPolyOp(open, del, {{"op", "delete"}});
  auto loops = openBorderLoops(open);
  CHECK(loops.size() == 1 && loops[0].size() == 4);
  CHECK(borderLoopOf(open, loops[0][0], loops[0][1]).size() == 4);

  // Soft selection: weights fall off with distance.
  PolySelection ss;
  ss.level = PolyVertex;
  int center = -1;
  for (size_t i = 0; i < g.positions.size(); ++i)
    if (length(g.positions[i]) < 1e-3f) center = static_cast<int>(i);
  ss.verts = {center};
  SoftSelection soft;
  soft.enabled = true;
  soft.falloff = 40.0f;
  auto w = softSelectionWeights(g, ss, soft);
  CHECK_NEAR(w[center], 1.0, 1e-6);
  int near = -1, far = -1;
  for (size_t i = 0; i < g.positions.size(); ++i) {
    float d = length(g.positions[i]);
    if (std::fabs(d - 25.0f) < 1e-3f) near = static_cast<int>(i);
    if (d > 60.0f) far = static_cast<int>(i);
  }
  CHECK(w[near] > 0.0f && w[near] < 1.0f);
  CHECK(w[far] == 0.0f);

  // Transform op with soft selection moves neighbours partially.
  PolyMesh t = g;
  json op = {{"op", "transform"}, {"soft", {{"enabled", true}, {"falloff", 40.0}}}};
  Mat4 mv = Mat4::translate(Vec3(0, 0, 10));
  json mat = json::array();
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) mat.push_back(mv.m[r][c]);
  op["matrix"] = mat;
  CHECK(runPolyOp(t, ss, op));
  CHECK_NEAR(t.positions[center].z, 10.0, 1e-4);
  CHECK(t.positions[near].z > 0.0f && t.positions[near].z < 10.0f);
  CHECK_NEAR(t.positions[far].z, 0.0, 1e-6);

  // Selection survives a JSON round trip.
  PolySelection js;
  js.level = PolyEdge;
  js.edges = {edgeKey(1, 2), edgeKey(5, 3)};
  js.hiddenFaces = {4};
  PolySelection back;
  back.fromJson(js.toJson());
  CHECK(back.level == PolyEdge && back.edges == js.edges && back.hiddenFaces == js.hiddenFaces);

  // Editable Poly objects store the selection in scene files.
  EditablePolyObject obj;
  obj.mesh = box100();
  obj.sel = js;
  EditablePolyObject copy;
  copy.fromJson(obj.toJson());
  CHECK(copy.sel.edges == js.edges && copy.mesh.faces.size() == 6);
}

static void testStackModifiers() {
  Scene scene;
  // Edit Poly records operations and replays them on its input.
  Node* n = scene.createNode("Box");
  auto ep = std::make_shared<EditPolyModifier>();
  n->modifiers.push_back(ep);
  n->invalidate();
  CHECK(n->mesh().faces.size() == 6);
  PolySelection s = faceSel({faceToward(n->mesh(), Vec3(0, 0, 1))});
  ep->setSelection(s);
  ep->record({{"op", "extrudeFaces"}, {"height", 25.0}});
  CHECK(n->mesh().faces.size() == 10);
  CHECK_NEAR(n->mesh().bounds().hi.z, 125.0, 1e-3);
  CHECK(ep->selection().faces.size() == 1);
  // Preview operations are transient.
  ep->setPreview({{"op", "insetFaces"}, {"amount", 10.0}});
  CHECK(n->mesh().faces.size() == 14);
  ep->clearPreview();
  CHECK(n->mesh().faces.size() == 10);
  // Survives a scene round trip.
  json saved = scene.toJson();
  Scene loaded;
  CHECK(loaded.fromJson(saved));
  CHECK(loaded.nodes.size() == 1 && loaded.nodes[0]->mesh().faces.size() == 10);

  // The selection flows up: Push moves only the selected (top) vertices.
  Node* p = scene.createNode("Box");
  auto poly = std::make_shared<EditablePolyObject>();
  poly->mesh = box100();
  poly->sel = faceSel({faceToward(poly->mesh, Vec3(0, 0, 1))});
  p->object = poly;
  auto push = createModifier("Push");
  push->params.setNumber("amount", 10.0);
  p->modifiers.push_back(std::shared_ptr<Modifier>(std::move(push)));
  p->invalidate();
  Aabb pb = p->mesh().bounds();
  CHECK(pb.lo.z > -1e-3f);         // bottom unchanged
  CHECK(pb.hi.z > 100.0f + 1.0f);  // top pushed up
  // At object level, everything is pushed.
  poly->sel.level = PolyObject;
  p->invalidate();
  CHECK(p->mesh().bounds().lo.z < -1.0f);

  // Poly Select and Vol. Select drive deformers too.
  Node* q = scene.createNode("Box");
  auto ps = std::make_shared<PolySelectModifier>();
  ps->sel.level = PolyVertex;
  ps->sel.verts = {0};
  q->modifiers.push_back(ps);
  auto push2 = createModifier("Push");
  push2->params.setNumber("amount", 10.0);
  q->modifiers.push_back(std::shared_ptr<Modifier>(std::move(push2)));
  q->invalidate();
  int moved = 0;
  PolyMesh qb = box100();
  for (size_t v = 0; v < qb.positions.size(); ++v)
    if (length(q->mesh().positions[v] - qb.positions[v]) > 1e-3f) ++moved;
  CHECK(moved == 1);

  Node* r = scene.createNode("Box");
  auto vol = createModifier("VolSelect");
  vol->params.setNumber("centerZ", 50.0);  // gizmo around the top half
  vol->params.setNumber("sizeX", 200.0);
  vol->params.setNumber("sizeY", 200.0);
  vol->params.setNumber("sizeZ", 60.0);
  r->modifiers.push_back(std::shared_ptr<Modifier>(std::move(vol)));
  auto push3 = createModifier("Push");
  push3->params.setNumber("amount", 10.0);
  r->modifiers.push_back(std::shared_ptr<Modifier>(std::move(push3)));
  r->invalidate();
  CHECK(r->mesh().bounds().lo.z > -1e-3f);
  CHECK(r->mesh().bounds().hi.z > 101.0f);

  // Turn to Poly: limit polygon size splits a box's quads when the limit is 3.
  Node* t = scene.createNode("Box");
  auto ttp = createModifier("TurnToPoly");
  ttp->params.setNumber("limitSize", 1);
  ttp->params.setNumber("maxSize", 3);
  t->modifiers.push_back(std::shared_ptr<Modifier>(std::move(ttp)));
  t->invalidate();
  CHECK(t->mesh().faces.size() == 12);

  // Hidden faces are left out of the evaluated mesh.
  Node* h = scene.createNode("Box");
  auto hp = std::make_shared<EditablePolyObject>();
  hp->mesh = box100();
  hp->sel.hiddenFaces = {0, 1};
  h->object = hp;
  h->invalidate();
  CHECK(h->mesh().faces.size() == 4);
}

static PolyMesh withModifier(const PolyMesh& in, const std::string& type, const json& params = json::object(),
                             const std::function<void(Modifier&)>& setup = nullptr) {
  auto mod = createModifier(type);
  if (!mod) {
    std::printf("  missing modifier %s\n", type.c_str());
    ++g_failures;
    return in;
  }
  for (auto it = params.begin(); it != params.end(); ++it) mod->params.setNumber(it.key(), it.value().get<double>());
  if (setup) setup(*mod);
  GeomState st;
  st.mesh = in;
  mod->applyState(st);
  return st.mesh;
}

static void testMeshModifiers() {
  // FFD 2x2x2: moving the four top control points up by 50 raises the top by 50.
  {
    FfdModifier ffd(FfdModifier::Fixed2);
    PolyMesh m = box100();
    PolyMesh warm = m;
    ffd.apply(warm);  // fits the lattice
    for (int i = 0; i < ffd.pointCount(); ++i)
      if (ffd.restPoint(i).z > 50.0f) ffd.setOffset(i, Vec3(0, 0, 50));
    ffd.apply(m);
    CHECK_NEAR(m.bounds().hi.z, 150.0, 1e-3);
    CHECK_NEAR(m.bounds().lo.z, 0.0, 1e-3);
    CHECK(ffd.latticeEdges().size() == 12);
    // One corner of a 4x4x4 lattice only moves nearby vertices.
    FfdModifier f4(FfdModifier::Fixed4);
    PolyMesh g = buildObject("Box", {{"widthSegs", 4}, {"lengthSegs", 4}, {"heightSegs", 4}});
    PolyMesh gw = g;
    f4.apply(gw);
    f4.setOffset(0, Vec3(-30, 0, 0));
    PolyMesh gd = g;
    f4.apply(gd);
    CHECK(gd.bounds().lo.x < g.bounds().lo.x - 10.0f);
    int moved = 0;
    for (size_t v = 0; v < g.positions.size(); ++v)
      if (length(gd.positions[v] - g.positions[v]) > 1e-3f) ++moved;
    CHECK(moved > 0 && moved < static_cast<int>(g.positions.size()));
    // Offsets survive JSON.
    FfdModifier copy(FfdModifier::Fixed4);
    copy.fromJson(f4.toJson());
    CHECK(length(copy.offset(0) - Vec3(-30, 0, 0)) < 1e-4f);
    // FFD(cyl): moving the outer ring of the top outward widens the top.
    FfdModifier fc(FfdModifier::Cylinder);
    PolyMesh c = buildObject("Cylinder");
    PolyMesh cw = c;
    fc.apply(cw);
    for (int i = 0; i < fc.pointCount(); ++i) {
      Vec3 r = fc.restPoint(i) - fc.latticeBounds().center();
      if (fc.restPoint(i).z > fc.latticeBounds().center().z) fc.setOffset(i, Vec3(r.x, r.y, 0) * 0.5f);
    }
    PolyMesh cd = c;
    fc.apply(cd);
    CHECK(cd.bounds().extent().x > c.bounds().extent().x + 1.0f);
  }
  // Symmetry: half of the box mirrored back is still a closed 100-cube.
  {
    PolyMesh m = withModifier(box100(), "Symmetry", {{"axis", 0}});
    CHECK(isClosedManifold(m));
    CHECK_NEAR(meshVolume(m), 1e6, 5.0);
    CHECK(m.faces.size() == 10);
    // Offset mirror plane: the kept side is x in [20, 50], mirrored to [-10, 20].
    PolyMesh o = withModifier(box100(), "Symmetry", {{"axis", 0}, {"offset", 20.0}});
    CHECK(isClosedManifold(o));
    CHECK_NEAR(o.bounds().lo.x, -10.0, 1e-3);
    CHECK_NEAR(meshVolume(o), 60.0 * 100.0 * 100.0, 5.0);
  }
  // Slice: remove top cuts the box in half; refine keeps it closed.
  {
    PolyMesh r = withModifier(box100(), "Slice", {{"mode", 0}});
    CHECK(isClosedManifold(r) && r.faces.size() == 10);
    PolyMesh t = withModifier(box100(), "Slice", {{"mode", 2}});
    CHECK_NEAR(t.bounds().hi.z, 50.0, 1e-3);
    PolyMesh capped = withModifier(t, "CapHoles");
    CHECK(isClosedManifold(capped));
    CHECK_NEAR(meshVolume(capped), 5e5, 5.0);
    PolyMesh rot = withModifier(box100(), "Slice", {{"mode", 3}, {"rotX", 45.0}});
    CHECK(rot.faces.size() == 4);  // the diagonal plane keeps top, front and two side triangles
  }
  // Chamfer modifier rounds all 90-degree edges: 26 faces, closed.
  {
    PolyMesh c = withModifier(box100(), "Chamfer", {{"amount", 5.0}});
    CHECK(isClosedManifold(c));
    CHECK(c.faces.size() == 26);
  }
  // Optimize (quadric decimation) reaches the face budget and stays closed.
  {
    PolyMesh s = buildObject("Sphere", {{"segments", 48}});
    size_t tris = s.triangleCount();
    PolyMesh d = withModifier(s, "Optimize", {{"percent", 25.0}});
    CHECK(d.faces.size() <= tris / 4 + 2);
    CHECK(d.faces.size() >= tris / 4 - 2);
    CHECK(isClosedManifold(d));
    double v0 = meshVolume(s), v1 = meshVolume(d);
    CHECK(std::fabs(v1 - v0) / v0 < 0.1);
    PolyMesh d2 = decimateMesh(s, 100, true);
    CHECK(d2.faces.size() <= 100 && d2.faces.size() >= 90);
    CHECK(isClosedManifold(d2));
  }
  // Lattice: 12 struts (closed cylinders) and 8 joints.
  {
    PolyMesh l = withModifier(box100(), "Lattice", {{"strutSides", 6}, {"jointType", 1}});
    CHECK(isClosedManifold(l));
    CHECK(l.faces.size() == 12 * (6 + 2) + 8 * 8);
  }
  // Deformers.
  {
    PolyMesh sph = withModifier(box100(), "Spherify", {{"percent", 100.0}});
    Vec3 c = sph.bounds().center();
    float r0 = length(sph.positions[0] - c), r1 = length(sph.positions[5] - c);
    CHECK(std::fabs(r0 - r1) < 1e-3f);
    PolyMesh sk = withModifier(box100(), "Skew", {{"amount", 30.0}});
    CHECK_NEAR(sk.bounds().hi.x, 80.0, 1e-3);
    PolyMesh st = withModifier(box100(), "Stretch", {{"stretch", 1.0}});
    CHECK(st.bounds().extent().z > 150.0f);
    PolyMesh grid = buildObject("Plane", {{"widthSegs", 20}, {"lengthSegs", 20}});
    PolyMesh rp = withModifier(grid, "Ripple", {{"amp1", 5.0}, {"amp2", 5.0}, {"wavelength", 30.0}});
    CHECK(rp.bounds().extent().z > 1.0f);
    PolyMesh wv = withModifier(grid, "Wave", {{"amp1", 5.0}, {"amp2", 5.0}, {"wavelength", 30.0}});
    CHECK(wv.bounds().extent().z > 1.0f);
    PolyMesh ml = withModifier(box100(), "Melt", {{"amount", 100.0}});
    CHECK(ml.bounds().extent().z < 99.0f && ml.bounds().extent().x > 100.0f);
    PolyMesh sq = withModifier(buildObject("Cylinder"), "Squeeze", {{"squeeze", 0.5}});
    CHECK(sq.bounds().extent().x <= buildObject("Cylinder").bounds().extent().x + 1e-3f);
    PolyMesh dn = withModifier(grid, "Displace", {{"strength", 20.0}, {"noiseSize", 10.0}});
    CHECK(dn.bounds().extent().z > 0.5f);
    PolyMesh rl = withModifier(grid, "Relax", {{"amount", 1.0}, {"iterations", 5}});
    CHECK(rl.positions.size() == grid.positions.size());
  }
  // Topology modifiers.
  {
    PolyMesh t = withModifier(box100(), "Tessellate", {{"iterations", 2}});
    CHECK(t.faces.size() == 6 * 16);
    CHECK(isClosedManifold(t));
    PolyMesh sd = withModifier(box100(), "Subdivide", {{"size", 30.0}});
    CHECK(sd.faces.size() == 6 * 16);
    PolyMesh fe = withModifier(box100(), "FaceExtrude", {{"amount", 10.0}});
    CHECK(isClosedManifold(fe));
    CHECK(fe.bounds().hi.z > 105.0f);
    PolyMesh mat = withModifier(box100(), "Material", {{"id", 4}});
    CHECK(facesWithMaterial(mat, 3).size() == 6);
    PolyMesh two = box100();
    PolyMesh other = box100();
    other.transform(Mat4::translate(Vec3(300, 0, 0)));
    two.append(other);
    PolyMesh mbe = withModifier(two, "MaterialByElement", {{"idCount", 5}});
    CHECK(mbe.faces[0].matId == mbe.faces[5].matId && mbe.faces[6].matId == mbe.faces[11].matId);
    PolyMesh as = withModifier(box100(), "AutoSmooth", {{"threshold", 30.0}});
    CHECK(!(as.faces[0].smoothGroups & as.faces[2].smoothGroups) || as.faces[0].smoothGroups == 0);
    // Vertex Weld rejoins a box whose corners were broken apart.
    PolyMesh br = box100();
    PolySelection bs;
    bs.level = PolyVertex;
    bs.verts = {0, 1, 2, 3, 4, 5, 6, 7};
    runPolyOp(br, bs, {{"op", "breakVerts"}});
    CHECK(br.positions.size() == 24);
    PolyMesh vw = withModifier(br, "VertexWeld", {{"threshold", 0.5}});
    CHECK(vw.positions.size() == 8 && isClosedManifold(vw));
    PolyMesh tp = withModifier(buildObject("Plane"), "TurnToPoly", {{"limitSize", 1}, {"maxSize", 3}});
    CHECK(tp.faces.size() == 32);
  }
  // UVs: UVW XForm tiles; Unwrap flattens into the unit square.
  {
    PolyMesh m = buildObject("Plane");
    bool hasUv = !m.uvs.empty();
    if (hasUv) {
      PolyMesh x = withModifier(m, "UVWXform", {{"uTile", 2.0}});
      float maxU = 0;
      for (const auto& t : x.uvs) maxU = std::max(maxU, t.x);
      CHECK(maxU > 1.5f);
    }
    PolyMesh b = box100();
    std::vector<Vec2> uvs;
    std::vector<std::vector<int>> fuv;
    flattenUvs(b, uvs, fuv, 45.0f, 0.02f);
    CHECK(fuv.size() == 6);
    bool inside = true;
    for (const auto& t : uvs) inside = inside && t.x >= -1e-4f && t.x <= 1.0001f && t.y >= -1e-4f && t.y <= 1.0001f;
    CHECK(inside);
    // Charts must not overlap: total UV area of six 1x1 squares stays below 1.
    double area = 0;
    for (size_t f = 0; f < fuv.size(); ++f) {
      const auto& c = fuv[f];
      for (size_t k = 0; k < c.size(); ++k) {
        const Vec2& p = uvs[c[k]];
        const Vec2& q = uvs[c[(k + 1) % c.size()]];
        area += 0.5 * (p.x * q.y - q.x * p.y);
      }
    }
    CHECK(std::fabs(area) > 0.2 && std::fabs(area) <= 1.0001);
    UnwrapUvwModifier uw;
    uw.initFrom(b);
    CHECK(uw.matches(b));
    PolyMesh applied = b;
    uw.apply(applied);
    CHECK(applied.faces[0].uv.size() == 4);
    UnwrapUvwModifier uw2;
    uw2.fromJson(uw.toJson());
    CHECK(uw2.uvs.size() == uw.uvs.size() && uw2.faceUvs == uw.faceUvs);
    // Different topology passes through untouched.
    PolyMesh tri = buildObject("Plane");
    PolyMesh tri2 = tri;
    uw.apply(tri2);
    CHECK(tri2.uvs.size() == tri.uvs.size());
    // Planar projection of one face, stitch and relax keep indices valid.
    projectFaceUvs(b, uvs, fuv, {0}, 0);
    std::vector<int> sel;
    for (int t : fuv[0]) sel.push_back(t);
    for (int t : fuv[1]) sel.push_back(t);
    stitchUvs(b, uvs, fuv, sel);
    relaxUvs(uvs, fuv, {}, 0.5f, 3);
    compactUvList(uvs, fuv);
    bool valid = true;
    for (const auto& f : fuv)
      for (int t : f) valid = valid && t >= 0 && t < static_cast<int>(uvs.size());
    CHECK(valid);
  }
}

static void testCsg() {
  PolyMesh a = box100();
  PolyMesh b = box100();
  b.transform(Mat4::translate(Vec3(50, 50, 50)));  // overlap: 50 x 50 x 50
  double overlap = 125000.0;
  PolyMesh u = csgMesh(a, b, CsgOp::Union);
  CHECK_NEAR(meshVolume(u), 2e6 - overlap, 10.0);
  PolyMesh in = csgMesh(a, b, CsgOp::Intersection);
  CHECK_NEAR(meshVolume(in), overlap, 10.0);
  PolyMesh ab = csgMesh(a, b, CsgOp::SubtractAB);
  CHECK_NEAR(meshVolume(ab), 1e6 - overlap, 10.0);
  PolyMesh ba = csgMesh(a, b, CsgOp::SubtractBA);
  CHECK_NEAR(meshVolume(ba), 1e6 - overlap, 10.0);
  CHECK(ab.bounds().lo.x < -49.0f && ba.bounds().hi.x > 99.0f && ba.bounds().lo.x > -1.0f);
  PolyMesh mg = csgMesh(a, b, CsgOp::Merge);
  CHECK(mg.faces.size() == 12);
  // Disjoint operands: subtraction keeps A, intersection is empty.
  PolyMesh far = box100();
  far.transform(Mat4::translate(Vec3(500, 0, 0)));
  CHECK_NEAR(meshVolume(csgMesh(a, far, CsgOp::SubtractAB)), 1e6, 10.0);
  CHECK(csgMesh(a, far, CsgOp::Intersection).faces.empty());
  // A cylinder drilled through a box removes pi r^2 h.
  PolyMesh cyl = buildObject("Cylinder", {{"radius", 20.0}, {"height", 200.0}, {"sides", 64}});
  cyl.transform(Mat4::translate(Vec3(0, 0, -50)));
  PolyMesh drilled = csgMesh(a, cyl, CsgOp::SubtractAB);
  double hole = meshVolume(csgMesh(a, cyl, CsgOp::Intersection));
  CHECK(std::fabs(hole - kPi * 400.0 * 100.0) / (kPi * 400.0 * 100.0) < 0.01);
  CHECK_NEAR(meshVolume(drilled), 1e6 - hole, 50.0);
  // Sphere minus box: the remaining volume is positive and smaller.
  PolyMesh sph = buildObject("Sphere", {{"radius", 60.0}});
  double vs = meshVolume(sph);
  double vsb = meshVolume(csgMesh(sph, a, CsgOp::SubtractAB));
  CHECK(vsb > 0 && vsb < vs);
  CHECK_NEAR(vsb + meshVolume(csgMesh(sph, a, CsgOp::Intersection)), vs, vs * 1e-3);

  // The Boolean object combines its operands and survives a JSON round trip.
  BooleanObject bo;
  bo.meshA = box100();
  BooleanObject::Operand op;
  op.mesh = box100();
  op.transform = Mat4::translate(Vec3(50, 50, 50));
  op.op = CsgOp::SubtractAB;
  bo.operands.push_back(op);
  CHECK_NEAR(meshVolume(bo.build()), 1e6 - overlap, 10.0);
  BooleanObject copy;
  copy.fromJson(bo.toJson());
  CHECK(copy.operands.size() == 1);
  CHECK_NEAR(meshVolume(copy.build()), 1e6 - overlap, 10.0);
}

static void testPrimitives() {
  // Every Hedra family (with and without truncation) is a closed solid.
  for (int fam = 0; fam < 7; ++fam)
    for (double p : {0.0, 0.5}) {
      PolyMesh h = buildObject("Hedra", {{"family", fam}, {"p", p}});
      bool ok = isClosedManifold(h) && meshVolume(h) > 0;
      if (!ok) std::printf("  Hedra family %d p %.1f is not a closed solid\n", fam, p);
      CHECK(ok);
      CHECK_NEAR(h.bounds().radius(), 50.0 * std::sqrt(3.0), 40.0);
    }
  CHECK(buildObject("Hedra", {{"family", 1}}).faces.size() == 6);
  CHECK(buildObject("Hedra", {{"family", 3}}).faces.size() == 12);
  CHECK(buildObject("Hedra", {{"family", 4}}).faces.size() == 20);
  CHECK(buildObject("Hedra", {{"family", 1}, {"p", 0.5}}).faces.size() == 14);  // truncated cube
  // Parameter variations stay closed.
  const char* closedTypes[] = {"TorusKnot", "ChamferCyl", "OilTank", "Capsule", "Spindle", "LExt",
                               "Gengon",    "CExt",       "RingWave", "Hose",   "Prism"};
  for (const char* t : closedTypes) {
    PolyMesh m = buildObject(t);
    bool ok = isClosedManifold(m) && meshVolume(m) > 0;
    if (!ok) std::printf("  %s is not a closed solid\n", t);
    CHECK(ok);
  }
  CHECK(isClosedManifold(buildObject("ChamferCyl", {{"fillet", 0.0}})));
  CHECK(isClosedManifold(buildObject("Gengon", {{"fillet", 8.0}, {"filletSegs", 3}})));
  CHECK(isClosedManifold(buildObject("TorusKnot", {{"p", 3.0}, {"q", 5.0}, {"twist", 90.0}})));
  CHECK(isClosedManifold(buildObject("Hose", {{"flex", 0}})));
  // Capsule: overall height includes the caps.
  PolyMesh cap = buildObject("Capsule", {{"radius", 20.0}, {"height", 100.0}});
  CHECK_NEAR(cap.bounds().extent().z, 100.0, 1e-3);
  PolyMesh capC = buildObject("Capsule", {{"radius", 20.0}, {"height", 100.0}, {"heightMode", 1}});
  CHECK_NEAR(capC.bounds().extent().z, 140.0, 1e-3);
  // ChamferCyl volume is a bit less than the cylinder's.
  double vc = meshVolume(buildObject("ChamferCyl", {{"fillet", 10.0}, {"sides", 64}}));
  CHECK(vc < kPi * 2500.0 * 100.0 && vc > kPi * 2500.0 * 100.0 * 0.9);
  // L-Ext area: 100x20 + 20x80 = 3600 per unit height.
  CHECK_NEAR(meshVolume(buildObject("LExt")), 3600.0 * 50.0, 1.0);
  // Prism: equilateral triangle of side 100.
  CHECK_NEAR(meshVolume(buildObject("Prism")), 100.0 * 100.0 * std::sqrt(3.0) / 4.0 * 100.0, 10.0);
  // Teapot: patches face outward (positive volume), every part can be turned off.
  PolyMesh tp = buildObject("Teapot");
  CHECK(tp.faces.size() > 500);
  CHECK(meshVolume(tp) > 0);
  CHECK_NEAR(tp.bounds().hi.z, 25.0 / 2.0 * 3.15, 0.01);
  PolyMesh noHandle = buildObject("Teapot", {{"handle", 0}, {"spout", 0}, {"lid", 0}});
  CHECK(noHandle.faces.size() < tp.faces.size());
  CHECK(noHandle.bounds().extent().x < tp.bounds().extent().x);
}

int main() {
  testPrimitives();
  testCsg();
  testStackModifiers();
  testMeshModifiers();
  testExtrudeInsetBevel();
  testChamfer();
  testEdgeVertexOps();
  testBordersBridgeCap();
  testFaceOps();
  testSelectionTools();
  if (g_failures) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("All poly tests passed\n");
  return 0;
}
