// FBX: import with ufbx (binary and ASCII, any version ufbx reads), export as ASCII FBX 7.4.
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <unordered_map>

#include "io_util.h"
#include "ufbx/ufbx.h"

namespace veraa {

namespace {

Mat4 toMat4(const ufbx_matrix& m) {
  Mat4 r;
  for (int row = 0; row < 3; ++row)
    for (int col = 0; col < 4; ++col) r.m[row][col] = static_cast<float>(m.cols[col].v[row]);
  r.m[3][0] = r.m[3][1] = r.m[3][2] = 0.0f;
  r.m[3][3] = 1.0f;
  return r;
}

Vec3 toVec3(const ufbx_vec3& v) { return Vec3(static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)); }

Vec3 axisVector(ufbx_coordinate_axis a) {
  switch (a) {
    case UFBX_COORDINATE_AXIS_POSITIVE_X: return {1, 0, 0};
    case UFBX_COORDINATE_AXIS_NEGATIVE_X: return {-1, 0, 0};
    case UFBX_COORDINATE_AXIS_POSITIVE_Y: return {0, 1, 0};
    case UFBX_COORDINATE_AXIS_NEGATIVE_Y: return {0, -1, 0};
    case UFBX_COORDINATE_AXIS_POSITIVE_Z: return {0, 0, 1};
    case UFBX_COORDINATE_AXIS_NEGATIVE_Z: return {0, 0, -1};
    default: return {1, 0, 0};
  }
}

std::string str(const ufbx_string& s) { return std::string(s.data, s.length); }

}  // namespace

