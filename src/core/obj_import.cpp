#include "obj_import.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <climits>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <unordered_map>

#include "parallel.h"
#include "scene.h"

namespace veraa {

namespace fs = std::filesystem;

namespace {

struct MtlInfo {
  Vec3 kd{0.6f, 0.6f, 0.6f}, ks, ke;
  float ns = 0.0f, ni = 1.5f, d = 1.0f;
  int illum = 2;
  std::string mapKd, mapD, mapBump;
};

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

// Texture statements may carry options ("-bm 1.0 file.png"); the file name is the last token(s).
std::string mapFileName(const std::string& rest) {
  std::istringstream in(rest);
  std::vector<std::string> tokens;
  std::string t;
  while (in >> t) tokens.push_back(t);
  size_t i = 0;
  while (i < tokens.size() && !tokens[i].empty() && tokens[i][0] == '-') {
    // Skip option and its numeric arguments.
    ++i;
    while (i < tokens.size() && (std::isdigit(static_cast<unsigned char>(tokens[i][0])) || tokens[i][0] == '.' ||
                                 (tokens[i][0] == '-' && tokens[i].size() > 1 &&
                                  std::isdigit(static_cast<unsigned char>(tokens[i][1])))))
      ++i;
  }
  std::string name;
  for (; i < tokens.size(); ++i) name += (name.empty() ? "" : " ") + tokens[i];
  return name;
}

Vec3 readVec3(std::istringstream& in, const Vec3& def) {
  Vec3 v = def;
  in >> v.x;
  if (!(in >> v.y)) v.y = v.x;
  if (!(in >> v.z)) v.z = v.y;
  return v;
}

std::map<std::string, MtlInfo> parseMtl(const fs::path& path, std::vector<std::string>& warnings) {
  std::map<std::string, MtlInfo> mats;
  std::ifstream f(path);
  if (!f) {
    warnings.push_back("Material library not found: " + path.u8string());
    return mats;
  }
  fs::path dir = path.parent_path();
  auto resolve = [&](const std::string& p) {
    if (p.empty()) return p;
    fs::path fp = fs::u8path(p);
    return fp.is_absolute() ? p : (dir / fp).u8string();
  };
  MtlInfo* cur = nullptr;
  std::string line;
  while (std::getline(f, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#') continue;
    std::istringstream in(line);
    std::string key;
    in >> key;
    std::string rest = trim(line.substr(key.size()));
    if (key == "newmtl") { cur = &mats[rest]; continue; }
    if (!cur) continue;
    if (key == "Kd") cur->kd = readVec3(in, cur->kd);
    else if (key == "Ks") cur->ks = readVec3(in, cur->ks);
    else if (key == "Ke") cur->ke = readVec3(in, cur->ke);
    else if (key == "Ns") in >> cur->ns;
    else if (key == "Ni") in >> cur->ni;
    else if (key == "d") in >> cur->d;
    else if (key == "Tr") { float tr; if (in >> tr) cur->d = 1.0f - tr; }
    else if (key == "illum") in >> cur->illum;
    else if (key == "map_Kd") cur->mapKd = resolve(mapFileName(rest));
    else if (key == "map_d") cur->mapD = resolve(mapFileName(rest));
    else if (key == "map_bump" || key == "map_Bump" || key == "bump") cur->mapBump = resolve(mapFileName(rest));
  }
  return mats;
}

// ---- Parsing (parallel over chunks of whole lines) ----

// A state change between faces: object/group name, material, smoothing group or material library.
struct ObjEvent {
  enum Kind : uint8_t { Name, Material, Smooth, MtlLib } kind;
  size_t face;       // index (in the chunk) of the first face it applies to
  std::string text;  // Name / Material / MtlLib
  uint32_t smooth = 0;
};

struct ObjChunk {
  std::vector<Vec3> pos, nrm;
  std::vector<Vec2> tex;
  // Faces: corners [faceStart[f], faceStart[f + 1]) of vi/ti/ni (raw file indices, 0 = absent).
  std::vector<uint32_t> faceStart{0};
  std::vector<int> vi, ti, ni;
  // Counts of v/vt/vn read in this chunk before each face (needed for relative indices).
  std::vector<uint32_t> basePos, baseTex, baseNrm;
  bool relative = false;
  std::vector<ObjEvent> events;
  const char* errorAt = nullptr;  // first malformed face
  bool badIndex = false;          // an index points outside the file's vertices
};

inline bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r'; }

inline const char* skipSpace(const char* p, const char* e) {
  while (p < e && isSpace(*p)) ++p;
  return p;
}

// Parses a number at p (leading '+' allowed); returns the end, or p on failure.
template <typename T>
inline const char* parseNumber(const char* p, const char* e, T& out) {
  const char* s = p < e && *p == '+' ? p + 1 : p;
  auto r = std::from_chars(s, e, out);
  return r.ec == std::errc() ? r.ptr : p;
}

std::string restOfLine(const char* p, const char* e) {
  p = skipSpace(p, e);
  while (e > p && isSpace(e[-1])) --e;
  return std::string(p, e);
}

void parseChunk(const char* p, const char* end, ObjChunk& c) {
  while (p < end) {
    const char* eol = static_cast<const char*>(std::memchr(p, '\n', static_cast<size_t>(end - p)));
    if (!eol) eol = end;
    const char* line = p;
    const char* q = skipSpace(p, eol);
    p = eol + 1;
    if (q >= eol || *q == '#') continue;
    const char* k = q;
    while (q < eol && !isSpace(*q)) ++q;
    size_t klen = static_cast<size_t>(q - k);
    if (klen == 1 && k[0] == 'v') {
      Vec3 v;
      q = parseNumber(skipSpace(q, eol), eol, v.x);
      q = parseNumber(skipSpace(q, eol), eol, v.y);
      parseNumber(skipSpace(q, eol), eol, v.z);
      c.pos.push_back(v);
    } else if (klen == 1 && k[0] == 'f') {
      size_t first = c.vi.size();
      bool bad = false;
      for (;;) {
        q = skipSpace(q, eol);
        if (q >= eol) break;
        int v = 0, t = 0, n = 0;
        const char* r = parseNumber(q, eol, v);
        if (r == q || v == 0) {
          bad = true;
          break;
        }
        q = r;
        if (q < eol && *q == '/') {
          ++q;
          if (q < eol && *q != '/') q = parseNumber(q, eol, t);
          if (q < eol && *q == '/') q = parseNumber(q + 1, eol, n);
        }
        while (q < eol && !isSpace(*q)) ++q;  // ignore anything else glued to the token
        c.relative |= v < 0 || t < 0 || n < 0;
        c.vi.push_back(v);
        c.ti.push_back(t);
        c.ni.push_back(n);
      }
      if (bad && !c.errorAt) c.errorAt = line;
      if (bad || c.vi.size() - first < 3) {
        c.vi.resize(first);
        c.ti.resize(first);
        c.ni.resize(first);
        continue;
      }
      c.faceStart.push_back(static_cast<uint32_t>(c.vi.size()));
      c.basePos.push_back(static_cast<uint32_t>(c.pos.size()));
      c.baseTex.push_back(static_cast<uint32_t>(c.tex.size()));
      c.baseNrm.push_back(static_cast<uint32_t>(c.nrm.size()));
    } else if (klen == 2 && k[0] == 'v' && k[1] == 't') {
      Vec2 t;
      q = parseNumber(skipSpace(q, eol), eol, t.x);
      parseNumber(skipSpace(q, eol), eol, t.y);
      c.tex.push_back(t);
    } else if (klen == 2 && k[0] == 'v' && k[1] == 'n') {
      Vec3 n;
      q = parseNumber(skipSpace(q, eol), eol, n.x);
      q = parseNumber(skipSpace(q, eol), eol, n.y);
      parseNumber(skipSpace(q, eol), eol, n.z);
      c.nrm.push_back(n);
    } else if (klen == 1 && (k[0] == 'o' || k[0] == 'g')) {
      c.events.push_back({ObjEvent::Name, c.faceStart.size() - 1, restOfLine(q, eol)});
    } else if (klen == 6 && std::memcmp(k, "usemtl", 6) == 0) {
      c.events.push_back({ObjEvent::Material, c.faceStart.size() - 1, restOfLine(q, eol)});
    } else if (klen == 6 && std::memcmp(k, "mtllib", 6) == 0) {
      c.events.push_back({ObjEvent::MtlLib, c.faceStart.size() - 1, restOfLine(q, eol)});
    } else if (klen == 1 && k[0] == 's') {
      std::string v = restOfLine(q, eol);
      v = v.substr(0, v.find_first_of(" \t"));
      ObjEvent ev{ObjEvent::Smooth, c.faceStart.size() - 1, std::string()};
      if (!(v == "off" || v == "0" || v.empty())) {
        long g = std::strtol(v.c_str(), nullptr, 10);
        ev.smooth = g > 0 ? (1u << ((g - 1) % 32)) : 1u;
      }
      c.events.push_back(std::move(ev));
    }
  }
}

// A run of consecutive faces of one chunk that go to the same group with the same smoothing.
struct FaceRun {
  uint32_t chunk;
  uint32_t begin, end;  // faces in the chunk
  uint32_t smooth;
  bool fromNormals;  // no "s" statement seen yet: decide smoothing from the file's normals
};

struct ObjGroup {
  std::string name, material;
  std::vector<FaceRun> runs;
  PolyMesh mesh;
  Vec3 pivot;
};

// Maps file-wide indices to a group's own numbering: a dense table over the used index range
// when it is compact (the usual case: objects use a contiguous block of vertices), else a hash.
struct IndexRemap {
  int lo = 0;
  bool dense = true;
  std::vector<int> table;
  std::unordered_map<int, int> map;
  void init(int l, int h, size_t corners) {
    lo = l;
    size_t range = h >= l ? static_cast<size_t>(h - l) + 1 : 0;
    dense = range <= corners * 4 + 4096;
    if (dense) table.assign(range, -1);
    else map.reserve(corners);
  }
  template <typename Add>
  int get(int idx, Add add) {
    if (dense) {
      int& slot = table[static_cast<size_t>(idx - lo)];
      if (slot < 0) slot = add(idx);
      return slot;
    }
    auto it = map.find(idx);
    if (it != map.end()) return it->second;
    int v = add(idx);
    map.emplace(idx, v);
    return v;
  }
};

}  // namespace

bool importObj(Scene& scene, const std::string& path, const ObjImportOptions& opt, ObjImportResult* result,
               std::string* error) {
  ObjImportResult local;
  ObjImportResult& res = result ? *result : local;
  ImportProgress* progress = opt.progress;
  auto cancelled = [&] {
    if (error) *error = "Import cancelled";
    return false;
  };
  auto isCancelled = [&] { return progress && progress->cancelled(); };
  // Read the whole file; parsing then runs on memory.
  std::string data;
  {
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f) {
      if (error) *error = "Cannot open " + path;
      return false;
    }
    f.seekg(0, std::ios::end);
    std::streamoff size = f.tellg();
    f.seekg(0);
    if (size > 0) {
      data.resize(static_cast<size_t>(size));
      const size_t step = size_t(64) << 20;
      for (size_t at = 0; at < data.size() && f; at += step) {
        f.read(&data[at], static_cast<std::streamsize>(std::min(step, data.size() - at)));
        if (reportProgress(progress, 0.0, 0.1, double(at) / double(data.size()))) return cancelled();
      }
      if (!f) {
        if (error) *error = "Cannot read " + path;
        return false;
      }
    }
  }
  fs::path dir = fs::u8path(path).parent_path();

