#include "renderer.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <embree4/rtcore.h>

#include "bsdf.h"
#include "common.h"
#include "environment.h"
#include "lights.h"
#include "postprocess.h"
#include "texture.h"
#include "threadpool.h"

namespace veraa {

using namespace render;

namespace {

constexpr int kTileSize = 32;
constexpr int kAovPasses = 16;         // passes used to average the albedo/normal guides
constexpr int kMaxPassThrough = 64;    // cut-out (opacity) crossings per path
constexpr int kMaxShadowLayers = 24;   // transparent surfaces crossed by one shadow ray
constexpr int kMaxSampledLights = 8;   // above this, one light is picked per shading point
constexpr float kBumpScale = 0.5f;     // bump height (scene units) at bump amount 1

RTCDevice embreeDevice() {
  static RTCDevice device = rtcNewDevice(nullptr);
  return device;
}

int64_t nowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

struct GeomData {
  std::vector<Vec3> P, N;
  std::vector<Vec2> UV;
  std::vector<uint32_t> idx;
  int material = 0;
};

struct SurfaceHit {
  Vec3 p, ng, ns;
  Vec2 uv;
  int material = 0;
  unsigned geomID = 0, primID = 0;
};

struct Camera {
  Vec3 pos, f, r, u;
  float tanHalf = 0.36f, aspect = 1.0f, halfH = 100.0f;
  bool ortho = false;
  float aperture = 0.0f, focus = 300.0f;
};

}  // namespace

struct Renderer::Impl {
  ~Impl() {
    stop();
    releaseScene();
  }

  // ---- scene ----
  RTCScene rtc = nullptr;        // everything (camera and path rays)
  RTCScene rtcOpaque = nullptr;  // geometry that fully blocks shadow rays
  RTCScene rtcTransp = nullptr;  // cut-outs and "affect shadows" glass
  std::vector<int> transpMap;    // rtcTransp geometry id -> geoms index
  std::vector<GeomData> geoms;
  std::vector<MaterialData> materials;
  std::unique_ptr<TextureCache> textures = std::make_unique<TextureCache>();
  std::vector<Light> lights;
  std::vector<int> areaLights;
  Environment env;
  bool anyShadowTransparent = false;
  size_t triangles = 0;
  std::vector<std::string> warnings;

  // ---- settings ----
  int sceneW = 1280, sceneH = 720, sceneSpp = 512;
  double sceneTime = 0.0;
  int maxDepth = 8;
  float clampIndirect = 10.0f;
  int overW = 0, overH = 0, overSpp = 0;
  double overTime = -1.0;
  RenderView view;
  Camera cam;

  // ---- buffers ----
  int w = 0, h = 0;
  std::vector<float> accum, accumAlbedo, accumNormal;
  mutable std::mutex snapMutex;
  std::vector<float> snapColor, snapAlbedo, snapNormal;
  int snapPasses = 0, snapAovPasses = 0;
  mutable std::mutex denoiseMutex;
  mutable std::vector<float> denoised;
  mutable int denoisedPasses = -1;

  // ---- control ----
  std::atomic<bool> stopFlag{false}, running{false}, finished{false};
  std::atomic<int> passesDone{0};
  std::atomic<int64_t> startNs{0}, elapsedNs{0};
  int targetPasses = 0;
  double timeLimit = 0.0;
  std::thread controller;
  std::unique_ptr<ThreadPool> pool;

  int resW() const { return overW > 0 ? overW : sceneW; }
  int resH() const { return overH > 0 ? overH : sceneH; }

  void releaseScene() {
    for (RTCScene* sc : {&rtc, &rtcOpaque, &rtcTransp}) {
      if (*sc) rtcReleaseScene(*sc);
      *sc = nullptr;
    }
    geoms.clear();
    transpMap.clear();
  }

  void stop() {
    stopFlag = true;
    if (controller.joinable()) controller.join();
  }

  bool build(const Scene& scene, std::string* error);
  void prepare();
  void run();
  bool renderPass(int pass);
  void publish(int passes);

