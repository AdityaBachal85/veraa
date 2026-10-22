// Turns a Scene into render data: Embree geometry (world-space meshes, instances for shared
// meshes, proxies and moving objects, fur curves), infinite planes, compiled materials, lights
// and the environment.
#include <algorithm>
#include <cstring>
#include <numeric>
#include <sstream>

#include "motionhook.h"
#include "render_impl.h"
#include "renderobjects.h"
#include "rendersettings.h"
#include "scene.h"

namespace veraa {

using namespace render;

namespace {

RTCDevice embreeDevice() {
  static RTCDevice device = rtcNewDevice(nullptr);
  return device;
}

uint64_t quantKey(const Vec3& p) {
  auto q = [](float v) { return static_cast<uint64_t>(static_cast<int64_t>(std::lround(v * 1e4f)) & 0x1FFFFF); };
  return (q(p.x) << 42) | (q(p.y) << 21) | q(p.z);
}

int findRoot(std::vector<int>& parent, int i) {
  while (parent[i] != i) {
    parent[i] = parent[parent[i]];
    i = parent[i];
  }
  return i;
}

void computeElements(MeshData& md) {
  size_t nv = md.P.size(), nt = md.triangles();
  std::vector<int> parent(nv);
  std::iota(parent.begin(), parent.end(), 0);
  std::unordered_map<uint64_t, int> firstAt;
  for (size_t i = 0; i < nv; ++i) {
    auto it = firstAt.emplace(quantKey(md.P[i]), static_cast<int>(i));
    if (!it.second) parent[findRoot(parent, static_cast<int>(i))] = findRoot(parent, it.first->second);
  }
  for (size_t t = 0; t < nt; ++t) {
    int a = findRoot(parent, static_cast<int>(md.idx[t * 3]));
    for (int k = 1; k < 3; ++k) {
      int b = findRoot(parent, static_cast<int>(md.idx[t * 3 + k]));
      if (a != b) parent[b] = a;
    }
  }
  std::unordered_map<int, int> ids;
  md.elements.resize(nt);
  for (size_t t = 0; t < nt; ++t) {
    int r = findRoot(parent, static_cast<int>(md.idx[t * 3]));
    auto it = ids.emplace(r, static_cast<int>(ids.size()));
    md.elements[t] = it.first->second;
  }
}

void computeEdgeMasks(MeshData& md, const TriMesh& tm) {
  std::set<std::pair<uint64_t, uint64_t>> edges;
  for (size_t i = 0; i + 1 < tm.wireIndices.size(); i += 2) {
    uint64_t a = quantKey(tm.wirePositions[tm.wireIndices[i]]), b = quantKey(tm.wirePositions[tm.wireIndices[i + 1]]);
    edges.insert({std::min(a, b), std::max(a, b)});
  }
  size_t nt = md.triangles();
  md.edgeMask.assign(nt, 0);
  for (size_t t = 0; t < nt; ++t)
    for (int k = 0; k < 3; ++k) {
      uint64_t a = quantKey(md.P[md.idx[t * 3 + k]]), b = quantKey(md.P[md.idx[t * 3 + (k + 1) % 3]]);
      if (edges.count({std::min(a, b), std::max(a, b)}) || edges.empty()) md.edgeMask[t] |= static_cast<uint8_t>(1u << k);
    }
}

MeshData meshFromTri(const TriMesh& tm) {
  MeshData md;
  md.P = tm.positions;
  md.N = tm.normals;
  md.UV = tm.uvs;
  if (md.UV.size() != md.P.size()) md.UV.assign(md.P.size(), Vec2());
  if (md.N.size() != md.P.size()) md.N.assign(md.P.size(), Vec3(0, 0, 1));
  md.idx = tm.indices;
  md.matIds = tm.triMatIds;
  if (md.matIds.size() != md.triangles()) md.matIds.assign(md.triangles(), 0);
  return md;
}

// Smooth vertex normals that agree across duplicated (seam) vertices.
void recomputeNormals(MeshData& md) {
  std::unordered_map<uint64_t, Vec3> acc;
  size_t nt = md.triangles();
  for (size_t t = 0; t < nt; ++t) {
    const Vec3 &a = md.P[md.idx[t * 3]], &b = md.P[md.idx[t * 3 + 1]], &c = md.P[md.idx[t * 3 + 2]];
    Vec3 n = cross(b - a, c - a);  // area weighted
    for (int k = 0; k < 3; ++k) acc[quantKey(md.P[md.idx[t * 3 + k]])] += n;
  }
  for (size_t i = 0; i < md.P.size(); ++i) {
    Vec3 n = acc[quantKey(md.P[i])];
    md.N[i] = length2(n) > 1e-30f ? normalize(n) : Vec3(0, 0, 1);
  }
}

struct DisplacementSpec {
  const MapNode* map = nullptr;
  MapPtr owned;
  float amount = 0.0f, shift = 0.0f, edgeLength = 1.0f;
  int maxLevels = 6;
  bool continuity = true;
  bool water = false;
  float waterLevel = 0.0f;
};

// Midpoint subdivision until edges are short enough, then moves vertices along the normal.
void displaceMesh(MeshData& md, const DisplacementSpec& spec, const Mat4& objToWorld, Rng& rng) {
  float scale = std::cbrt(std::max(std::fabs(objToWorld.determinant3x3()), 1e-12f));
  float maxEdge = 0.0f;
  size_t nt = md.triangles();
  for (size_t t = 0; t < nt; ++t)
    for (int k = 0; k < 3; ++k)
      maxEdge = std::max(maxEdge, length(md.P[md.idx[t * 3 + k]] - md.P[md.idx[t * 3 + (k + 1) % 3]]) * scale);
  int levels = 0;
  while (levels < spec.maxLevels && maxEdge / static_cast<float>(1 << levels) > spec.edgeLength &&
         nt * (size_t(1) << (2 * (levels + 1))) <= 8000000)
    ++levels;
  for (int l = 0; l < levels; ++l) {
    std::unordered_map<uint64_t, uint32_t> mid;
    std::vector<uint32_t> idx;
    std::vector<int> mats;
    idx.reserve(md.idx.size() * 4);
    mats.reserve(md.matIds.size() * 4);
    auto midpoint = [&](uint32_t a, uint32_t b) {
      uint64_t key = (static_cast<uint64_t>(std::min(a, b)) << 32) | std::max(a, b);
      auto it = mid.find(key);
      if (it != mid.end()) return it->second;
      uint32_t i = static_cast<uint32_t>(md.P.size());
      md.P.push_back((md.P[a] + md.P[b]) * 0.5f);
      Vec3 n = md.N[a] + md.N[b];
      md.N.push_back(length2(n) > 1e-20f ? normalize(n) : md.N[a]);
      md.UV.push_back((md.UV[a] + md.UV[b]) * 0.5f);
      mid.emplace(key, i);
      return i;
    };
    size_t count = md.triangles();
    for (size_t t = 0; t < count; ++t) {
      uint32_t a = md.idx[t * 3], b = md.idx[t * 3 + 1], c = md.idx[t * 3 + 2];
      uint32_t ab = midpoint(a, b), bc = midpoint(b, c), ca = midpoint(c, a);
      uint32_t tris[4][3] = {{a, ab, ca}, {ab, b, bc}, {ca, bc, c}, {ab, bc, ca}};
      for (auto& tr : tris) {
        idx.insert(idx.end(), {tr[0], tr[1], tr[2]});
        mats.push_back(md.matIds[t]);
      }
    }
    md.idx = std::move(idx);
    md.matIds = std::move(mats);
  }
  // Displacement direction: the averaged normal of coincident vertices keeps seams closed.
  std::vector<Vec3> dir = md.N;
  if (spec.continuity) {
    std::unordered_map<uint64_t, Vec3> acc;
    for (size_t i = 0; i < md.P.size(); ++i) acc[quantKey(md.P[i])] += md.N[i];
    for (size_t i = 0; i < md.P.size(); ++i) {
      Vec3 n = acc[quantKey(md.P[i])];
      dir[i] = length2(n) > 1e-20f ? normalize(n) : md.N[i];
    }
  }
  for (size_t i = 0; i < md.P.size(); ++i) {
    ShadeContext sc;
    sc.pObj = md.P[i];
    sc.p = objToWorld.transformPoint(md.P[i]);
    sc.uv = md.UV[i];
    sc.ns = sc.ng = sc.nObj = dir[i];
    sc.wo = dir[i];
    sc.rng = &rng;
    float v = spec.map ? spec.map->value(sc) : 0.0f;
    if (spec.water) v = std::max(v, spec.waterLevel);
    float hgt = (spec.amount * v + spec.shift) / scale;
    md.P[i] += dir[i] * hgt;
  }
  recomputeNormals(md);
}

RTCGeometry makeTriangles(RTCDevice device, const MeshData& md) {
  size_t nv = md.P.size(), nt = md.triangles();
  RTCGeometry geom = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_TRIANGLE);
  auto* vb = static_cast<float*>(
      rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, 3 * sizeof(float), nv));
  auto* ib = static_cast<unsigned*>(
      rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, 3 * sizeof(unsigned), nt));
  if (!vb || !ib) {
    rtcReleaseGeometry(geom);
    return nullptr;
  }
  for (size_t i = 0; i < nv; ++i) {
    vb[i * 3] = md.P[i].x;
    vb[i * 3 + 1] = md.P[i].y;
    vb[i * 3 + 2] = md.P[i].z;
  }
  std::memcpy(ib, md.idx.data(), nt * 3 * sizeof(unsigned));
  rtcCommitGeometry(geom);
  return geom;
}