  // Split into chunks at line ends and parse them in parallel.
  const char* base = data.data();
  const char* end = base + data.size();
  size_t want = std::max<size_t>(1, std::min<size_t>(size_t(parallelThreads()) * 8, data.size() >> 20));
  std::vector<const char*> cuts{base};
  for (size_t i = 1; i < want; ++i) {
    const char* p = base + data.size() / want * i;
    if (p <= cuts.back()) continue;
    const char* nl = static_cast<const char*>(std::memchr(p, '\n', static_cast<size_t>(end - p)));
    if (!nl) break;
    cuts.push_back(nl + 1);
  }
  cuts.push_back(end);
  const size_t chunkCount = cuts.size() - 1;
  std::vector<ObjChunk> chunks(chunkCount);
  std::atomic<size_t> parsed{0};
  parallelFor(chunkCount, [&](size_t i) {
    if (isCancelled()) return;
    parseChunk(cuts[i], cuts[i + 1], chunks[i]);
    reportProgress(progress, 0.1, 0.45, double(++parsed) / double(chunkCount));
  });
  if (isCancelled()) return cancelled();
  for (const ObjChunk& c : chunks)
    if (c.errorAt) {
      size_t lineNo = 1 + static_cast<size_t>(std::count(base, c.errorAt, '\n'));
      if (error) *error = "Bad vertex index on line " + std::to_string(lineNo);
      return false;
    }
  data = std::string();