  // Tracing.
  bool intersect(RTCScene scene, const Vec3& o, const Vec3& d, float tFar, RTCRayHit& rh) const;
  bool intersect(const Vec3& o, const Vec3& d, float tFar, RTCRayHit& rh) const { return intersect(rtc, o, d, tFar, rh); }
  void surface(const RTCRayHit& rh, SurfaceHit& s) const;
  Vec3 transmittance(Vec3 o, const Vec3& d, float tMax) const;
  void cameraRay(float px, float py, Rng& rng, Vec3& o, Vec3& d) const;
  Vec3 directLight(const SurfaceHit& s, const Vec3& ngf, const Frame& fr, const Vec3& woL, const Bsdf& bsdf,
                   Rng& rng) const;
  Vec3 radiance(Vec3 o, Vec3 d, Rng& rng, Vec3& albedo, Vec3& normal) const;
  void applyBump(const MaterialData& m, SurfaceHit& s) const;
};

// -------------------------------------------------------------------------------------------------
// Scene building

bool Renderer::Impl::build(const Scene& scene, std::string* error) {
  releaseScene();
  warnings.clear();
  materials.clear();
  lights.clear();
  areaLights.clear();
  triangles = 0;
  anyShadowTransparent = false;
  transpMap.clear();

  RTCDevice device = embreeDevice();
  if (!device) {
    if (error) *error = "Could not start the Embree ray tracing device (CPU not supported?)";
    return false;
  }

  const ParamBlock& rs = scene.renderSettings;
  sceneW = rs.getInt("width");
  sceneH = rs.getInt("height");
  sceneSpp = rs.getInt("samples");
  sceneTime = rs.getFloat("timeLimit");
  maxDepth = rs.getInt("maxDepth");
  clampIndirect = rs.getFloat("clampIndirect");

  // Materials: scene materials first, then wire-color defaults per untextured node.
  std::unordered_map<uint32_t, int> matIndex;
  for (const auto& m : scene.materials) {
    matIndex[m->id] = static_cast<int>(materials.size());
    materials.push_back(MaterialData::fromMaterial(*m, &scene, *textures, &warnings));
  }

  for (RTCScene* sc : {&rtc, &rtcOpaque, &rtcTransp}) {
    *sc = rtcNewScene(device);
    rtcSetSceneFlags(*sc, RTC_SCENE_FLAG_ROBUST);
    rtcSetSceneBuildQuality(*sc, RTC_BUILD_QUALITY_HIGH);
  }
  // Uploads a triangle mesh to an Embree scene and returns its geometry id (or -1).
  auto attach = [&](RTCScene target, const GeomData& g) -> int {
    size_t nv = g.P.size(), nt = g.idx.size() / 3;
    RTCGeometry geom = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_TRIANGLE);
    auto* vb = static_cast<float*>(
        rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, 3 * sizeof(float), nv));
    auto* ib = static_cast<unsigned*>(
        rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, 3 * sizeof(unsigned), nt));
    if (!vb || !ib) {
      rtcReleaseGeometry(geom);
      return -1;
    }
    for (size_t i = 0; i < nv; ++i) {
      vb[i * 3] = g.P[i].x;
      vb[i * 3 + 1] = g.P[i].y;
      vb[i * 3 + 2] = g.P[i].z;
    }
    std::memcpy(ib, g.idx.data(), nt * 3 * sizeof(unsigned));
    rtcCommitGeometry(geom);
    unsigned id = rtcAttachGeometry(target, geom);
    rtcReleaseGeometry(geom);
    return static_cast<int>(id);
  };

  for (const auto& node : scene.nodes) {
    ObjectKind kind = node->kind();
    if (kind != ObjectKind::Geometry && kind != ObjectKind::Shape) continue;
    if (!scene.isNodeRenderable(*node)) continue;
    // Splines render only with "Enable In Renderer" (they may show a tube in viewports only).
    if (node->isShapeResult() && !(node->object && node->object->params.getBool("renderInRenderer"))) continue;
    const TriMesh& tm = node->triMesh();
    if (tm.empty()) continue;

    GeomData g;
    auto it = matIndex.find(node->materialId);
    if (it != matIndex.end()) {
      g.material = it->second;
    } else {
      g.material = static_cast<int>(materials.size());
      materials.push_back(MaterialData::fromColor(node->wireColor));
    }

    Mat4 M = node->objectWorldMatrix();
    Mat4 Minv = M.inverse();
    bool mirrored = M.determinant3x3() < 0.0f;
    size_t nv = tm.positions.size();
    g.P.resize(nv);
    g.N.resize(nv);
    g.UV = tm.uvs;
    for (size_t i = 0; i < nv; ++i) {
      g.P[i] = M.transformPoint(tm.positions[i]);
      g.N[i] = normalize(Minv.transformNormalWithInverse(tm.normals[i]));
    }
    g.idx = tm.indices;
    if (mirrored)
      for (size_t t = 0; t + 2 < g.idx.size(); t += 3) std::swap(g.idx[t + 1], g.idx[t + 2]);
    size_t nt = g.idx.size() / 3;

    int id = attach(rtc, g);
    if (id < 0) {
      if (error) *error = "Out of memory while building the ray tracing scene";
      return false;
    }
    if (materials[g.material].shadowTransparent) {
      int tid = attach(rtcTransp, g);
      if (tid >= 0) {
        if (transpMap.size() <= static_cast<size_t>(tid)) transpMap.resize(tid + 1, 0);
        transpMap[tid] = id;
        anyShadowTransparent = true;
      }
    } else {
      attach(rtcOpaque, g);
    }
    if (geoms.size() <= static_cast<size_t>(id)) geoms.resize(id + 1);
    triangles += nt;
    geoms[id] = std::move(g);
  }
  rtcCommitScene(rtc);
  rtcCommitScene(rtcOpaque);
  rtcCommitScene(rtcTransp);
  if (rtcGetDeviceError(device) != RTC_ERROR_NONE) {
    if (error) *error = "Embree failed to build the scene";
    return false;
  }

  bool hasSun = false;
  Vec3 sunDir(0.0f, 0.0f, 1.0f);
  float sunE = 0.0f;
  lights = collectLights(scene, &hasSun, &sunDir, &sunE);
  env.build(scene, *textures, hasSun ? &sunDir : nullptr, sunE, &warnings);
  if (rs.getInt("envMode") == 3) {
    float turbidity = rs.getFloat("skyTurbidity");
    for (auto& l : lights)
      if (l.type == Light::Type::Sun) l.color *= sunAtmosphereTint(std::asin(clampf(l.dir.z, -1.0f, 1.0f)), turbidity);
  }
  for (size_t i = 0; i < lights.size(); ++i)
    if (lights[i].isArea()) areaLights.push_back(static_cast<int>(i));
  return true;
}

