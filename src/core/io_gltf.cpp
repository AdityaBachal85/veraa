// glTF 2.0: import (.gltf with external or embedded buffers, .glb) via cgltf; export written
// directly (.gltf + .bin, or a single .glb). glTF is Y-up in meters; Veraa is Z-up in centimeters.
#define CGLTF_IMPLEMENTATION
#include "cgltf/cgltf.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <unordered_map>

#include "io_util.h"

namespace veraa {

namespace {

// Veraa (Z-up, cm) -> glTF (Y-up, m) linear map without the unit scale: (x, y, z) -> (x, z, -y).
Mat4 zUpToYUp() {
  Mat4 m;
  m.m[0][0] = 1; m.m[0][1] = 0; m.m[0][2] = 0;
  m.m[1][0] = 0; m.m[1][1] = 0; m.m[1][2] = 1;
  m.m[2][0] = 0; m.m[2][1] = -1; m.m[2][2] = 0;
  return m;
}

Mat4 fromColumnMajor(const float* f) {
  Mat4 m;
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r) m.m[r][c] = f[c * 4 + r];
  return m;
}

// Rotation matrix (orthonormal, det +1) to quaternion (x, y, z, w).
std::array<double, 4> quatFromMatrix(const Mat4& m) {
  double tr = m.m[0][0] + m.m[1][1] + m.m[2][2];
  double x, y, z, w;
  if (tr > 0) {
    double s = std::sqrt(tr + 1.0) * 2;
    w = 0.25 * s;
    x = (m.m[2][1] - m.m[1][2]) / s;
    y = (m.m[0][2] - m.m[2][0]) / s;
    z = (m.m[1][0] - m.m[0][1]) / s;
  } else if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2]) {
    double s = std::sqrt(1.0 + m.m[0][0] - m.m[1][1] - m.m[2][2]) * 2;
    w = (m.m[2][1] - m.m[1][2]) / s;
    x = 0.25 * s;
    y = (m.m[0][1] + m.m[1][0]) / s;
    z = (m.m[0][2] + m.m[2][0]) / s;
  } else if (m.m[1][1] > m.m[2][2]) {
    double s = std::sqrt(1.0 + m.m[1][1] - m.m[0][0] - m.m[2][2]) * 2;
    w = (m.m[0][2] - m.m[2][0]) / s;
    x = (m.m[0][1] + m.m[1][0]) / s;
    y = 0.25 * s;
    z = (m.m[1][2] + m.m[2][1]) / s;
  } else {
    double s = std::sqrt(1.0 + m.m[2][2] - m.m[0][0] - m.m[1][1]) * 2;
    w = (m.m[1][0] - m.m[0][1]) / s;
    x = (m.m[0][2] + m.m[2][0]) / s;
    y = (m.m[1][2] + m.m[2][1]) / s;
    z = 0.25 * s;
  }
  double len = std::sqrt(x * x + y * y + z * z + w * w);
  return {x / len, y / len, z / len, w / len};
}

std::string uriDecode(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
      i += 2;
    } else {
      out += s[i];
    }
  }
  return out;
}

}  // namespace

