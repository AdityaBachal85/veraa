// 3ds Max .max import tests. The files are generated here: a small compound-file (OLE2) writer
// and a chunk builder produce a scene with an Editable Poly cube, a Box primitive linked to it,
// materials (VRayMtl, Standard/Blinn, Multi/Sub-Object) and a V-Ray plane light, laid out the
// way docs/MAX_FORMAT.md describes. Damaged copies must fail or import partially, never crash.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "io_formats.h"
#include "material.h"
#include "scene.h"

using namespace veraa;
namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                \
    }                                                              \
  } while (0)

using Bytes = std::vector<uint8_t>;

static void put16(Bytes& b, uint32_t v) {
  b.push_back(static_cast<uint8_t>(v));
  b.push_back(static_cast<uint8_t>(v >> 8));
}
static void put32(Bytes& b, uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
static void putF(Bytes& b, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  put32(b, v);
}
static void putUtf16(Bytes& b, const std::string& s) {
  for (char c : s) put16(b, static_cast<unsigned char>(c));
}

// ---- Chunks ----

struct Chunk {
  uint16_t id = 0;
  Bytes data;
  std::vector<Chunk> kids;
  bool container = false;
};

static Chunk leaf(uint16_t id, Bytes data) {
  Chunk c;
  c.id = id;
  c.data = std::move(data);
  return c;
}
static Chunk box(uint16_t id, std::vector<Chunk> kids) {
  Chunk c;
  c.id = id;
  c.kids = std::move(kids);
  c.container = true;
  return c;
}
static void write(Bytes& out, const Chunk& c) {
  Bytes body;
  if (c.container)
    for (const Chunk& k : c.kids) write(body, k);
  else
    body = c.data;
  put16(out, c.id);
  uint32_t size = static_cast<uint32_t>(body.size() + 6);
  put32(out, c.container ? (size | 0x80000000u) : size);
  out.insert(out.end(), body.begin(), body.end());
}
static Bytes ints(std::initializer_list<int32_t> v) {
  Bytes b;
  for (int32_t x : v) put32(b, static_cast<uint32_t>(x));
  return b;
}
static Bytes floats(std::initializer_list<float> v) {
  Bytes b;
  for (float x : v) putF(b, x);
  return b;
}
static Bytes text(const std::string& s) {
  Bytes b;
  putUtf16(b, s);
  return b;
}

// ---- Compound file writer (v3, 512-byte sectors, small streams in the mini stream) ----

static Bytes writeCfb(const std::vector<std::pair<std::string, Bytes>>& streams) {
  const size_t S = 512, M = 64, cutoff = 4096;
  const uint32_t END = 0xFFFFFFFEu, FREE = 0xFFFFFFFFu, FATSECT = 0xFFFFFFFDu;
  Bytes mini;
  std::vector<uint32_t> miniFat;
  std::vector<uint32_t> start(streams.size());
  std::vector<Bytes> big;
  for (size_t i = 0; i < streams.size(); ++i) {
    const Bytes& d = streams[i].second;
    if (d.size() < cutoff) {
      uint32_t first = static_cast<uint32_t>(mini.size() / M);
      size_t n = (d.size() + M - 1) / M;
      for (size_t k = 0; k < n; ++k) miniFat.push_back(k + 1 < n ? first + static_cast<uint32_t>(k) + 1 : END);
      start[i] = n ? first : END;
      mini.insert(mini.end(), d.begin(), d.end());
      mini.resize((mini.size() + M - 1) / M * M, 0);
    }
  }
  auto secs = [&](size_t bytes) { return (bytes + S - 1) / S; };
  size_t dirSecs = secs((streams.size() + 1) * 128);
  size_t miniFatSecs = secs(miniFat.size() * 4);
  size_t miniSecs = secs(mini.size());
  size_t bigSecs = 0;
  for (size_t i = 0; i < streams.size(); ++i)
    if (streams[i].second.size() >= cutoff) bigSecs += secs(streams[i].second.size());
  size_t fatSecs = 1;
  while (fatSecs * (S / 4) < fatSecs + dirSecs + miniFatSecs + miniSecs + bigSecs) ++fatSecs;
  size_t total = fatSecs + dirSecs + miniFatSecs + miniSecs + bigSecs;
  std::vector<uint32_t> fat(fatSecs * (S / 4), FREE);
  size_t at = 0;
  for (size_t k = 0; k < fatSecs; ++k) fat[at++] = FATSECT;
  auto chain = [&](size_t n) {
    uint32_t first = static_cast<uint32_t>(at);
    for (size_t k = 0; k < n; ++k, ++at) fat[at] = k + 1 < n ? static_cast<uint32_t>(at + 1) : END;
    return n ? first : END;
  };
  uint32_t dirStart = chain(dirSecs);
  uint32_t miniFatStart = chain(miniFatSecs);
  uint32_t miniStart = chain(miniSecs);
  for (size_t i = 0; i < streams.size(); ++i)
    if (streams[i].second.size() >= cutoff) start[i] = chain(secs(streams[i].second.size()));

  Bytes out(512 + total * S, 0);
  const uint8_t sig[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
  std::memcpy(out.data(), sig, 8);
  auto set16 = [&](size_t o, uint32_t v) { out[o] = uint8_t(v); out[o + 1] = uint8_t(v >> 8); };
  auto set32 = [&](size_t o, uint32_t v) { for (int i = 0; i < 4; ++i) out[o + i] = uint8_t(v >> (8 * i)); };
  set16(0x18, 0x3E);
  set16(0x1A, 3);
  set16(0x1C, 0xFFFE);
  set16(0x1E, 9);
  set16(0x20, 6);
  set32(0x2C, static_cast<uint32_t>(fatSecs));
  set32(0x30, dirStart);
  set32(0x38, static_cast<uint32_t>(cutoff));
  set32(0x3C, miniFatSecs ? miniFatStart : END);
  set32(0x40, static_cast<uint32_t>(miniFatSecs));
  set32(0x44, END);
  for (size_t i = 0; i < 109; ++i) set32(0x4C + i * 4, i < fatSecs ? static_cast<uint32_t>(i) : FREE);
  auto sec = [&](size_t n) { return 512 + n * S; };
  for (size_t i = 0; i < fat.size(); ++i) set32(sec(0) + i * 4, fat[i]);
  // Directory: root, then one entry per stream chained through right siblings.
  auto entry = [&](size_t idx, const std::string& name, int type, uint32_t right, uint32_t child, uint32_t st,
                   uint32_t size) {
    size_t o = sec(dirStart) + idx * 128;
    for (size_t k = 0; k < name.size(); ++k) set16(o + k * 2, static_cast<unsigned char>(name[k]));
    set16(o + 0x40, static_cast<uint32_t>((name.size() + 1) * 2));
    out[o + 0x42] = static_cast<uint8_t>(type);
    out[o + 0x43] = 1;
    set32(o + 0x44, FREE);
    set32(o + 0x48, right);
    set32(o + 0x4C, child);
    set32(o + 0x74, st);
    set32(o + 0x78, size);
  };
  entry(0, "Root Entry", 5, FREE, streams.empty() ? FREE : 1, miniSecs ? miniStart : END, static_cast<uint32_t>(mini.size()));
  for (size_t i = 0; i < streams.size(); ++i)
    entry(i + 1, streams[i].first, 2, i + 1 < streams.size() ? static_cast<uint32_t>(i + 2) : FREE, FREE, start[i],
          static_cast<uint32_t>(streams[i].second.size()));
  for (size_t i = 0; i < miniFat.size(); ++i) set32(sec(miniFatStart) + i * 4, miniFat[i]);
  if (!mini.empty()) std::memcpy(out.data() + sec(miniStart), mini.data(), mini.size());
  for (size_t i = 0; i < streams.size(); ++i)
    if (streams[i].second.size() >= cutoff)
      std::memcpy(out.data() + sec(start[i]), streams[i].second.data(), streams[i].second.size());
  return out;
}

// ---- Synthetic scene ----

struct ClassDef {
  const char* name;
  uint32_t a, b, super;
};
static const ClassDef kClasses[] = {
    {"ParamBlock2", 0x82, 0, 0x82},           // 0
    {"Bezier Float", 0x2007, 0, 0x9003},      // 1
    {"Position XYZ", 0x118f7e02, 0xffee238a, 0x900b},  // 2
    {"Euler XYZ", 0x2012, 0, 0x900c},         // 3
    {"Bezier Scale", 0x2010, 0, 0x900d},      // 4
    {"Position/Rotation/Scale", 0x2005, 0, 0x9008},  // 5
    {"Editable Poly", 0x1bf8338d, 0x192f6098, 0x10},  // 6
    {"Node", 1, 0, 1},                        // 7
    {"RootNode", 2, 0, 1},                    // 8
    {"Box", 0x10, 0, 0x10},                   // 9
    {"ParamBlock", 8, 0, 8},                  // 10
    {"VRayMtl", 0x37bf3f2f, 0x7034695c, 0xc00},  // 11
    {"Standard", 2, 0, 0xc00},                // 12
    {"Blinn", 0x38, 0, 0x10b0},               // 13
    {"Multi/Sub-Object", 0x200, 0, 0xc00},    // 14
    {"VRayLight", 0x3c5575a1, 0x5fd602df, 0x30},  // 15
};

static Bytes classDirectory() {
  Bytes out;
  for (const ClassDef& c : kClasses) {
    Bytes hdr = ints({-1, static_cast<int32_t>(c.a), static_cast<int32_t>(c.b), static_cast<int32_t>(c.super)});
    write(out, box(0x2040, {leaf(0x2060, hdr), leaf(0x2042, text(c.name))}));
  }
  return out;
}

static Chunk pb2Param(uint16_t id, uint32_t type, const Bytes& value) {
  Bytes b;
  put16(b, id);
  put32(b, type);
  const uint8_t mid[9] = {0, 0, 0x82, 0, 0, 0, 0, 0, 0x40};
  b.insert(b.end(), mid, mid + 9);
  b.insert(b.end(), value.begin(), value.end());
  return leaf(0x000e, b);
}
static Chunk pb2(int ownerClass, int blockId, int owner, std::vector<Chunk> params) {
  Bytes h;
  put32(h, static_cast<uint32_t>(ownerClass));
  put16(h, static_cast<uint32_t>(blockId));
  put16(h, 0x1500);
  put16(h, 0x251c);
  put16(h, static_cast<uint32_t>(params.size()));
  put32(h, static_cast<uint32_t>(owner));
  std::vector<Chunk> kids{leaf(0x0009, h)};
  for (auto& p : params) kids.push_back(std::move(p));
  return box(0, std::move(kids));  // class 0 = ParamBlock2
}
static Chunk floatCtrl(float v) { return box(1, {leaf(0x2035, ints({2})), leaf(0x2501, floats({v}))}); }

struct SceneBuilder {
  std::vector<Chunk> objs;
  int add(Chunk c) {
    objs.push_back(std::move(c));
    return static_cast<int>(objs.size()) - 1;
  }
  // PRS controller tree; returns the PRS index.
  int prs(const Vec3& pos, const Vec3& rotRad, const Vec3& scale) {
    int px = add(floatCtrl(pos.x)), py = add(floatCtrl(pos.y)), pz = add(floatCtrl(pos.z));
    int p = add(box(2, {leaf(0x2034, ints({px, py, pz}))}));
    int rx = add(floatCtrl(rotRad.x)), ry = add(floatCtrl(rotRad.y)), rz = add(floatCtrl(rotRad.z));
    int r = add(box(3, {leaf(0x2034, ints({rx, ry, rz})), leaf(0x1003, ints({0}))}));
    int s = add(box(4, {leaf(0x2035, ints({2})), leaf(0x2505, floats({scale.x, scale.y, scale.z, 0, 0, 0, 1}))}));
    return add(box(5, {leaf(0x2034, ints({p, r, s}))}));
  }
  int node(const std::string& name, int parent, int tm, int object, int material) {
    std::vector<int32_t> r{16, 0, tm, 1, object};
    if (material >= 0) {
      r.push_back(3);
      r.push_back(material);
    }
    Bytes refs;
    for (int32_t x : r) put32(refs, static_cast<uint32_t>(x));
    return add(box(7, {leaf(0x2035, refs), leaf(0x0960, ints({parent, 0})), leaf(0x0962, text(name)),
                       leaf(0x096a, floats({0, 0, 0})), leaf(0x096b, floats({0, 0, 0, 1})),
                       leaf(0x096c, floats({1, 1, 1, 0, 0, 0, 1})), leaf(0x0974, ints({0x00336699}))}));
  }
};

// Editable Poly cube (side 20) with UVs, material IDs and smoothing groups.
static Chunk polyCube() {
  Bytes verts;
  put32(verts, 8);
  for (int i = 0; i < 8; ++i) {
    put32(verts, 0);
    putF(verts, (i & 1) ? 10.0f : -10.0f);
    putF(verts, (i & 2) ? 10.0f : -10.0f);
    putF(verts, (i & 4) ? 10.0f : -10.0f);
  }
  const int quads[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
  Bytes faces, mapFaces;
  put32(faces, 6);
  for (int f = 0; f < 6; ++f) {
    put32(faces, 4);
    for (int k = 0; k < 4; ++k) put32(faces, static_cast<uint32_t>(quads[f][k]));
    put16(faces, 0x39);  // flags, material id, smoothing group, triangulation
    put32(faces, 0);
    put16(faces, static_cast<uint32_t>(f % 2));
    put32(faces, 1u << f);
    put32(faces, 0);
    put32(faces, 2);
    put32(mapFaces, 4);
    for (int k = 0; k < 4; ++k) put32(mapFaces, static_cast<uint32_t>(k));
  }
  Bytes mapVerts;
  put32(mapVerts, 4);
  const float uvs[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
  for (const auto& t : uvs) {
    putF(mapVerts, t[0]);
    putF(mapVerts, t[1]);
    putF(mapVerts, 0);
  }
  Chunk mn = box(0x08fe, {leaf(0x0100, verts), leaf(0x011a, faces), leaf(0x0120, ints({2})), leaf(0x0124, ints({1})),
                          leaf(0x0128, mapVerts), leaf(0x012b, mapFaces)});
  return box(6, {leaf(0x2034, ints({-1, -1})), mn});
}

static Bytes buildMaxFile() {
  SceneBuilder sb;
  // Materials.
  int vrayPb = sb.add(pb2(11, 1, 0, {pb2Param(1, 2, floats({0.8f, 0.1f, 0.1f})), pb2Param(2, 2, floats({0.5f, 0.5f, 0.5f})),
                                     pb2Param(3, 0, floats({0.75f})), pb2Param(9, 0, floats({1.5f})),
                                     pb2Param(5, 2, floats({1, 1, 1}))}));
  int vray = sb.add(box(11, {leaf(0x2035, ints({23, 1, vrayPb})), box(0x5431, {leaf(0x4001, text("RedGlass"))})}));
  int blinnPb = sb.add(pb2(13, 0, 0, {pb2Param(0, 2, floats({0.1f, 0.1f, 0.1f})), pb2Param(1, 2, floats({0.2f, 0.7f, 0.3f})),
                                      pb2Param(2, 2, floats({1, 1, 1})), pb2Param(9, 6, floats({0.5f})),
                                      pb2Param(10, 6, floats({0.3f}))}));
  int blinn = sb.add(box(13, {leaf(0x2034, ints({blinnPb}))}));
  int extPb = sb.add(pb2(12, 1, 0, {pb2Param(1, 6, floats({0.5f}))}));
  int standard = sb.add(box(12, {leaf(0x2034, ints({-1, -1, blinn, extPb})), box(0x4000, {leaf(0x4001, text("GreenStd"))})}));
  Bytes tab;
  put32(tab, 2);
  for (int m : {vray, standard}) {
    tab.push_back(0x40);
    put32(tab, static_cast<uint32_t>(m));
  }
  Bytes idTab;
  put32(idTab, 2);
  for (int id : {0, 4}) {
    idTab.push_back(0x40);
    put32(idTab, static_cast<uint32_t>(id));
  }
  int multiPb = sb.add(pb2(14, 0, 0, {pb2Param(0, 0x80E, tab), pb2Param(3, 0x801, idTab)}));
  int multi = sb.add(box(14, {leaf(0x2034, ints({multiPb, vray, standard})), box(0x4000, {leaf(0x4001, text("Multi"))})}));
  int root = sb.add(box(8, {leaf(0x2035, ints({16}))}));
  // Editable Poly cube at (10, 20, 30) rotated 90 degrees about Z.
  int cube = sb.add(polyCube());
  int cubeTm = sb.prs(Vec3(10, 20, 30), Vec3(0, 0, 1.5707963f), Vec3(1));
  int cubeNode = sb.node("PolyCube", root, cubeTm, cube, multi);
  // Box primitive (old-style parameter block) linked to the cube, 5 units along its local X.
  Chunk pb = box(10, {leaf(0x0001, ints({3}))});
  float dims[3] = {30, 40, 20};
  for (int i = 0; i < 3; ++i) pb.kids.push_back(box(0x0002, {leaf(0x0003, ints({i})), leaf(0x0100, floats({dims[i]}))}));
  int boxPb = sb.add(pb);
  int boxObj = sb.add(box(9, {leaf(0x2034, ints({boxPb}))}));
  int boxTm = sb.prs(Vec3(5, 0, 0), Vec3(0), Vec3(2, 2, 2));
  sb.node("Box01", cubeNode, boxTm, boxObj, standard);
  // Instance of the cube.
  int instTm = sb.prs(Vec3(-50, 0, 0), Vec3(0), Vec3(1));
  sb.node("PolyCube001", root, instTm, cube, vray);
  // V-Ray plane light 60 x 40.
  int lightPb = sb.add(pb2(15, 0, 0, {pb2Param(1, 1, ints({0})), pb2Param(2, 7, floats({30})), pb2Param(3, 7, floats({20})),
                                      pb2Param(7, 2, floats({1, 0.9f, 0.8f})), pb2Param(8, 0, floats({12}))}));
  int light = sb.add(box(15, {leaf(0x2034, ints({lightPb}))}));
  int lightTm = sb.prs(Vec3(0, 0, 200), Vec3(0), Vec3(1));
  sb.node("VRayLight01", root, lightTm, light, -1);

  Bytes scene;
  write(scene, box(0x2008, sb.objs));
  return writeCfb({{"ClassDirectory3", classDirectory()}, {"Scene", scene}});
}

static Node* find(Scene& s, const char* name) { return s.findNodeByName(name); }

static void testImport(const fs::path& file) {
  Scene s;
  ImportResult r;
  std::string err;
  bool ok = importScene(s, file.string(), ImportOptions(), &r, &err);
  CHECK(ok);
  if (!ok) {
    std::printf("  error: %s\n", err.c_str());
    return;
  }
  for (const auto& w : r.warnings) std::printf("  %s\n", w.c_str());
  CHECK(r.nodeIds.size() == 4);
  Node* cube = find(s, "PolyCube");
  Node* boxN = find(s, "Box01");
  Node* inst = find(s, "PolyCube001");
  Node* light = find(s, "VRayLight01");
  CHECK(cube && boxN && inst && light);
  if (!cube || !boxN || !inst || !light) return;
  // Geometry.
  const PolyMesh& m = cube->mesh();
  CHECK(m.positions.size() == 8);
  CHECK(m.faces.size() == 6);
  CHECK(m.uvs.size() == 4);
  if (m.faces.size() == 6) {
    CHECK(m.faces[1].matId == 1 && m.faces[2].matId == 0);
    CHECK(m.faces[3].smoothGroups == 8u);
    CHECK(m.faces[0].uv.size() == 4);
  }
  CHECK(inst->object == cube->object);  // instances share the object
  CHECK(boxN->object->type() == "Box");
  CHECK(std::fabs(boxN->object->params.getFloat("length") - 30) < 1e-4f);
  CHECK(std::fabs(boxN->object->params.getFloat("width") - 40) < 1e-4f);
  CHECK(std::fabs(boxN->object->params.getFloat("height") - 20) < 1e-4f);
  // Hierarchy and transforms.
  CHECK(boxN->parentId == cube->id);
  Vec3 cp = cube->worldPosition();
  CHECK(std::fabs(cp.x - 10) < 1e-3f && std::fabs(cp.y - 20) < 1e-3f && std::fabs(cp.z - 30) < 1e-3f);
  Vec3 bp = boxN->worldPosition();
  CHECK(std::fabs(bp.x - 10) < 1e-3f && std::fabs(bp.y - 25) < 1e-3f && std::fabs(bp.z - 30) < 1e-3f);
  CHECK(std::fabs(cube->transform.rotation.z - 90) < 1e-2f);
  CHECK(std::fabs(boxN->transform.scale.x - 2) < 1e-4f);
  // Materials.
  const Material* mm = s.findMaterial(cube->materialId);
  CHECK(mm && mm->materialType == "MultiSubObject" && mm->subs.size() == 2);
  if (mm && mm->subs.size() == 2) {
    CHECK(mm->subs[1].matId == 5);
    CHECK(mm->subs[0].material && mm->subs[0].material->name == "RedGlass");
  }
  const Material* red = s.findMaterial(inst->materialId);
  CHECK(red && red->name == "RedGlass");
  if (red) {
    Vec3 d = red->params.getColor("diffuse");
    CHECK(std::fabs(d.x - 0.8f) < 1e-4f && std::fabs(d.y - 0.1f) < 1e-4f);
    CHECK(std::fabs(red->params.getFloat("ior") - 1.5f) < 1e-4f);
    CHECK(std::fabs(red->params.getFloat("reflectGlossiness") - 0.75f) < 1e-4f);
    CHECK(red->params.getColor("refract").x > 0.99f);
  }
  const Material* green = s.findMaterial(boxN->materialId);
  CHECK(green && green->name == "GreenStd");
  if (green) {
    CHECK(std::fabs(green->params.getColor("diffuse").y - 0.7f) < 1e-4f);
    CHECK(std::fabs(green->params.getFloat("opacity") - 0.5f) < 1e-4f);
  }
  // Light.
  CHECK(light->object->type() == "RectLight");
  CHECK(std::fabs(light->object->params.getFloat("width") - 60) < 1e-3f);
  CHECK(std::fabs(light->object->params.getFloat("length") - 40) < 1e-3f);
  CHECK(std::fabs(light->object->params.getFloat("intensity") - 12) < 1e-3f);
  CHECK(std::fabs(light->worldPosition().z - 200) < 1e-3f);
}

// Truncated and corrupted copies: the importer may fail or import part of the scene, but it
// must return.
static void testDamaged(const Bytes& good, const fs::path& dir) {
  fs::path p = dir / "damaged.max";
  std::mt19937 rng(1234);
  int imported = 0, refused = 0;
  for (int k = 0; k < 120; ++k) {
    Bytes d = good;
    if (k < 40) {
      d.resize(d.size() * static_cast<size_t>(k) / 40);
    } else {
      int flips = 1 + static_cast<int>(rng() % 16);
      for (int f = 0; f < flips; ++f) d[rng() % d.size()] = static_cast<uint8_t>(rng());
    }
    {
      std::ofstream o(p, std::ios::binary);
      o.write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()));
    }
    Scene s;
    ImportResult r;
    std::string err;
    if (importScene(s, p.string(), ImportOptions(), &r, &err)) {
      ++imported;
      for (const auto& n : s.nodes) (void)n->mesh();
    } else {
      ++refused;
      CHECK(!err.empty());
    }
  }
  std::printf("  damaged copies: %d imported (partially), %d refused\n", imported, refused);
}

int main() {
  fs::path dir = fs::temp_directory_path() / "veraa_max_tests";
  fs::create_directories(dir);
  Bytes file = buildMaxFile();
  fs::path p = dir / "synthetic.max";
  {
    std::ofstream o(p, std::ios::binary);
    o.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
  }
  std::printf("Synthetic .max import\n");
  testImport(p);
  std::printf("Damaged files\n");
  testDamaged(file, dir);
  {
    Scene s;
    std::string err;
    CHECK(!importScene(s, (dir / "missing.max").string(), ImportOptions(), nullptr, &err));
  }
  std::error_code ec;
  fs::remove_all(dir, ec);
  if (g_failures) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("All .max tests passed\n");
  return 0;
}
