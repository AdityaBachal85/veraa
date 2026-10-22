// STL and PLY (import and export), 3DS (import) and OBJ (export with MTL).
#include <algorithm>
#include <array>
#include <functional>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "io_util.h"

namespace veraa {

namespace {

// Y-up file axes -> Veraa Z-up: (x, y, z) -> (x, -z, y).
Vec3 fromYUp(const Vec3& v) { return Vec3(v.x, -v.z, v.y); }
Vec3 toYUp(const Vec3& v) { return Vec3(v.x, v.z, -v.y); }

bool readFile(const std::string& path, std::vector<char>& data, std::string* error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (error) *error = "Cannot open " + path;
    return false;
  }
  data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

template <typename T>
T readLE(const char* p) {
  T v;
  std::memcpy(&v, p, sizeof(T));
  return v;
}

// Merged world-space geometry of the exported nodes.
PolyMesh worldMesh(const Scene& scene, const ExportOptions& options) {
  PolyMesh out;
  for (const Node* n : io::exportNodes(scene, options)) {
    if (n->kind() != ObjectKind::Geometry || n->mesh().empty() || !scene.isNodeVisible(*n)) continue;
    PolyMesh m = n->mesh();
    m.transform(Mat4::scale(Vec3(options.scale)) * n->objectWorldMatrix());
    out.append(m);
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// STL

bool importStl(Scene& scene, const std::string& path, const ImportOptions& options, ImportResult* result,
               std::string* error) {
  std::vector<char> data;
  if (!readFile(path, data, error)) return false;
  PolyMesh pm;
  auto addTri = [&](Vec3 a, Vec3 b, Vec3 c) {
    if (options.yUpOverride) {
      a = fromYUp(a);
      b = fromYUp(b);
      c = fromYUp(c);
    }
    int base = static_cast<int>(pm.positions.size());
    pm.positions.push_back(a * options.scale);
    pm.positions.push_back(b * options.scale);
    pm.positions.push_back(c * options.scale);
    pm.addFace({base, base + 1, base + 2}, {}, 1);
  };
  bool binary = false;
  if (data.size() >= 84) {
    uint32_t count = readLE<uint32_t>(data.data() + 80);
    binary = data.size() == 84 + static_cast<size_t>(count) * 50;
    if (binary)
      for (uint32_t i = 0; i < count; ++i) {
        const char* t = data.data() + 84 + i * 50 + 12;
        Vec3 v[3];
        for (int k = 0; k < 3; ++k)
          v[k] = Vec3(readLE<float>(t + k * 12), readLE<float>(t + k * 12 + 4), readLE<float>(t + k * 12 + 8));
        addTri(v[0], v[1], v[2]);
      }
  }
  if (!binary) {
    std::istringstream in(std::string(data.begin(), data.end()));
    std::string word;
    std::vector<Vec3> verts;
    while (in >> word) {
      if (word == "vertex") {
        Vec3 v;
        in >> v.x >> v.y >> v.z;
        verts.push_back(v);
        if (verts.size() == 3) {
          addTri(verts[0], verts[1], verts[2]);
          verts.clear();
        }
      } else if (word == "endfacet") {
        verts.clear();
      }
    }
    if (pm.faces.empty()) {
      if (error) *error = "STL: no triangles found in " + path;
      return false;
    }
  }
  // Facets are flat-shaded in STL.
  for (auto& f : pm.faces) f.smoothGroups = 0;
  if (options.weld) weldVertices(pm);
  if (result) result->triangles += pm.triangleCount();
  Node* n = io::addMeshNode(scene, io::baseNameOf(path), std::move(pm));
  if (!n) return false;
  if (result) result->nodeIds.push_back(n->id);
  return true;
}

bool exportStl(const Scene& scene, const std::string& path, const ExportOptions& options, std::string* error) {
  PolyMesh m = worldMesh(scene, options);
  auto tris = io::fanTriangles(m);
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    if (error) *error = "Cannot write " + path;
    return false;
  }
  auto conv = [&](const Vec3& v) { return options.yUp ? toYUp(v) : v; };
  if (options.binary) {
    char header[80] = {};
    std::snprintf(header, sizeof(header), "Veraa STL export");
    out.write(header, 80);
    uint32_t count = static_cast<uint32_t>(tris.size());
    out.write(reinterpret_cast<const char*>(&count), 4);
    for (const auto& t : tris) {
      Vec3 a = conv(m.positions[t[0]]), b = conv(m.positions[t[1]]), c = conv(m.positions[t[2]]);
      Vec3 nrm = cross(b - a, c - a);
      nrm = length(nrm) > 0 ? normalize(nrm) : Vec3();
      float f[12] = {nrm.x, nrm.y, nrm.z, a.x, a.y, a.z, b.x, b.y, b.z, c.x, c.y, c.z};
      out.write(reinterpret_cast<const char*>(f), 48);
      uint16_t attr = 0;
      out.write(reinterpret_cast<const char*>(&attr), 2);
    }
  } else {
    out << "solid veraa\n";
    for (const auto& t : tris) {
      Vec3 a = conv(m.positions[t[0]]), b = conv(m.positions[t[1]]), c = conv(m.positions[t[2]]);
      Vec3 nrm = cross(b - a, c - a);
      nrm = length(nrm) > 0 ? normalize(nrm) : Vec3();
      out << "  facet normal " << nrm.x << ' ' << nrm.y << ' ' << nrm.z << "\n    outer loop\n";
      for (const Vec3& v : {a, b, c}) out << "      vertex " << v.x << ' ' << v.y << ' ' << v.z << "\n";
      out << "    endloop\n  endfacet\n";
    }
    out << "endsolid veraa\n";
  }
  return static_cast<bool>(out);
}

// ---------------------------------------------------------------------------------------------
// PLY

namespace {

struct PlyProperty {
  std::string name, type, countType;  // countType set for lists
  bool list = false;
};
struct PlyElement {
  std::string name;
  size_t count = 0;
  std::vector<PlyProperty> props;
};

size_t plySize(const std::string& t) {
  if (t == "char" || t == "uchar" || t == "int8" || t == "uint8") return 1;
  if (t == "short" || t == "ushort" || t == "int16" || t == "uint16") return 2;
  if (t == "double" || t == "float64") return 8;
  return 4;
}

double plyRead(const char*& p, const std::string& t, bool bigEndian) {
  size_t n = plySize(t);
  char buf[8];
  std::memcpy(buf, p, n);
  if (bigEndian) std::reverse(buf, buf + n);
  p += n;
  if (t == "char" || t == "int8") return static_cast<int8_t>(buf[0]);
  if (t == "uchar" || t == "uint8") return static_cast<uint8_t>(buf[0]);
  if (t == "short" || t == "int16") return readLE<int16_t>(buf);
  if (t == "ushort" || t == "uint16") return readLE<uint16_t>(buf);
  if (t == "int" || t == "int32") return readLE<int32_t>(buf);
  if (t == "uint" || t == "uint32") return readLE<uint32_t>(buf);
  if (t == "double" || t == "float64") return readLE<double>(buf);
  return readLE<float>(buf);
}

}  // namespace

bool importPly(Scene& scene, const std::string& path, const ImportOptions& options, ImportResult* result,
               std::string* error) {
  std::vector<char> data;
  if (!readFile(path, data, error)) return false;
  std::string text(data.begin(), data.end());
  size_t end = text.find("end_header");
  if (text.compare(0, 3, "ply") != 0 || end == std::string::npos) {
    if (error) *error = "PLY: not a PLY file: " + path;
    return false;
  }
  size_t bodyStart = text.find('\n', end) + 1;
  std::istringstream header(text.substr(0, end));
  std::string line, format;
  std::vector<PlyElement> elements;
  while (std::getline(header, line)) {
    std::istringstream ls(line);
    std::string w;
    ls >> w;
    if (w == "format") ls >> format;
    else if (w == "element") {
      PlyElement e;
      ls >> e.name >> e.count;
      elements.push_back(e);
    } else if (w == "property" && !elements.empty()) {
      PlyProperty p;
      ls >> p.type;
      if (p.type == "list") {
        p.list = true;
        ls >> p.countType >> p.type;
      }
      ls >> p.name;
      elements.back().props.push_back(p);
    }
  }
  bool ascii = format == "ascii", big = format == "binary_big_endian";
  PolyMesh pm;
  std::vector<Vec2> vertexUv;
  bool haveUv = false;
  const char* p = data.data() + bodyStart;
  const char* pend = data.data() + data.size();
  std::istringstream ain(ascii ? text.substr(bodyStart) : std::string());
  auto value = [&](const std::string& type) -> double {
    if (ascii) {
      double v = 0;
      ain >> v;
      return v;
    }
    if (p + plySize(type) > pend) return 0.0;
    return plyRead(p, type, big);
  };
  for (const PlyElement& e : elements) {
    for (size_t i = 0; i < e.count; ++i) {
      Vec3 pos;
      Vec2 uv;
      std::vector<int> face;
      for (const PlyProperty& prop : e.props) {
        if (prop.list) {
          int n = static_cast<int>(value(prop.countType));
          for (int k = 0; k < n; ++k) face.push_back(static_cast<int>(value(prop.type)));
          continue;
        }
        double v = value(prop.type);
        if (e.name == "vertex") {
          if (prop.name == "x") pos.x = static_cast<float>(v);
          else if (prop.name == "y") pos.y = static_cast<float>(v);
          else if (prop.name == "z") pos.z = static_cast<float>(v);
          else if (prop.name == "u" || prop.name == "s" || prop.name == "texture_u") {
            uv.x = static_cast<float>(v);
            haveUv = true;
          } else if (prop.name == "v" || prop.name == "t" || prop.name == "texture_v") {
            uv.y = static_cast<float>(v);
          }
        }
      }
      if (e.name == "vertex") {
        if (options.yUpOverride) pos = fromYUp(pos);
        pm.positions.push_back(pos * options.scale);
        vertexUv.push_back(uv);
      } else if (e.name == "face" && face.size() >= 3) {
        bool ok = true;
        for (int v : face) ok &= v >= 0 && v < static_cast<int>(pm.positions.size());
        if (ok) pm.addFace(face, {}, 1);
      }
    }
  }
  if (pm.faces.empty()) {
    if (error) *error = "PLY: no faces found in " + path;
    return false;
  }
  if (haveUv) {
    pm.uvs = vertexUv;
    for (auto& f : pm.faces) f.uv = f.v;
  }
  if (result) result->triangles += pm.triangleCount();
  Node* n = io::addMeshNode(scene, io::baseNameOf(path), std::move(pm));
  if (!n) return false;
  if (result) result->nodeIds.push_back(n->id);
  return true;
}

bool exportPly(const Scene& scene, const std::string& path, const ExportOptions& options, std::string* error) {
  PolyMesh m = worldMesh(scene, options);
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    if (error) *error = "Cannot write " + path;
    return false;
  }
  out << "ply\nformat " << (options.binary ? "binary_little_endian" : "ascii") << " 1.0\ncomment Veraa export\n";
  out << "element vertex " << m.positions.size() << "\nproperty float x\nproperty float y\nproperty float z\n";
  out << "element face " << m.faces.size() << "\nproperty list uchar int vertex_indices\nend_header\n";
  for (Vec3 v : m.positions) {
    if (options.yUp) v = toYUp(v);
    if (options.binary) {
      float f[3] = {v.x, v.y, v.z};
      out.write(reinterpret_cast<const char*>(f), 12);
    } else {
      out << v.x << ' ' << v.y << ' ' << v.z << '\n';
    }
  }
  for (const auto& f : m.faces) {
    uint8_t n = static_cast<uint8_t>(std::min<size_t>(f.v.size(), 255));
    if (options.binary) {
      out.write(reinterpret_cast<const char*>(&n), 1);
      for (uint8_t k = 0; k < n; ++k) {
        int32_t idx = f.v[k];
        out.write(reinterpret_cast<const char*>(&idx), 4);
      }
    } else {
      out << int(n);
      for (uint8_t k = 0; k < n; ++k) out << ' ' << f.v[k];
      out << '\n';
    }
  }
  return static_cast<bool>(out);
}

// ---------------------------------------------------------------------------------------------
// 3DS

namespace {

struct Chunk3ds {
  uint16_t id;
  uint32_t length;
  size_t start, end;  // data range (after the 6-byte header)
};

struct Object3ds {
  std::string name;
  std::vector<Vec3> verts;
  std::vector<Vec2> uvs;
  std::vector<std::array<int, 3>> faces;
  std::vector<std::string> faceMaterial;
  Mat4 local;
  bool hasLocal = false;
};

struct Material3ds {
  std::string name, texture;
  Vec3 diffuse{0.6f, 0.6f, 0.6f};
  float transparency = 0.0f;
};

}  // namespace

bool import3ds(Scene& scene, const std::string& path, const ImportOptions& options, ImportResult* result,
               std::string* error) {
  std::vector<char> data;
  if (!readFile(path, data, error)) return false;
  if (data.size() < 6 || readLE<uint16_t>(data.data()) != 0x4D4D) {
    if (error) *error = "3DS: not a 3DS file: " + path;
    return false;
  }
  auto chunkAt = [&](size_t pos, Chunk3ds& c) {
    if (pos + 6 > data.size()) return false;
    c.id = readLE<uint16_t>(data.data() + pos);
    c.length = readLE<uint32_t>(data.data() + pos + 2);
    if (c.length < 6 || pos + c.length > data.size()) return false;
    c.start = pos + 6;
    c.end = pos + c.length;
    return true;
  };
  auto cstring = [&](size_t& pos, size_t end) {
    std::string s;
    while (pos < end && data[pos]) s += data[pos++];
    ++pos;
    return s;
  };
  std::vector<Object3ds> objects;
  std::vector<Material3ds> materials;
  auto readColor = [&](size_t pos, size_t end, Vec3& out) {
    Chunk3ds c;
    while (pos < end && chunkAt(pos, c)) {
      if (c.id == 0x0011 && c.end - c.start >= 3)
        out = Vec3(static_cast<uint8_t>(data[c.start]) / 255.0f, static_cast<uint8_t>(data[c.start + 1]) / 255.0f,
                   static_cast<uint8_t>(data[c.start + 2]) / 255.0f);
      else if (c.id == 0x0010 && c.end - c.start >= 12)
        out = Vec3(readLE<float>(data.data() + c.start), readLE<float>(data.data() + c.start + 4),
                   readLE<float>(data.data() + c.start + 8));
      pos = c.end;
    }
  };
  std::function<void(size_t, size_t, int)> walk = [&](size_t pos, size_t end, int depth) {
    Chunk3ds c;
    while (pos < end && chunkAt(pos, c)) {
      switch (c.id) {
        case 0x4D4D:  // main
        case 0x3D3D:  // editor
          walk(c.start, c.end, depth + 1);
          break;
        case 0x4000: {  // named object
          size_t p = c.start;
          Object3ds o;
          o.name = cstring(p, c.end);
          objects.push_back(o);
          walk(p, c.end, depth + 1);
          break;
        }
        case 0x4100:  // triangle mesh
          walk(c.start, c.end, depth + 1);
          break;
        case 0x4110: {  // vertices
          if (objects.empty()) break;
          uint16_t n = readLE<uint16_t>(data.data() + c.start);
          for (uint16_t i = 0; i < n && c.start + 2 + (i + 1) * 12 <= c.end; ++i) {
            const char* q = data.data() + c.start + 2 + i * 12;
            objects.back().verts.push_back(Vec3(readLE<float>(q), readLE<float>(q + 4), readLE<float>(q + 8)));
          }
          break;
        }
        case 0x4120: {  // faces (+ material groups inside)
          if (objects.empty()) break;
          uint16_t n = readLE<uint16_t>(data.data() + c.start);
          size_t p = c.start + 2;
          for (uint16_t i = 0; i < n && p + 8 <= c.end; ++i, p += 8)
            objects.back().faces.push_back({readLE<uint16_t>(data.data() + p), readLE<uint16_t>(data.data() + p + 2),
                                            readLE<uint16_t>(data.data() + p + 4)});
          objects.back().faceMaterial.assign(objects.back().faces.size(), std::string());
          Chunk3ds sub;
          while (p < c.end && chunkAt(p, sub)) {
            if (sub.id == 0x4130) {
              size_t q = sub.start;
              std::string mat = cstring(q, sub.end);
              uint16_t count = readLE<uint16_t>(data.data() + q);
              q += 2;
              for (uint16_t k = 0; k < count && q + 2 <= sub.end; ++k, q += 2) {
                uint16_t f = readLE<uint16_t>(data.data() + q);
                if (f < objects.back().faceMaterial.size()) objects.back().faceMaterial[f] = mat;
              }
            }
            p = sub.end;
          }
          break;
        }
        case 0x4140: {  // texture coordinates
          if (objects.empty()) break;
          uint16_t n = readLE<uint16_t>(data.data() + c.start);
          for (uint16_t i = 0; i < n && c.start + 2 + (i + 1) * 8 <= c.end; ++i) {
            const char* q = data.data() + c.start + 2 + i * 8;
            objects.back().uvs.push_back(Vec2(readLE<float>(q), readLE<float>(q + 4)));
          }
          break;
        }
        case 0x4160: {  // local coordinate system (axes rows then origin)
          if (objects.empty() || c.end - c.start < 48) break;
          const char* q = data.data() + c.start;
          Mat4 m;
          for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 3; ++row) m.m[row][col] = readLE<float>(q + (col * 3 + row) * 4);
          objects.back().local = m;
          objects.back().hasLocal = true;
          break;
        }
        case 0xAFFF: {  // material
          materials.push_back(Material3ds());
          Chunk3ds sub;
          size_t p = c.start;
          while (p < c.end && chunkAt(p, sub)) {
            if (sub.id == 0xA000) {
              size_t q = sub.start;
              materials.back().name = cstring(q, sub.end);
            } else if (sub.id == 0xA020) {
              readColor(sub.start, sub.end, materials.back().diffuse);
            } else if (sub.id == 0xA050) {
              Chunk3ds pc;
              if (chunkAt(sub.start, pc) && pc.id == 0x0030)
                materials.back().transparency = readLE<uint16_t>(data.data() + pc.start) / 100.0f;
            } else if (sub.id == 0xA200) {
              Chunk3ds tc;
              size_t q = sub.start;
              while (q < sub.end && chunkAt(q, tc)) {
                if (tc.id == 0xA300) {
                  size_t r = tc.start;
                  materials.back().texture = io::joinPath(io::directoryOf(path), cstring(r, tc.end));
                }
                q = tc.end;
              }
            }
            p = sub.end;
          }
          break;
        }
        default: break;
      }
      pos = c.end;
    }
  };
  walk(0, data.size(), 0);