bool importGltf(Scene& scene, const std::string& path, const ImportOptions& options, ImportResult* result,
                std::string* error) {
  cgltf_options opts = {};
  cgltf_data* data = nullptr;
  if (cgltf_parse_file(&opts, path.c_str(), &data) != cgltf_result_success) {
    if (error) *error = "glTF: cannot parse " + path;
    return false;
  }
  if (cgltf_load_buffers(&opts, data, path.c_str()) != cgltf_result_success) {
    cgltf_free(data);
    if (error) *error = "glTF: cannot load the buffers of " + path;
    return false;
  }
  std::string dir = io::directoryOf(path);
  // glTF space -> Veraa space: rotate Y-up to Z-up and convert meters to centimeters.
  float unit = (options.convertUnits ? 100.0f : 1.0f) * options.scale;
  Mat4 C = options.convertAxes ? zUpToYUp().inverse() : Mat4::identity();
  C = Mat4::scale(Vec3(unit)) * C;
  Mat4 Cinv = C.inverse();

  // Materials and their textures (embedded images are written next to the file).
  std::unordered_map<const cgltf_material*, uint32_t> materials;
  if (options.importMaterials)
    for (cgltf_size i = 0; i < data->materials_count; ++i) {
      const cgltf_material& m = data->materials[i];
      Vec3 diffuse(0.8f);
      float opacity = 1.0f, metallic = 0.0f, roughness = 1.0f;
      std::string tex;
      if (m.has_pbr_metallic_roughness) {
        const auto& pbr = m.pbr_metallic_roughness;
        diffuse = Vec3(pbr.base_color_factor[0], pbr.base_color_factor[1], pbr.base_color_factor[2]);
        opacity = pbr.base_color_factor[3];
        metallic = pbr.metallic_factor;
        roughness = pbr.roughness_factor;
        if (const cgltf_texture* t = pbr.base_color_texture.texture; t && t->image) {
          const cgltf_image* img = t->image;
          if (img->uri && std::strncmp(img->uri, "data:", 5) != 0) {
            tex = io::joinPath(dir, uriDecode(img->uri));
          } else if (img->buffer_view) {
            std::string ext = img->mime_type && std::strstr(img->mime_type, "png") ? ".png" : ".jpg";
            std::string texDir = io::joinPath(dir, io::baseNameOf(path) + "_textures");
            std::filesystem::create_directories(texDir);
            std::string name = img->name && *img->name ? img->name : "image" + std::to_string(i);
            tex = io::joinPath(texDir, name + ext);
            std::ofstream out(tex, std::ios::binary);
            out.write(reinterpret_cast<const char*>(cgltf_buffer_view_data(img->buffer_view)),
                      static_cast<std::streamsize>(img->buffer_view->size));
          }
        }
      }
      uint32_t id = io::addMaterial(scene, m.name ? m.name : "Material", diffuse, tex,
                                    m.alpha_mode == cgltf_alpha_mode_opaque ? 1.0f : opacity);
      if (Material* mat = scene.findMaterial(id)) {
        mat->params.setNumber("metalness", metallic);
        if (roughness < 0.999f || metallic > 0.0f) {
          mat->params.setColor("reflect", Vec3(1.0f));
          mat->params.setNumber("reflectGlossiness", 1.0f - roughness);
        }
        Vec3 emissive(m.emissive_factor[0], m.emissive_factor[1], m.emissive_factor[2]);
        if (maxComponent(emissive) > 0) mat->params.setColor("selfIllum", emissive);
      }
      materials[&m] = id;
    }

  const bool haveDummy = findObjectType("Dummy") != nullptr;
  std::function<void(const cgltf_node*, uint32_t)> visit = [&](const cgltf_node* gn, uint32_t parentId) {
    float lm[16];
    cgltf_node_transform_local(gn, lm);
    Mat4 local = C * fromColumnMajor(lm) * Cinv;
    Node* n = nullptr;
    std::string name = gn->name ? gn->name : "";
    if (gn->mesh) {
      PolyMesh pm;
      uint32_t matId = 0;
      bool mixed = false;
      for (cgltf_size p = 0; p < gn->mesh->primitives_count; ++p) {
        const cgltf_primitive& prim = gn->mesh->primitives[p];
        if (prim.type != cgltf_primitive_type_triangles) continue;
        const cgltf_accessor* pos = nullptr;
        const cgltf_accessor* uv = nullptr;
        for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
          if (prim.attributes[a].type == cgltf_attribute_type_position) pos = prim.attributes[a].data;
          if (prim.attributes[a].type == cgltf_attribute_type_texcoord && prim.attributes[a].index == 0) uv = prim.attributes[a].data;
        }
        if (!pos) continue;
        int base = static_cast<int>(pm.positions.size());
        int uvBase = static_cast<int>(pm.uvs.size());
        for (cgltf_size v = 0; v < pos->count; ++v) {
          float f[3] = {0, 0, 0};
          cgltf_accessor_read_float(pos, v, f, 3);
          pm.positions.push_back(C.transformPoint(Vec3(f[0], f[1], f[2])));
        }
        if (uv)
          for (cgltf_size v = 0; v < uv->count; ++v) {
            float f[2] = {0, 0};
            cgltf_accessor_read_float(uv, v, f, 2);
            pm.uvs.push_back(Vec2(f[0], 1.0f - f[1]));
          }
        cgltf_size count = prim.indices ? prim.indices->count : pos->count;
        for (cgltf_size k = 0; k + 2 < count; k += 3) {
          int i0 = static_cast<int>(prim.indices ? cgltf_accessor_read_index(prim.indices, k) : k);
          int i1 = static_cast<int>(prim.indices ? cgltf_accessor_read_index(prim.indices, k + 1) : k + 1);
          int i2 = static_cast<int>(prim.indices ? cgltf_accessor_read_index(prim.indices, k + 2) : k + 2);
          std::vector<int> uvs;
          if (uv) uvs = {uvBase + i0, uvBase + i1, uvBase + i2};
          pm.addFace({base + i0, base + i1, base + i2}, uvs, 1, static_cast<int>(p));
        }
        auto it = materials.find(prim.material);
        uint32_t m = it == materials.end() ? 0 : it->second;
        if (p == 0) matId = m;
        else if (m != matId) mixed = true;
      }
      if (options.weld) weldVertices(pm);
      if (result) result->triangles += pm.triangleCount();
      n = io::addMeshNode(scene, name.empty() ? (gn->mesh->name ? gn->mesh->name : "Mesh") : name, std::move(pm));
      if (n) n->materialId = matId;
      if (mixed && result) result->warnings.push_back(name + ": several materials; the first one was assigned");
    } else if (gn->camera && options.importCameras) {
      n = scene.createNode("Camera");
      if (n && gn->camera->type == cgltf_camera_type_perspective) {
        const auto& p = gn->camera->data.perspective;
        float aspect = p.has_aspect_ratio && p.aspect_ratio > 0 ? p.aspect_ratio : 16.0f / 9.0f;
        float hfov = 2.0f * std::atan(std::tan(p.yfov * 0.5f) * aspect);
        n->object->params.setNumber("fov", degrees(hfov));
      }
    } else if (gn->light && options.importLights) {
      const cgltf_light* l = gn->light;
      n = scene.createNode(l->type == cgltf_light_type_directional ? "SunLight"
                           : l->type == cgltf_light_type_spot      ? "SpotLight"
                                                                   : "OmniLight");
      if (n) {
        ParamBlock& p = n->object->params;
        p.setColor("color", Vec3(l->color[0], l->color[1], l->color[2]));
        if (l->type == cgltf_light_type_spot) {
          p.setNumber("hotspot", degrees(l->spot_inner_cone_angle) * 2.0);
          p.setNumber("falloff", degrees(l->spot_outer_cone_angle) * 2.0);
        }
      }
    } else {
      n = haveDummy ? scene.createNode("Dummy") : io::addMeshNode(scene, name.empty() ? "Group" : name, PolyMesh());
    }
    uint32_t myId = parentId;
    if (n) {
      if (!name.empty()) n->name = io::uniqueName(scene, name);
      n->parentId = parentId;
      n->transform = decomposeTransform(local);
      if (gn->camera || gn->light) {
        // glTF cameras and lights look along local -Z; rebuild a Veraa placement aimed the same way.
        float wm[16];
        cgltf_node_transform_world(gn, wm);
        Mat4 world = C * fromColumnMajor(wm) * Cinv;
        Vec3 dir = normalize(world.transformDir(C.transformDir(Vec3(0, 0, -1))));
        Transform t;
        t.position = world.translation();
        t.rotation = io::lookAlong(dir);
        n->setWorldMatrix(t.matrix());
      }
      n->invalidate();
      myId = n->id;
      if (result) result->nodeIds.push_back(n->id);
    }
    for (cgltf_size c = 0; c < gn->children_count; ++c) visit(gn->children[c], myId);
  };
  const cgltf_scene* sc = data->scene ? data->scene : (data->scenes_count ? &data->scenes[0] : nullptr);
  if (sc) {
    for (cgltf_size i = 0; i < sc->nodes_count; ++i) visit(sc->nodes[i], 0);
  } else {
    for (cgltf_size i = 0; i < data->nodes_count; ++i)
      if (!data->nodes[i].parent) visit(&data->nodes[i], 0);
  }
  cgltf_free(data);
  return true;
}