void toRowMajor3x4(const Mat4& m, float out[12]) {
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 4; ++c) out[r * 4 + c] = m.m[r][c];
}

bool sameMatrix(const Mat4& a, const Mat4& b) {
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c)
      if (std::fabs(a.m[r][c] - b.m[r][c]) > 1e-6f * (1.0f + std::fabs(a.m[r][c]))) return false;
  return true;
}

std::vector<uint32_t> parseIds(const std::string& s) {
  std::vector<uint32_t> out;
  std::string tok;
  std::istringstream in(s);
  while (std::getline(in, tok, ',')) {
    try {
      if (!tok.empty()) out.push_back(static_cast<uint32_t>(std::stoul(tok)));
    } catch (...) {
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Distance fields (kd-tree of surface sample points)

float DistanceField::query(const Vec3& p, float maxDist) const {
  float best2 = maxDist * maxDist;
  int r = static_cast<int>(std::ceil(maxDist / cell));
  r = std::min(r, 6);
  int cx = static_cast<int>(std::floor((p.x - origin.x) / cell)), cy = static_cast<int>(std::floor((p.y - origin.y) / cell)),
      cz = static_cast<int>(std::floor((p.z - origin.z) / cell));
  for (int dz = -r; dz <= r; ++dz)
    for (int dy = -r; dy <= r; ++dy)
      for (int dx = -r; dx <= r; ++dx) {
        uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(cx + dx) & 0x1FFFFF) << 42) |
                       (static_cast<uint64_t>(static_cast<uint32_t>(cy + dy) & 0x1FFFFF) << 21) |
                       (static_cast<uint64_t>(static_cast<uint32_t>(cz + dz) & 0x1FFFFF));
        auto it = cells.find(key);
        if (it == cells.end()) continue;
        for (const Vec3& q : it->second) best2 = std::min(best2, length2(q - p));
      }
  return std::sqrt(best2);
}

// ---------------------------------------------------------------------------------------------

Renderer::Impl::~Impl() {
  stop();
  releaseScene();
}

void Renderer::Impl::releaseScene() {
  if (rtc) rtcReleaseScene(rtc);
  rtc = nullptr;
  for (auto& m : meshes)
    if (m.child) rtcReleaseScene(m.child);
  meshes.clear();
  curves.clear();
  records.clear();
  planes.clear();
}

void Renderer::Impl::stop() {
  stopFlag = true;
  if (controller.joinable()) controller.join();
}

bool Renderer::Impl::build(const Scene& scene, std::string* error) {
  releaseScene();
  warnings.clear();
  materials.clear();
  lights.clear();
  areaLights.clear();
  portals.clear();
  distanceFields.clear();
  triangles = 0;
  anyShadowTransparent = false;
  anyLightLinks = false;

  RTCDevice device = embreeDevice();
  if (!device) {
    if (error) *error = "Could not start the Embree ray tracing device (CPU not supported?)";
    return false;
  }
  masksSupported = rtcGetDeviceProperty(device, RTC_DEVICE_PROPERTY_RAY_MASK_SUPPORTED) != 0;

  // ---- Settings ----
  const ParamBlock& rs = scene.renderSettings;
  sceneW = rs.getInt("width");
  sceneH = rs.getInt("height");
  sceneSpp = rs.getInt("samples");
  sceneTime = rs.getFloat("timeLimit");
  maxDepth = rs.getInt("maxDepth");
  clampIndirect = rs.getFloat("clampIndirect");
  clampDirect = rs.getFloat("clampDirect");
  giMultiplier = rs.getFloat("giMultiplier");
  caustics = rs.getBool("caustics");
  samplerType = rs.getInt("samplerType");
  minSamples = rs.getInt("minSamples");
  noiseThreshold = rs.getFloat("noiseThreshold");
  bucketSize = rs.getInt("bucketSize");
  sceneRegion = rs.getBool("renderRegion");
  sceneRegionBox[0] = rs.getFloat("regionX0");
  sceneRegionBox[1] = rs.getFloat("regionY0");
  sceneRegionBox[2] = rs.getFloat("regionX1");
  sceneRegionBox[3] = rs.getFloat("regionY1");
  fogOn = rs.getBool("fogOn");
  fogType = rs.getInt("fogType");
  fogAlbedo = clampColor(rs.getColor("fogColor"), 0.0f, 1.0f);
  fogDensity = 1.0f / std::max(rs.getFloat("fogDistance"), 1e-3f);
  fogHeight = std::max(rs.getFloat("fogHeight"), 1e-3f);
  fogFalloff = std::max(rs.getFloat("fogFalloff"), 1e-3f);
  fogG = clampf(rs.getFloat("fogAnisotropy"), -0.95f, 0.95f);
  fogAffectBackground = rs.getBool("fogAffectBackground");
  fogEmission = rs.getColor("fogEmission");
  physicalExposure = rs.getInt("exposureControl") == 1 ? exposureScale(rs.getFloat("ev100")) : 1.0f;
  {
    Vec3 wb = kelvinToRgb(rs.getFloat("whiteBalanceK"));
    settingsWhiteBalance = Vec3(1.0f / std::max(wb.x, 1e-3f), 1.0f / std::max(wb.y, 1e-3f), 1.0f / std::max(wb.z, 1e-3f));
    if (rs.getInt("exposureControl") != 1) settingsWhiteBalance = Vec3(1.0f);
  }
  zMin = rs.getFloat("zDepthMin");
  zMax = rs.getFloat("zDepthMax");
  const auto& elems = renderElementInfos();
  anyBeautyElement = false;
  for (size_t i = 0; i < elems.size() && i < 32; ++i) {
    elemEnabled[i] = rs.getBool(std::string("elem") + elems[i].key);
    if (elemEnabled[i] && elems[i].sumsToBeauty) anyBeautyElement = true;
  }
  lightMixOn = rs.getBool("lightMix");

  // ---- Materials ----
  MaterialCompileContext mctx;
  mctx.maps.scene = &scene;
  mctx.maps.cache = textures.get();
  mctx.maps.warnings = &warnings;
  mctx.exposureCompensation = 1.0f / std::max(physicalExposure, 1e-12f);
  std::unordered_map<uint32_t, int> matIndex;
  for (const auto& m : scene.materials) {
    matIndex[m->id] = static_cast<int>(materials.size());
    materials.push_back(compileMaterial(*m, mctx));
  }
  int blackMaterial = static_cast<int>(materials.size());
  materials.push_back(compileColorMaterial(Vec3(0.0f)));
  std::map<std::array<float, 3>, int> wireMaterials;
  auto materialFor = [&](const Node& n) {
    auto it = matIndex.find(n.materialId);
    if (it != matIndex.end()) return it->second;
    std::array<float, 3> key{n.wireColor.x, n.wireColor.y, n.wireColor.z};
    auto w = wireMaterials.find(key);
    if (w != wireMaterials.end()) return w->second;
    int idx = static_cast<int>(materials.size());
    materials.push_back(compileColorMaterial(n.wireColor));
    wireMaterials[key] = idx;
    return idx;
  };

  // ---- Lights ----
  LightBuildResult lb = collectLights(scene, *textures, &warnings);
  lights = std::move(lb.lights);
  groupNames = lb.groupNames;
  envGroup = 0;
  if (lb.hasDome) {
    env.buildDome(scene, *textures, lb, &warnings);
    if (lb.domeGroup >= 0) envGroup = lb.domeGroup;
    separateBackground = lb.domeInvisible;
    if (separateBackground) envBack.build(scene, *textures, lb, &warnings);
  } else {
    env.build(scene, *textures, lb, &warnings);
    separateBackground = false;
  }
  if (rs.getInt("envMode") == 3 && !lb.physicalSun) {
    float turbidity = rs.getFloat("skyTurbidity");
    for (auto& l : lights)
      if (l.type == Light::Type::Sun) l.color *= sunAtmosphereTint(std::asin(clampf(l.dir.z, -1.0f, 1.0f)), turbidity);
  }
  if (lb.physicalSun && rs.getInt("exposureControl") == 0) {
    bool cameraExposure = false;
    if (const Node* camNode = scene.findNodeByName(rs.getString("camera")))
      cameraExposure = camNode->object && camNode->object->type() == "VeraaPhysicalCamera" &&
                       camNode->object->params.getBool("exposure");
    if (!cameraExposure)
      warnings.push_back(
          "The physical sun is in real-world units: enable Physical exposure (EV100 about 14) in Environment and "
          "Effects or use a VeraaPhysicalCamera with exposure, or the image will be overexposed");
  }
  std::vector<uint32_t> hiddenNodes = lb.hiddenByMeshLights;
  std::sort(hiddenNodes.begin(), hiddenNodes.end());
  std::vector<uint32_t> selectedOnly = parseIds(rs.getString("renderSelected"));

  RTCScene top = rtcNewScene(device);
  rtcSetSceneFlags(top, RTC_SCENE_FLAG_ROBUST);
  rtcSetSceneBuildQuality(top, RTC_BUILD_QUALITY_HIGH);
  rtc = top;

  auto shadowMaskFor = [&](int material, bool castShadows) -> unsigned {
    if (!castShadows) return 0u;
    if (materials[material]->shadowTransparent()) {
      anyShadowTransparent = true;
      return kMaskShadowTransp;
    }
    return kMaskShadowOpaque;
  };
  auto attachRecord = [&](RTCGeometry geom, GeomRecord rec) -> bool {
    if (!geom) return false;
    rtcSetGeometryMask(geom, rec.mask);
    rtcCommitGeometry(geom);
    unsigned id = rtcAttachGeometry(top, geom);
    rtcReleaseGeometry(geom);
    if (records.size() <= id) records.resize(id + 1);
    records[id] = std::move(rec);
    return true;
  };

  // Shared meshes become instances; everything else is flattened into world space.
  struct Candidate {
    const Node* node;
    std::string key;
  };
  std::vector<Candidate> candidates;
  std::map<std::string, int> keyCount;
  double shutterOpen = view.shutterOpen, shutterClose = view.shutterClose;
  bool motionBlur = view.motionBlur && shutterClose > shutterOpen;
  for (const auto& np : scene.nodes) {
    const Node& node = *np;
    ObjectKind kind = node.kind();
    if (kind != ObjectKind::Geometry && kind != ObjectKind::Shape) continue;
    if (!scene.isNodeRenderable(node)) continue;
    if (std::binary_search(hiddenNodes.begin(), hiddenNodes.end(), node.id)) continue;
    if (!selectedOnly.empty() && !std::binary_search(selectedOnly.begin(), selectedOnly.end(), node.id)) continue;
    if (node.isShapeResult() && !(node.object && node.object->params.getBool("renderInRenderer"))) continue;
    std::ostringstream key;
    std::string type = node.object ? node.object->type() : std::string();
    if (type == "VeraaProxy") {
      key << "proxy:" << scene.resolvePath(node.object->params.getString("file"));
    } else {
      key << "obj:" << static_cast<const void*>(node.object.get());
      for (const auto& m : node.modifiers) key << ',' << static_cast<const void*>(m.get());
    }
    key << "|m" << node.materialId;
    candidates.push_back({&node, key.str()});
    keyCount[key.str()]++;
  }

  std::map<std::string, int> sharedMesh;  // key -> meshes index (object space)
  uint32_t instanceCounter = 0;
  Rng dispRng(17, 5);
  MapCompileContext dispCtx = mctx.maps;
  for (const auto& cand : candidates) {
    const Node& node = *cand.node;
    std::string type = node.object ? node.object->type() : std::string();
    int material = materialFor(node);
    unsigned mask = kMaskGI | shadowMaskFor(material, node.castShadows);
    if (node.visibleToCamera) mask |= kMaskCamera;
    if (node.visibleToReflections) mask |= kMaskSecondary;

    GeomRecord base;
    base.material = material;
    base.nodeId = node.id;
    base.gBufferId = node.gBufferId;
    base.sceneMaterialId = node.materialId;
    base.wireColor = node.wireColor;
    base.receiveShadows = node.receiveShadows;
    base.mask = mask;

    if (type == "VeraaInfinitePlane") {
      Mat4 M = node.objectWorldMatrix();
      InfinitePlane pl;
      pl.p = M.translation();
      pl.n = normalize(M.inverse().transformNormalWithInverse(Vec3(0, 0, 1)));
      pl.tu = normalize(M.transformDir(Vec3(1, 0, 0)));
      pl.tv = normalize(cross(pl.n, pl.tu));
      pl.uvScale = std::max(node.object->params.getFloat("uvSize"), 1e-3f);
      pl.material = material;
      pl.nodeId = node.id;
      pl.sceneMaterialId = node.materialId;
      pl.gBufferId = node.gBufferId;
      pl.wireColor = node.wireColor;
      pl.mask = mask;
      pl.receiveShadows = node.receiveShadows;
      planes.push_back(pl);
      continue;
    }

    if (type == "VeraaFur") {
      const Node* src = scene.findNodeByName(node.object->params.getString("sourceNode"));
      if (!src || src->kind() != ObjectKind::Geometry) {
        warnings.push_back("VeraaFur " + node.name + " has no valid source object");
        continue;
      }
      TriMesh world = src->triMesh();
      Mat4 SM = src->objectWorldMatrix(), SMi = SM.inverse();
      for (auto& p : world.positions) p = SM.transformPoint(p);
      for (auto& n : world.normals) n = normalize(SMi.transformNormalWithInverse(n));
      std::vector<FurStrand> strands = generateFur(node.object->params, world, 0);
      if (strands.empty()) continue;
      CurveData cd;
      for (size_t s = 0; s < strands.size(); ++s) {
        const FurStrand& st = strands[s];
        uint32_t first = static_cast<uint32_t>(cd.v.size() / 4);
        size_t np = st.points.size();
        for (size_t k = 0; k < np; ++k) {
          float t = np > 1 ? static_cast<float>(k) / (np - 1) : 0.0f;
          const Vec3& p = st.points[k];
          cd.v.insert(cd.v.end(), {p.x, p.y, p.z, st.rootRadius + (st.tipRadius - st.rootRadius) * t});
        }
        for (size_t k = 0; k + 1 < np; ++k) {
          cd.segments.push_back(first + static_cast<uint32_t>(k));
          uint8_t f = 0;
          if (k > 0) f |= RTC_CURVE_FLAG_NEIGHBOR_LEFT;
          if (k + 2 < np) f |= RTC_CURVE_FLAG_NEIGHBOR_RIGHT;
          cd.flags.push_back(f);
          cd.strandOfSegment.push_back(static_cast<int>(s));
        }
        cd.rootUv.push_back(st.rootUv);
      }
      RTCGeometry geom = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_ROUND_LINEAR_CURVE);
      size_t nv = cd.v.size() / 4, ns = cd.segments.size();
      auto* vb = static_cast<float*>(
          rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT4, 4 * sizeof(float), nv));
      auto* ib = static_cast<unsigned*>(
          rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT, sizeof(unsigned), ns));
      auto* fb = static_cast<uint8_t*>(
          rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_FLAGS, 0, RTC_FORMAT_UCHAR, sizeof(uint8_t), ns));
      if (!vb || !ib || !fb) {
        rtcReleaseGeometry(geom);
        continue;
      }
      std::memcpy(vb, cd.v.data(), cd.v.size() * sizeof(float));
      std::memcpy(ib, cd.segments.data(), ns * sizeof(unsigned));
      std::memcpy(fb, cd.flags.data(), ns);
      GeomRecord rec = base;
      rec.kind = GeomRecord::Kind::Curves;
      rec.curves = static_cast<int>(curves.size());
      triangles += ns;
      curves.push_back(std::move(cd));
      rtcCommitGeometry(geom);
      attachRecord(geom, rec);
      continue;
    }

    // Moving?
    Mat4 M0 = node.objectWorldMatrix(), M1 = M0;
    bool moves = false;
    if (motionBlur && node.motionBlur) {
      Mat4 a, b;
      bool ha = nodeMatrixAtTime(scene, node, shutterOpen, false, a);
      bool hb = nodeMatrixAtTime(scene, node, shutterClose, false, b);
      if (ha) M0 = a;
      if (hb) M1 = b;
      moves = (ha || hb) && !sameMatrix(M0, M1);
    }
    // Instancing saves memory but costs traversal time: use it for proxies and for meshes whose
    // copies would add up to many triangles.
    int copies = keyCount[cand.key];
    bool shared = type == "VeraaProxy" ||
                  (copies > 1 && node.triMesh().triangleCount() * static_cast<size_t>(copies) >= 200000);
    bool instanced = shared || moves;

    // Object-space mesh (cached per key for instancing).
    auto buildObjectMesh = [&](MeshData& md, const Mat4& objToWorld) -> bool {
      if (type == "VeraaProxy") {
        std::string err;
        auto tm = loadProxyFile(scene.resolvePath(node.object->params.getString("file")), &err);
        if (!tm) {
          warnings.push_back(err);
          return false;
        }
        md = meshFromTri(*tm);
      } else {
        const TriMesh& tm = node.triMesh();
        if (tm.empty()) return false;
        md = meshFromTri(tm);
        if (mctx.maps.needsEdges) computeEdgeMasks(md, tm);
      }
      // Render-time displacement: modifier first, then the material's displacement slot.
      DisplacementSpec spec;
      for (const auto& m : node.modifiers) {
        if (!m->enabled || m->type() != "VeraaDisplacementMod") continue;
        const auto* dm = static_cast<const VeraaDisplacementModifier*>(m.get());
        if (dm->texmap.active()) {
          spec.owned = compileMap(dm->texmap.map.get(), dispCtx);
        } else if (!dm->params.getString("bitmap").empty()) {
          TexMap bmp("Bitmap");
          bmp.params.setString("file", dm->params.getString("bitmap"));
          spec.owned = compileMap(&bmp, dispCtx);
        }
        spec.map = spec.owned.get();
        spec.amount = dm->params.getFloat("amount");
        spec.shift = dm->params.getFloat("shift");
        spec.edgeLength = std::max(dm->params.getFloat("edgeLength"), 1e-3f);
        spec.maxLevels = dm->params.getInt("maxSubdivs");
        spec.continuity = dm->params.getBool("keepContinuity");
        spec.water = dm->params.getBool("useWaterLevel");
        spec.waterLevel = dm->params.getFloat("waterLevel");
      }
      if (!spec.map) {
        float amt = 0.0f, shift = 0.0f;
        if (const MapNode* d = materials[material]->displacement(amt, shift)) {
          spec.map = d;
          spec.amount = amt;
          spec.shift = shift;
          spec.edgeLength = 1.0f;
          spec.maxLevels = 5;
        }
      }
      if (spec.map && (spec.amount != 0.0f || spec.shift != 0.0f)) displaceMesh(md, spec, objToWorld, dispRng);
      if (mctx.maps.needsElements) computeElements(md);
      return true;
    };

    if (instanced) {
      int meshIdx;
      auto it = sharedMesh.find(cand.key);
      if (it != sharedMesh.end() && !moves) {
        meshIdx = it->second;
      } else {
        MeshData md;
        if (!buildObjectMesh(md, M0)) continue;
        md.child = rtcNewScene(device);
        rtcSetSceneFlags(md.child, RTC_SCENE_FLAG_ROBUST);
        rtcSetSceneBuildQuality(md.child, RTC_BUILD_QUALITY_HIGH);
        RTCGeometry g = makeTriangles(device, md);
        if (!g) {
          if (error) *error = "Out of memory while building the ray tracing scene";
          return false;
        }
        rtcAttachGeometry(md.child, g);
        rtcReleaseGeometry(g);
        rtcCommitScene(md.child);
        meshIdx = static_cast<int>(meshes.size());
        meshes.push_back(std::move(md));
        if (!moves) sharedMesh[cand.key] = meshIdx;
      }
      RTCGeometry inst = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_INSTANCE);
      rtcSetGeometryInstancedScene(inst, meshes[meshIdx].child);
      float m0[12], m1[12];
      toRowMajor3x4(M0, m0);
      toRowMajor3x4(M1, m1);
      if (moves) {
        rtcSetGeometryTimeStepCount(inst, 2);
        rtcSetGeometryTransform(inst, 0, RTC_FORMAT_FLOAT3X4_ROW_MAJOR, m0);
        rtcSetGeometryTransform(inst, 1, RTC_FORMAT_FLOAT3X4_ROW_MAJOR, m1);
      } else {
        rtcSetGeometryTransform(inst, 0, RTC_FORMAT_FLOAT3X4_ROW_MAJOR, m0);
      }
      GeomRecord rec = base;
      rec.kind = GeomRecord::Kind::Instance;
      rec.mesh = meshIdx;
      rec.M0 = M0;
      rec.M1 = M1;
      rec.motion = moves;
      rec.instanceId = ++instanceCounter;
      rec.objZ = normalize(M0.transformDir(Vec3(0, 0, 1)));
      triangles += meshes[meshIdx].triangles();
      attachRecord(inst, rec);
      continue;
    }

    MeshData md;
    if (!buildObjectMesh(md, M0)) continue;
    Mat4 Minv = M0.inverse();
    bool mirrored = M0.determinant3x3() < 0.0f;
    for (size_t i = 0; i < md.P.size(); ++i) {
      md.P[i] = M0.transformPoint(md.P[i]);
      md.N[i] = normalize(Minv.transformNormalWithInverse(md.N[i]));
    }
    if (mirrored)
      for (size_t t = 0; t + 2 < md.idx.size(); t += 3) std::swap(md.idx[t + 1], md.idx[t + 2]);
    RTCGeometry g = makeTriangles(device, md);
    if (!g) {
      if (error) *error = "Out of memory while building the ray tracing scene";
      return false;
    }
    GeomRecord rec = base;
    rec.kind = GeomRecord::Kind::Mesh;
    rec.mesh = static_cast<int>(meshes.size());
    rec.worldToObject = Minv;
    rec.objZ = normalize(M0.transformDir(Vec3(0, 0, 1)));
    triangles += md.triangles();
    meshes.push_back(std::move(md));
    attachRecord(g, rec);
  }

  // ---- Mesh lights from VeraaLight (mesh type) ----
  for (const auto& req : lb.meshRequests) {
    const Node* src = scene.findNode(req.sourceNode);
    Light& L = lights[req.lightIndex];
    const Node* lightNode = scene.findNode(L.nodeId);
    if (!src || !lightNode) continue;
    const TriMesh& tm = src->triMesh();
    if (tm.empty()) continue;
    MeshData md = meshFromTri(tm);
    Mat4 M = src->objectWorldMatrix(), Mi = M.inverse();
    for (size_t i = 0; i < md.P.size(); ++i) {
      md.P[i] = M.transformPoint(md.P[i]);
      md.N[i] = normalize(Mi.transformNormalWithInverse(md.N[i]));
    }
    if (M.determinant3x3() < 0.0f)
      for (size_t t = 0; t + 2 < md.idx.size(); t += 3) std::swap(md.idx[t + 1], md.idx[t + 2]);
    double area = 0.0;
    L.tri.clear();
    L.triCdf.clear();
    for (size_t t = 0; t < md.triangles(); ++t) {
      const Vec3 &a = md.P[md.idx[t * 3]], &b = md.P[md.idx[t * 3 + 1]], &c = md.P[md.idx[t * 3 + 2]];
      L.tri.insert(L.tri.end(), {a, b, c});
      area += 0.5 * length(cross(b - a, c - a));
      L.triCdf.push_back(static_cast<float>(area));
    }
    if (area <= 0.0) continue;
    for (float& c : L.triCdf) c = static_cast<float>(c / area);
    L.area = static_cast<float>(area);
    const ParamBlock& lp = lightNode->object->params;
    L.color = L.color * luminanceFromUnits(static_cast<LightUnits>(lp.getInt("units")), 1.0f, L.area * 1e-4f,
                                           L.doubleSided ? 2 : 1);
    Vec3 radiance = L.color;
    bool twoSided = L.doubleSided;
    L.meshRadiance = [radiance, twoSided](int, const Vec2&, bool front) { return front || twoSided ? radiance : Vec3(0.0f); };
    RTCGeometry g = makeTriangles(device, md);
    GeomRecord rec;
    rec.kind = GeomRecord::Kind::Mesh;
    rec.mesh = static_cast<int>(meshes.size());
    rec.material = blackMaterial;
    rec.nodeId = src->id;
    rec.lightIndex = req.lightIndex;
    rec.mask = kMaskGI | kMaskSecondary | (L.invisible ? 0u : kMaskCamera) | kMaskShadowOpaque;
    rec.worldToObject = Mi;
    triangles += md.triangles();
    meshes.push_back(std::move(md));
    L.meshGeom = static_cast<int>(records.size());
    attachRecord(g, rec);
    L.meshGeom = static_cast<int>(records.size()) - 1;
  }

  rtcCommitScene(top);
  if (rtcGetDeviceError(device) != RTC_ERROR_NONE) {
    if (error) *error = "Embree failed to build the scene";
    return false;
  }

  // ---- Emissive materials sampled as mesh lights ----
  for (size_t gi = 0; gi < records.size(); ++gi) {
    GeomRecord& rec = records[gi];
    if (rec.kind == GeomRecord::Kind::Curves || rec.mesh < 0 || rec.lightIndex >= 0) continue;
    if (!materials[rec.material]->isMeshLight()) continue;
    const MeshData& md = meshes[rec.mesh];
    Light L;
    L.type = Light::Type::Mesh;
    L.nodeId = rec.nodeId;
    const Node* n = scene.findNode(rec.nodeId);
    L.name = n ? n->name : "Light material";
    L.group = 1;  // self-illumination
    L.doubleSided = true;
    L.color = Vec3(1.0f);
    double area = 0.0;
    for (size_t t = 0; t < md.triangles(); ++t) {
      Vec3 a = md.P[md.idx[t * 3]], b = md.P[md.idx[t * 3 + 1]], c = md.P[md.idx[t * 3 + 2]];
      if (rec.kind == GeomRecord::Kind::Instance) {
        a = rec.M0.transformPoint(a);
        b = rec.M0.transformPoint(b);
        c = rec.M0.transformPoint(c);
      }
      L.tri.insert(L.tri.end(), {a, b, c});
      area += 0.5 * length(cross(b - a, c - a));
      L.triCdf.push_back(static_cast<float>(area));
    }
    if (area <= 0.0) continue;
    for (float& c : L.triCdf) c = static_cast<float>(c / area);
    L.area = static_cast<float>(area);
    L.meshGeom = static_cast<int>(gi);
    int geomIndex = static_cast<int>(gi);
    const Impl* self = this;
    L.meshRadiance = [self, geomIndex](int t, const Vec2& b, bool front) {
      SurfaceHit s;
      const GeomRecord& r = self->records[geomIndex];
      const MeshData& m = self->meshes[r.mesh];
      uint32_t i0 = m.idx[t * 3], i1 = m.idx[t * 3 + 1], i2 = m.idx[t * 3 + 2];
      float b0 = 1.0f - b.x - b.y;
      s.uv = m.UV[i0] * b0 + m.UV[i1] * b.x + m.UV[i2] * b.y;
      s.pObj = m.P[i0] * b0 + m.P[i1] * b.x + m.P[i2] * b.y;
      s.p = r.kind == GeomRecord::Kind::Instance ? r.M0.transformPoint(s.pObj) : s.pObj;
      Vec3 ng = cross(m.P[i1] - m.P[i0], m.P[i2] - m.P[i0]);
      s.ng = s.ns = length2(ng) > 0.0f ? normalize(ng) : Vec3(0, 0, 1);
      s.nodeId = r.nodeId;
      s.material = r.material;
      s.faceMatId = t < static_cast<int>(m.matIds.size()) ? m.matIds[t] : 0;
      Rng rng(static_cast<uint64_t>(t) * 7919u + 3, 11);
      ShadeContext sc = self->makeContext(s, front ? s.ng : -s.ng, RayKind::Camera, 1, rng, 0.0f, 0.0f);
      sc.outside = front;
      SurfaceShade out;
      self->materials[r.material]->resolve(sc)->shade(sc, out);
      return out.emission;
    };
    rec.lightIndex = static_cast<int>(lights.size());
    lights.push_back(std::move(L));
  }

  // ---- Light lists ----
  for (size_t i = 0; i < lights.size(); ++i) {
    Light& l = lights[i];
    if (l.type == Light::Type::Portal) {
      l.env = &env;
      portals.push_back(static_cast<int>(i));
    }
    if (l.isArea()) areaLights.push_back(static_cast<int>(i));
    if (l.link.active()) anyLightLinks = true;
  }
  hasPortals = !portals.empty() && !env.isBlack();
  // Power-based selection when there are many lights.
  sampleAllLights = lights.size() <= 8;
  lightCdf.assign(lights.size(), 0.0f);
  lightProb.assign(lights.size(), 0.0f);
  double total = 0.0;
  std::vector<double> power(lights.size());
  for (size_t i = 0; i < lights.size(); ++i) {
    float p = lights[i].power();
    if (lights[i].type == Light::Type::Portal) p = env.averageLuminance() * lights[i].area * kPi;
    power[i] = std::max(static_cast<double>(p), 1e-12);
    total += power[i];
  }
  // Mix in a uniform part so weak lights near the camera are not starved.
  double acc = 0.0;
  for (size_t i = 0; i < lights.size(); ++i) {
    double pr = 0.7 * power[i] / total + 0.3 / static_cast<double>(lights.size());
    lightProb[i] = static_cast<float>(pr);
    acc += pr;
    lightCdf[i] = static_cast<float>(acc);
  }
  if (!lightCdf.empty()) lightCdf.back() = 1.0f;

  // Brightest light direction (Falloff "Shadow/Light").
  float best = -1.0f;
  for (const auto& l : lights) {
    float p = l.power();
    if (p > best) {
      best = p;
      keyLightDir = l.type == Light::Type::Sun ? l.dir : Vec3(0, 0, 1);
    }
  }

  // ---- Distance fields ----
  for (const auto& names : mctx.maps.distanceFields) {
    DistanceField f;
    std::vector<Vec3> pts;
    double area = 0.0;
    std::vector<std::pair<const Node*, Mat4>> srcs;
    for (const auto& nm : names)
      if (const Node* n = scene.findNodeByName(nm))
        if (n->kind() == ObjectKind::Geometry) srcs.push_back({n, n->objectWorldMatrix()});
    for (const auto& [n, M] : srcs) {
      const TriMesh& tm = n->triMesh();
      for (size_t t = 0; t < tm.triangleCount(); ++t) {
        Vec3 a = M.transformPoint(tm.positions[tm.indices[t * 3]]), b = M.transformPoint(tm.positions[tm.indices[t * 3 + 1]]),
             c = M.transformPoint(tm.positions[tm.indices[t * 3 + 2]]);
        area += 0.5 * length(cross(b - a, c - a));
      }
    }
    float spacing = area > 0.0 ? static_cast<float>(std::sqrt(area / 60000.0)) : 1.0f;
    f.cell = std::max(spacing * 6.0f, 0.5f);
    Rng rng(99, 7);
    for (const auto& [n, M] : srcs) {
      const TriMesh& tm = n->triMesh();
      for (size_t t = 0; t < tm.triangleCount(); ++t) {
        Vec3 a = M.transformPoint(tm.positions[tm.indices[t * 3]]), b = M.transformPoint(tm.positions[tm.indices[t * 3 + 1]]),
             c = M.transformPoint(tm.positions[tm.indices[t * 3 + 2]]);
        float ar = 0.5f * length(cross(b - a, c - a));
        float expected = ar / (spacing * spacing);
        int count = static_cast<int>(expected);
        if (rng.next() < expected - count) ++count;
        count = std::max(count, 1);
        for (int k = 0; k < count; ++k) {
          float r1 = std::sqrt(rng.next()), r2 = rng.next();
          pts.push_back(a * (1.0f - r1) + b * (r1 * (1.0f - r2)) + c * (r1 * r2));
        }
        pts.push_back((a + b + c) / 3.0f);
      }
    }
    for (const Vec3& p : pts) {
      int cx = static_cast<int>(std::floor(p.x / f.cell)), cy = static_cast<int>(std::floor(p.y / f.cell)),
          cz = static_cast<int>(std::floor(p.z / f.cell));
      uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(cx) & 0x1FFFFF) << 42) |
                     (static_cast<uint64_t>(static_cast<uint32_t>(cy) & 0x1FFFFF) << 21) |
                     (static_cast<uint64_t>(static_cast<uint32_t>(cz) & 0x1FFFFF));
      f.cells[key].push_back(p);
    }
    distanceFields.push_back(std::move(f));
  }
  return true;
}

}  // namespace veraa