bool importFbx(Scene& scene, const std::string& path, const ImportOptions& options, ImportResult* result,
               std::string* error) {
  ufbx_load_opts opts = {};
  if (options.convertAxes) opts.target_axes = ufbx_axes_right_handed_z_up;
  if (options.convertUnits) opts.target_unit_meters = 0.01f;  // centimeters
  opts.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
  opts.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_PRESERVE;
  ufbx_error err;
  ufbx_scene* fbx = ufbx_load_file(path.c_str(), &opts, &err);
  if (!fbx) {
    if (error) *error = "FBX: " + str(err.description);
    return false;
  }
  std::string dir = io::directoryOf(path);
  float s = options.scale;
  Mat4 scaleM = Mat4::scale(Vec3(s));

  // Materials.
  std::unordered_map<const ufbx_material*, uint32_t> materials;
  if (options.importMaterials)
    for (size_t i = 0; i < fbx->materials.count; ++i) {
      const ufbx_material* m = fbx->materials.data[i];
      Vec3 diffuse(0.6f);
      if (m->fbx.diffuse_color.has_value) diffuse = toVec3(m->fbx.diffuse_color.value_vec3);
      else if (m->pbr.base_color.has_value) diffuse = toVec3(m->pbr.base_color.value_vec3);
      if (m->fbx.diffuse_factor.has_value && m->fbx.diffuse_factor.value_real > 0)
        diffuse = diffuse * static_cast<float>(m->fbx.diffuse_factor.value_real);
      std::string tex;
      const ufbx_texture* t = m->fbx.diffuse_color.texture ? m->fbx.diffuse_color.texture : m->pbr.base_color.texture;
      if (t) {
        tex = str(t->filename);
        if (tex.empty()) tex = io::joinPath(dir, str(t->relative_filename));
      }
      float opacity = 1.0f;
      if (m->pbr.opacity.has_value) opacity = static_cast<float>(m->pbr.opacity.value_real);
      if (m->fbx.transparency_factor.has_value && m->fbx.transparency_factor.value_real > 0 &&
          m->fbx.transparency_factor.value_real <= 1)
        opacity = std::min(opacity, 1.0f - static_cast<float>(m->fbx.transparency_factor.value_real));
      uint32_t id = io::addMaterial(scene, str(m->name), diffuse, tex, std::clamp(opacity, 0.0f, 1.0f));
      if (Material* mat = scene.findMaterial(id)) {
        if (m->pbr.metalness.has_value) mat->params.setNumber("metalness", m->pbr.metalness.value_real);
        if (m->pbr.roughness.has_value && m->features.pbr.enabled) {
          mat->params.setColor("reflect", Vec3(1.0f));
          mat->params.setNumber("reflectGlossiness", 1.0 - m->pbr.roughness.value_real);
        }
      }
      materials[m] = id;
    }

  const ObjectTypeInfo* dummyType = findObjectType("Dummy");
  std::function<void(const ufbx_node*, uint32_t)> visit = [&](const ufbx_node* fn, uint32_t parentId) {
    uint32_t myId = parentId;
    if (!fn->is_root) {
      Node* n = nullptr;
      std::string name = str(fn->name);
      if (fn->mesh) {
        const ufbx_mesh* mesh = fn->mesh;
        PolyMesh pm;
        pm.positions.reserve(mesh->num_vertices);
        for (size_t v = 0; v < mesh->num_vertices; ++v) pm.positions.push_back(toVec3(mesh->vertices.data[v]));
        bool hasUv = mesh->vertex_uv.exists;
        if (hasUv)
          for (size_t u = 0; u < mesh->vertex_uv.values.count; ++u) {
            ufbx_vec2 uv = mesh->vertex_uv.values.data[u];
            pm.uvs.push_back(Vec2(static_cast<float>(uv.x), static_cast<float>(uv.y)));
          }
        for (size_t f = 0; f < mesh->num_faces; ++f) {
          ufbx_face face = mesh->faces.data[f];
          if (face.num_indices < 3) continue;
          std::vector<int> vi, ui;
          for (uint32_t k = 0; k < face.num_indices; ++k) {
            uint32_t idx = face.index_begin + k;
            vi.push_back(static_cast<int>(mesh->vertex_indices.data[idx]));
            if (hasUv) ui.push_back(static_cast<int>(mesh->vertex_uv.indices.data[idx]));
          }
          uint32_t smooth = 1;
          if (mesh->face_smoothing.count > f && !mesh->face_smoothing.data[f]) smooth = 0;
          int matId = mesh->face_material.count > f ? static_cast<int>(mesh->face_material.data[f]) : 0;
          pm.addFace(std::move(vi), std::move(ui), smooth, matId);
        }
        if (result) result->triangles += pm.triangleCount();
        n = io::addMeshNode(scene, name.empty() ? "Mesh" : name, std::move(pm));
        if (n && options.importMaterials && fn->materials.count > 0) {
          auto it = materials.find(fn->materials.data[0]);
          if (it != materials.end()) n->materialId = it->second;
        }
        if (n && fn->materials.count > 1 && result)
          result->warnings.push_back(name + ": uses several materials; the first one was assigned");
      } else if (fn->light && options.importLights) {
        const ufbx_light* l = fn->light;
        const char* type = l->type == UFBX_LIGHT_DIRECTIONAL ? "SunLight"
                           : l->type == UFBX_LIGHT_SPOT      ? "SpotLight"
                           : l->type == UFBX_LIGHT_AREA      ? "RectLight"
                                                             : "OmniLight";
        n = scene.createNode(type);
        if (n) {
          ParamBlock& p = n->object->params;
          p.setColor("color", toVec3(l->color));
          p.setNumber("intensity", l->intensity);
          if (l->type == UFBX_LIGHT_SPOT) {
            p.setNumber("hotspot", std::max(0.5, l->inner_angle));
            p.setNumber("falloff", std::max(1.0, l->outer_angle));
          }
          p.setNumber("castShadows", l->cast_shadows ? 1 : 0);
        }
      } else if (fn->camera && options.importCameras) {
        n = scene.createNode("Camera");
        if (n) n->object->params.setNumber("fov", fn->camera->field_of_view_deg.x);
      } else {
        n = dummyType ? scene.createNode("Dummy") : io::addMeshNode(scene, name.empty() ? "Group" : name, PolyMesh());
      }
      if (n) {
        if (!name.empty() && n->name != name && (fn->light || fn->camera || (!fn->mesh && dummyType))) n->name = io::uniqueName(scene, name);
        n->parentId = parentId;
        if (fn->light || fn->camera) {
          // Lights and cameras: Veraa aims them along local -Z, so rebuild the world placement.
          Mat4 world = scaleM * toMat4(fn->node_to_world);
          Vec3 dir = fn->light ? toVec3(ufbx_transform_direction(&fn->node_to_world, fn->light->local_direction))
                               : normalize(world.transformDir(axisVector(fn->camera->projection_axes.front)));
          Transform t;
          t.position = world.translation();
          if (length(dir) > 1e-6f) t.rotation = io::lookAlong(normalize(dir));
          n->setWorldMatrix(t.matrix());
        } else {
          Mat4 local = toMat4(fn->node_to_parent);
          if (parentId == 0) local = scaleM * local;
          n->transform = decomposeTransform(local);
          Mat4 geo = toMat4(fn->geometry_to_node);
          n->objectOffset = decomposeTransform(geo);
        }
        n->invalidate();
        myId = n->id;
        if (result) result->nodeIds.push_back(n->id);
      }
    }
    for (size_t c = 0; c < fn->children.count; ++c) visit(fn->children.data[c], myId);
  };
  visit(fbx->root_node, 0);
  ufbx_free_scene(fbx);
  return true;
}