  // File-wide vertex arrays (chunk prefix offsets), and indices resolved to 0-based file-wide
  // numbers (-1 = absent or out of range for uv/normal).
  std::vector<size_t> posBase(chunkCount + 1, 0), texBase(chunkCount + 1, 0), nrmBase(chunkCount + 1, 0);
  for (size_t i = 0; i < chunkCount; ++i) {
    posBase[i + 1] = posBase[i] + chunks[i].pos.size();
    texBase[i + 1] = texBase[i] + chunks[i].tex.size();
    nrmBase[i + 1] = nrmBase[i] + chunks[i].nrm.size();
  }
  if (posBase[chunkCount] > 0x7fffffffu || texBase[chunkCount] > 0x7fffffffu || nrmBase[chunkCount] > 0x7fffffffu) {
    if (error) *error = "Too many vertices in " + path;
    return false;
  }
  std::vector<Vec3> pos(posBase[chunkCount]), nrm(nrmBase[chunkCount]);
  std::vector<Vec2> tex(texBase[chunkCount]);
  parallelFor(chunkCount, [&](size_t i) {
    ObjChunk& c = chunks[i];
    std::copy(c.pos.begin(), c.pos.end(), pos.begin() + static_cast<std::ptrdiff_t>(posBase[i]));
    std::copy(c.tex.begin(), c.tex.end(), tex.begin() + static_cast<std::ptrdiff_t>(texBase[i]));
    std::copy(c.nrm.begin(), c.nrm.end(), nrm.begin() + static_cast<std::ptrdiff_t>(nrmBase[i]));
    c.pos = {};
    c.tex = {};
    c.nrm = {};
    const long long np = static_cast<long long>(pos.size()), nt = static_cast<long long>(tex.size()),
                    nn = static_cast<long long>(nrm.size());
    size_t nf = c.faceStart.size() - 1;
    for (size_t f = 0; f < nf; ++f) {
      long long bp = static_cast<long long>(posBase[i] + c.basePos[f]);
      long long bt = static_cast<long long>(texBase[i] + c.baseTex[f]);
      long long bn = static_cast<long long>(nrmBase[i] + c.baseNrm[f]);
      for (uint32_t k = c.faceStart[f]; k < c.faceStart[f + 1]; ++k) {
        long long v = c.vi[k];
        v = v < 0 ? bp + v : v - 1;
        if (v < 0 || v >= np) {
          c.badIndex = true;
          v = 0;
        }
        c.vi[k] = static_cast<int>(v);
        long long t = c.ti[k];
        t = t == 0 ? -1 : (t < 0 ? bt + t : t - 1);
        c.ti[k] = t >= 0 && t < nt ? static_cast<int>(t) : -1;
        long long n = c.ni[k];
        n = n == 0 ? -1 : (n < 0 ? bn + n : n - 1);
        c.ni[k] = n >= 0 && n < nn ? static_cast<int>(n) : -1;
      }
    }
    c.basePos = {};
    c.baseTex = {};
    c.baseNrm = {};
  });
  for (const ObjChunk& c : chunks)
    if (c.badIndex) {
      if (error) *error = "Bad vertex index in " + path;
      return false;
    }