// -------------------------------------------------------------------------------------------------
// Tracing helpers

bool Renderer::Impl::intersect(RTCScene scene, const Vec3& o, const Vec3& d, float tFar, RTCRayHit& rh) const {
  rh.ray.org_x = o.x;
  rh.ray.org_y = o.y;
  rh.ray.org_z = o.z;
  rh.ray.tnear = 0.0f;
  rh.ray.dir_x = d.x;
  rh.ray.dir_y = d.y;
  rh.ray.dir_z = d.z;
  rh.ray.time = 0.0f;
  rh.ray.tfar = tFar;
  rh.ray.mask = 0xFFFFFFFFu;
  rh.ray.id = 0;
  rh.ray.flags = 0;
  rh.hit.geomID = RTC_INVALID_GEOMETRY_ID;
  rh.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;
  rtcIntersect1(scene, &rh, nullptr);
  return rh.hit.geomID != RTC_INVALID_GEOMETRY_ID;
}

void Renderer::Impl::surface(const RTCRayHit& rh, SurfaceHit& s) const {
  const GeomData& g = geoms[rh.hit.geomID];
  unsigned prim = rh.hit.primID;
  uint32_t i0 = g.idx[prim * 3], i1 = g.idx[prim * 3 + 1], i2 = g.idx[prim * 3 + 2];
  float bu = rh.hit.u, bv = rh.hit.v, b0 = 1.0f - bu - bv;
  const Vec3 &p0 = g.P[i0], &p1 = g.P[i1], &p2 = g.P[i2];
  s.p = p0 * b0 + p1 * bu + p2 * bv;
  Vec3 ng = cross(p1 - p0, p2 - p0);
  float l = length(ng);
  s.ng = l > 0.0f ? ng / l : Vec3(0, 0, 1);
  Vec3 ns = g.N[i0] * b0 + g.N[i1] * bu + g.N[i2] * bv;
  s.ns = length2(ns) > 1e-20f ? normalize(ns) : s.ng;
  if (dot(s.ns, s.ng) < 0.0f) s.ns = s.ng;  // inconsistent normals: trust the geometry
  s.uv = g.UV[i0] * b0 + g.UV[i1] * bu + g.UV[i2] * bv;
  s.material = g.material;
  s.geomID = rh.hit.geomID;
  s.primID = prim;
}

