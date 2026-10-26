// Core library tests: primitives, modifiers, scene files and OBJ import.
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <filesystem>
#include <fstream>
#include <map>

#include "modifier.h"
#include "obj_import.h"
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

// Signed volume of a closed triangle mesh; positive when faces point outward.
static double signedVolume(const TriMesh& t) {
  double v = 0;
  for (size_t i = 0; i + 2 < t.indices.size(); i += 3) {
    const Vec3 &a = t.positions[t.indices[i]], &b = t.positions[t.indices[i + 1]], &c = t.positions[t.indices[i + 2]];
    v += dot(a, cross(b, c)) / 6.0;
  }
  return v;
}

// Every edge used by exactly two faces, in opposite directions.
static bool isClosedManifold(const PolyMesh& m) {
  std::map<std::pair<int, int>, int> directed;
  for (const auto& f : m.faces)
    for (size_t k = 0; k < f.v.size(); ++k) directed[{f.v[k], f.v[(k + 1) % f.v.size()]}]++;
  for (const auto& [e, n] : directed) {
    if (n != 1) return false;
    auto it = directed.find({e.second, e.first});
    if (it == directed.end() || it->second != 1) return false;
  }
  return true;
}

static void testPrimitives() {
  for (const auto& type : geometryTypes()) {
    auto obj = createObject(type);
    CHECK(obj != nullptr);
    auto* g = dynamic_cast<GeometryObject*>(obj.get());
    CHECK(g != nullptr);
    PolyMesh m = g->build();
    CHECK(m.isValid());
    TriMesh t = triangulate(m);
    CHECK(!t.empty());
    CHECK(t.normals.size() == t.positions.size());
    // The teapot is a set of patches (spout and handle pass into the body), so it is not closed.
    if (type != "Plane" && type != "Teapot") {
      double vol = signedVolume(t);
      if (vol <= 0) std::printf("  %s has inward-facing faces (volume %f)\n", type.c_str(), vol);
      CHECK(vol > 0);
      bool closed = isClosedManifold(m);
      if (!closed) std::printf("  %s is not a closed manifold\n", type.c_str());
      CHECK(closed);
    }
  }
  auto box = createObject("Box");
  PolyMesh bm = static_cast<GeometryObject*>(box.get())->build();
  CHECK(bm.positions.size() == 8);
  CHECK(bm.faces.size() == 6);
  // Faceted box: 24 render vertices (4 per side), 12 triangles.
  TriMesh bt = triangulate(bm);
  CHECK(bt.positions.size() == 24);
  CHECK(bt.triangleCount() == 12);
  CHECK(bt.wireIndices.size() == 24);  // 12 edges
  // Box volume 100^3.
  CHECK(std::fabs(signedVolume(bt) - 1e6) < 1.0);
}

static void testModifiers() {
  auto box = createObject("Box");
  PolyMesh base = static_cast<GeometryObject*>(box.get())->build();
  PolyMesh s = subdivideCatmullClark(base);
  CHECK(s.faces.size() == 24);
  CHECK(s.positions.size() == 26);
  CHECK(isClosedManifold(s));
  CHECK(signedVolume(triangulate(s)) > 0);

  for (const auto& type : modifierTypes()) {
    auto mod = createModifier(type);
    CHECK(mod != nullptr);
    PolyMesh m = base;
    mod->apply(m);
    CHECK(m.isValid());
    CHECK(!triangulate(m).empty());
  }

  // Bend 90 degrees: a 200-tall box curls over with radius R = 200/(pi/2) ~ 127.3. Its top
  // face ends up vertical at x = R, and the outer side reaches z = R + 50.
  auto tall = createObject("Box");
  tall->params.setNumber("height", 200);
  tall->params.setNumber("heightSegs", 20);
  PolyMesh tm = static_cast<GeometryObject*>(tall.get())->build();
  auto bend = createModifier("Bend");
  bend->apply(tm);
  CHECK(std::fabs(tm.bounds().hi.z - 177.3f) < 1.0f);
  CHECK(std::fabs(tm.bounds().hi.x - 127.3f) < 1.0f);

  // Shell on a plane closes it.
  auto plane = createObject("Plane");
  PolyMesh pm = static_cast<GeometryObject*>(plane.get())->build();
  auto shell = createModifier("Shell");
  shell->apply(pm);
  CHECK(isClosedManifold(pm));
}

