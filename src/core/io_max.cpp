// 3ds Max .max scene import. The layout notes this code relies on are in docs/MAX_FORMAT.md;
// everything was worked out from sample files, so unknown data is skipped, every read is
// bounds-checked, and anything that cannot be understood becomes a warning, never a crash.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>

#include "cfb.h"
#include "io_util.h"
#include "material.h"
#include "miniz/miniz.h"
#include "texmap.h"

namespace veraa {

namespace {

// ---------------------------------------------------------------------------------------------
// Chunk trees

struct Chunk {
  uint16_t id = 0;
  bool container = false;
  size_t begin = 0, end = 0;  // payload range in the stream
  std::vector<Chunk> kids;
  size_t size() const { return end - begin; }
};

struct ChunkParser {
  const std::vector<uint8_t>& d;
  size_t budget = 50'000'000;  // max chunks, guards against pathological input
  bool damaged = false;

  uint32_t u32(size_t o) const {
    return static_cast<uint32_t>(d[o]) | (static_cast<uint32_t>(d[o + 1]) << 8) | (static_cast<uint32_t>(d[o + 2]) << 16) |
           (static_cast<uint32_t>(d[o + 3]) << 24);
  }

  void parse(size_t b, size_t e, int depth, std::vector<Chunk>& out) {
    size_t off = b;
    while (off + 6 <= e) {
      if (budget == 0) {
        damaged = true;
        return;
      }
      --budget;
      Chunk c;
      c.id = static_cast<uint16_t>(d[off] | (d[off + 1] << 8));
      uint64_t sz = u32(off + 2);
      size_t hdr = 6;
      if (sz == 0) {
        if (off + 14 > e) {
          damaged = true;
          return;
        }
        uint64_t lo = u32(off + 6), hi = u32(off + 10);
        sz = lo | (hi << 32);
        hdr = 14;
        c.container = (sz >> 63) != 0;
        sz &= ~(uint64_t(1) << 63);
      } else {
        c.container = (sz & 0x80000000u) != 0;
        sz &= 0x7FFFFFFFu;
      }
      if (sz < hdr || sz > e - off) {
        damaged = true;
        return;
      }
      c.begin = off + hdr;
      c.end = off + static_cast<size_t>(sz);
      if (c.container) {
        if (depth >= 48) {
          damaged = true;
        } else {
          parse(c.begin, c.end, depth + 1, c.kids);
        }
      }
      out.push_back(std::move(c));
      off += static_cast<size_t>(sz);
    }
    if (off != e) damaged = true;
  }
};

const Chunk* child(const Chunk* c, uint16_t id) {
  if (!c) return nullptr;
  for (const Chunk& k : c->kids)
    if (k.id == id) return &k;
  return nullptr;
}

// Depth-first search for a chunk with this id.
const Chunk* findDeep(const Chunk* c, uint16_t id, int depth = 0) {
  if (!c || depth > 12) return nullptr;
  for (const Chunk& k : c->kids) {
    if (k.id == id) return &k;
    if (const Chunk* r = findDeep(&k, id, depth + 1)) return r;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------------------------
// Byte access

struct Bytes {
  const uint8_t* p = nullptr;
  size_t n = 0;
  Bytes() = default;
  Bytes(const std::vector<uint8_t>& d, const Chunk* c) {
    if (c && c->end <= d.size() && c->begin <= c->end) {
      p = d.data() + c->begin;
      n = c->size();
    }
  }
  bool has(size_t o, size_t len) const { return p && o <= n && len <= n - o; }
  uint8_t u8(size_t o) const { return has(o, 1) ? p[o] : 0; }
  uint16_t u16(size_t o) const { return has(o, 2) ? static_cast<uint16_t>(p[o] | (p[o + 1] << 8)) : 0; }
  uint32_t u32(size_t o) const {
    if (!has(o, 4)) return 0;
    return static_cast<uint32_t>(p[o]) | (static_cast<uint32_t>(p[o + 1]) << 8) | (static_cast<uint32_t>(p[o + 2]) << 16) |
           (static_cast<uint32_t>(p[o + 3]) << 24);
  }
  int32_t i32(size_t o) const { return static_cast<int32_t>(u32(o)); }
  float f32(size_t o) const {
    uint32_t v = u32(o);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
  }
  Vec3 vec3(size_t o) const { return Vec3(f32(o), f32(o + 4), f32(o + 8)); }
};

bool finite(float f) { return std::isfinite(f) && std::fabs(f) < 1e12f; }
bool finite(const Vec3& v) { return finite(v.x) && finite(v.y) && finite(v.z); }

std::string utf16(const Bytes& b, size_t o = 0, size_t chars = std::string::npos) {
  std::string s;
  for (size_t i = 0; i < chars && b.has(o + i * 2, 2); ++i) {
    uint32_t c = b.u16(o + i * 2);
    if (c == 0) break;
    if (c >= 0xD800 && c <= 0xDBFF && b.has(o + i * 2 + 2, 2)) {
      uint32_t lo = b.u16(o + i * 2 + 2);
      if (lo >= 0xDC00 && lo <= 0xDFFF) {
        c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
        ++i;
      }
    }
    if (c < 0x80) {
      s += static_cast<char>(c);
    } else if (c < 0x800) {
      s += static_cast<char>(0xC0 | (c >> 6));
      s += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      s += static_cast<char>(0xE0 | (c >> 12));
      s += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      s += static_cast<char>(0x80 | (c & 0x3F));
    } else {
      s += static_cast<char>(0xF0 | (c >> 18));
      s += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
      s += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      s += static_cast<char>(0x80 | (c & 0x3F));
    }
  }
  return s;
}

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool looksLikeImagePath(const std::string& s) {
  static const char* exts[] = {".jpg", ".jpeg", ".png", ".tga", ".tif", ".tiff", ".bmp", ".exr",
                               ".hdr", ".psd", ".dds", ".gif", ".webp", ".hdri"};
  std::string l = lower(s);
  for (const char* e : exts) {
    size_t n = std::strlen(e);
    if (l.size() > n && l.compare(l.size() - n, n, e) == 0) return true;
  }
  return false;
}

// ---------------------------------------------------------------------------------------------
// Parameter blocks

struct PValue {
  int type = -1;  // ParamBlock2 type code (0 float, 1 int, 2 color, 4 bool, 5 angle, 6 percent, 7 world, 8 string, 9 file, 14 material, 15 map, ...)
  std::vector<uint8_t> raw;
  bool isTab = false;
  std::vector<uint32_t> tab;  // 4-byte tab elements (ints, floats, references)

  bool has4() const { return raw.size() >= 4; }
  float f() const {
    if (!has4()) return 0.0f;
    float v;
    std::memcpy(&v, raw.data(), 4);
    return v;
  }
  int i() const {
    if (!has4()) return 0;
    int32_t v;
    std::memcpy(&v, raw.data(), 4);
    return v;
  }
  bool isColor() const { return raw.size() >= 12 && !isTab; }
  Vec3 color() const {
    if (raw.size() < 12) return Vec3(f());
    float c[3];
    std::memcpy(c, raw.data(), 12);
    return Vec3(c[0], c[1], c[2]);
  }
  std::string str() const {
    if (raw.size() < 4) return {};
    uint32_t len = static_cast<uint32_t>(i());
    if (len == 0xFFFFFFFFu || len == 0 || len > raw.size() - 4) return {};
    std::string s(reinterpret_cast<const char*>(raw.data() + 4), len);
    while (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
  }
};

using Params = std::map<int, PValue>;

float pf(const Params& p, int id, float def) {
  auto it = p.find(id);
  if (it == p.end() || !it->second.has4()) return def;
  float v = it->second.f();
  return finite(v) ? v : def;
}
int pi(const Params& p, int id, int def) {
  auto it = p.find(id);
  if (it == p.end() || !it->second.has4()) return def;
  return it->second.i();
}
Vec3 pc(const Params& p, int id, const Vec3& def) {
  auto it = p.find(id);
  if (it == p.end() || !it->second.isColor()) return def;
  Vec3 c = it->second.color();
  return finite(c) ? c : def;
}

// ---------------------------------------------------------------------------------------------
// The scene

struct ClassEntry {
  std::string name;
  int dll = -1;
  uint32_t a = 0, b = 0, super = 0;
};

constexpr uint32_t kSuperGeom = 0x10, kSuperCamera = 0x20, kSuperLight = 0x30, kSuperShape = 0x40, kSuperHelper = 0x50;
constexpr uint32_t kSuperMaterial = 0xC00, kSuperTexmap = 0xC10;

struct Warnings {
  std::map<std::string, int> counts;
  std::vector<std::string> order;
  void add(const std::string& w) {
    if (counts[w]++ == 0) order.push_back(w);
  }
  std::vector<std::string> list() const {
    std::vector<std::string> out;
    for (const auto& w : order) {
      int n = counts.at(w);
      out.push_back(n > 1 ? w + " (x" + std::to_string(n) + ")" : w);
    }
    return out;
  }
};

class MaxScene {
 public:
  std::vector<uint8_t> data;
  std::vector<ClassEntry> classes;
  std::vector<Chunk> top;
  std::vector<const Chunk*> objs;
  bool damaged = false;
  Warnings* warn = nullptr;

  int classOf(int obj) const {
    if (obj < 0 || obj >= static_cast<int>(objs.size())) return -1;
    int c = objs[obj]->id;
    return c < static_cast<int>(classes.size()) ? c : -1;
  }
  const ClassEntry* cls(int obj) const {
    int c = classOf(obj);
    return c >= 0 ? &classes[c] : nullptr;
  }
  std::string className(int obj) const {
    const ClassEntry* c = cls(obj);
    return c ? c->name : std::string();
  }
  uint32_t superOf(int obj) const {
    const ClassEntry* c = cls(obj);
    return c ? c->super : 0;
  }
  const Chunk* obj(int i) const { return i >= 0 && i < static_cast<int>(objs.size()) ? objs[i] : nullptr; }
  Bytes bytes(const Chunk* c) const { return Bytes(data, c); }

  // References by slot (-1 = none). 0x2034 lists them in slot order; 0x2035 stores a leading
  // value followed by (slot, index) pairs.
  std::vector<int> refs(int o) const {
    std::vector<int> out;
    const Chunk* c = obj(o);
    if (!c) return out;
    if (const Chunk* r = child(c, 0x2034)) {
      Bytes b = bytes(r);
      for (size_t k = 0; k + 4 <= b.n && out.size() < 100000; k += 4) out.push_back(b.i32(k));
    }
    if (const Chunk* r = child(c, 0x2035)) {
      Bytes b = bytes(r);
      for (size_t k = 4; k + 8 <= b.n; k += 8) {
        int slot = b.i32(k), idx = b.i32(k + 4);
        if (slot < 0 || slot > 100000) continue;
        if (static_cast<int>(out.size()) <= slot) out.resize(static_cast<size_t>(slot) + 1, -1);
        out[slot] = idx;
      }
    }
    for (int& r : out)
      if (r < -1 || r >= static_cast<int>(objs.size())) r = -1;
    return out;
  }
  int ref(int o, int slot) const {
    std::vector<int> r = refs(o);
    return slot >= 0 && slot < static_cast<int>(r.size()) ? r[slot] : -1;
  }

  bool isParamBlock2(int o) const {
    const Chunk* c = obj(o);
    return c && (className(o) == "ParamBlock2" || child(c, 0x0009) != nullptr) && child(c, 0x000e) != nullptr;
  }
  bool isParamBlock1(int o) const {
    const Chunk* c = obj(o);
    return c && className(o) == "ParamBlock" && child(c, 0x0002) != nullptr;
  }
  bool isParamBlock(int o) const { return isParamBlock2(o) || isParamBlock1(o); }
  int pb2BlockId(int o) const {
    const Chunk* h = child(obj(o), 0x0009);
    return h ? bytes(h).u16(4) : -1;
  }

  Params params(int o) const {
    Params out;
    const Chunk* c = obj(o);
    if (!c) return out;
    if (isParamBlock2(o)) {
      for (const Chunk& k : c->kids) {
        if (k.id != 0x000e || k.container) continue;
        Bytes b = bytes(&k);
        if (b.n < 15) continue;
        PValue v;
        int id = b.u16(0);
        v.type = static_cast<int>(b.u32(2));
        v.raw.assign(b.p + 15, b.p + b.n);
        if ((v.type & 0x800) && b.n >= 19) {
          v.isTab = true;
          uint32_t count = b.u32(15);
          int base = v.type & 0x7FF;
          bool fourBytes = base == 0 || base == 1 || base == 4 || base == 5 || base == 6 || base == 7 || base == 13 ||
                           base == 14 || base == 15 || base == 16 || base == 17;
          if (fourBytes)
            for (size_t e = 0, at = 19; e < count && at + 5 <= b.n; ++e, at += 5) v.tab.push_back(b.u32(at + 1));
        }
        out[id] = std::move(v);
      }
    } else if (isParamBlock1(o)) {
      for (const Chunk& k : c->kids) {
        if (k.id != 0x0002 || !k.container) continue;
        int idx = -1;
        PValue v;
        for (const Chunk& e : k.kids) {
          Bytes b = bytes(&e);
          if (e.id == 0x0003 && b.n >= 4) {
            idx = b.i32(0);
          } else if (e.id >= 0x0100 && e.id <= 0x010F && !e.container && b.n >= 4) {
            v.raw.assign(b.p, b.p + b.n);
            v.type = e.id == 0x0100 ? 0 : e.id == 0x0101 ? 1 : e.id == 0x0104 ? 4 : b.n >= 12 ? 2 : 0;
          }
        }
        if (idx >= 0 && idx < 100000 && !v.raw.empty()) out[idx] = std::move(v);
      }
    }
    return out;
  }

  // Parameter block referenced by an object: the first reference that is one (or by block id).
  int paramBlockOf(int o, int blockId = -1) const {
    for (int r : refs(o)) {
      if (r < 0 || !isParamBlock(r)) continue;
      if (blockId < 0 || pb2BlockId(r) == blockId) return r;
    }
    return -1;
  }
  Params paramsOf(int o, int blockId = -1) const {
    int pb = paramBlockOf(o, blockId);
    return pb >= 0 ? params(pb) : Params();
  }
};

// ---------------------------------------------------------------------------------------------
// Controllers

Mat4 quatMatrix(float x, float y, float z, float w) {
  // 3ds Max stores quaternions with the opposite rotation sense; conjugate to get the usual one.
  x = -x;
  y = -y;
  z = -z;
  float len = std::sqrt(x * x + y * y + z * z + w * w);
  Mat4 m;
  if (!(len > 1e-8f) || !std::isfinite(len)) return m;
  x /= len;
  y /= len;
  z /= len;
  w /= len;
  m.m[0][0] = 1 - 2 * (y * y + z * z);
  m.m[0][1] = 2 * (x * y - z * w);
  m.m[0][2] = 2 * (x * z + y * w);
  m.m[1][0] = 2 * (x * y + z * w);
  m.m[1][1] = 1 - 2 * (x * x + z * z);
  m.m[1][2] = 2 * (y * z - x * w);
  m.m[2][0] = 2 * (x * z - y * w);
  m.m[2][1] = 2 * (y * z + x * w);
  m.m[2][2] = 1 - 2 * (x * x + y * y);
  return m;
}

class Controllers {
 public:
  explicit Controllers(const MaxScene& s) : s_(s) {}

  float evalFloat(int o, int depth = 0) const {
    const Chunk* c = s_.obj(o);
    if (!c || depth > 8) return 0.0f;
    if (const Chunk* v = child(c, 0x2501)) {
      float f = s_.bytes(v).f32(0);
      return finite(f) ? f : 0.0f;
    }
    // List / wrapper controllers: sum of the sub-controllers that hold values.
    float sum = 0.0f;
    for (int r : s_.refs(o))
      if (r >= 0 && hasValue(r)) sum += evalFloat(r, depth + 1);
    return sum;
  }

  Vec3 evalPosition(int o, int depth = 0) const {
    const Chunk* c = s_.obj(o);
    if (!c || depth > 8) return Vec3();
    if (const Chunk* v = child(c, 0x2503)) {
      Vec3 p = s_.bytes(v).vec3(0);
      return finite(p) ? p : Vec3();
    }
    std::string n = s_.className(o);
    std::vector<int> r = s_.refs(o);
    if (n.find("XYZ") != std::string::npos && r.size() >= 3)
      return Vec3(evalFloat(r[0], depth + 1), evalFloat(r[1], depth + 1), evalFloat(r[2], depth + 1));
    Vec3 sum;
    bool any = false;
    for (int k : r)
      if (k >= 0 && hasValue(k)) {
        sum += evalPosition(k, depth + 1);
        any = true;
      }
    if (!any && !r.empty() && !n.empty()) unknown(n);
    return sum;
  }

  Mat4 evalRotation(int o, int depth = 0) const {
    const Chunk* c = s_.obj(o);
    if (!c || depth > 8) return Mat4();
    if (const Chunk* v = child(c, 0x2504)) {
      Bytes b = s_.bytes(v);
      return quatMatrix(b.f32(0), b.f32(4), b.f32(8), b.f32(12));
    }
    std::string n = s_.className(o);
    std::vector<int> r = s_.refs(o);
    if (n.find("Euler") != std::string::npos && r.size() >= 3) {
      float x = evalFloat(r[0], depth + 1), y = evalFloat(r[1], depth + 1), z = evalFloat(r[2], depth + 1);
      int order = 0;
      if (const Chunk* oc = child(c, 0x1003)) order = s_.bytes(oc).i32(0);
      Mat4 rx = Mat4::rotateX(x), ry = Mat4::rotateY(y), rz = Mat4::rotateZ(z);
      switch (order) {
        case 1: return ry * rz * rx;  // XZY
        case 2: return rx * rz * ry;  // YZX
        case 3: return rz * rx * ry;  // YXZ
        case 4: return ry * rx * rz;  // ZXY
        case 5: return rx * ry * rz;  // ZYX
        default: return rz * ry * rx;  // XYZ
      }
    }
    Mat4 m;
    bool any = false;
    for (int k : r)
      if (k >= 0 && hasValue(k)) {
        m = evalRotation(k, depth + 1) * m;
        any = true;
      }
    if (!any && !r.empty() && !n.empty()) unknown(n);
    return m;
  }

  Vec3 evalScale(int o, int depth = 0) const {
    const Chunk* c = s_.obj(o);
    if (!c || depth > 8) return Vec3(1.0f);
    if (const Chunk* v = child(c, 0x2505)) {
      Vec3 sc = s_.bytes(v).vec3(0);
      return finite(sc) ? sc : Vec3(1.0f);
    }
    std::string n = s_.className(o);
    std::vector<int> r = s_.refs(o);
    if (n.find("XYZ") != std::string::npos && r.size() >= 3)
      return Vec3(evalFloat(r[0], depth + 1), evalFloat(r[1], depth + 1), evalFloat(r[2], depth + 1));
    Vec3 sc(1.0f);
    for (int k : r)
      if (k >= 0 && hasValue(k)) {
        Vec3 t = evalScale(k, depth + 1);
        sc = Vec3(sc.x * t.x, sc.y * t.y, sc.z * t.z);
      }
    return sc;
  }

  // Local matrix of a node's transform controller.
  Mat4 evalTransform(int o) const {
    std::vector<int> r = s_.refs(o);
    if (r.size() < 3) {
      if (!s_.className(o).empty()) unknown(s_.className(o));
      return Mat4();
    }
    Vec3 p = evalPosition(r[0]);
    Mat4 rot = evalRotation(r[1]);
    Vec3 sc = evalScale(r[2]);
    return Mat4::translate(p) * rot * Mat4::scale(sc);
  }

  // Look-at controllers: position of the node only (aiming is done from the target node).
  Vec3 evalLookAtPosition(int o) const {
    std::vector<int> r = s_.refs(o);
    for (int k : r)
      if (k >= 0 && hasValue(k)) return evalPosition(k);
    return Vec3();
  }

 private:
  bool hasValue(int o) const {
    const Chunk* c = s_.obj(o);
    if (!c) return false;
    uint32_t sup = s_.superOf(o);
    // Controllers are superclasses 0x9003..0x9012 (float, point3, position, rotation, scale, ...).
    return (sup & 0xFFFFFF00u) == 0x9000u || child(c, 0x2501) || child(c, 0x2503) || child(c, 0x2504) || child(c, 0x2505);
  }
  void unknown(const std::string& n) const {
    if (s_.warn) s_.warn->add("Controller \"" + n + "\" is not supported; its value was taken as zero/identity");
  }
  const MaxScene& s_;
};

// ---------------------------------------------------------------------------------------------
// Geometry

// MNMesh (Editable Poly) inside a 0x08fe chunk: 0x0100 vertices, 0x011a faces, 0x0124/0x0128/0x012b
// map channels.
bool readPolyMesh(const MaxScene& s, const Chunk* mn, PolyMesh& out, Warnings& w, const std::string& name) {
  Bytes vb = s.bytes(child(mn, 0x0100));
  Bytes fb = s.bytes(child(mn, 0x011a));
  if (!vb.p || !fb.p) return false;
  uint32_t nv = vb.u32(0);
  if (nv > 50'000'000 || !vb.has(4, size_t(nv) * 16)) return false;
  out.positions.resize(nv);
  for (uint32_t i = 0; i < nv; ++i) {
    Vec3 p = vb.vec3(4 + size_t(i) * 16 + 4);
    out.positions[i] = finite(p) ? p : Vec3();
  }
  uint32_t nf = fb.u32(0);
  if (nf > fb.n / 18) {  // a face record takes at least 18 bytes
    w.add(name + ": polygon count is damaged; only the stored faces were read");
    nf = static_cast<uint32_t>(fb.n / 18);
  }
  size_t pos = 4;
  std::vector<int> faceOf;  // poly face index -> out.faces index (or -1)
  faceOf.reserve(nf);
  std::vector<std::vector<int>> faceVerts;
  faceVerts.reserve(nf);
  bool unknownBits = false;

  // Reads one face at `at`; returns the end offset or 0 when the record does not fit.
  auto faceAt = [&](size_t at, std::vector<int>* verts, int* mat, uint32_t* smg, uint32_t* flags) -> size_t {
    if (!fb.has(at, 4)) return 0;
    uint32_t deg = fb.u32(at);
    if (deg < 3 || deg > 100000 || !fb.has(at + 4, size_t(deg) * 4 + 2)) return 0;
    if (verts) verts->resize(deg);
    for (uint32_t k = 0; k < deg; ++k) {
      uint32_t v = fb.u32(at + 4 + size_t(k) * 4);
      if (v >= nv) return 0;
      if (verts) (*verts)[k] = static_cast<int>(v);
    }
    size_t p = at + 4 + size_t(deg) * 4;
    uint16_t pres = fb.u16(p);
    p += 2;
    if (pres & ~0x39u) unknownBits = true;
    uint32_t fl = 0, sg = 0;
    int m = 0;
    if (pres & 0x01) { fl = fb.u32(p); p += 4; }
    if (pres & 0x08) { m = fb.u16(p); p += 2; }
    if (pres & 0x10) { sg = fb.u32(p); p += 4; }
    if (pres & 0x20) p += size_t(deg - 3) * 8;  // triangulation diagonals
    if (p > fb.n) return 0;
    if (mat) *mat = m;
    if (smg) *smg = sg;
    if (flags) *flags = fl;
    return p;
  };

  size_t resyncs = 0;
  for (uint32_t f = 0; f < nf; ++f) {
    std::vector<int> v;
    int mat = 0;
    uint32_t smg = 0, flags = 0;
    size_t next = faceAt(pos, &v, &mat, &smg, &flags);
    if (next && f + 1 < nf && !faceAt(next, nullptr, nullptr, nullptr, nullptr)) next = 0;
    if (!next) {
      // Unknown record layout: look for the next offset where two faces parse in a row.
      bool found = false;
      size_t start = pos + 4;
      for (size_t tryAt = start; tryAt < std::min(fb.n, start + 4096); tryAt += 2) {
        size_t e1 = faceAt(tryAt, nullptr, nullptr, nullptr, nullptr);
        if (e1 && (f + 2 >= nf || faceAt(e1, nullptr, nullptr, nullptr, nullptr))) {
          ++resyncs;
          pos = tryAt;
          found = true;
          break;
        }
      }
      if (!found || resyncs > 64) {
        w.add(name + ": polygon data could not be fully read; " + std::to_string(nf - f) + " faces were skipped");
        break;
      }
      faceOf.push_back(-1);
      faceVerts.emplace_back();
      continue;
    }
    pos = next;
    faceVerts.push_back(v);
    if (flags & 0x2) {  // dead face
      faceOf.push_back(-1);
      continue;
    }
    faceOf.push_back(static_cast<int>(out.faces.size()));
    out.addFace(std::move(v), {}, smg, mat);
  }
  if (unknownBits) w.add(name + ": some polygon attributes use an unknown layout and were ignored");

  // Map channel 1 (UVs).
  int channel = -1;
  const Chunk* mverts = nullptr;
  const Chunk* mfaces = nullptr;
  for (const Chunk& k : mn->kids) {
    if (k.id == 0x0124) channel = s.bytes(&k).i32(0);
    else if (k.id == 0x0128 && channel == 1) mverts = &k;
    else if (k.id == 0x012b && channel == 1) mfaces = &k;
  }
  if (mverts && mfaces) {
    Bytes mv = s.bytes(mverts), mf = s.bytes(mfaces);
    uint32_t n = mv.u32(0);
    if (n <= 50'000'000 && mv.has(4, size_t(n) * 12)) {
      PolyMesh withUv = out;
      withUv.uvs.resize(n);
      for (uint32_t i = 0; i < n; ++i) {
        Vec3 t = mv.vec3(4 + size_t(i) * 12);
        withUv.uvs[i] = Vec2(finite(t.x) ? t.x : 0.0f, finite(t.y) ? t.y : 0.0f);
      }
      size_t p = 0;
      bool ok = true;
      for (size_t f = 0; f < faceVerts.size() && ok; ++f) {
        uint32_t deg = mf.u32(p);
        if (!faceVerts[f].empty() && deg != faceVerts[f].size()) ok = false;
        if (!mf.has(p + 4, size_t(deg) * 4)) ok = false;
        if (!ok) break;
        if (faceOf[f] >= 0) {
          IndexList& uv = withUv.faces[faceOf[f]].uv;
          uv.resize(deg);
          for (uint32_t k = 0; k < deg; ++k) {
            uint32_t t = mf.u32(p + 4 + size_t(k) * 4);
            if (t >= n) ok = false;
            uv[k] = static_cast<int>(std::min(t, n ? n - 1 : 0));
          }
        }
        p += 4 + size_t(deg) * 4;
      }
      if (ok) out = std::move(withUv);
      else w.add(name + ": UV channel 1 could not be matched to the polygons and was skipped");
    }
  }
  return !out.faces.empty();
}

// Editable Mesh: a vertex array (count + xyz) and a face array (count + 3 indices + flags) are
// located by shape, preferring the chunk ids seen in files (0x0914 / 0x0912).
bool readTriMesh(const MaxScene& s, const Chunk* c, PolyMesh& out) {
  std::vector<const Chunk*> leaves;
  std::function<void(const Chunk*, int)> collect = [&](const Chunk* k, int depth) {
    if (depth > 6) return;
    for (const Chunk& x : k->kids) {
      if (x.container) collect(&x, depth + 1);
      else if (x.size() >= 16) leaves.push_back(&x);
    }
  };
  collect(c, 0);
  auto vertsOk = [&](const Chunk* k) {
    Bytes b = s.bytes(k);
    uint32_t n = b.u32(0);
    return n > 0 && n < 50'000'000 && b.n == 4 + size_t(n) * 12;
  };
  const Chunk* vc = nullptr;
  for (const Chunk* k : leaves)
    if (k->id == 0x0914 && vertsOk(k)) vc = k;
  if (!vc)
    for (const Chunk* k : leaves)
      if (vertsOk(k)) {
        vc = k;
        break;
      }
  if (!vc) return false;
  Bytes vb = s.bytes(vc);
  uint32_t nv = vb.u32(0);
  auto facesOk = [&](const Chunk* k) {
    Bytes b = s.bytes(k);
    uint32_t n = b.u32(0);
    if (n == 0 || n > 50'000'000 || b.n != 4 + size_t(n) * 16) return false;
    for (uint32_t i = 0; i < n; ++i)
      for (int j = 0; j < 3; ++j)
        if (b.u32(4 + size_t(i) * 16 + j * 4) >= nv) return false;
    return true;
  };
  const Chunk* fc = nullptr;
  for (const Chunk* k : leaves)
    if (k->id == 0x0912 && facesOk(k)) fc = k;
  if (!fc)
    for (const Chunk* k : leaves)
      if (k != vc && facesOk(k)) {
        fc = k;
        break;
      }
  if (!fc) return false;
  for (uint32_t i = 0; i < nv; ++i) {
    Vec3 p = vb.vec3(4 + size_t(i) * 12);
    out.addVertex(finite(p) ? p : Vec3());
  }
  Bytes fb = s.bytes(fc);
  uint32_t nf = fb.u32(0);
  for (uint32_t i = 0; i < nf; ++i) {
    size_t o = 4 + size_t(i) * 16;
    out.addFace({static_cast<int>(fb.u32(o)), static_cast<int>(fb.u32(o + 4)), static_cast<int>(fb.u32(o + 8))}, {}, 1u, 0);
  }
  return true;
}

// ---------------------------------------------------------------------------------------------
// Import

struct Importer {
  Scene& scene;
  const ImportOptions& opt;
  MaxScene& s;
  Warnings& w;
  std::string dir;
  std::map<std::string, std::string> assetPaths;  // asset GUID bytes -> path

  std::unordered_map<int, uint32_t> materialIds;  // max object -> Veraa material id
  std::unordered_map<int, Node*> geometryNodes;   // max geometry object -> first node using it
  std::unordered_map<int, PolyMesh> meshCache;
  int polyObjects = 0, primitives = 0, lights = 0, cameras = 0, placeholders = 0, materialsMade = 0;
  size_t polys = 0;

  std::string resolveTexture(const std::string& p) const {
    // Absolute paths are kept (Veraa finds moved files through its search paths); relative
    // ones are taken relative to the .max file.
    if (p.empty() || p.find(':') != std::string::npos || p[0] == '/' || p[0] == '\\') return p;
    return io::joinPath(dir, p);
  }

  // Bitmap file of a texture map: a file-name parameter, a string chunk that looks like an
  // image path, or an asset id resolved through the FileAssetMetaData stream.
  std::string bitmapPath(int o) const {
    std::vector<int> todo{o};
    for (int r : s.refs(o))
      if (r >= 0 && s.isParamBlock(r)) todo.push_back(r);
    for (int x : todo) {
      if (s.isParamBlock2(x))
        for (auto& kv : s.params(x)) {
          const PValue& v = kv.second;
          if (v.type == 8 || v.type == 9) {
            std::string str = v.str();
            if (looksLikeImagePath(str)) return str;
            if (v.raw.size() >= 16) {
              auto it = assetPaths.find(std::string(v.raw.begin(), v.raw.begin() + 16));
              if (it != assetPaths.end()) return it->second;
            }
          }
        }
      std::string found;
      std::function<void(const Chunk*, int)> scan = [&](const Chunk* c, int depth) {
        if (!c || depth > 6 || !found.empty()) return;
        for (const Chunk& k : c->kids) {
          if (!found.empty()) return;
          if (k.container) {
            scan(&k, depth + 1);
            continue;
          }
          Bytes b = s.bytes(&k);
          if (b.n >= 8 && b.n < 4096) {
            std::string t = utf16(b);
            if (looksLikeImagePath(t)) found = t;
            else {
              std::string a(reinterpret_cast<const char*>(b.p), b.n);
              size_t z = a.find('\0');
              if (z != std::string::npos) a.resize(z);
              if (looksLikeImagePath(a)) found = a;
              else if (b.n >= 16)
                for (size_t off = 0; off + 16 <= b.n && found.empty(); off += 4) {
                  auto it = assetPaths.find(std::string(reinterpret_cast<const char*>(b.p + off), 16));
                  if (it != assetPaths.end()) found = it->second;
                }
            }
          }
        }
      };
      scan(s.obj(x), 0);
      if (!found.empty()) return found;
    }
    return {};
  }

  std::unique_ptr<TexMap> makeMap(int o, int depth = 0) {
    if (o < 0 || depth > 4) return nullptr;
    std::string n = s.className(o);
    if (n == "Bitmap" || n == "VRayHDRI" || n == "VRayBitmap" || n == "CoronaBitmap") {
      std::string path = bitmapPath(o);
      if (path.empty()) {
        w.add("Bitmap map without a readable file name was skipped");
        return nullptr;
      }
      auto m = createTexMap("Bitmap");
      if (!m) return nullptr;
      m->params.setString("file", resolveTexture(path));
      return m;
    }
    if (n == "Falloff") {
      auto m = createTexMap("Falloff");
      if (!m) return nullptr;
      Params p = s.paramsOf(o);
      m->params.setColor("frontColor", pc(p, 0, Vec3(0.0f)));
      m->params.setColor("sideColor", pc(p, 4, Vec3(1.0f)));
      int type = pi(p, 8, 1);
      if (type >= 0 && type <= 4) m->params.setNumber("falloffType", type);
      int direction = pi(p, 9, 0);
      m->params.setNumber("direction", direction >= 0 && direction <= 2 ? direction : 0);
      m->params.setNumber("ior", pf(p, 12, 1.6f));
      // Sub-maps in the two color slots.
      if (auto it = p.find(2); it != p.end() && it->second.has4())
        if (auto sub = makeMap(it->second.i(), depth + 1)) m->maps["frontColor"].map = std::move(sub);
      if (auto it = p.find(6); it != p.end() && it->second.has4())
        if (auto sub = makeMap(it->second.i(), depth + 1)) m->maps["sideColor"].map = std::move(sub);
      return m;
    }
    if (!n.empty()) w.add("Map type \"" + n + "\" is not supported and was skipped");
    return nullptr;
  }

  void plugMap(Material& m, const std::string& key, int mapObj, float amount = 1.0f) {
    if (mapObj < 0) return;
    if (auto map = makeMap(mapObj)) {
      MapSlot& slot = m.maps[key];
      slot.map = std::move(map);
      slot.amount = std::clamp(amount, 0.0f, 1.0f);
    }
  }

  // Fills `m` from a 3ds Max material object. Returns false for unsupported types.
  bool fillMaterial(Material& m, int o, int depth) {
    std::string n = s.className(o);
    // Name: 0x4000/0x5431 container with a 0x4001 UTF-16 string.
    for (uint16_t id : {uint16_t(0x4000), uint16_t(0x5431)})
      if (const Chunk* c = child(s.obj(o), id))
        if (const Chunk* nm = child(c, 0x4001)) {
          std::string name = utf16(s.bytes(nm));
          if (!name.empty()) m.name = name;
        }
    if (n == "VRayMtl") {
      Params p = s.paramsOf(o, 1);
      m.params.setColor("diffuse", pc(p, 1, Vec3(0.5f)));
      m.params.setColor("reflect", pc(p, 2, Vec3(0.0f)));
      m.params.setNumber("reflectGlossiness", std::clamp(pf(p, 3, 1.0f), 0.0f, 1.0f));
      m.params.setNumber("fresnel", pi(p, 8, 0) != 0 ? 1 : 0);
      m.params.setNumber("fresnelIor", std::clamp(pf(p, 63, 1.6f), 1.0f, 50.0f));
      m.params.setColor("refract", pc(p, 5, Vec3(0.0f)));
      m.params.setNumber("refractGlossiness", std::clamp(pf(p, 6, 1.0f), 0.0f, 1.0f));
      m.params.setNumber("ior", std::clamp(pf(p, 9, 1.6f), 1.0f, 50.0f));
      m.params.setColor("fogColor", pc(p, 23, Vec3(1.0f)));
      m.params.setNumber("fogMultiplier", std::max(0.0f, pf(p, 24, 1.0f)));
      Params maps = s.paramsOf(o, 4);
      auto mapRef = [&](int id) {
        auto it = maps.find(id);
        return it != maps.end() && it->second.has4() ? it->second.i() : -1;
      };
      auto mapOn = [&](int id) { return pi(maps, id + 50, 1) != 0; };
      auto amount = [&](int id) { return pf(maps, id + 100, 100.0f) / 100.0f; };
      const char* keys[] = {"diffuse", "reflect", "refract"};
      for (int k = 0; k < 3; ++k)
        if (mapOn(100 + k)) plugMap(m, keys[k], mapRef(100 + k), amount(100 + k));
      if (mapOn(103) && mapRef(103) >= 0) {
        plugMap(m, "bump", mapRef(103));
        m.params.setNumber("bumpAmount", amount(103));
      }
      return true;
    }
    if (n == "Standard" || n == "Standardmaterial") {
      // Shader (Blinn, Phong, ...) parameters live in the shader's own block.
      Params sh;
      for (int r : s.refs(o))
        if (r >= 0 && !s.isParamBlock(r) && s.superOf(r) != kSuperTexmap && s.paramBlockOf(r) >= 0) {
          Params cand = s.paramsOf(r);
          if (cand.count(1) && cand[1].isColor()) {
            sh = std::move(cand);
            break;
          }
        }
      Vec3 diffuse = pc(sh, 1, Vec3(0.6f));
      Vec3 spec = pc(sh, 2, Vec3(0.9f));
      float level = pf(sh, 9, 0.0f), gloss = pf(sh, 10, 0.1f);
      m.params.setColor("diffuse", diffuse);
      m.params.setColor("reflect", spec * std::clamp(level, 0.0f, 1.0f) * 0.25f);
      m.params.setNumber("reflectGlossiness", std::clamp(0.4f + 0.6f * gloss, 0.0f, 1.0f));
      m.params.setNumber("fresnel", 1);
      Params ext = s.paramsOf(o, 1);
      float opacity = std::clamp(pf(ext, 1, 1.0f), 0.0f, 1.0f);
      if (opacity < 0.999f) m.params.setNumber("opacity", opacity);
      Params maps = s.paramsOf(o, 3);
      auto it = maps.find(1);  // map slots: 0 ambient, 1 diffuse, 2 specular, ..., 6 opacity, 8 bump
      auto on = maps.find(0);
      auto amt = maps.find(2);
      auto slotOn = [&](size_t i) { return on == maps.end() || i >= on->second.tab.size() || on->second.tab[i] != 0; };
      auto slotAmt = [&](size_t i) {
        if (amt == maps.end() || i >= amt->second.tab.size()) return 1.0f;
        float f;
        uint32_t u = amt->second.tab[i];
        std::memcpy(&f, &u, 4);
        return finite(f) ? f : 1.0f;
      };
      if (it != maps.end()) {
        const auto& tab = it->second.tab;
        auto ref = [&](size_t i) { return i < tab.size() ? static_cast<int>(tab[i]) : -1; };
        if (slotOn(1)) plugMap(m, "diffuse", ref(1), slotAmt(1));
        if (slotOn(6)) plugMap(m, "opacity", ref(6), slotAmt(6));
        if (slotOn(8) && ref(8) >= 0) {
          plugMap(m, "bump", ref(8));
          m.params.setNumber("bumpAmount", slotAmt(8));
        }
      }
      return true;
    }
    if (n == "Multi/Sub-Object" || n == "MultiMaterial") {
      if (depth > 3) return false;
      m.setMaterialType("MultiSubObject");
      m.subs.clear();
      Params p = s.paramsOf(o);
      std::vector<uint32_t> mats, ids, on;
      if (p.count(0)) mats = p[0].tab;
      if (p.count(3)) ids = p[3].tab;
      if (p.count(1)) on = p[1].tab;
      if (mats.empty()) {  // fall back to the references after the parameter block
        std::vector<int> r = s.refs(o);
        for (size_t k = 1; k < r.size(); ++k) mats.push_back(static_cast<uint32_t>(r[k]));
      }
      for (size_t k = 0; k < mats.size() && k < 1000; ++k) {
        SubMaterialSlot slot;
        slot.matId = k < ids.size() ? static_cast<int>(ids[k]) + 1 : static_cast<int>(k) + 1;
        slot.label = "ID " + std::to_string(slot.matId);
        slot.enabled = k >= on.size() || on[k] != 0;
        int sub = static_cast<int>(mats[k]);
        if (sub >= 0 && sub < static_cast<int>(s.objs.size())) {
          auto sm = std::make_unique<Material>();
          if (fillMaterial(*sm, sub, depth + 1)) slot.material = std::move(sm);
        }
        m.subs.push_back(std::move(slot));
      }
      return true;
    }
    // Wrappers with a single base material (VRayMtlWrapper, Shellac, ...): use the first
    // referenced material.
    for (int r : s.refs(o))
      if (r >= 0 && s.superOf(r) == kSuperMaterial && depth < 4) {
        w.add("Material type \"" + n + "\" was replaced by its base material");
        return fillMaterial(m, r, depth + 1);
      }
    w.add("Material type \"" + n + "\" is not supported; a gray material was used");
    m.params.setColor("diffuse", Vec3(0.6f));
    return true;
  }

  uint32_t material(int o) {
    if (o < 0 || !opt.importMaterials) return 0;
    auto it = materialIds.find(o);
    if (it != materialIds.end()) return it->second;
    if (s.superOf(o) != kSuperMaterial) return 0;
    auto made = std::make_unique<Material>();
    made->name = s.className(o);
    fillMaterial(*made, o, 0);
    made->name = scene.uniqueMaterialName(made->name);
    Material* m = scene.addMaterial(std::move(made));
    if (!m) return 0;
    materialIds[o] = m->id;
    ++materialsMade;
    return m->id;
  }

  // Follows references from a node's object (through derived objects and modifiers) to the
  // base object Veraa can build.
  int baseObject(int o, bool* modifiers, int depth = 0) {
    if (o < 0 || depth > 10) return -1;
    if (recognized(o)) return o;
    for (int r : s.refs(o)) {
      if (r < 0 || s.isParamBlock(r)) continue;
      uint32_t sup = s.superOf(r);
      if (sup == 0x810 || sup == 0x820) {  // modifiers
        if (modifiers) *modifiers = true;
        continue;
      }
      int b = baseObject(r, modifiers, depth + 1);
      if (b >= 0) {
        if (modifiers) *modifiers = true;
        return b;
      }
    }
    return -1;
  }

  bool recognized(int o) const {
    std::string n = s.className(o);
    uint32_t sup = s.superOf(o);
    return isPoly(o) || isTriMesh(o) || primitiveType(n) != nullptr || sup == kSuperLight || sup == kSuperCamera ||
           sup == kSuperHelper;
  }
  bool isPoly(int o) const {
    const ClassEntry* c = s.cls(o);
    return c && ((c->a == 0x1bf8338du && c->b == 0x192f6098u) || c->name == "Editable Poly");
  }
  bool isTriMesh(int o) const {
    const ClassEntry* c = s.cls(o);
    return c && ((c->a == 0x0009 && c->b == 0 && c->super == kSuperGeom) || c->name == "Editable Mesh");
  }
  static const char* primitiveType(const std::string& n) {
    static const std::pair<const char*, const char*> map[] = {
        {"Box", "Box"},           {"Sphere", "Sphere"}, {"GeoSphere", "GeoSphere"}, {"Cylinder", "Cylinder"},
        {"Cone", "Cone"},         {"Tube", "Tube"},     {"Torus", "Torus"},         {"Pyramid", "Pyramid"},
        {"Plane", "Plane"},       {"Teapot", "Teapot"}, {"ChamferBox", "ChamferBox"}};
    for (const auto& kv : map)
      if (n == kv.first) return kv.second;
    return nullptr;
  }

  // Builds the Veraa node for a 3ds Max object (geometry, light, camera or placeholder).
  Node* makeObjectNode(int base, const std::string& name) {
    std::string n = s.className(base);
    uint32_t sup = s.superOf(base);
    if (isPoly(base) || isTriMesh(base)) {
      auto cached = meshCache.find(base);
      if (cached == meshCache.end()) {
        PolyMesh pm;
        bool ok = false;
        if (isPoly(base)) {
          const Chunk* mn = child(s.obj(base), 0x08fe);
          if (!mn) mn = findDeep(s.obj(base), 0x08fe);
          ok = mn && readPolyMesh(s, mn, pm, w, name);
        } else {
          ok = readTriMesh(s, s.obj(base), pm);
        }
        if (!ok) {
          w.add(name + ": " + n + " geometry could not be read; a placeholder was created");
          return nullptr;
        }
        cached = meshCache.emplace(base, std::move(pm)).first;
        ++polyObjects;
      }
      polys += cached->second.faces.size();
      Node* node = io::addMeshNode(scene, name, cached->second);
      return node;
    }
    if (const char* type = primitiveType(n)) {
      Node* node = scene.createNode(type);
      if (!node) return nullptr;
      Params p = s.paramsOf(base);
      ParamBlock& q = node->object->params;
      auto setF = [&](const char* key, int id, bool positive = true) {
        if (!p.count(id)) return;
        float v = pf(p, id, NAN);
        if (finite(v) && (!positive || v >= 0.0f)) q.setNumber(key, v);
      };
      auto setI = [&](const char* key, int id) {
        if (p.count(id)) q.setNumber(key, std::max(1, pi(p, id, 1)));
      };
      std::string t = type;
      if (t == "Box" || t == "ChamferBox") {
        setF("length", 0); setF("width", 1); setF("height", 2, false);
        if (t == "Box") { setI("widthSegs", 3); setI("lengthSegs", 4); setI("heightSegs", 5); }
        else setF("fillet", 3);
      } else if (t == "Sphere") {
        setF("radius", 0); setI("segments", 1);
      } else if (t == "GeoSphere") {
        setF("radius", 0); setI("segments", 1);
      } else if (t == "Cylinder") {
        setF("radius", 0); setF("height", 1, false); setI("heightSegs", 2); setI("capSegs", 3); setI("sides", 4);
      } else if (t == "Cone" || t == "Tube") {
        setF("radius1", 0); setF("radius2", 1); setF("height", 2, false); setI("heightSegs", 3); setI("capSegs", 4);
        setI("sides", 5);
      } else if (t == "Torus") {
        setF("radius1", 0); setF("radius2", 1); setI("segments", 4); setI("sides", 5);
      } else if (t == "Pyramid") {
        setF("width", 0); setF("depth", 1); setF("height", 2, false);
      } else if (t == "Plane") {
        setF("length", 0); setF("width", 1); setI("lengthSegs", 2); setI("widthSegs", 3);
      } else if (t == "Teapot") {
        setF("radius", 0); setI("segments", 1);
        if (p.count(2)) q.setNumber("smooth", pi(p, 2, 1) ? 1 : 0);
        const char* parts[] = {"body", "handle", "spout", "lid"};
        for (int k = 0; k < 4; ++k)
          if (p.count(4 + k)) q.setNumber(parts[k], pi(p, 4 + k, 1) ? 1 : 0);
      }
      if (p.empty()) w.add(name + ": " + n + " parameters were not found; defaults were used");
      node->object->touch();
      node->invalidate();
      ++primitives;
      polys += node->mesh().faces.size();
      return node;
    }
    if (sup == kSuperLight && opt.importLights) return makeLight(base, n, name);
    if (sup == kSuperCamera && opt.importCameras) {
      Node* node = scene.createNode("Camera");
      if (!node) return nullptr;
      Params p = s.paramsOf(base);
      float fov = pf(p, 0, 0.0f);
      if (s.isParamBlock1(s.paramBlockOf(base)) && fov > 0.01f && fov < 3.1f)
        node->object->params.setNumber("fov", fov * 180.0f / 3.14159265f);
      else
        w.add(name + ": camera field of view not found; 45 degrees was used");
      ++cameras;
      return node;
    }
    return nullptr;
  }

  Node* makeLight(int o, const std::string& n, const std::string& name) {
    Params p = s.paramsOf(o);
    Node* node = nullptr;
    if (n == "VRayLight") {
      int type = pi(p, 1, 0);
      if (type == 1) {
        w.add(name + ": V-Ray dome lights are not imported (use the environment instead)");
        return nullptr;
      }
      if (type == 2) {
        node = scene.createNode("SphereLight");
        if (node) node->object->params.setNumber("radius", std::max(0.01f, pf(p, 2, 10.0f)));
      } else {
        if (type != 0) w.add(name + ": V-Ray light type " + std::to_string(type) + " was imported as a plane light");
        node = scene.createNode("RectLight");
        if (node) {
          node->object->params.setNumber("width", std::max(0.01f, 2.0f * pf(p, 2, 50.0f)));
          node->object->params.setNumber("length", std::max(0.01f, 2.0f * pf(p, 3, 50.0f)));
        }
      }
      if (node) {
        node->object->params.setColor("color", pc(p, 7, Vec3(1.0f)));
        node->object->params.setNumber("intensity", std::max(0.0f, pf(p, 8, 30.0f)));
      }
    } else {
      std::string l = lower(n);
      const char* type = l.find("spot") != std::string::npos      ? "SpotLight"
                         : l.find("direct") != std::string::npos ? "SunLight"
                         : l.find("sun") != std::string::npos    ? "SunLight"
                                                                 : "OmniLight";
      if (l.find("sky") != std::string::npos || l.find("dome") != std::string::npos) {
        w.add(name + ": light type \"" + n + "\" is not imported");
        return nullptr;
      }
      node = scene.createNode(type);
      if (node && s.isParamBlock1(s.paramBlockOf(o))) {
        // Standard lights: color then multiplier.
        if (p.count(0) && p[0].isColor()) node->object->params.setColor("color", pc(p, 0, Vec3(1.0f)));
        float mult = pf(p, 1, 1.0f);
        if (mult >= 0.0f && mult < 1e5f) node->object->params.setNumber("intensity", mult);
      } else if (node) {
        w.add(name + ": light \"" + n + "\" parameters are approximate");
      }
    }
    if (node) ++lights;
    return node;
  }

  bool run(ImportResult* result) {
    // Nodes: class "Node" (BASENODE superclass, class id 1).
    std::vector<int> nodeObjs;
    for (int i = 0; i < static_cast<int>(s.objs.size()); ++i) {
      const ClassEntry* c = s.cls(i);
      if (c && c->super == 0x1 && c->a == 0x1 && c->b == 0) nodeObjs.push_back(i);
      else if (c && c->name == "Node" && c->super == 0x1) nodeObjs.push_back(i);
    }
    Controllers ctl(s);
    std::unordered_map<int, Node*> made;
    std::unordered_map<int, int> parentOf;
    std::unordered_map<int, Mat4> localOf;
    std::vector<std::pair<Node*, std::string>> needsTarget;
    for (int no : nodeObjs) {
      const Chunk* c = s.obj(no);
      std::string name = utf16(s.bytes(child(c, 0x0962)), 0, 1024);
      if (name.empty()) name = "Object";
      std::vector<int> r = s.refs(no);
      int tm = r.size() > 0 ? r[0] : -1, ob = r.size() > 1 ? r[1] : -1, mt = r.size() > 3 ? r[3] : -1;
      bool lookAt = lower(s.className(tm)).find("look") != std::string::npos;
      Mat4 local = lookAt ? Mat4::translate(ctl.evalLookAtPosition(tm)) : ctl.evalTransform(tm);
      int parent = -1;
      if (const Chunk* pc0 = child(c, 0x0960)) parent = s.bytes(pc0).i32(0);
      parentOf[no] = parent;

      bool modifiers = false;
      int base = baseObject(ob, &modifiers);
      Node* node = nullptr;
      if (base >= 0) {
        auto inst = geometryNodes.find(base);
        if (inst != geometryNodes.end() && (isPoly(base) || isTriMesh(base) || primitiveType(s.className(base)))) {
          auto copy = inst->second->instance();
          copy->name = io::uniqueName(scene, name);
          node = scene.addNode(std::move(copy));
          if (node) polys += node->mesh().faces.size();
        } else if (s.superOf(base) != kSuperHelper) {
          node = makeObjectNode(base, name);
          if (node && node->kind() == ObjectKind::Geometry) geometryNodes[base] = node;
        }
        if (modifiers && node && node->kind() == ObjectKind::Geometry)
          w.add(name + ": modifiers on top of the base object were not applied");
      } else if (ob >= 0) {
        std::string cn = s.className(ob);
        if (s.superOf(ob) == kSuperShape)
          w.add(name + ": spline \"" + cn + "\" is not imported yet; a placeholder was created");
        else
          w.add(name + ": object type \"" + (cn.empty() ? std::string("unknown") : cn) + "\" is not supported; a placeholder was created");
      }
      if (!node) {
        node = scene.createNode("Dummy");
        if (!node) node = io::addMeshNode(scene, name, PolyMesh());
        if (!node) continue;
        if (base < 0 || s.superOf(base) != kSuperHelper) ++placeholders;
      }
      if (node->name != name) node->name = io::uniqueName(scene, name);
      // Object offset (pivot): position, rotation quaternion, scale.
      Bytes op = s.bytes(child(c, 0x096a)), orot = s.bytes(child(c, 0x096b)), osc = s.bytes(child(c, 0x096c));
      Mat4 offset;
      if (op.has(0, 12)) offset = Mat4::translate(finite(op.vec3(0)) ? op.vec3(0) : Vec3());
      if (orot.has(0, 16)) offset = offset * quatMatrix(orot.f32(0), orot.f32(4), orot.f32(8), orot.f32(12));
      if (osc.has(0, 12) && finite(osc.vec3(0))) offset = offset * Mat4::scale(osc.vec3(0));
      node->objectOffset = decomposeTransform(offset);
      if (Bytes wc = s.bytes(child(c, 0x0974)); wc.has(0, 4))
        node->wireColor = Vec3(wc.u8(0) / 255.0f, wc.u8(1) / 255.0f, wc.u8(2) / 255.0f);
      if (node->kind() == ObjectKind::Geometry && mt >= 0) node->materialId = material(mt);
      localOf[no] = local;
      made[no] = node;
      if (lookAt) needsTarget.push_back({node, name});
      if (result) result->nodeIds.push_back(node->id);
    }

    // Hierarchy and transforms (local transforms are relative to the parent node).
    float sc = opt.scale;
    for (int no : nodeObjs) {
      auto it = made.find(no);
      if (it == made.end()) continue;
      Node* node = it->second;
      auto pit = made.find(parentOf[no]);
      Mat4 local = localOf[no];
      if (pit != made.end() && pit->second != node) {
        node->parentId = pit->second->id;
      } else {
        node->parentId = 0;
        local = Mat4::scale(Vec3(sc)) * local;
      }
      node->transform = decomposeTransform(local);
    }
    for (int no : nodeObjs) {
      auto it = made.find(no);
      if (it == made.end()) continue;
      // Guard against cycles in damaged files.
      Node* n = it->second;
      int steps = 0;
      for (Node* p = n->parent(); p && steps < 10000; p = p->parent(), ++steps)
        if (p == n) {
          n->parentId = 0;
          break;
        }
    }
    // Target cameras and lights: aim at "<name>.Target".
    for (auto& [node, name] : needsTarget) {
      Node* target = scene.findNodeByName(name + ".Target");
      if (!target) {
        w.add(name + ": look-at target not found; orientation is unknown");
        continue;
      }
      Vec3 from = node->worldPosition(), to = target->worldPosition();
      if (length(to - from) > 1e-6f) {
        Transform t;
        t.position = from;
        t.rotation = io::lookAlong(normalize(to - from));
        node->setWorldMatrix(t.matrix());
      }
    }
    for (auto& kv : made) kv.second->invalidate();
    if (result)
      for (auto& kv : made)
        if (kv.second->kind() == ObjectKind::Geometry) result->triangles += kv.second->mesh().triangleCount();
    return true;
  }
};

bool inflateIfNeeded(std::vector<uint8_t>& d) {
  if (d.size() < 2 || d[0] != 0x78 || (d[1] != 0x9C && d[1] != 0xDA && d[1] != 0x01 && d[1] != 0x5E)) return true;
  size_t outLen = 0;
  void* p = tinfl_decompress_mem_to_heap(d.data(), d.size(), &outLen, TINFL_FLAG_PARSE_ZLIB_HEADER);
  if (!p) return false;
  d.assign(static_cast<uint8_t*>(p), static_cast<uint8_t*>(p) + outLen);
  mz_free(p);
  return true;
}

std::string versionInfo(const cfb::CompoundFile& f, const MaxScene& s, uint16_t topId) {
  std::string out;
  std::vector<uint8_t> d;
  if (f.readStream("\x05" "DocumentSummaryInformation", d)) {
    // Newer files list "3ds Max Version: NN.NN" among the document properties.
    std::string ascii(d.begin(), d.end());
    for (const char* key : {"3ds Max Version:", "3ds max Version:", "Max Version:"}) {
      size_t at = ascii.find(key);
      if (at != std::string::npos) {
        size_t e = at;
        while (e < ascii.size() && ascii[e] >= 0x20 && ascii[e] < 0x7F && e - at < 40) ++e;
        out = ascii.substr(at, e - at);
        break;
      }
    }
    if (out.empty()) {
      Bytes b;
      b.p = d.data();
      b.n = d.size();
      for (size_t o = 0; o + 32 < d.size() && out.empty(); o += 2) {
        if (d[o] != '3' || d[o + 1] != 0) continue;
        std::string t = utf16(b, o, 40);
        if (t.rfind("3ds Max Version", 0) == 0 || t.rfind("3ds max Version", 0) == 0) out = t;
      }
    }
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "scene chunk 0x%04x", topId);
  out = out.empty() ? std::string(buf) : out + ", " + buf;
  for (const ClassEntry& c : s.classes)
    if (c.name.rfind("V-Ray", 0) == 0) {
      out += ", renderer " + c.name;
      break;
    }
  return out;
}

void readAssetMetaData(const cfb::CompoundFile& f, std::map<std::string, std::string>& out) {
  for (const std::string& name : f.streamNames()) {
    if (name.rfind("FileAssetMetaData", 0) != 0) continue;
    std::vector<uint8_t> d;
    if (!f.readStream(name, d) || !inflateIfNeeded(d)) continue;
    ChunkParser cp{d};
    std::vector<Chunk> tree;
    cp.parse(0, d.size(), 0, tree);
    std::function<void(const std::vector<Chunk>&, int)> walk = [&](const std::vector<Chunk>& ks, int depth) {
      if (depth > 8) return;
      std::string guid, path;
      for (const Chunk& k : ks) {
        if (k.container) {
          walk(k.kids, depth + 1);
          continue;
        }
        Bytes b(d, &k);
        if (b.n == 16 && guid.empty()) guid.assign(reinterpret_cast<const char*>(b.p), 16);
        else if (b.n >= 8) {
          std::string t = utf16(b, 0, 2048);
          if (looksLikeImagePath(t)) path = t;
        }
      }
      if (!guid.empty() && !path.empty()) out[guid] = path;
    };
    walk(tree, 0);
  }
}

}  // namespace

bool importMax(Scene& scene, const std::string& path, const ImportOptions& options, ImportResult* result,
               std::string* error) {
  cfb::CompoundFile file;
  std::string err;
  if (!file.openFile(path, &err)) {
    if (error) *error = err;
    return false;
  }
  MaxScene s;
  Warnings w;
  s.warn = &w;
  // Class directory: 0x2040 entries with a 0x2060 header (dll, class id A/B, superclass) and a
  // 0x2042 UTF-16 name.
  std::vector<uint8_t> cd;
  std::string cdName = file.hasStream("ClassDirectory3") ? "ClassDirectory3" : "ClassDirectory2";
  if (!file.readStream(cdName, cd, &err) || !inflateIfNeeded(cd)) {
    if (error) *error = "Not a 3ds Max scene: " + err;
    return false;
  }
  {
    ChunkParser cp{cd};
    std::vector<Chunk> tree;
    cp.parse(0, cd.size(), 0, tree);
    for (const Chunk& e : tree) {
      ClassEntry ce;
      if (const Chunk* h = child(&e, 0x2060)) {
        Bytes b(cd, h);
        ce.dll = b.i32(0);
        ce.a = b.u32(4);
        ce.b = b.u32(8);
        ce.super = b.u32(12);
      }
      if (const Chunk* n = child(&e, 0x2042)) ce.name = utf16(Bytes(cd, n), 0, 512);
      s.classes.push_back(std::move(ce));
    }
  }
  if (!file.readStream("Scene", s.data, &err)) {
    if (error) *error = "Not a 3ds Max scene: " + err;
    return false;
  }
  if (!inflateIfNeeded(s.data)) {
    if (error) *error = "3ds Max scene: the compressed scene data is damaged";
    return false;
  }
  ChunkParser cp{s.data};
  cp.parse(0, s.data.size(), 0, s.top);
  if (s.top.empty() || !s.top[0].container) {
    if (error) *error = "3ds Max scene: no scene data found";
    return false;
  }
  if (cp.damaged) w.add("The scene data is damaged or truncated; what could be read was imported");
  for (const Chunk& k : s.top[0].kids) s.objs.push_back(&k);

  Importer im{scene, options, s, w, io::directoryOf(path), {}};
  readAssetMetaData(file, im.assetPaths);
  std::string version = versionInfo(file, s, s.top[0].id);
  ImportResult local;
  ImportResult* r = result ? result : &local;
  size_t before = r->nodeIds.size();
  im.run(r);

  char summary[512];
  std::snprintf(summary, sizeof(summary),
                "3ds Max file (%s): %zu nodes - %d mesh objects (%zu polygons), %d primitives, %d materials, %d lights, "
                "%d cameras, %d placeholders",
                version.c_str(), r->nodeIds.size() - before, im.polyObjects, im.polys, im.primitives, im.materialsMade,
                im.lights, im.cameras, im.placeholders);
  r->warnings.push_back(summary);
  for (const std::string& x : w.list()) r->warnings.push_back(x);
  return true;
}

}  // namespace veraa
