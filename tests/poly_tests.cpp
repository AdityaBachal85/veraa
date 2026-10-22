// Poly modeling tests: Editable Poly operations, selection tools, mesh modifiers, Boolean,
// decimation and snapping math. Topology checks use closed-manifold and volume tests.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>

#include "modifier.h"
#include "object.h"
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

int main() {
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