static void testSceneRoundTrip() {
  Scene scene;
  Node* b = scene.createNode("Box");
  CHECK(b && b->name == "Box001");
  Node* b2 = scene.createNode("Box");
  CHECK(b2 && b2->name == "Box002");
  b->transform.position = Vec3(10, 20, 30);
  b->transform.rotation = Vec3(0, 0, 45);
  b->modifiers.push_back(createModifier("Bend"));
  b->modifiers.push_back(createModifier("Smooth"));
  Material* m = scene.createMaterial("Glass");
  m->params.setColor("refract", Vec3(1.0f));
  b->materialId = m->id;
  scene.createNode("RectLight");
  scene.createNode("Camera");
  auto copy = b->clone();
  Node* c = scene.addNode(std::move(copy));
  CHECK(c->name == "Box003");

  json j = scene.toJson();
  Scene loaded;
  std::string err;
  CHECK(loaded.fromJson(j, &err));
  CHECK(loaded.toJson() == j);
  CHECK(loaded.nodes.size() == 5);
  Node* lb = loaded.findNodeByName("Box001");
  CHECK(lb && lb->modifiers.size() == 2);
  CHECK(lb && loaded.findMaterial(lb->materialId) != nullptr);
  CHECK(lb && lb->triMesh().triangleCount() == lb->mesh().triangleCount());

  // File round trip.
  auto path = (std::filesystem::temp_directory_path() / "veraa_test.veraa").u8string();
  CHECK(scene.save(path, &err));
  Scene fromFile;
  CHECK(fromFile.load(path, &err));
  CHECK(fromFile.toJson() == j);
  std::filesystem::remove(path);
}

static void testEuler() {
  Vec3 angles[] = {{10, 20, 30}, {-45, 60, 170}, {0, 0, 0}, {90, 0, 0}};
  for (const Vec3& a : angles) {
    Vec3 r = eulerDegFromMatrix(Mat4::fromEulerDeg(a));
    Mat4 m1 = Mat4::fromEulerDeg(a), m2 = Mat4::fromEulerDeg(r);
    float err = 0;
    for (int i = 0; i < 3; ++i)
      for (int k = 0; k < 3; ++k) err = std::max(err, std::fabs(m1.m[i][k] - m2.m[i][k]));
    CHECK(err < 1e-4f);
  }
  Mat4 t = Mat4::translate({1, 2, 3}) * Mat4::fromEulerDeg({10, 20, 30}) * Mat4::scale({2, 3, 4});
  Mat4 id = t * t.inverse();
  for (int i = 0; i < 4; ++i)
    for (int k = 0; k < 4; ++k) CHECK(std::fabs(id.m[i][k] - (i == k ? 1.0f : 0.0f)) < 1e-4f);
}