// ---------------------------------------------------------------------------------------------
// Export

namespace {

struct Blob {
  std::vector<uint8_t> bytes;
  json bufferViews = json::array();
  json accessors = json::array();

  size_t append(const void* p, size_t n) {
    while (bytes.size() % 4) bytes.push_back(0);
    size_t off = bytes.size();
    const uint8_t* b = static_cast<const uint8_t*>(p);
    bytes.insert(bytes.end(), b, b + n);
    return off;
  }
  int view(const void* p, size_t n, int target) {
    size_t off = append(p, n);
    json v = {{"buffer", 0}, {"byteOffset", off}, {"byteLength", n}};
    if (target) v["target"] = target;
    bufferViews.push_back(v);
    return static_cast<int>(bufferViews.size()) - 1;
  }
  int floats(const std::vector<float>& f, int comps, const char* type, bool minmax) {
    int v = view(f.data(), f.size() * sizeof(float), 34962);
    json a = {{"bufferView", v}, {"componentType", 5126}, {"count", f.size() / comps}, {"type", type}};
    if (minmax && !f.empty()) {
      std::vector<float> lo(comps, 1e30f), hi(comps, -1e30f);
      for (size_t i = 0; i < f.size(); ++i) {
        lo[i % comps] = std::min(lo[i % comps], f[i]);
        hi[i % comps] = std::max(hi[i % comps], f[i]);
      }
      a["min"] = lo;
      a["max"] = hi;
    }
    accessors.push_back(a);
    return static_cast<int>(accessors.size()) - 1;
  }
  int indices(const std::vector<uint32_t>& idx) {
    int v = view(idx.data(), idx.size() * sizeof(uint32_t), 34963);
    accessors.push_back({{"bufferView", v}, {"componentType", 5125}, {"count", idx.size()}, {"type", "SCALAR"}});
    return static_cast<int>(accessors.size()) - 1;
  }
};

json trs(const Mat4& m) {
  Vec3 c0 = m.column(0), c1 = m.column(1), c2 = m.column(2);
  Vec3 s(length(c0), length(c1), length(c2));
  if (dot(cross(c0, c1), c2) < 0) s.x = -s.x;  // mirrored
  Mat4 r;
  for (int row = 0; row < 3; ++row) {
    r.m[row][0] = s.x != 0 ? c0[row] / s.x : 0;
    r.m[row][1] = s.y != 0 ? c1[row] / s.y : 0;
    r.m[row][2] = s.z != 0 ? c2[row] / s.z : 0;
  }
  auto q = quatFromMatrix(r);
  Vec3 t = m.translation();
  json j = json::object();
  if (length(t) > 1e-9f) j["translation"] = {t.x, t.y, t.z};
  if (std::fabs(q[3]) < 0.9999999) j["rotation"] = {q[0], q[1], q[2], q[3]};
  if (length(s - Vec3(1.0f)) > 1e-6f) j["scale"] = {s.x, s.y, s.z};
  return j;
}

}  // namespace

