// Internal state of the renderer, shared by scene building (scenebuild.cpp), the path tracer
// (integrator.cpp) and the sampling engine (renderer.cpp).
#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <embree4/rtcore.h>

#include "bsdf.h"
#include "common.h"
#include "environment.h"
#include "lights.h"
#include "materials.h"
#include "renderer.h"
#include "texture.h"
#include "threadpool.h"

namespace veraa {
namespace render {

// Embree ray mask bits.
enum : unsigned {
  kMaskCamera = 1u,         // visible to camera rays
  kMaskSecondary = 2u,      // visible to reflection and refraction rays
  kMaskGI = 4u,             // visible to diffuse (GI) rays
  kMaskShadowOpaque = 8u,   // blocks shadow rays completely
  kMaskShadowTransp = 16u,  // shadow rays look at its material (cut-outs, glass)
};

// Lock-free accumulation buffer: one writer per pixel at a time, readers at any time.
class AccumBuffer {
 public:
  void reset(size_t n) {
    n_ = n;
    data_.reset(n ? new std::atomic<float>[n] : nullptr);
    for (size_t i = 0; i < n; ++i) data_[i].store(0.0f, std::memory_order_relaxed);
  }
  size_t size() const { return n_; }
  bool empty() const { return n_ == 0; }
  void add(size_t i, float v) { data_[i].store(data_[i].load(std::memory_order_relaxed) + v, std::memory_order_relaxed); }
  void set(size_t i, float v) { data_[i].store(v, std::memory_order_relaxed); }
  float get(size_t i) const { return data_[i].load(std::memory_order_relaxed); }
  void add3(size_t px, const Vec3& v) {
    add(px * 3, v.x);
    add(px * 3 + 1, v.y);
    add(px * 3 + 2, v.z);
  }
  Vec3 get3(size_t px) const { return {get(px * 3), get(px * 3 + 1), get(px * 3 + 2)}; }

 private:
  std::unique_ptr<std::atomic<float>[]> data_;
  size_t n_ = 0;
};

struct MeshData {
  std::vector<Vec3> P, N;
  std::vector<Vec2> UV;
  std::vector<uint32_t> idx;
  std::vector<int> matIds;      // per triangle
  std::vector<int> elements;    // per triangle (connected parts), when a map needs them
  std::vector<uint8_t> edgeMask;  // per triangle polygon edges (Edges map)
  RTCScene child = nullptr;     // object-space scene for instancing
  size_t triangles() const { return idx.size() / 3; }
};

struct CurveData {
  std::vector<float> v;              // x, y, z, radius per vertex
  std::vector<uint32_t> segments;    // first vertex of each segment
  std::vector<uint8_t> flags;        // neighbor flags per segment
  std::vector<int> strandOfSegment;
  std::vector<Vec2> rootUv;          // per strand
};

struct GeomRecord {
  enum class Kind { Mesh, Instance, Curves } kind = Kind::Mesh;
  int mesh = -1;
  int curves = -1;
  int material = 0;
  uint32_t nodeId = 0;
  uint32_t instanceId = 0;
  int gBufferId = 0;
  uint32_t sceneMaterialId = 0;
  Vec3 wireColor{0.6f, 0.6f, 0.6f};
  bool receiveShadows = true;
  unsigned mask = ~0u;
  // Object-to-world at shutter open and close (identity for world-space meshes).
  Mat4 M0, M1;
  bool motion = false;
  Mat4 worldToObject;  // for object-space shading coordinates of world-space meshes
  Vec3 objZ{0, 0, 1};
  int lightIndex = -1;  // emitting geometry of a mesh light
};

struct InfinitePlane {
  Vec3 p, n, tu, tv;
  float uvScale = 100.0f;
  int material = 0;
  uint32_t nodeId = 0;
  uint32_t sceneMaterialId = 0;
  int gBufferId = 0;
  Vec3 wireColor;
  unsigned mask = ~0u;
  bool receiveShadows = true;
};

struct SurfaceHit {
  Vec3 p, ng, ns;
  Vec2 uv;
  Vec3 dpdu, dpdv;
  Vec3 pObj, nObj, objZ{0, 0, 1};
  int material = 0;
  int geom = -1;      // GeomRecord index, or -2 - plane index for infinite planes
  unsigned prim = 0;
  Vec2 bary;
  uint32_t nodeId = 0, instanceId = 0, sceneMaterialId = 0;
  int gBufferId = 0;
  Vec3 wireColor;
  int faceMatId = 0, elementId = 0;
  int lightIndex = -1;
  bool receiveShadows = true;
  float t = 0.0f;
};

// Beauty components (order = the first entries of renderElementInfos()).
enum BeautyElement { kElemLighting, kElemGI, kElemSpecular, kElemReflection, kElemRefraction, kElemSSS,
                     kElemSelfIllum, kElemAtmosphere, kElemBackground, kBeautyElements };
constexpr int kMaxLightGroups = 32;

struct PathOutput {
  Vec3 L;
  Vec3 albedo, normal;
  float alpha = 0.0f;
  float depth = 0.0f;      // camera distance of the first hit (0 = none)
  bool hit = false;
  uint32_t nodeId = 0;
  uint32_t sceneMaterialId = 0;
  int gBufferId = 0;
  Vec3 wireColor;
  Vec3 elem[kBeautyElements];
  Vec3 group[kMaxLightGroups];
};

// Distance map support: sample points on surfaces in a uniform grid.
struct DistanceField {
  float cell = 1.0f;
  Vec3 origin;
  std::unordered_map<uint64_t, std::vector<Vec3>> cells;
  float query(const Vec3& p, float maxDist) const;
};

struct RenderCamera {
  Vec3 pos, f, r, u;
  Vec3 pos1, f1, r1, u1;  // at shutter close (motion blur)
  bool moves = false;
  float tanHalf = 0.36f, aspect = 1.0f, halfH = 100.0f;
  bool ortho = false;
  float aperture = 0.0f, focus = 300.0f;
  int blades = 0;
  float bladeRot = 0.0f, anisotropy = 0.0f;
  float distortion = 0.0f, shiftX = 0.0f, shiftY = 0.0f;
  float nearClip = 0.0f, farClip = 0.0f;
  float vignetting = 0.0f;
  Vec3 scale{1, 1, 1};  // exposure x white balance
  bool motionBlur = false;
};

}  // namespace render

struct Renderer::Impl : public render::Tracer {
  ~Impl() override;