void Renderer::Impl::applyBump(const MaterialData& m, SurfaceHit& s) const {
  const Texture* t = m.bumpMap;
  if (!t || m.bumpAmount == 0.0f) return;
  const GeomData& g = geoms[s.geomID];
  uint32_t i0 = g.idx[s.primID * 3], i1 = g.idx[s.primID * 3 + 1], i2 = g.idx[s.primID * 3 + 2];
  Vec2 t0 = m.tiled(g.UV[i0]), t1 = m.tiled(g.UV[i1]), t2 = m.tiled(g.UV[i2]);
  Vec2 d1 = t1 - t0, d2 = t2 - t0;
  Vec3 e1 = g.P[i1] - g.P[i0], e2 = g.P[i2] - g.P[i0];
  float det = d1.x * d2.y - d1.y * d2.x;
  if (std::fabs(det) < 1e-12f) return;
  float inv = 1.0f / det;
  Vec3 dpdu = (e1 * d2.y - e2 * d1.y) * inv;
  Vec3 dpdv = (e2 * d1.x - e1 * d2.x) * inv;
  Vec2 uv = m.tiled(s.uv);
  float du = 1.0f / t->width(), dv = 1.0f / t->height();
  float h0 = t->value(uv), hu = t->value(Vec2(uv.x + du, uv.y)), hv = t->value(Vec2(uv.x, uv.y + dv));
  float scale = m.bumpAmount * kBumpScale;
  Vec3 pu = dpdu + s.ns * ((hu - h0) / du * scale);
  Vec3 pv = dpdv + s.ns * ((hv - h0) / dv * scale);
  Vec3 nb = cross(pu, pv);
  if (length2(nb) < 1e-30f) return;
  nb = normalize(nb);
  if (dot(nb, s.ns) < 0.0f) nb = -nb;
  s.ns = nb;
}

Vec3 Renderer::Impl::transmittance(Vec3 o, const Vec3& d, float tMax) const {
  // Opaque blockers first: a cheap any-hit query that settles most shadow rays.
  RTCRay ray;
  ray.org_x = o.x; ray.org_y = o.y; ray.org_z = o.z;
  ray.tnear = 0.0f;
  ray.dir_x = d.x; ray.dir_y = d.y; ray.dir_z = d.z;
  ray.time = 0.0f;
  ray.tfar = tMax;
  ray.mask = 0xFFFFFFFFu;
  ray.id = 0;
  ray.flags = 0;
  rtcOccluded1(rtcOpaque, &ray, nullptr);
  if (ray.tfar < 0.0f) return Vec3(0.0f);  // Embree sets tfar = -inf on a hit
  if (!anyShadowTransparent) return Vec3(1.0f);
  // Then attenuate through glass and cut-outs, nearest first.
  Vec3 T(1.0f);
  RTCRayHit rh;
  for (int layer = 0; layer < kMaxShadowLayers; ++layer) {
    if (!intersect(rtcTransp, o, d, tMax, rh)) return T;
    rh.hit.geomID = static_cast<unsigned>(transpMap[rh.hit.geomID]);
    const MaterialData& m = materials[geoms[rh.hit.geomID].material];
    SurfaceHit s;
    surface(rh, s);
    T *= m.shadowTransmission(m.tiled(s.uv), std::fabs(dot(s.ng, d)));
    if (maxComponent(T) <= 1e-4f) return Vec3(0.0f);
    o = offsetRayOrigin(s.p, dot(d, s.ng) > 0.0f ? s.ng : -s.ng);
    tMax -= rh.ray.tfar;
    if (tMax <= 0.0f) return T;
  }
  return T;
}

void Renderer::Impl::cameraRay(float px, float py, Rng& rng, Vec3& o, Vec3& d) const {
  float sx = 2.0f * px / w - 1.0f;
  float sy = 1.0f - 2.0f * py / h;
  if (cam.ortho) {
    o = cam.pos + cam.r * (sx * cam.halfH * cam.aspect) + cam.u * (sy * cam.halfH);
    d = cam.f;
    return;
  }
  d = normalize(cam.f + cam.r * (sx * cam.tanHalf * cam.aspect) + cam.u * (sy * cam.tanHalf));
  o = cam.pos;
  if (cam.aperture > 0.0f) {
    Vec3 focusPoint = o + d * (cam.focus / std::max(dot(d, cam.f), 1e-4f));
    Vec2 lens = sampleConcentricDisk(rng.next(), rng.next()) * cam.aperture;
    o = cam.pos + cam.r * lens.x + cam.u * lens.y;
    d = normalize(focusPoint - o);
  }
}