  // Walk the state changes in file order: assign face runs to (name, material) groups.
  std::vector<ObjGroup> groups;
  std::map<std::pair<std::string, std::string>, size_t> groupIndex;
  std::vector<std::string> mtlLibs;
  std::string curName = fs::u8path(path).stem().u8string(), curMat;
  uint32_t curSmooth = 0;
  bool sawSmoothing = false;
  size_t curGroup = SIZE_MAX;
  for (size_t i = 0; i < chunkCount; ++i) {
    const ObjChunk& c = chunks[i];
    size_t at = 0;
    auto emit = [&](size_t upTo) {
      if (upTo <= at) return;
      if (curGroup == SIZE_MAX) {
        auto it = groupIndex.find(std::make_pair(curName, curMat));
        if (it == groupIndex.end()) {
          it = groupIndex.emplace(std::make_pair(curName, curMat), groups.size()).first;
          groups.push_back(ObjGroup{curName, curMat, {}, {}, {}});
        }
        curGroup = it->second;
      }
      groups[curGroup].runs.push_back(
          FaceRun{static_cast<uint32_t>(i), static_cast<uint32_t>(at), static_cast<uint32_t>(upTo), curSmooth, !sawSmoothing});
      at = upTo;
    };
    for (const ObjEvent& ev : c.events) {
      emit(ev.face);
      switch (ev.kind) {
        case ObjEvent::Name:
          if (!ev.text.empty() && ev.text != "default" && ev.text != curName) {
            curName = ev.text;
            curGroup = SIZE_MAX;
          }
          break;
        case ObjEvent::Material:
          if (ev.text != curMat) {
            curMat = ev.text;
            curGroup = SIZE_MAX;
          }
          break;
        case ObjEvent::Smooth:
          curSmooth = ev.smooth;
          sawSmoothing = true;
          break;
        case ObjEvent::MtlLib: mtlLibs.push_back(ev.text); break;
      }
    }
    emit(c.faceStart.size() - 1);
  }