static void testObjImport() {
  auto dir = std::filesystem::temp_directory_path() / "veraa_obj_test";
  std::filesystem::create_directories(dir);
  {
    std::ofstream mtl(dir / "cube.mtl");
    mtl << "newmtl red\nKd 0.8 0.1 0.1\nKs 0.5 0.5 0.5\nNs 100\nmap_Kd tex/red.png\n";
    std::ofstream obj(dir / "cube.obj");
    obj << "mtllib cube.mtl\no Cube\n"
           "v -1 -1 -1\nv 1 -1 -1\nv 1 1 -1\nv -1 1 -1\nv -1 -1 1\nv 1 -1 1\nv 1 1 1\nv -1 1 1\n"
           "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
           "usemtl red\n"
           "f 1/1 4/4 3/3 2/2\nf 5/1 6/2 7/3 8/4\nf 1/1 2/2 6/3 5/4\n"
           "f 2/1 3/2 7/3 6/4\nf 3/1 4/2 8/3 7/4\nf 4/1 1/2 5/3 8/4\n";
  }
  Scene scene;
  ObjImportResult res;
  std::string err;
  CHECK(importObj(scene, (dir / "cube.obj").u8string(), ObjImportOptions{}, &res, &err));
  CHECK(res.nodeIds.size() == 1);
  CHECK(res.triangles == 12);
  Node* n = scene.findNode(res.nodeIds.empty() ? 0 : res.nodeIds[0]);
  CHECK(n && n->name == "Cube");
  Material* m = n ? scene.findMaterial(n->materialId) : nullptr;
  CHECK(m && m->name == "red");
  CHECK(m && m->params.getString("diffuseMap").find("red.png") != std::string::npos);
  CHECK(n && signedVolume(n->triMesh()) > 0);
  // Y-up -> Z-up: the cube's base sits on the ground plane, pivot at the bottom center.
  CHECK(n && std::fabs(n->transform.position.z - (-1.0f)) < 1e-5f);
  std::filesystem::remove_all(dir);
}

static bool near(const Vec3& a, const Vec3& b, float eps = 1e-3f) { return length(a - b) < eps; }

static void testHierarchyInstancesLayers() {
  Scene scene;
  Node* parent = scene.createNode("Box");
  Node* child = scene.createNode("Sphere");
  parent->transform.position = Vec3(100, 0, 0);
  parent->transform.rotation = Vec3(0, 0, 90);
  child->transform.position = Vec3(10, 0, 0);
  // Linking keeps the child's world position.
  CHECK(scene.setParent(child->id, parent->id));
  CHECK(near(child->worldPosition(), Vec3(10, 0, 0)));
  // Rotating the parent carries the child around it.
  parent->transform.rotation = Vec3(0, 0, 180);
  Vec3 w = child->worldPosition();
  CHECK(near(w, Vec3(100, -90, 0)));  // child local (0, 90, 0) rotated 180 deg about the parent
  // No cycles.
  CHECK(!scene.setParent(parent->id, child->id));
  // Deleting the parent unlinks the child in place.
  uint32_t childId = child->id;
  CHECK(scene.removeNode(parent->id));
  Node* c = scene.findNode(childId);
  CHECK(c && c->parentId == 0 && near(c->worldPosition(), Vec3(100, -90, 0)));

  // Pivot offset moves geometry, not the pivot.
  Node* box = scene.createNode("Box");
  box->objectOffset.position = Vec3(0, 0, -50);
  CHECK(std::fabs(box->worldBounds().lo.z - (-50.0f)) < 1e-3f);

  // Instances share the object; edits show up in both, and survive save/load.
  auto inst = box->instance();
  Node* b2 = scene.addNode(std::move(inst));
  CHECK(b2->object == box->object);
  box->object->params.setNumber("height", 300);
  box->invalidate();
  CHECK(std::fabs(b2->worldBounds().hi.z - 250.0f) < 1e-3f);
  box->modifiers.push_back(createModifier("Bend"));
  b2->modifiers = box->modifiers;
  CHECK(scene.instancesOf(*box).size() == 2);

  Layer* l = scene.createLayer("Walls");
  box->layerId = l->id;
  l->hidden = true;
  CHECK(!scene.isNodeVisible(*box));
  CHECK(scene.isNodeVisible(*b2));

  json j = scene.toJson();
  Scene loaded;
  std::string err;
  CHECK(loaded.fromJson(j, &err));
  CHECK(loaded.toJson() == j);
  Node* lb = loaded.findNodeByName(box->name);
  Node* lb2 = loaded.findNodeByName(b2->name);
  CHECK(lb && lb2 && lb->object == lb2->object);
  CHECK(lb && lb2 && lb->modifiers.size() == 1 && lb->modifiers[0] == lb2->modifiers[0]);
  CHECK(lb && !loaded.isNodeVisible(*lb));

  // Shapes: an editable spline evaluates to display lines; renderable gives a tube mesh.
  Node* line = scene.createNode("EditableSpline");
  CHECK(line && line->kind() == ObjectKind::Shape);
  auto* es = static_cast<EditableSplineObject*>(line->object.get());
  Spline s;
  s.knots = {cornerKnot({0, 0, 0}), cornerKnot({100, 0, 0}), cornerKnot({100, 100, 0})};
  s.updateAutoHandles();
  es->shape.splines.push_back(s);
  line->invalidate();
  CHECK(line->isShapeResult());
  CHECK(line->shapeLines().size() == 1 && line->shapeLines()[0].size() == 3);  // optimized straight segments
  CHECK(line->triMesh().empty());
  es->params.setNumber("renderInRenderer", 1);
  es->params.setNumber("thickness", 4);
  line->invalidate();
  CHECK(!line->triMesh().empty());
  json sj = scene.toJson();
  Scene sl;
  CHECK(sl.fromJson(sj, &err));
  CHECK(sl.toJson() == sj);

  // Registry lists categories in order.
  auto subs = objectSubcategories("Geometry");
  CHECK(!subs.empty() && subs[0] == "Standard Primitives");
  CHECK(findModifierType("Bend") && findModifierType("Bend")->category == "Parametric Deformers");
}