// -------------------------------------------------------------------------------------------------
// Integrator

Vec3 Renderer::Impl::directLight(const SurfaceHit& s, const Vec3& ngf, const Frame& fr, const Vec3& woL,
                                 const Bsdf& bsdf, Rng& rng) const {
  Vec3 sum(0.0f);
  int n = static_cast<int>(lights.size());
  bool all = n <= kMaxSampledLights;
  int count = all ? n : 1;
  float sel = all ? 1.0f : 1.0f / static_cast<float>(n);
  for (int k = 0; k < count; ++k) {
    int li = all ? k : std::min(static_cast<int>(rng.next() * n), n - 1);
    const Light& light = lights[li];
    float u1 = rng.next(), u2 = rng.next();
    LightSample ls;
    if (!light.sample(s.p, u1, u2, ls)) continue;
    Vec3 wiL = fr.toLocal(ls.wi);
    float side = dot(ls.wi, ngf);
    if (wiL.z * side <= 0.0f) continue;  // light is behind the actual surface
    float bpdf = 0.0f;
    Vec3 f = bsdf.eval(woL, wiL, bpdf);
    if (isBlack(f)) continue;
    Vec3 c = f * (std::fabs(wiL.z)) * ls.radiance;
    if (ls.delta) {
      c = c / sel;
    } else {
      float lpdf = ls.pdf * sel;
      c = c * (powerHeuristic(lpdf, bpdf) / lpdf);
    }
    if (isBlack(c)) continue;
    if (light.castShadows) {
      Vec3 o = offsetRayOrigin(s.p, side > 0.0f ? ngf : -ngf);
      float tMax = std::isfinite(ls.dist) ? ls.dist * (1.0f - 1e-4f) : kInfinity;
      c *= transmittance(o, ls.wi, tMax);
    }
    sum += c;
  }
  if (!env.isBlack()) {
    Vec3 wi, Le;
    float epdf = 0.0f;
    if (env.sample(rng.next(), rng.next(), wi, Le, epdf) && epdf > 0.0f) {
      Vec3 wiL = fr.toLocal(wi);
      float side = dot(wi, ngf);
      if (wiL.z * side > 0.0f) {
        float bpdf = 0.0f;
        Vec3 f = bsdf.eval(woL, wiL, bpdf);
        if (!isBlack(f)) {
          Vec3 c = f * Le * (std::fabs(wiL.z) * powerHeuristic(epdf, bpdf) / epdf);
          if (!isBlack(c)) {
            Vec3 o = offsetRayOrigin(s.p, side > 0.0f ? ngf : -ngf);
            c *= transmittance(o, wi, kInfinity);
            sum += c;
          }
        }
      }
    }
  }
  return sum;
}

