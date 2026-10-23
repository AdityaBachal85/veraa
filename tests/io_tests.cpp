// Exchange format tests: FBX, glTF/GLB, STL, PLY and OBJ round trips (export, then import into a
// fresh scene and compare vertex counts, polygon counts, hierarchy and world transforms), plus a
// hand-written 3DS file.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "io_formats.h"
#include "scene.h"

using namespace veraa;
namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
      ++g_failures;                                                          \
    }                                                                        \
  } while (0)

// Tight world-space bounds of a node's triangles.
static Aabb tightBounds(const Node& n) {
  Aabb b;
  Mat4 m = n.objectWorldMatrix();
  for (const Vec3& p : n.triMesh().positions) b.expand(m.transformPoint(p));
  return b;
}

static bool near(const Mat4& a, const Mat4& b, float tol) {
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c)
      if (std::fabs(a.m[r][c] - b.m[r][c]) > tol * (c == 3 ? 10.0f : 1.0f)) return false;
  return true;
}

// Box (parent, moved, rotated, scaled) with a linked child sphere, plus a material.
static void buildScene(Scene& s) {
  Node* box = s.createNode("Box");
  box->name = "Crate";
  box->object->params.setNumber("width", 40);
  box->object->params.setNumber("length", 30);
  box->object->params.setNumber("height", 20);
  box->transform.position = Vec3(10, -20, 5);
  box->transform.rotation = Vec3(10, 20, 35);
  box->transform.scale = Vec3(1.5f, 1.0f, 0.75f);
  Material* m = s.createMaterial("Red");
  m->params.setColor("diffuse", Vec3(0.8f, 0.1f, 0.1f));
  box->materialId = m->id;
  Node* ball = s.createNode("Sphere");
  ball->name = "Ball";
  ball->object->params.setNumber("radius", 8);
  ball->transform.position = Vec3(100, 50, 30);
  s.setParent(ball->id, box->id);
  box->invalidate();
  ball->invalidate();
}

static void roundTrip(const char* ext, bool exactVertexCount, bool hierarchy) {
  Scene src;
  buildScene(src);
  std::string path = (fs::temp_directory_path() / (std::string("veraa_io_test.") + ext)).string();
  ExportOptions eo;
  eo.yUp = std::string(ext) != "stl" && std::string(ext) != "ply";
  std::string err;
  bool ok = exportScene(src, path, eo, &err);
  if (!ok) std::printf("export %s: %s\n", ext, err.c_str());
  CHECK(ok);
  Scene dst;
  ImportOptions io;
  io.convertAxes = eo.yUp;
  io.yUpOverride = false;
  ImportResult res;
  ok = importScene(dst, path, io, &res, &err);
  if (!ok) std::printf("import %s: %s\n", ext, err.c_str());
  CHECK(ok);
  if (!ok) return;
  std::printf("  %s: %zu node(s), %zu triangles:", ext, res.nodeIds.size(), res.triangles);
  for (const auto& n : dst.nodes) std::printf(" '%s'", n->name.c_str());
  std::printf("\n");
  if (hierarchy) {
    for (const char* name : {"Crate", "Ball"}) {
      Node* a = src.findNodeByName(name);
      Node* b = dst.findNodeByName(name);
      CHECK(b != nullptr);
      if (!a || !b) continue;
      size_t va = a->mesh().positions.size(), vb = b->mesh().positions.size();
      size_t fa = a->mesh().faces.size(), fb = b->mesh().faces.size();
      std::printf("  %s %s: vertices %zu -> %zu, faces %zu -> %zu\n", ext, name, va, vb, fa, fb);
      if (exactVertexCount) {
        CHECK(va == vb);
        CHECK(fa == fb);
      } else {
        CHECK(b->mesh().triangleCount() == a->mesh().triangleCount());
      }
      bool same = near(a->objectWorldMatrix(), b->objectWorldMatrix(), 1e-3f);
      if (!same) std::printf("  %s %s: world transform differs\n", ext, name);
      CHECK(same);
      // Geometry in world space matches (bounds).
      Aabb ba = tightBounds(*a), bb = tightBounds(*b);
      CHECK(length(ba.lo - bb.lo) < 0.05f && length(ba.hi - bb.hi) < 0.05f);
    }
    Node* ball = dst.findNodeByName("Ball");
    Node* crate = dst.findNodeByName("Crate");
    CHECK(ball && crate && ball->parentId == crate->id);
    CHECK(crate && dst.findMaterial(crate->materialId) != nullptr);
    if (crate && dst.findMaterial(crate->materialId)) {
      Vec3 d = dst.findMaterial(crate->materialId)->params.getColor("diffuse");
      CHECK(std::fabs(d.x - 0.8f) < 0.01f && std::fabs(d.y - 0.1f) < 0.01f);
    }
  } else {
    // Flattened formats: the merged world-space mesh keeps every triangle and the bounds.
    size_t tris = 0;
    Aabb srcBounds;
    for (const auto& n : src.nodes) {
      tris += n->triMesh().triangleCount();
      srcBounds.expand(tightBounds(*n).lo);
      srcBounds.expand(tightBounds(*n).hi);
    }
    size_t dstTris = 0;
    Aabb b;
    for (const auto& n : dst.nodes) {
      dstTris += n->triMesh().triangleCount();
      b.expand(tightBounds(*n).lo);
      b.expand(tightBounds(*n).hi);
    }
    std::printf("  %s: triangles %zu -> %zu\n", ext, tris, dstTris);
    CHECK(!dst.nodes.empty());
    CHECK(dstTris == tris);
    CHECK(length(b.lo - srcBounds.lo) < 0.05f && length(b.hi - srcBounds.hi) < 0.05f);
  }
  fs::remove(path);
  fs::remove(fs::path(path).replace_extension(".bin"));
  fs::remove(fs::path(path).replace_extension(".mtl"));
}