  std::map<std::string, uint32_t> matIds;
  if (options.importMaterials)
    for (const Material3ds& m : materials)
      matIds[m.name] = io::addMaterial(scene, m.name, m.diffuse, m.texture, 1.0f - m.transparency);
  for (const Object3ds& o : objects) {
    if (o.verts.empty() || o.faces.empty()) continue;
    PolyMesh pm;
    // 3DS stores vertices in world space; put them back into the object's local frame.
    Mat4 toLocal = o.hasLocal ? o.local.inverse() : Mat4::identity();
    for (const Vec3& v : o.verts) pm.positions.push_back(toLocal.transformPoint(v));
    bool uv = o.uvs.size() == o.verts.size();
    if (uv) pm.uvs = o.uvs;
    for (const auto& f : o.faces) {
      if (f[0] >= static_cast<int>(pm.positions.size()) || f[1] >= static_cast<int>(pm.positions.size()) ||
          f[2] >= static_cast<int>(pm.positions.size()))
        continue;
      pm.addFace({f[0], f[1], f[2]}, uv ? std::vector<int>{f[0], f[1], f[2]} : std::vector<int>(), 1);
    }
    if (options.weld) weldVertices(pm);
    if (result) result->triangles += pm.triangleCount();
    Node* n = io::addMeshNode(scene, o.name, std::move(pm));
    if (!n) continue;
    Mat4 world = Mat4::scale(Vec3(options.scale)) * (o.hasLocal ? o.local : Mat4::identity());
    n->transform = decomposeTransform(world);
    if (!o.faceMaterial.empty() && matIds.count(o.faceMaterial[0])) n->materialId = matIds[o.faceMaterial[0]];
    n->invalidate();
    if (result) result->nodeIds.push_back(n->id);
  }
  if (result && result->nodeIds.empty()) {
    if (error) *error = "3DS: no meshes found in " + path;
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------------------------
// OBJ export

bool exportObj(const Scene& scene, const std::string& path, const ExportOptions& options, std::string* error) {
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    if (error) *error = "Cannot write " + path;
    return false;
  }
  std::string dir = io::directoryOf(path);
  std::string mtlName = io::baseNameOf(path) + ".mtl";
  out << "# Veraa OBJ export\n";
  if (options.exportMaterials) out << "mtllib " << mtlName << "\n";
  std::set<uint32_t> usedMaterials;
  size_t vBase = 1, tBase = 1;
  for (const Node* n : io::exportNodes(scene, options)) {
    if (n->kind() != ObjectKind::Geometry || n->mesh().empty()) continue;
    PolyMesh m = n->mesh();
    m.transform(Mat4::scale(Vec3(options.scale)) * n->objectWorldMatrix());
    std::string name = n->name;
    std::replace(name.begin(), name.end(), ' ', '_');
    out << "o " << name << "\n";
    for (Vec3 v : m.positions) {
      if (options.yUp) v = toYUp(v);
      out << "v " << v.x << ' ' << v.y << ' ' << v.z << "\n";
    }
    for (const Vec2& t : m.uvs) out << "vt " << t.x << ' ' << t.y << "\n";
    if (options.exportMaterials) {
      const Material* mat = scene.findMaterial(n->materialId);
      if (mat) {
        out << "usemtl " << mat->name << "\n";
        usedMaterials.insert(mat->id);
      } else {
        out << "usemtl " << name << "_color\n";
        usedMaterials.insert(0x80000000u | n->id);
      }
    }
    uint32_t lastSmooth = ~0u;
    for (const auto& f : m.faces) {
      uint32_t sg = f.smoothGroups ? 1u : 0u;
      if (sg != lastSmooth) {
        out << (sg ? "s 1\n" : "s off\n");
        lastSmooth = sg;
      }
      out << "f";
      for (size_t k = 0; k < f.v.size(); ++k) {
        out << ' ' << vBase + f.v[k];
        if (k < f.uv.size() && f.uv[k] >= 0) out << '/' << tBase + f.uv[k];
      }
      out << "\n";
    }
    vBase += m.positions.size();
    tBase += m.uvs.size();
  }
  if (!out) {
    if (error) *error = "Write error: " + path;
    return false;
  }
  if (!options.exportMaterials) return true;
  std::ofstream mtl(io::joinPath(dir, mtlName), std::ios::binary);
  if (!mtl) {
    if (error) *error = "Cannot write " + mtlName;
    return false;
  }
  mtl << "# Veraa MTL export\n";
  for (uint32_t key : usedMaterials) {
    if (key & 0x80000000u) {
      const Node* n = scene.findNode(key & 0x7fffffffu);
      if (!n) continue;
      std::string name = n->name;
      std::replace(name.begin(), name.end(), ' ', '_');
      mtl << "\nnewmtl " << name << "_color\nKd " << n->wireColor.x << ' ' << n->wireColor.y << ' ' << n->wireColor.z
          << "\nKa 0 0 0\nKs 0 0 0\nNs 10\nd 1\nillum 2\n";
      continue;
    }
    const Material* m = scene.findMaterial(key);
    if (!m) continue;
    Vec3 kd = m->params.getColor("diffuse"), ks = m->params.getColor("reflect");
    float gloss = m->params.getFloat("reflectGlossiness");
    mtl << "\nnewmtl " << m->name << "\nKd " << kd.x << ' ' << kd.y << ' ' << kd.z << "\nKa 0 0 0\nKs " << ks.x << ' '
        << ks.y << ' ' << ks.z << "\nNs " << 2.0f + gloss * 998.0f << "\nNi " << m->params.getFloat("ior") << "\nd "
        << m->params.getFloat("opacity") << "\nillum 2\n";
    std::string map = m->params.getString("diffuseMap");
    if (!map.empty()) mtl << "map_Kd " << io::relativePath(dir, scene.resolvePath(map)) << "\n";
    std::string bump = m->params.getString("bumpMap");
    if (!bump.empty()) mtl << "map_bump " << io::relativePath(dir, scene.resolvePath(bump)) << "\n";
  }
  return static_cast<bool>(mtl);
}

}  // namespace veraa