Vec3 Renderer::Impl::radiance(Vec3 o, Vec3 d, Rng& rng, Vec3& albedo, Vec3& normal) const {
  Vec3 L(0.0f), beta(1.0f), sigma(0.0f);
  bool specular = true;  // last scattering was delta (or this is the camera ray)
  bool primary = true;   // no surface interaction yet (cut-out crossings do not count)
  float prevPdf = 0.0f;
  int depth = 0, passThrough = 0;
  bool aovDone = false;
  albedo = Vec3(0.0f);
  normal = Vec3(0.0f);
  int nLights = static_cast<int>(lights.size());
  float lightSel = nLights <= kMaxSampledLights ? 1.0f : 1.0f / static_cast<float>(nLights);

  // Contributions arriving at path vertex >= 1 are indirect light and get clamped.
  auto add = [&](const Vec3& c, int receivingVertex) {
    if (!isFiniteColor(c)) return;
    L += receivingVertex >= 1 ? clampLuminance(c, clampIndirect) : c;
  };

  RTCRayHit rh;
  for (;;) {
    bool hit = intersect(o, d, kInfinity, rh);
    float tHit = hit ? rh.ray.tfar : kInfinity;

    // Area lights in front of the surface.
    int lightIdx = -1;
    Vec3 Le;
    float lightPdf = 0.0f;
    for (int li : areaLights) {
      const Light& l = lights[li];
      if (primary && l.invisible) continue;
      float t, pdf;
      Vec3 le;
      if (l.intersect(o, d, tHit, t, le, pdf)) {
        tHit = t;
        Le = le;
        lightPdf = pdf;
        lightIdx = li;
      }
    }
    if (!isBlack(sigma) && std::isfinite(tHit))
      beta *= Vec3(std::exp(-sigma.x * tHit), std::exp(-sigma.y * tHit), std::exp(-sigma.z * tHit));

    if (lightIdx >= 0) {
      float wgt = specular ? 1.0f : powerHeuristic(prevPdf, lightPdf * lightSel);
      add(beta * Le * wgt, depth - 1);
      if (!aovDone) {
        albedo = clampColor(Le, 0.0f, 1.0f);
        normal = -d;
      }
      break;
    }

    if (!hit) {
      Vec3 envL;
      if (primary && !env.visibleToCamera) {
        envL = env.background;
      } else {
        envL = env.eval(d);
        if (!specular) envL *= powerHeuristic(prevPdf, env.pdf(d));
      }
      add(beta * envL, depth - 1);
      if (!aovDone) albedo = clampColor(primary && !env.visibleToCamera ? env.background : env.eval(d), 0.0f, 1.0f);
      break;
    }

    SurfaceHit s;
    surface(rh, s);
    const MaterialData& mat = materials[s.material];
    Vec2 tuv = mat.tiled(s.uv);

    // Stochastic cut-out (opacity maps for leaves, fences, ...).
    if (mat.hasOpacity && rng.next() >= mat.alphaAt(tuv)) {
      if (++passThrough > kMaxPassThrough) break;
      o = offsetRayOrigin(s.p, dot(d, s.ng) > 0.0f ? s.ng : -s.ng);
      continue;
    }

    if (mat.bumpMap) applyBump(mat, s);

    Vec3 wo = -d;
    bool entering = dot(s.ng, wo) > 0.0f;
    Vec3 ngf = entering ? s.ng : -s.ng;
    Vec3 nsf = entering ? s.ns : -s.ns;
    if (dot(nsf, wo) <= 1e-4f) nsf = ngf;
    Frame fr(nsf);
    Vec3 woL = fr.toLocal(wo);
    if (woL.z <= 0.0f) woL.z = 1e-4f;

    if (mat.hasEmission) add(beta * mat.emission, depth - 1);

    Bsdf bsdf;
    bsdf.init(mat, tuv, woL, entering);
    if (!aovDone) {
      albedo = bsdf.albedo();
      normal = nsf;
      aovDone = true;
    }
    if (depth >= maxDepth || bsdf.isBlack()) break;

    if (bsdf.hasNonDelta() && (!lights.empty() || !env.isBlack()))
      add(beta * directLight(s, ngf, fr, woL, bsdf, rng), depth);

    BsdfSample bs;
    float uL = rng.next(), u1 = rng.next(), u2 = rng.next();
    if (!bsdf.sample(woL, uL, u1, u2, bs)) break;
    Vec3 wi = normalize(fr.toWorld(bs.wi));
    float side = dot(wi, ngf);
    if (bs.wi.z * side <= 0.0f) break;  // shading normal sent the ray through the geometry
    beta *= bs.weight;
    if (!isFiniteColor(beta) || isBlack(beta)) break;

    specular = bs.delta;
    prevPdf = bs.pdf;
    primary = false;
    if (bs.transmission && !bsdf.thin()) sigma = entering ? mat.absorption : Vec3(0.0f);

    o = offsetRayOrigin(s.p, side > 0.0f ? ngf : -ngf);
    d = wi;
    ++depth;

    if (depth >= 3) {
      float survive = std::min(maxComponent(beta), 0.95f);
      if (rng.next() >= survive) break;
      beta /= survive;
    }
  }
  return L;
}

// -------------------------------------------------------------------------------------------------
// Progressive engine