static void put16(std::vector<char>& b, uint16_t v) { b.insert(b.end(), reinterpret_cast<char*>(&v), reinterpret_cast<char*>(&v) + 2); }
static void put32(std::vector<char>& b, uint32_t v) { b.insert(b.end(), reinterpret_cast<char*>(&v), reinterpret_cast<char*>(&v) + 4); }
static void putF(std::vector<char>& b, float v) { b.insert(b.end(), reinterpret_cast<char*>(&v), reinterpret_cast<char*>(&v) + 4); }
static std::vector<char> chunk(uint16_t id, const std::vector<char>& body) {
  std::vector<char> c;
  put16(c, id);
  put32(c, static_cast<uint32_t>(body.size() + 6));
  c.insert(c.end(), body.begin(), body.end());
  return c;
}

static void test3ds() {
  // One tetrahedron named "Tet" with a local frame translated to (5, 0, 0).
  std::vector<char> verts;
  put16(verts, 4);
  float v[4][3] = {{5, 0, 0}, {15, 0, 0}, {5, 10, 0}, {5, 0, 10}};
  for (auto& p : v)
    for (float f : p) putF(verts, f);
  std::vector<char> faces;
  put16(faces, 4);
  int f[4][3] = {{0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {1, 2, 3}};
  for (auto& t : f) {
    for (int i : t) put16(faces, static_cast<uint16_t>(i));
    put16(faces, 0);
  }
  std::vector<char> local;
  float m[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 5, 0, 0};
  for (float x : m) putF(local, x);
  std::vector<char> mesh;
  for (const auto& c : {chunk(0x4110, verts), chunk(0x4120, faces), chunk(0x4160, local)}) mesh.insert(mesh.end(), c.begin(), c.end());
  std::vector<char> object = {'T', 'e', 't', 0};
  auto tri = chunk(0x4100, mesh);
  object.insert(object.end(), tri.begin(), tri.end());
  auto file = chunk(0x4D4D, chunk(0x3D3D, chunk(0x4000, object)));
  std::string path = (fs::temp_directory_path() / "veraa_io_test.3ds").string();
  std::ofstream(path, std::ios::binary).write(file.data(), static_cast<std::streamsize>(file.size()));
  Scene s;
  ImportResult res;
  std::string err;
  ImportOptions io;
  CHECK(importScene(s, path, io, &res, &err));
  Node* n = s.findNodeByName("Tet");
  CHECK(n != nullptr);
  if (n) {
    CHECK(n->mesh().positions.size() == 4 && n->mesh().faces.size() == 4);
    CHECK(length(n->worldPosition() - Vec3(5, 0, 0)) < 1e-4f);
    Aabb b = n->worldBounds();
    CHECK(length(b.lo - Vec3(5, 0, 0)) < 1e-4f && length(b.hi - Vec3(15, 10, 10)) < 1e-4f);
  }
  fs::remove(path);
}

int main() {
  std::printf("FBX round trip\n");
  roundTrip("fbx", true, true);
  std::printf("glTF round trip\n");
  roundTrip("gltf", false, true);
  std::printf("GLB round trip\n");
  roundTrip("glb", false, true);
  std::printf("OBJ / STL / PLY round trips\n");
  roundTrip("obj", false, false);
  roundTrip("stl", false, false);
  roundTrip("ply", false, false);
  std::printf("3DS import\n");
  test3ds();
  if (g_failures) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("All IO tests passed\n");
  return 0;
}