  // ---- scene ----
  RTCScene rtc = nullptr;
  bool masksSupported = true;
  std::vector<render::MeshData> meshes;
  std::vector<render::CurveData> curves;
  std::vector<render::GeomRecord> records;     // by top-level geometry id
  std::vector<render::InfinitePlane> planes;
  std::vector<std::unique_ptr<render::ShadeMaterial>> materials;
  std::unique_ptr<render::TextureCache> textures = std::make_unique<render::TextureCache>();
  std::vector<render::Light> lights;
  std::vector<int> areaLights;   // analytic area lights hit by rays (rect, disc, sphere, portal)
  std::vector<float> lightCdf;   // power-based light selection
  std::vector<float> lightProb;
  bool sampleAllLights = true;
  bool hasPortals = false;
  std::vector<int> portals;
  render::Environment env;       // lighting
  render::Environment envBack;   // camera background when it differs (invisible dome light)
  bool separateBackground = false;
  int envGroup = 0;
  std::vector<std::string> groupNames;
  std::vector<render::DistanceField> distanceFields;
  bool anyShadowTransparent = false;
  bool anyLightLinks = false;
  size_t triangles = 0;
  std::vector<std::string> warnings;
  Vec3 keyLightDir{0, 0, 1};

  // ---- settings ----
  int sceneW = 1280, sceneH = 720, sceneSpp = 512;
  double sceneTime = 0.0;
  int maxDepth = 8;
  float clampIndirect = 10.0f, clampDirect = 0.0f, giMultiplier = 1.0f;
  bool caustics = false;
  int samplerType = 0, minSamples = 16, bucketSize = 48;
  float noiseThreshold = 0.0f;
  bool sceneRegion = false;
  float sceneRegionBox[4] = {0, 0, 1, 1};
  bool fogOn = false;
  int fogType = 0;
  Vec3 fogAlbedo{1, 1, 1}, fogEmission;
  float fogDensity = 0.0f, fogHeight = 200.0f, fogFalloff = 1.0f, fogG = 0.0f, fogBase = 0.0f;
  Aabb fogBox;  // fog fills the scene's bounding volume
  bool fogAffectBackground = true;
  float physicalExposure = 1.0f;  // render-settings EV100 exposure control
  Vec3 settingsWhiteBalance{1, 1, 1};
  float zMin = 0.0f, zMax = 2000.0f;
  bool elemEnabled[32] = {};
  bool anyBeautyElement = false;
  bool lightMixOn = false;
  int overW = 0, overH = 0, overSpp = 0;
  double overTime = -1.0;
  bool overRegion = false;
  float overRegionBox[4] = {0, 0, 1, 1};
  RenderView view;
  render::RenderCamera cam;