// Large-scene paths: binary .veraa meshes, picking BVH, parallel OBJ parsing, scene adoption.
static float bruteForcePick(const TriMesh& m, const Ray& ray) {
  float best = -1.0f;
  for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
    const Vec3 &p0 = m.positions[m.indices[i]], &p1 = m.positions[m.indices[i + 1]], &p2 = m.positions[m.indices[i + 2]];
    Vec3 e1 = p1 - p0, e2 = p2 - p0, pv = cross(ray.d, e2);
    float det = dot(e1, pv);
    if (std::fabs(det) < 1e-12f) continue;
    Vec3 tv = ray.o - p0;
    float u = dot(tv, pv) / det;
    if (u < 0 || u > 1) continue;
    Vec3 qv = cross(tv, e1);
    float v = dot(ray.d, qv) / det;
    if (v < 0 || u + v > 1) continue;
    float t = dot(e2, qv) / det;
    if (t > 1e-5f && (best < 0 || t < best)) best = t;
  }
  return best;
}

static PolyMesh wavyGrid(int k) {
  PolyMesh g;
  for (int y = 0; y <= k; ++y)
    for (int x = 0; x <= k; ++x) g.addVertex(Vec3(float(x), float(y), std::sin(x * 0.3f) * std::cos(y * 0.2f) * 3.0f));
  for (int y = 0; y < k; ++y)
    for (int x = 0; x < k; ++x) {
      int a = y * (k + 1) + x;
      g.addFace({a, a + 1, a + k + 2, a + k + 1}, {}, (x + y) % 3 == 0 ? 0u : 1u, (x * 7 + y) % 3);
    }
  return g;
}