  // Materials.
  std::map<std::string, uint32_t> matIds;
  if (opt.importMaterials) {
    std::map<std::string, MtlInfo> mtl;
    for (const auto& lib : mtlLibs) {
      auto parsed = parseMtl(dir / fs::u8path(lib), res.warnings);
      mtl.insert(parsed.begin(), parsed.end());
    }
    for (const auto& g : groups) {
      if (g.material.empty() || matIds.count(g.material)) continue;
      Material* m = scene.createMaterial(g.material);
      auto it = mtl.find(g.material);
      if (it != mtl.end()) {
        const MtlInfo& mi = it->second;
        ParamBlock& p = m->params;
        p.setColor("diffuse", clampColor(mi.kd, 0.0f, 1.0f));
        if (!mi.mapKd.empty()) { p.setString("diffuseMap", mi.mapKd); p.setColor("diffuse", Vec3(1.0f)); }
        if (maxComponent(mi.ks) > 0.0f && mi.illum != 1) {
          p.setColor("reflect", clampColor(mi.ks, 0.0f, 1.0f));
          float rough = std::pow(2.0f / (std::max(mi.ns, 0.0f) + 2.0f), 0.25f);
          p.setNumber("reflectGlossiness", 1.0f - rough);
        }
        if (mi.ni > 1.0f) { p.setNumber("ior", mi.ni); p.setNumber("fresnelIor", mi.ni); }
        bool refractive = mi.illum == 4 || mi.illum == 6 || mi.illum == 7 || mi.illum == 9;
        if (mi.d < 1.0f) {
          if (refractive) p.setColor("refract", Vec3(1.0f - mi.d));
          else p.setNumber("opacity", mi.d);
        }
        if (!mi.mapD.empty()) p.setString("opacityMap", mi.mapD);
        if (!mi.mapBump.empty()) p.setString("bumpMap", mi.mapBump);
        if (maxComponent(mi.ke) > 0.0f) p.setColor("selfIllum", mi.ke);
      } else {
        res.warnings.push_back("Material definition not found: " + g.material);
      }
      matIds[g.material] = m->id;
    }
  }