void Renderer::Impl::prepare() {
  w = resW();
  h = resH();
  size_t n = static_cast<size_t>(w) * h * 3;
  accum.assign(n, 0.0f);
  accumAlbedo.assign(n, 0.0f);
  accumNormal.assign(n, 0.0f);
  {
    std::lock_guard<std::mutex> lock(snapMutex);
    snapColor.assign(n, 0.0f);
    snapAlbedo.assign(n, 0.0f);
    snapNormal.assign(n, 0.0f);
    snapPasses = 0;
    snapAovPasses = 0;
  }
  {
    std::lock_guard<std::mutex> lock(denoiseMutex);
    denoisedPasses = -1;
    denoised.clear();
  }
  targetPasses = std::max(1, overSpp > 0 ? overSpp : sceneSpp);
  timeLimit = overTime >= 0.0 ? overTime : sceneTime;

  cam = Camera();
  cam.pos = view.position;
  cam.f = normalize(view.target - view.position);
  Vec3 r = cross(cam.f, view.up);
  if (length2(r) < 1e-12f) r = cross(cam.f, std::fabs(cam.f.z) < 0.99f ? Vec3(0, 0, 1) : Vec3(0, 1, 0));
  cam.r = normalize(r);
  cam.u = cross(cam.r, cam.f);
  cam.aspect = static_cast<float>(w) / static_cast<float>(h);
  cam.tanHalf = std::tan(radians(clampf(view.fovYDeg, 0.01f, 179.0f)) * 0.5f);
  cam.ortho = view.orthographic;
  cam.halfH = view.orthoHeight * 0.5f;
  cam.aperture = view.apertureRadius;
  cam.focus = std::max(view.focusDistance, 1e-3f);

  if (!pool) {
    int threads = static_cast<int>(std::thread::hardware_concurrency());
    pool = std::make_unique<ThreadPool>(threads > 0 ? threads : 4);
  }
  passesDone = 0;
  elapsedNs = 0;
  finished = false;
  stopFlag = false;
}

bool Renderer::Impl::renderPass(int pass) {
  int tilesX = (w + kTileSize - 1) / kTileSize, tilesY = (h + kTileSize - 1) / kTileSize;
  std::atomic<bool> aborted{false};
  bool aov = pass < kAovPasses;
  pool->run(tilesX * tilesY, [&](int tile, int) {
    if (stopFlag.load(std::memory_order_relaxed)) {
      aborted = true;
      return;
    }
    int x0 = (tile % tilesX) * kTileSize, y0 = (tile / tilesX) * kTileSize;
    int x1 = std::min(x0 + kTileSize, w), y1 = std::min(y0 + kTileSize, h);
    for (int y = y0; y < y1; ++y)
      for (int x = x0; x < x1; ++x) {
        size_t pi = static_cast<size_t>(y) * w + x;
        Rng rng(hash64((static_cast<uint64_t>(pass) << 32) ^ pi ^ 0x9E3779B97F4A7C15ULL), pi);
        float px = x + 0.5f + 0.75f * sampleTent(rng.next());
        float py = y + 0.5f + 0.75f * sampleTent(rng.next());
        Vec3 o, d, albedo, normal;
        cameraRay(px, py, rng, o, d);
        Vec3 L = radiance(o, d, rng, albedo, normal);
        if (!isFiniteColor(L)) L = Vec3(0.0f);
        float* a = &accum[pi * 3];
        a[0] += L.x;
        a[1] += L.y;
        a[2] += L.z;
        if (aov) {
          float* al = &accumAlbedo[pi * 3];
          float* nm = &accumNormal[pi * 3];
          al[0] += albedo.x; al[1] += albedo.y; al[2] += albedo.z;
          nm[0] += normal.x; nm[1] += normal.y; nm[2] += normal.z;
        }
      }
  });
  return !aborted;
}

void Renderer::Impl::publish(int passes) {
  std::lock_guard<std::mutex> lock(snapMutex);
  snapColor = accum;
  if (passes <= kAovPasses) {
    snapAlbedo = accumAlbedo;
    snapNormal = accumNormal;
    snapAovPasses = passes;
  }
  snapPasses = passes;
}

void Renderer::Impl::run() {
  startNs = nowNs();
  bool natural = true;
  for (int pass = 0; pass < targetPasses; ++pass) {
    if (stopFlag) { natural = false; break; }
    if (!renderPass(pass)) { natural = false; break; }
    publish(pass + 1);
    passesDone = pass + 1;
    elapsedNs = nowNs() - startNs;
    if (timeLimit > 0.0 && elapsedNs.load() * 1e-9 >= timeLimit) break;
  }
  elapsedNs = nowNs() - startNs;
  finished = natural;
  running = false;
}