  // ---- buffers ----
  int w = 0, h = 0;
  int rx0 = 0, ry0 = 0, rx1 = 0, ry1 = 0;  // rendered pixel rectangle
  render::AccumBuffer accum, accumAlbedo, accumNormal, accumAlpha, lumSum, lumSq, counts, aovCounts;
  render::AccumBuffer zbuf, objId, matId, wire;
  std::vector<render::AccumBuffer> elemBuf;   // per renderElementInfos() entry (empty when off)
  std::vector<render::AccumBuffer> groupBuf;  // light mix groups
  std::vector<uint8_t> converged;
  std::vector<uint8_t> idSet;  // ID elements come from the first sample that hits a surface
  std::atomic<int> version{0};
  mutable std::mutex denoiseMutex;
  mutable std::vector<float> denoised;
  mutable int denoisedVersion = -1;
  mutable std::mutex bucketMutex;
  std::set<std::array<int, 4>> activeBuckets;

  // ---- control ----
  std::atomic<bool> stopFlag{false}, running{false}, finished{false};
  std::atomic<int> passesDone{0};
  std::atomic<int64_t> startNs{0}, elapsedNs{0};
  std::atomic<int64_t> samplesTaken{0};
  std::atomic<float> convergedFraction{0.0f};
  int targetPasses = 0;
  double timeLimit = 0.0;
  std::thread controller;
  std::unique_ptr<render::ThreadPool> pool;

  int resW() const { return overW > 0 ? overW : sceneW; }
  int resH() const { return overH > 0 ? overH : sceneH; }

  void releaseScene();
  void stop();
  bool build(const Scene& scene, std::string* error);
  void prepare();
  void run();
  bool timeUp() const;
  void runProgressive();
  void runBuckets();
  void samplePixel(int x, int y, int sampleIndex, render::Rng& rng);
  void updateConvergence(int x0, int y0, int x1, int y1);
  float pixelNoise(size_t pi) const;

  // ---- tracing ----
  bool intersect(const Vec3& o, const Vec3& d, float tNear, float tFar, unsigned mask, float time, RTCRayHit& rh,
                 int& planeHit) const;
  void surface(const RTCRayHit& rh, int planeHit, const Vec3& o, const Vec3& d, float time,
               render::SurfaceHit& s) const;
  // Light from o toward d reaching tMax (shadow rays): opaque blockers, cut-outs and glass.
  Vec3 transmittance(Vec3 o, const Vec3& d, float tMax, float time, const render::Light* light) const;
  void cameraRay(float px, float py, render::Rng& rng, Vec3& o, Vec3& d, float& weight, float& time,
                 float& tNear, float& tFar) const;
  render::ShadeContext makeContext(const render::SurfaceHit& s, const Vec3& wo, render::RayKind kind, int depth,
                                   render::Rng& rng, float time, float footprint) const;
  void trace(Vec3 o, Vec3 d, float tNear, float tFar, float time, render::Rng& rng, render::PathOutput& out) const;
  bool randomWalk(const render::SurfaceHit& entry, const render::SubsurfaceParams& sss, const Vec3& dirIn,
                  float time, render::Rng& rng, render::SurfaceHit& exitHit, Vec3& exitDir, Vec3& weight) const;
  float fogTransmittanceTau(const Vec3& o, const Vec3& d, float t) const;
  bool sampleFog(const Vec3& o, const Vec3& d, float tMax, float u, float& t) const;
  int pickLight(float u, float& prob) const;
  float lightPickProb(int index) const;

  // Tracer (texture maps)
  float traceDistance(const Vec3& o, const Vec3& d, float tMax, uint32_t* nodeId) const override;
  bool triangle(int geom, unsigned prim, Vec3 corners[3], unsigned& edgeMask) const override;
  float distanceTo(int field, const Vec3& p, float maxDist) const override;
};

}  // namespace veraa