  // Build each group's mesh in parallel: own vertex numbering, smoothing, axis/scale and pivot.
  Mat4 axis = opt.yUp ? Mat4::rotateX(kPi * 0.5f) : Mat4::identity();
  Mat4 xf = axis * Mat4::scale(Vec3(opt.scale));
  std::atomic<size_t> built{0};
  parallelFor(groups.size(), [&](size_t gi) {
    if (isCancelled()) return;
    ObjGroup& g = groups[gi];
    PolyMesh& mesh = g.mesh;
    int vlo = INT_MAX, vhi = -1, tlo = INT_MAX, thi = -1;
    size_t corners = 0, faces = 0;
    for (const FaceRun& r : g.runs) {
      const ObjChunk& c = chunks[r.chunk];
      faces += r.end - r.begin;
      for (uint32_t k = c.faceStart[r.begin]; k < c.faceStart[r.end]; ++k) {
        vlo = std::min(vlo, c.vi[k]);
        vhi = std::max(vhi, c.vi[k]);
        if (c.ti[k] >= 0) {
          tlo = std::min(tlo, c.ti[k]);
          thi = std::max(thi, c.ti[k]);
        }
      }
      corners += c.faceStart[r.end] - c.faceStart[r.begin];
    }
    if (faces == 0) return;
    IndexRemap vmap, tmap;
    vmap.init(vlo, vhi, corners);
    tmap.init(thi >= 0 ? tlo : 0, thi, corners);
    mesh.faces.reserve(faces);
    mesh.positions.reserve(std::min<size_t>(corners, static_cast<size_t>(vhi - vlo) + 1));
    auto addPos = [&](int idx) { return mesh.addVertex(pos[static_cast<size_t>(idx)]); };
    auto addTex = [&](int idx) { return mesh.addUv(tex[static_cast<size_t>(idx)]); };
    for (const FaceRun& r : g.runs) {
      const ObjChunk& c = chunks[r.chunk];
      for (uint32_t f = r.begin; f < r.end; ++f) {
        uint32_t k0 = c.faceStart[f], k1 = c.faceStart[f + 1];
        PolyMesh::Face face;
        face.v.resize(k1 - k0);
        bool allUv = true;
        for (uint32_t k = k0; k < k1; ++k) {
          face.v[k - k0] = vmap.get(c.vi[k], addPos);
          allUv &= c.ti[k] >= 0;
        }
        if (allUv) {
          face.uv.resize(k1 - k0);
          for (uint32_t k = k0; k < k1; ++k) face.uv[k - k0] = tmap.get(c.ti[k], addTex);
        }
        face.smoothGroups = r.smooth;
        if (r.fromNormals) {
          // Decide from the file's normals: a face whose corner normals all match its own
          // normal is flat; otherwise it belongs to a smooth surface.
          bool curved = false, anyNormals = false;
          Vec3 fn = normalize(mesh.faceNormal(face));
          for (uint32_t k = k0; k < k1; ++k) {
            int n = c.ni[k];
            if (n < 0) continue;
            anyNormals = true;
            if (dot(normalize(nrm[static_cast<size_t>(n)]), fn) < 0.999f) {
              curved = true;
              break;
            }
          }
          face.smoothGroups = anyNormals ? (curved ? 1u : 0u) : 1u;
        }
        mesh.faces.push_back(std::move(face));
      }
    }
    mesh.transform(xf);
    Aabb b = mesh.bounds();
    g.pivot = Vec3(b.center().x, b.center().y, b.lo.z);
    for (auto& p : mesh.positions) p -= g.pivot;
    reportProgress(progress, 0.45, 0.95, double(++built) / double(groups.size()));
  });
  if (isCancelled()) return cancelled();
  chunks.clear();

  // Nodes.
  std::unordered_map<std::string, size_t> nameCount;
  for (const auto& g : groups) ++nameCount[g.name];
  scene.beginBulkNames();
  for (auto& g : groups) {
    if (g.mesh.faces.empty()) continue;
    auto node = std::make_unique<Node>();
    std::string name = g.name;
    if (groups.size() > 1 && !g.material.empty() && nameCount[g.name] > 1) name += "_" + g.material;
    node->name = name;
    auto obj = std::make_unique<EditablePolyObject>();
    res.triangles += g.mesh.triangleCount();
    obj->setMesh(std::move(g.mesh));
    node->object = std::move(obj);
    node->transform.position = g.pivot;
    node->wireColor = Vec3(0.75f, 0.75f, 0.75f);
    auto mit = matIds.find(g.material);
    if (mit != matIds.end()) node->materialId = mit->second;
    res.nodeIds.push_back(scene.addNode(std::move(node))->id);
  }
  scene.endBulkNames();
  if (res.nodeIds.empty()) {
    if (error) *error = "No faces found in " + path;
    return false;
  }
  return true;
}

}  // namespace veraa