// -------------------------------------------------------------------------------------------------
// Public API

Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}
Renderer::~Renderer() = default;

bool Renderer::setScene(const Scene& scene, std::string* error) {
  impl_->stop();
  return impl_->build(scene, error);
}

std::vector<std::string> Renderer::warnings() const { return impl_->warnings; }

void Renderer::setView(const RenderView& view) { impl_->view = view; }

void Renderer::setResolution(int width, int height) {
  impl_->overW = width;
  impl_->overH = height;
}

void Renderer::setMaxSamples(int spp) { impl_->overSpp = spp; }

void Renderer::setTimeLimit(double seconds) { impl_->overTime = seconds; }

void Renderer::start() {
  impl_->stop();
  if (!impl_->rtc) return;
  impl_->prepare();
  impl_->running = true;
  impl_->startNs = nowNs();
  impl_->controller = std::thread([this] { impl_->run(); });
}

void Renderer::stop() { impl_->stop(); }

bool Renderer::isRunning() const { return impl_->running; }

void Renderer::renderBlocking() {
  impl_->stop();
  if (!impl_->rtc) return;
  impl_->prepare();
  impl_->running = true;
  impl_->run();
}

RenderStats Renderer::stats() const {
  RenderStats s;
  s.passes = impl_->passesDone;
  s.targetPasses = impl_->targetPasses;
  s.running = impl_->running;
  s.finished = impl_->finished;
  s.triangles = impl_->triangles;
  int64_t ns = s.running ? nowNs() - impl_->startNs.load() : impl_->elapsedNs.load();
  s.seconds = static_cast<double>(ns) * 1e-9;
  return s;
}

int Renderer::width() const { return impl_->w > 0 ? impl_->w : impl_->resW(); }
int Renderer::height() const { return impl_->h > 0 ? impl_->h : impl_->resH(); }

void Renderer::resolveHdr(std::vector<float>& rgb) const {
  const Impl& I = *impl_;
  std::lock_guard<std::mutex> lock(I.snapMutex);
  size_t n = static_cast<size_t>(std::max(I.w, 0)) * std::max(I.h, 0) * 3;
  rgb.assign(n, 0.0f);
  if (I.snapPasses <= 0) return;
  float inv = 1.0f / static_cast<float>(I.snapPasses);
  for (size_t i = 0; i < n && i < I.snapColor.size(); ++i) rgb[i] = I.snapColor[i] * inv;
}

void Renderer::resolve(const DisplaySettings& display, std::vector<uint8_t>& rgba) const {
  const Impl& I = *impl_;
  int w = std::max(I.w, 0), h = std::max(I.h, 0);
  if (w == 0 || h == 0) {
    rgba.clear();
    return;
  }
  std::vector<float> color;
  std::vector<float> albedo, normal;
  int passes = 0;
  bool wantDenoise = display.denoise && denoiserAvailable();
  {
    std::lock_guard<std::mutex> lock(I.snapMutex);
    passes = I.snapPasses;
    color = I.snapColor;
    if (wantDenoise && passes > 0) {
      float invA = 1.0f / static_cast<float>(std::max(I.snapAovPasses, 1));
      albedo.resize(I.snapAlbedo.size());
      normal.resize(I.snapNormal.size());
      for (size_t i = 0; i < albedo.size(); ++i) {
        albedo[i] = I.snapAlbedo[i] * invA;
        normal[i] = I.snapNormal[i] * invA;
      }
    }
  }
  if (passes > 0) {
    float inv = 1.0f / static_cast<float>(passes);
    for (auto& c : color) c *= inv;
  }
  if (wantDenoise && passes > 0) {
    std::lock_guard<std::mutex> lock(I.denoiseMutex);
    if (I.denoisedPasses != passes || I.denoised.size() != color.size()) {
      std::vector<float> out;
      std::string err;
      if (denoiseImage(color, albedo, normal, w, h, out, &err)) {
        I.denoised = std::move(out);
        I.denoisedPasses = passes;
      } else {
        I.denoised.clear();
        I.denoisedPasses = -1;
      }
    }
    if (I.denoisedPasses == passes) color = I.denoised;
  }
  tonemapToRgba(color.data(), w, h, display, rgba);
}

bool Renderer::denoiserAvailable() {
  static const bool available = oidnAvailable();
  return available;
}

}  // namespace veraa
