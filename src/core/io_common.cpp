#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <unordered_set>

#include "io_util.h"
#include "obj_import.h"

namespace veraa {

namespace fs = std::filesystem;

namespace io {

std::string lowerExtension(const std::string& path) {
  std::string ext = fs::path(path).extension().string();
  if (!ext.empty() && ext[0] == '.') ext.erase(0, 1);
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return ext;
}

std::string directoryOf(const std::string& path) { return fs::path(path).parent_path().string(); }
std::string fileNameOf(const std::string& path) { return fs::path(path).filename().string(); }
std::string baseNameOf(const std::string& path) { return fs::path(path).stem().string(); }

std::string joinPath(const std::string& dir, const std::string& rel) {
  if (rel.empty()) return rel;
  fs::path p(rel);
  if (p.is_absolute() || dir.empty()) return p.lexically_normal().string();
  return (fs::path(dir) / p).lexically_normal().string();
}

std::string relativePath(const std::string& dir, const std::string& target) {
  if (target.empty() || dir.empty()) return target;
  std::error_code ec;
  fs::path rel = fs::relative(fs::path(target), fs::path(dir), ec);
  if (ec || rel.empty() || rel.string().rfind("..", 0) == 0) return target;
  return rel.generic_string();
}

std::vector<const Node*> exportNodes(const Scene& scene, const ExportOptions& options) {
  std::vector<const Node*> out;
  if (options.nodeIds.empty()) {
    for (const auto& n : scene.nodes) out.push_back(n.get());
    return out;
  }
  std::unordered_set<uint32_t> keep(options.nodeIds.begin(), options.nodeIds.end());
  for (const auto& n : scene.nodes) {
    bool in = false;
    for (const Node* p = n.get(); p && !in; p = p->parent()) in = keep.count(p->id) != 0;
    if (in) out.push_back(n.get());
  }
  return out;
}

PolyMesh nodeSpaceMesh(const Node& n) {
  PolyMesh m = n.mesh();
  if (!n.objectOffset.isIdentity()) m.transform(n.objectOffset.matrix());
  return m;
}

std::string uniqueName(const Scene& scene, const std::string& nameIn) {
  std::string name = nameIn.empty() ? std::string("Object") : nameIn;
  if (!scene.findNodeByName(name)) return name;
  for (int i = 1; i < 100000; ++i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%03d", i);
    std::string candidate = name + "_" + buf;
    if (!scene.findNodeByName(candidate)) return candidate;
  }
  return scene.uniqueNodeName(name);
}

Node* addMeshNode(Scene& scene, const std::string& name, PolyMesh mesh) {
  Node* n = scene.createNode("EditablePoly");
  if (!n) return nullptr;
  n->name = uniqueName(scene, name);
  static_cast<EditablePolyObject*>(n->object.get())->mesh = std::move(mesh);
  n->object->touch();
  n->invalidate();
  return n;
}

uint32_t addMaterial(Scene& scene, const std::string& name, const Vec3& diffuse, const std::string& diffuseMap,
                     float opacity) {
  Material* m = scene.createMaterial(name.empty() ? std::string("Material") : name);
  if (!m) return 0;
  m->params.setColor("diffuse", diffuse);
  if (!diffuseMap.empty()) m->params.setString("diffuseMap", diffuseMap);
  if (opacity < 0.999f) m->params.setNumber("opacity", opacity);
  return m->id;
}

Vec3 lookAlong(const Vec3& dirIn) {
  Vec3 dir = normalize(dirIn);
  Vec3 right = cross(dir, Vec3(0, 0, 1));
  if (length2(right) < 1e-8f) right = Vec3(1, 0, 0);
  right = normalize(right);
  Vec3 up = cross(right, dir);
  Vec3 back = -dir;
  Mat4 m;
  for (int r = 0; r < 3; ++r) {
    m.m[r][0] = right[r];
    m.m[r][1] = up[r];
    m.m[r][2] = back[r];
  }
  return eulerDegFromMatrix(m);
}

std::vector<std::array<int, 3>> fanTriangles(const PolyMesh& mesh, std::vector<int>* faceOfTriangle) {
  std::vector<std::array<int, 3>> out;
  for (size_t f = 0; f < mesh.faces.size(); ++f) {
    const auto& v = mesh.faces[f].v;
    for (size_t i = 1; i + 1 < v.size(); ++i) {
      out.push_back({v[0], v[i], v[i + 1]});
      if (faceOfTriangle) faceOfTriangle->push_back(static_cast<int>(f));
    }
  }
  return out;
}

}  // namespace io

std::vector<std::string> importFormats() { return {"obj", "fbx", "gltf", "glb", "stl", "ply", "3ds"}; }
std::vector<std::string> exportFormats() { return {"obj", "fbx", "gltf", "glb", "stl", "ply"}; }

bool importScene(Scene& scene, const std::string& path, const ImportOptions& options, ImportResult* result,
                 std::string* error) {
  std::string ext = io::lowerExtension(path);
  ImportResult local;
  ImportResult* r = result ? result : &local;
  if (ext == "obj") {
    ObjImportOptions o;
    o.yUp = options.convertAxes;
    o.scale = options.scale;
    o.importMaterials = options.importMaterials;
    ObjImportResult res;
    if (!importObj(scene, path, o, &res, error)) return false;
    r->nodeIds.insert(r->nodeIds.end(), res.nodeIds.begin(), res.nodeIds.end());
    r->triangles += res.triangles;
    r->warnings.insert(r->warnings.end(), res.warnings.begin(), res.warnings.end());
    return true;
  }
  if (ext == "fbx") return importFbx(scene, path, options, r, error);
  if (ext == "gltf" || ext == "glb") return importGltf(scene, path, options, r, error);
  if (ext == "stl") return importStl(scene, path, options, r, error);
  if (ext == "ply") return importPly(scene, path, options, r, error);
  if (ext == "3ds") return import3ds(scene, path, options, r, error);
  if (error) *error = "Unsupported file type: ." + ext;
  return false;
}

bool exportScene(const Scene& scene, const std::string& path, const ExportOptions& options, std::string* error) {
  std::string ext = io::lowerExtension(path);
  if (ext == "obj") return exportObj(scene, path, options, error);
  if (ext == "fbx") return exportFbx(scene, path, options, error);
  if (ext == "gltf" || ext == "glb") return exportGltf(scene, path, options, error);
  if (ext == "stl") return exportStl(scene, path, options, error);
  if (ext == "ply") return exportPly(scene, path, options, error);
  if (error) *error = "Unsupported export type: ." + ext;
  return false;
}

}  // namespace veraa