bool exportGltf(const Scene& scene, const std::string& path, const ExportOptions& options, std::string* error) {
  bool glb = io::lowerExtension(path) == "glb";
  std::vector<const Node*> nodes = io::exportNodes(scene, options);
  // Veraa -> glTF: Z-up to Y-up and centimeters to meters.
  Mat4 Ci = Mat4::scale(Vec3(0.01f * options.scale)) * zUpToYUp();
  Mat4 CiInv = Ci.inverse();
  Blob blob;
  json gNodes = json::array(), meshes = json::array(), materials = json::array(), cameras = json::array(),
       lights = json::array(), images = json::array(), textures = json::array();
  std::unordered_map<uint32_t, int> nodeIndex, materialIndex;
  std::unordered_map<std::string, int> imageIndex;
  for (const Node* n : nodes) nodeIndex[n->id] = static_cast<int>(nodeIndex.size());
  std::string dir = io::directoryOf(path);
  Mat4 camFix = Mat4::rotateX(radians(-90.0f));  // Veraa -Z/+Y camera axes in glTF node space

  auto materialFor = [&](const Node* n) -> int {
    const Material* mat = scene.findMaterial(n->materialId);
    uint32_t key = mat ? mat->id : 0x80000000u | n->id;
    auto it = materialIndex.find(key);
    if (it != materialIndex.end()) return it->second;
    Vec3 d = mat ? mat->params.getColor("diffuse") : n->wireColor;
    float opacity = mat ? mat->params.getFloat("opacity") : 1.0f;
    json pbr = {{"baseColorFactor", {d.x, d.y, d.z, opacity}},
                {"metallicFactor", mat ? mat->params.getFloat("metalness") : 0.0f},
                {"roughnessFactor", mat && maxComponent(mat->params.getColor("reflect")) > 0.01f
                                        ? 1.0f - mat->params.getFloat("reflectGlossiness")
                                        : 1.0f}};
    std::string map = mat ? mat->params.getString("diffuseMap") : std::string();
    if (!map.empty()) {
      std::string abs = scene.resolvePath(map);
      int img;
      auto ii = imageIndex.find(abs);
      if (ii != imageIndex.end()) {
        img = ii->second;
      } else {
        json image;
        std::string ext = io::lowerExtension(abs);
        if (glb) {
          std::ifstream in(abs, std::ios::binary);
          std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
          if (!bytes.empty()) {
            image["bufferView"] = blob.view(bytes.data(), bytes.size(), 0);
            image["mimeType"] = ext == "png" ? "image/png" : "image/jpeg";
          } else {
            image["uri"] = io::relativePath(dir, abs);
          }
        } else {
          image["uri"] = io::relativePath(dir, abs);
        }
        images.push_back(image);
        img = static_cast<int>(images.size()) - 1;
        imageIndex[abs] = img;
      }
      textures.push_back({{"source", img}});
      pbr["baseColorTexture"] = {{"index", static_cast<int>(textures.size()) - 1}};
    }
    json m = {{"name", mat ? mat->name : n->name + "_color"}, {"pbrMetallicRoughness", pbr}};
    if (opacity < 0.999f) m["alphaMode"] = "BLEND";
    if (mat && maxComponent(mat->params.getColor("selfIllum")) > 0) {
      Vec3 e = mat->params.getColor("selfIllum");
      m["emissiveFactor"] = {std::min(e.x, 1.0f), std::min(e.y, 1.0f), std::min(e.z, 1.0f)};
    }
    materials.push_back(m);
    int index = static_cast<int>(materials.size()) - 1;
    materialIndex[key] = index;
    return index;
  };

  json roots = json::array();
  for (const Node* n : nodes) {
    bool root = !n->parent() || !nodeIndex.count(n->parentId);
    Mat4 local = root ? n->worldMatrix() : n->transform.matrix();
    Mat4 g = Ci * local * CiInv;
    bool aimed = n->camera() || n->light();
    if (aimed) g = g * camFix;
    json gn = trs(g);
    gn["name"] = n->name;
    if (n->kind() == ObjectKind::Geometry && !n->triMesh().empty()) {
      const TriMesh& t = n->triMesh();
      Mat4 vm = Ci * n->objectOffset.matrix();
      Mat4 nm = vm.inverse().transposed();
      std::vector<float> pos, nrm, uv;
      for (size_t i = 0; i < t.positions.size(); ++i) {
        Vec3 p = vm.transformPoint(t.positions[i]);
        Vec3 q = normalize(nm.transformDir(t.normals[i]));
        pos.insert(pos.end(), {p.x, p.y, p.z});
        nrm.insert(nrm.end(), {q.x, q.y, q.z});
        if (!t.uvs.empty()) uv.insert(uv.end(), {t.uvs[i].x, 1.0f - t.uvs[i].y});
      }
      json attrs = {{"POSITION", blob.floats(pos, 3, "VEC3", true)}, {"NORMAL", blob.floats(nrm, 3, "VEC3", false)}};
      if (!uv.empty()) attrs["TEXCOORD_0"] = blob.floats(uv, 2, "VEC2", false);
      json prim = {{"attributes", attrs}, {"indices", blob.indices(t.indices)}, {"mode", 4}};
      if (options.exportMaterials) prim["material"] = materialFor(n);
      meshes.push_back({{"name", n->name}, {"primitives", json::array({prim})}});
      gn["mesh"] = static_cast<int>(meshes.size()) - 1;
    } else if (n->camera()) {
      const Scene& s = scene;
      float aspect = static_cast<float>(s.renderSettings.getInt("width")) / std::max(1, s.renderSettings.getInt("height"));
      cameras.push_back({{"type", "perspective"},
                         {"name", n->name},
                         {"perspective", {{"yfov", radians(n->camera()->verticalFovDeg(aspect))},
                                          {"aspectRatio", aspect},
                                          {"znear", 0.01}}}});
      gn["camera"] = static_cast<int>(cameras.size()) - 1;
    } else if (n->light()) {
      const ParamBlock& p = n->object->params;
      std::string type = n->object->type();
      Vec3 c = p.has("color") ? p.getColor("color") : Vec3(1.0f);
      json l = {{"name", n->name},
                {"color", {c.x, c.y, c.z}},
                {"intensity", p.has("intensity") ? p.getFloat("intensity") : 1.0f},
                {"type", type == "SunLight" ? "directional" : type == "SpotLight" ? "spot" : "point"}};
      if (type == "SpotLight")
        l["spot"] = {{"innerConeAngle", radians(p.getFloat("hotspot") * 0.5f)},
                     {"outerConeAngle", radians(p.getFloat("falloff") * 0.5f)}};
      lights.push_back(l);
      gn["extensions"] = {{"KHR_lights_punctual", {{"light", static_cast<int>(lights.size()) - 1}}}};
    }
    gNodes.push_back(gn);
    if (root) roots.push_back(nodeIndex[n->id]);
  }
  for (const Node* n : nodes) {
    json kids = json::array();
    for (Node* c : scene.children(n->id))
      if (nodeIndex.count(c->id)) kids.push_back(nodeIndex[c->id]);
    if (!kids.empty()) gNodes[nodeIndex[n->id]]["children"] = kids;
  }

  json doc = {{"asset", {{"version", "2.0"}, {"generator", "Veraa"}}},
              {"scene", 0},
              {"scenes", json::array({{{"nodes", roots}}})},
              {"nodes", gNodes}};
  if (!meshes.empty()) doc["meshes"] = meshes;
  if (!materials.empty()) doc["materials"] = materials;
  if (!images.empty()) doc["images"] = images;
  if (!textures.empty()) {
    doc["textures"] = textures;
    doc["samplers"] = json::array({{{"magFilter", 9729}, {"minFilter", 9987}}});
    for (auto& t : doc["textures"]) t["sampler"] = 0;
  }
  if (!cameras.empty()) doc["cameras"] = cameras;
  if (!lights.empty()) {
    doc["extensionsUsed"] = json::array({"KHR_lights_punctual"});
    doc["extensions"] = {{"KHR_lights_punctual", {{"lights", lights}}}};
  }
  while (blob.bytes.size() % 4) blob.bytes.push_back(0);
  if (!blob.bytes.empty()) {
    doc["bufferViews"] = blob.bufferViews;
    doc["accessors"] = blob.accessors;
    json buffer = {{"byteLength", blob.bytes.size()}};
    if (!glb) buffer["uri"] = io::baseNameOf(path) + ".bin";
    doc["buffers"] = json::array({buffer});
  }

  std::ofstream out(path, std::ios::binary);
  if (!out) {
    if (error) *error = "Cannot write " + path;
    return false;
  }
  std::string text = doc.dump(glb ? -1 : 2);
  if (glb) {
    while (text.size() % 4) text += ' ';
    uint32_t jsonLen = static_cast<uint32_t>(text.size()), binLen = static_cast<uint32_t>(blob.bytes.size());
    uint32_t total = 12 + 8 + jsonLen + (binLen ? 8 + binLen : 0);
    uint32_t header[3] = {0x46546C67u, 2u, total};
    out.write(reinterpret_cast<const char*>(header), 12);
    uint32_t jh[2] = {jsonLen, 0x4E4F534Au};
    out.write(reinterpret_cast<const char*>(jh), 8);
    out.write(text.data(), jsonLen);
    if (binLen) {
      uint32_t bh[2] = {binLen, 0x004E4942u};
      out.write(reinterpret_cast<const char*>(bh), 8);
      out.write(reinterpret_cast<const char*>(blob.bytes.data()), binLen);
    }
  } else {
    out << text;
    if (!blob.bytes.empty()) {
      std::ofstream bin(io::joinPath(dir, io::baseNameOf(path) + ".bin"), std::ios::binary);
      if (!bin) {
        if (error) *error = "Cannot write the .bin buffer next to " + path;
        return false;
      }
      bin.write(reinterpret_cast<const char*>(blob.bytes.data()), static_cast<std::streamsize>(blob.bytes.size()));
    }
  }
  if (!out) {
    if (error) *error = "Write error: " + path;
    return false;
  }
  return true;
}

}  // namespace veraa