// ---------------------------------------------------------------------------------------------
// ASCII FBX 7.4 writer

namespace {

std::string num(double v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.9g", std::fabs(v) < 1e-12 ? 0.0 : v);
  return buf;
}

std::string quote(const std::string& s) {
  std::string out;
  for (char c : s) out += c == '"' ? '\'' : c;
  return "\"" + out + "\"";
}

template <typename T, typename F>
void writeArray(std::ostream& o, const char* name, const std::vector<T>& values, const char* indent, F fmt) {
  o << indent << name << ": *" << values.size() << " {\n" << indent << "\ta: ";
  for (size_t i = 0; i < values.size(); ++i) {
    if (i) o << (i % 16 == 0 ? ",\n" + std::string(indent) + "\t" : ",");
    o << fmt(values[i]);
  }
  o << "\n" << indent << "}\n";
}

void prop(std::ostream& o, const char* name, const char* type, const char* type2, const char* flags, const Vec3& v) {
  o << "\t\t\tP: \"" << name << "\", \"" << type << "\", \"" << type2 << "\", \"" << flags << "\"," << num(v.x) << ","
    << num(v.y) << "," << num(v.z) << "\n";
}

}  // namespace

bool exportFbx(const Scene& scene, const std::string& path, const ExportOptions& options, std::string* error) {
  std::vector<const Node*> nodes = io::exportNodes(scene, options);
  std::ostringstream obj, conn;
  int64_t nextId = 1000000;
  std::unordered_map<uint32_t, int64_t> modelIds;
  std::map<uint32_t, int64_t> materialIds;
  std::map<std::string, int64_t> textureIds;
  int models = 0, geometries = 0, mats = 0, textures = 0;
  std::string dir = io::directoryOf(path);
  float s = options.scale;

  for (const Node* n : nodes) modelIds[n->id] = nextId++;

  for (const Node* n : nodes) {
    int64_t mid = modelIds[n->id];
    bool isMesh = n->kind() == ObjectKind::Geometry && !n->mesh().empty();
    ++models;
    // Geometry.
    if (isMesh) {
      PolyMesh m = n->mesh();
      int64_t gid = nextId++;
      ++geometries;
      std::vector<double> verts;
      for (const Vec3& p : m.positions) verts.insert(verts.end(), {p.x, p.y, p.z});
      std::vector<int> index;
      std::vector<double> uv;
      std::vector<int> uvIndex;
      bool hasUv = !m.uvs.empty();
      for (const auto& f : m.faces) {
        for (size_t k = 0; k < f.v.size(); ++k) {
          int vi = f.v[k];
          index.push_back(k + 1 == f.v.size() ? -vi - 1 : vi);
          if (hasUv) uvIndex.push_back(k < f.uv.size() ? f.uv[k] : 0);
        }
      }
      for (const Vec2& t : m.uvs) uv.insert(uv.end(), {t.x, t.y});
      obj << "\tGeometry: " << gid << ", " << quote("Geometry::" + n->name) << ", \"Mesh\" {\n";
      writeArray(obj, "Vertices", verts, "\t\t", [](double v) { return num(v); });
      writeArray(obj, "PolygonVertexIndex", index, "\t\t", [](int v) { return std::to_string(v); });
      obj << "\t\tGeometryVersion: 124\n";
      if (hasUv) {
        obj << "\t\tLayerElementUV: 0 {\n\t\t\tVersion: 101\n\t\t\tName: \"UVChannel_1\"\n"
               "\t\t\tMappingInformationType: \"ByPolygonVertex\"\n\t\t\tReferenceInformationType: \"IndexToDirect\"\n";
        writeArray(obj, "UV", uv, "\t\t\t", [](double v) { return num(v); });
        writeArray(obj, "UVIndex", uvIndex, "\t\t\t", [](int v) { return std::to_string(v); });
        obj << "\t\t}\n";
      }
      // Smoothing: one flag per polygon (smoothed if any smoothing group).
      std::vector<int> smoothing;
      for (const auto& f : m.faces) smoothing.push_back(f.smoothGroups ? 1 : 0);
      obj << "\t\tLayerElementSmoothing: 0 {\n\t\t\tVersion: 102\n\t\t\tName: \"\"\n"
             "\t\t\tMappingInformationType: \"ByPolygon\"\n\t\t\tReferenceInformationType: \"Direct\"\n";
      writeArray(obj, "Smoothing", smoothing, "\t\t\t", [](int v) { return std::to_string(v); });
      obj << "\t\t}\n";
      obj << "\t\tLayerElementMaterial: 0 {\n\t\t\tVersion: 101\n\t\t\tName: \"\"\n"
             "\t\t\tMappingInformationType: \"AllSame\"\n\t\t\tReferenceInformationType: \"IndexToDirect\"\n"
             "\t\t\tMaterials: *1 {\n\t\t\t\ta: 0\n\t\t\t}\n\t\t}\n";
      obj << "\t\tLayer: 0 {\n\t\t\tVersion: 100\n";
      if (hasUv) obj << "\t\t\tLayerElement:  {\n\t\t\t\tType: \"LayerElementUV\"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n";
      obj << "\t\t\tLayerElement:  {\n\t\t\t\tType: \"LayerElementSmoothing\"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n";
      obj << "\t\t\tLayerElement:  {\n\t\t\t\tType: \"LayerElementMaterial\"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n";
      obj << "\t\t}\n\t}\n";
      conn << "\tC: \"OO\"," << gid << "," << mid << "\n";
    }
    // Model.
    Transform t = n->transform;
    bool root = !n->parent() || !modelIds.count(n->parentId);
    if (root) {
      // Roots of the export are placed in world space.
      t = decomposeTransform(Mat4::scale(Vec3(s)) * n->worldMatrix());
    }
    obj << "\tModel: " << mid << ", " << quote("Model::" + n->name) << ", " << (isMesh ? "\"Mesh\"" : "\"Null\"") << " {\n";
    obj << "\t\tVersion: 232\n\t\tProperties70:  {\n";
    obj << "\t\t\tP: \"RotationOrder\", \"enum\", \"\", \"\",0\n";
    obj << "\t\t\tP: \"InheritType\", \"enum\", \"\", \"\",1\n";
    prop(obj, "Lcl Translation", "Lcl Translation", "", "A", t.position);
    prop(obj, "Lcl Rotation", "Lcl Rotation", "", "A", t.rotation);
    prop(obj, "Lcl Scaling", "Lcl Scaling", "", "A", t.scale);
    if (isMesh && !n->objectOffset.isIdentity()) {
      prop(obj, "GeometricTranslation", "Vector3D", "Vector", "", n->objectOffset.position);
      prop(obj, "GeometricRotation", "Vector3D", "Vector", "", n->objectOffset.rotation);
      prop(obj, "GeometricScaling", "Vector3D", "Vector", "", n->objectOffset.scale);
    }
    obj << "\t\t\tP: \"Visibility\", \"Visibility\", \"\", \"A\"," << (n->hidden ? 0 : 1) << "\n";
    obj << "\t\t}\n\t\tShading: T\n\t\tCulling: \"CullingOff\"\n\t}\n";
    conn << "\tC: \"OO\"," << mid << "," << (root ? 0 : modelIds[n->parentId]) << "\n";

    // Material.
    if (isMesh && options.exportMaterials) {
      const Material* mat = scene.findMaterial(n->materialId);
      uint32_t key = mat ? mat->id : 0;
      if (!materialIds.count(key)) {
        int64_t matId = nextId++;
        materialIds[key] = matId;
        ++mats;
        Vec3 diffuse = mat ? mat->params.getColor("diffuse") : n->wireColor;
        float opacity = mat ? mat->params.getFloat("opacity") : 1.0f;
        std::string name = mat ? mat->name : n->name + "_color";
        obj << "\tMaterial: " << matId << ", " << quote("Material::" + name) << ", \"\" {\n";
        obj << "\t\tVersion: 102\n\t\tShadingModel: \"phong\"\n\t\tMultiLayer: 0\n\t\tProperties70:  {\n";
        prop(obj, "DiffuseColor", "Color", "", "A", diffuse);
        obj << "\t\t\tP: \"DiffuseFactor\", \"Number\", \"\", \"A\",1\n";
        obj << "\t\t\tP: \"TransparencyFactor\", \"Number\", \"\", \"A\"," << num(1.0 - opacity) << "\n";
        if (mat) {
          Vec3 refl = mat->params.getColor("reflect");
          prop(obj, "SpecularColor", "Color", "", "A", refl);
          obj << "\t\t\tP: \"Shininess\", \"Number\", \"\", \"A\","
              << num(2.0 + 98.0 * mat->params.getFloat("reflectGlossiness")) << "\n";
        }
        obj << "\t\t}\n\t}\n";
        std::string map = mat ? mat->params.getString("diffuseMap") : std::string();
        if (!map.empty()) {
          std::string abs = scene.resolvePath(map);
          int64_t tid;
          if (textureIds.count(abs)) {
            tid = textureIds[abs];
          } else {
            tid = nextId++;
            textureIds[abs] = tid;
            ++textures;
            obj << "\tTexture: " << tid << ", " << quote("Texture::" + io::fileNameOf(abs)) << ", \"\" {\n";
            obj << "\t\tType: \"TextureVideoClip\"\n\t\tVersion: 202\n";
            obj << "\t\tTextureName: " << quote("Texture::" + io::fileNameOf(abs)) << "\n";
            obj << "\t\tFileName: " << quote(abs) << "\n";
            obj << "\t\tRelativeFilename: " << quote(io::relativePath(dir, abs)) << "\n\t}\n";
          }
          conn << "\tC: \"OP\"," << tid << "," << matId << ", \"DiffuseColor\"\n";
        }
      }
      conn << "\tC: \"OO\"," << materialIds[key] << "," << mid << "\n";
    }
  }

  std::ofstream out(path, std::ios::binary);
  if (!out) {
    if (error) *error = "Cannot write " + path;
    return false;
  }
  out << "; FBX 7.4.0 project file\n; Written by Veraa\n\n";
  out << "FBXHeaderExtension:  {\n\tFBXHeaderVersion: 1003\n\tFBXVersion: 7400\n\tCreator: \"Veraa\"\n}\n";
  out << "GlobalSettings:  {\n\tVersion: 1000\n\tProperties70:  {\n"
         "\t\tP: \"UpAxis\", \"int\", \"Integer\", \"\",2\n"
         "\t\tP: \"UpAxisSign\", \"int\", \"Integer\", \"\",1\n"
         "\t\tP: \"FrontAxis\", \"int\", \"Integer\", \"\",1\n"
         "\t\tP: \"FrontAxisSign\", \"int\", \"Integer\", \"\",-1\n"
         "\t\tP: \"CoordAxis\", \"int\", \"Integer\", \"\",0\n"
         "\t\tP: \"CoordAxisSign\", \"int\", \"Integer\", \"\",1\n"
         "\t\tP: \"OriginalUpAxis\", \"int\", \"Integer\", \"\",2\n"
         "\t\tP: \"OriginalUpAxisSign\", \"int\", \"Integer\", \"\",1\n"
         "\t\tP: \"UnitScaleFactor\", \"double\", \"Number\", \"\",1\n"
         "\t\tP: \"OriginalUnitScaleFactor\", \"double\", \"Number\", \"\",1\n"
         "\t}\n}\n";
  out << "Definitions:  {\n\tVersion: 100\n\tCount: " << (1 + models + geometries + mats + textures) << "\n";
  out << "\tObjectType: \"GlobalSettings\" {\n\t\tCount: 1\n\t}\n";
  out << "\tObjectType: \"Model\" {\n\t\tCount: " << models << "\n\t}\n";
  if (geometries) out << "\tObjectType: \"Geometry\" {\n\t\tCount: " << geometries << "\n\t}\n";
  if (mats) out << "\tObjectType: \"Material\" {\n\t\tCount: " << mats << "\n\t}\n";
  if (textures) out << "\tObjectType: \"Texture\" {\n\t\tCount: " << textures << "\n\t}\n";
  out << "}\n";
  out << "Objects:  {\n" << obj.str() << "}\n";
  out << "Connections:  {\n" << conn.str() << "}\n";
  if (!out) {
    if (error) *error = "Write error: " + path;
    return false;
  }
  return true;
}

}  // namespace veraa