static void testLargeScenePaths() {
  // Triangulation keeps every face (n - 2 triangles) and unique edges.
  PolyMesh grid = wavyGrid(64);
  TriMesh tri = triangulate(grid);
  CHECK(tri.triangleCount() == 64u * 64u * 2u);
  CHECK(tri.wireIndices.size() / 2 == size_t(2 * 64 * 65));
  CHECK(tri.triMatIds.size() == tri.triangleCount());
  // Picking through the BVH matches a brute-force test.
  uint32_t seed = 12345;
  auto rnd = [&] {
    seed = seed * 1664525u + 1013904223u;
    return (seed >> 8) / float(1 << 24);
  };
  int mismatches = 0, hits = 0;
  for (int i = 0; i < 300; ++i) {
    Ray r{Vec3(rnd() * 70 - 3, rnd() * 70 - 3, 20.0f), normalize(Vec3(rnd() - 0.5f, rnd() - 0.5f, -1.0f))};
    float a = intersectTriMesh(tri, r), b = bruteForcePick(tri, r);
    if (b > 0) ++hits;
    if ((a > 0) != (b > 0) || (b > 0 && std::fabs(a - b) > 1e-3f)) ++mismatches;
  }
  CHECK(tri.bvh != nullptr);
  CHECK(hits > 100);
  CHECK(mismatches == 0);

  // Binary mesh encoding round trip, and rejection of damaged data.
  grid.uvs = {Vec2(0, 0), Vec2(1, 0), Vec2(1, 1), Vec2(0, 1)};
  grid.faces[5].uv = {0, 1, 2, 3};
  std::string blob;
  encodePolyMesh(grid, blob);
  PolyMesh back;
  CHECK(decodePolyMesh(blob.data(), blob.size(), back));
  CHECK(back.positions.size() == grid.positions.size() && back.faces.size() == grid.faces.size());
  CHECK(back.faces[5].uv == grid.faces[5].uv && back.faces[6].uv.empty());
  CHECK(back.faces[7].matId == grid.faces[7].matId && back.faces[9].smoothGroups == grid.faces[9].smoothGroups);
  CHECK(!decodePolyMesh(blob.data(), blob.size() / 2, back));

  // Binary .veraa round trip keeps the meshes.
  Scene scene;
  for (int i = 0; i < 3; ++i) {
    auto node = std::make_unique<Node>();
    auto obj = std::make_unique<EditablePolyObject>();
    obj->setMesh(wavyGrid(8 + i));
    node->object = std::move(obj);
    node->name = "Grid";
    scene.addNode(std::move(node));
  }
  auto path = (std::filesystem::temp_directory_path() / "veraa_bin_test.veraa").u8string();
  std::string err;
  CHECK(scene.save(path, &err));
  Scene loaded;
  CHECK(loaded.load(path, &err));
  CHECK(loaded.nodes.size() == 3);
  CHECK(loaded.nodes.size() == 3 && loaded.nodes[2]->mesh().faces.size() == 100u);
  CHECK(loaded.toJson() == scene.toJson());
  std::filesystem::remove(path);

  // OBJ: groups, negative indices, n-gons; adopt() moves the result into another scene.
  auto dir = std::filesystem::temp_directory_path() / "veraa_obj_big";
  std::filesystem::create_directories(dir);
  {
    std::ofstream obj(dir / "parts.obj");
    obj << "o A\nv 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf -4 -3 -2 -1\n"
           "o B\nv 2 0 0\nv 3 0 0\nv 3 1 0\ns 1\nf 5 6 7\ng B\nv 2 2 0\nf 5 7 8\n";
  }
  Scene temp;
  ObjImportResult res;
  CHECK(importObj(temp, (dir / "parts.obj").u8string(), ObjImportOptions{}, &res, &err));
  CHECK(res.nodeIds.size() == 2 && res.triangles == 4);
  CHECK(temp.prepareMeshes());
  Scene dst;
  dst.createNode("Box");
  std::unordered_map<uint32_t, uint32_t> idMap;
  std::vector<uint32_t> ids = dst.adopt(temp, &idMap);
  CHECK(ids.size() == 2 && temp.nodes.empty() && dst.nodes.size() == 3);
  Node* a = dst.findNodeByName("A");
  Node* bnode = dst.findNodeByName("B");
  CHECK(a && a->mesh().faces.size() == 1 && a->mesh().faces[0].v.size() == 4);
  CHECK(bnode && bnode->triMesh().triangleCount() == 2);
  CHECK(a && idMap.size() == 2 && dst.findNode(ids[0]) == a);
  std::filesystem::remove_all(dir);
}

int main() {
  testHierarchyInstancesLayers();
  testPrimitives();
  testModifiers();
  testSceneRoundTrip();
  testEuler();
  testObjImport();
  testLargeScenePaths();
  if (g_failures) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("All core tests passed\n");
  return 0;
}
