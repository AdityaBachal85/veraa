#include "obj_import.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <unordered_map>

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

struct Group {
  std::string name, material;
  PolyMesh mesh;
  std::unordered_map<int, int> vRemap, tRemap;
  bool hasExplicitSmoothing = false;
};

int fixIndex(int idx, size_t count) { return idx < 0 ? static_cast<int>(count) + idx : idx - 1; }

}  // namespace

bool importObj(Scene& scene, const std::string& path, const ObjImportOptions& opt, ObjImportResult* result,
               std::string* error) {
  ObjImportResult local;
  ObjImportResult& res = result ? *result : local;
  std::ifstream f(fs::u8path(path));
  if (!f) {
    if (error) *error = "Cannot open " + path;
    return false;
  }
  fs::path dir = fs::u8path(path).parent_path();

  std::vector<Vec3> pos, nrm;
  std::vector<Vec2> tex;
  std::vector<Group> groups;
  std::map<std::pair<std::string, std::string>, size_t> groupIndex;
  std::map<std::string, MtlInfo> mtl;
  std::string curName = fs::u8path(path).stem().u8string(), curMat;
  uint32_t curSmooth = 0;
  bool sawSmoothing = false;

  auto groupFor = [&]() -> Group& {
    auto key = std::make_pair(curName, curMat);
    auto it = groupIndex.find(key);
    if (it != groupIndex.end()) return groups[it->second];
    groupIndex[key] = groups.size();
    groups.push_back(Group{curName, curMat, {}, {}, {}, false});
    return groups.back();
  };

  std::string line;
  size_t lineNo = 0;
  while (std::getline(f, line)) {
    ++lineNo;
    if (line.empty() || line[0] == '#') continue;
    std::istringstream in(line);
    std::string key;
    in >> key;
    if (key == "v") {
      Vec3 p;
      in >> p.x >> p.y >> p.z;
      pos.push_back(p);
    } else if (key == "vt") {
      Vec2 t;
      in >> t.x >> t.y;
      tex.push_back(t);
    } else if (key == "vn") {
      Vec3 n;
      in >> n.x >> n.y >> n.z;
      nrm.push_back(n);
    } else if (key == "o" || key == "g") {
      std::string name = trim(line.substr(key.size()));
      if (!name.empty() && name != "default") curName = name;
    } else if (key == "usemtl") {
      curMat = trim(line.substr(6));
    } else if (key == "mtllib" && opt.importMaterials) {
      std::string lib = trim(line.substr(6));
      auto parsed = parseMtl(dir / fs::u8path(lib), res.warnings);
      mtl.insert(parsed.begin(), parsed.end());
    } else if (key == "s") {
      std::string v;
      in >> v;
      sawSmoothing = true;
      if (v == "off" || v == "0") curSmooth = 0;
      else {
        long g = std::strtol(v.c_str(), nullptr, 10);
        curSmooth = g > 0 ? (1u << ((g - 1) % 32)) : 1u;
      }
    } else if (key == "f") {
      Group& g = groupFor();
      PolyMesh::Face face;
      std::vector<int> normalIdx;
      bool allUv = true;
      std::string tok;
      while (in >> tok) {
        int vi = 0, ti = 0, ni = 0;
        int slash1 = static_cast<int>(tok.find('/'));
        if (slash1 < 0) {
          vi = std::atoi(tok.c_str());
        } else {
          vi = std::atoi(tok.substr(0, slash1).c_str());
          int slash2 = static_cast<int>(tok.find('/', slash1 + 1));
          std::string ts = slash2 < 0 ? tok.substr(slash1 + 1) : tok.substr(slash1 + 1, slash2 - slash1 - 1);
          if (!ts.empty()) ti = std::atoi(ts.c_str());
          if (slash2 >= 0 && slash2 + 1 < static_cast<int>(tok.size())) ni = std::atoi(tok.substr(slash2 + 1).c_str());
        }
        int gv = fixIndex(vi, pos.size());
        if (vi == 0 || gv < 0 || gv >= static_cast<int>(pos.size())) {
          if (error) *error = "Bad vertex index on line " + std::to_string(lineNo);
          return false;
        }
        auto vit = g.vRemap.find(gv);
        int lv = vit != g.vRemap.end() ? vit->second : (g.vRemap[gv] = g.mesh.addVertex(pos[gv]));
        face.v.push_back(lv);
        int gt = ti ? fixIndex(ti, tex.size()) : -1;
        if (gt >= 0 && gt < static_cast<int>(tex.size())) {
          auto tit = g.tRemap.find(gt);
          face.uv.push_back(tit != g.tRemap.end() ? tit->second : (g.tRemap[gt] = g.mesh.addUv(tex[gt])));
        } else {
          allUv = false;
        }
        int gn = ni ? fixIndex(ni, nrm.size()) : -1;
        normalIdx.push_back(gn >= 0 && gn < static_cast<int>(nrm.size()) ? gn : -1);
      }
      if (face.v.size() < 3) continue;
      if (!allUv) face.uv.clear();
      face.smoothGroups = curSmooth;
      if (!sawSmoothing) {
        // Decide from the file's normals: a face whose corner normals all match its own
        // normal is flat; otherwise it belongs to a smooth surface.
        bool curved = false;
        Vec3 fn = normalize(g.mesh.faceNormal(face));
        for (int n : normalIdx) {
          if (n < 0) continue;
          if (dot(normalize(nrm[n]), fn) < 0.999f) { curved = true; break; }
        }
        bool anyNormals = std::any_of(normalIdx.begin(), normalIdx.end(), [](int n) { return n >= 0; });
        face.smoothGroups = anyNormals ? (curved ? 1u : 0u) : 1u;
      }
      g.mesh.faces.push_back(std::move(face));
    }
  }

  // Materials.
  std::map<std::string, uint32_t> matIds;
  if (opt.importMaterials) {
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

  // Nodes.
  Mat4 axis = opt.yUp ? Mat4::rotateX(kPi * 0.5f) : Mat4::identity();
  Mat4 xf = axis * Mat4::scale(Vec3(opt.scale));
  for (auto& g : groups) {
    if (g.mesh.faces.empty()) continue;
    g.mesh.transform(xf);
    Aabb b = g.mesh.bounds();
    Vec3 pivot(b.center().x, b.center().y, b.lo.z);
    for (auto& p : g.mesh.positions) p -= pivot;

    auto node = std::make_unique<Node>();
    std::string name = g.name;
    if (groups.size() > 1 && !g.material.empty()) {
      size_t sameName = std::count_if(groups.begin(), groups.end(), [&](const Group& o) { return o.name == g.name; });
      if (sameName > 1) name += "_" + g.material;
    }
    node->name = name;
    auto obj = std::make_unique<EditablePolyObject>();
    res.triangles += g.mesh.triangleCount();
    obj->mesh = std::move(g.mesh);
    node->object = std::move(obj);
    node->transform.position = pivot;
    node->wireColor = Vec3(0.75f, 0.75f, 0.75f);
    auto mit = matIds.find(g.material);
    if (mit != matIds.end()) node->materialId = mit->second;
    res.nodeIds.push_back(scene.addNode(std::move(node))->id);
  }
  if (res.nodeIds.empty()) {
    if (error) *error = "No faces found in " + path;
    return false;
  }
  return true;
}

}  // namespace veraa
