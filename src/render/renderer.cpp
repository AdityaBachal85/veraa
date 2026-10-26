// Sampling engine: progressive passes or buckets, adaptive sampling by noise threshold, render
// regions, render elements, Light Mix and the public Renderer API.
#include "renderer.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "postprocess.h"
#include "render_impl.h"
#include "rendersettings.h"

namespace veraa {

using namespace render;

namespace {

constexpr int kTileSize = 32;
constexpr int kAovPasses = 16;  // samples averaged into the albedo/normal guides

int64_t nowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Setup

void Renderer::Impl::prepare() {
  w = resW();
  h = resH();
  size_t np = static_cast<size_t>(w) * h;
  accum.reset(np * 3);
  accumAlbedo.reset(np * 3);
  accumNormal.reset(np * 3);
  accumAlpha.reset(np);
  lumSum.reset(np);
  lumSq.reset(np);
  counts.reset(np);
  aovCounts.reset(np);
  const auto& infos = renderElementInfos();
  bool wantZ = false, wantIds = false;
  elemBuf.clear();
  elemBuf.resize(infos.size());
  for (size_t i = 0; i < infos.size(); ++i) {
    if (!elemEnabled[i]) continue;
    std::string key = infos[i].key;
    if (key == "ZDepth") wantZ = true;
    if (key == "ObjectID" || key == "MaterialID" || key == "WireColor") wantIds = true;
    if (key == "DiffuseFilter" || key == "Normals" || key == "Alpha" || key == "ZDepth" || key == "ObjectID" ||
        key == "MaterialID" || key == "WireColor")
      continue;  // derived from the guide buffers
    elemBuf[i].reset(np * 3);
  }
  zbuf.reset(wantZ ? np : 0);
  objId.reset(wantIds ? np * 3 : 0);
  matId.reset(wantIds ? np * 3 : 0);
  wire.reset(wantIds ? np * 3 : 0);
  groupBuf.clear();
  if (lightMixOn) {
    groupBuf.resize(std::min<size_t>(groupNames.size(), kMaxLightGroups));
    for (auto& g : groupBuf) g.reset(np * 3);
  }
  converged.assign(np, 0);
  idSet.assign(wantIds ? np : 0, 0);
  version = 0;
  {
    std::lock_guard<std::mutex> lock(denoiseMutex);
    denoisedVersion = -1;
    denoised.clear();
  }
  targetPasses = std::max(1, overSpp > 0 ? overSpp : sceneSpp);
  timeLimit = overTime >= 0.0 ? overTime : sceneTime;

  // Render region (pixel rectangle).
  bool regionOn = overRegion || sceneRegion;
  const float* box = overRegion ? overRegionBox : sceneRegionBox;
  rx0 = 0;
  ry0 = 0;
  rx1 = w;
  ry1 = h;
  if (regionOn) {
    rx0 = std::clamp(static_cast<int>(std::floor(std::min(box[0], box[2]) * w)), 0, w - 1);
    ry0 = std::clamp(static_cast<int>(std::floor(std::min(box[1], box[3]) * h)), 0, h - 1);
    rx1 = std::clamp(static_cast<int>(std::ceil(std::max(box[0], box[2]) * w)), rx0 + 1, w);
    ry1 = std::clamp(static_cast<int>(std::ceil(std::max(box[1], box[3]) * h)), ry0 + 1, h);
  }

  // Camera.
  cam = RenderCamera();
  auto basis = [](const Vec3& pos, const Vec3& target, const Vec3& up, Vec3& f, Vec3& r, Vec3& u) {
    f = normalize(target - pos);
    Vec3 rr = cross(f, up);
    if (length2(rr) < 1e-12f) rr = cross(f, std::fabs(f.z) < 0.99f ? Vec3(0, 0, 1) : Vec3(0, 1, 0));
    r = normalize(rr);
    u = cross(r, f);
  };
  cam.pos = view.position;
  basis(view.position, view.target, view.up, cam.f, cam.r, cam.u);
  cam.motionBlur = view.motionBlur && view.shutterClose > view.shutterOpen;
  if (cam.motionBlur && view.cameraMoves) {
    cam.moves = true;
    cam.pos1 = view.positionClose;
    basis(view.positionClose, view.targetClose, view.upClose, cam.f1, cam.r1, cam.u1);
  }
  cam.aspect = static_cast<float>(w) / static_cast<float>(h);
  cam.tanHalf = std::tan(radians(clampf(view.fovYDeg, 0.01f, 179.0f)) * 0.5f);
  cam.ortho = view.orthographic;
  cam.halfH = view.orthoHeight * 0.5f;
  cam.aperture = view.apertureRadius;
  cam.focus = std::max(view.focusDistance, 1e-3f);
  cam.blades = view.blades;
  cam.bladeRot = radians(view.bladeRotationDeg);
  cam.anisotropy = clampf(view.bokehAnisotropy, -0.99f, 0.99f);
  cam.distortion = view.distortion;
  cam.shiftX = view.shiftX;
  cam.shiftY = view.shiftY;
  cam.nearClip = view.nearClip;
  cam.farClip = view.farClip;
  cam.vignetting = view.vignetting;
  bool cameraExposure = view.exposure != 1.0f;
  cam.scale = (cameraExposure ? Vec3(view.exposure) * view.whiteBalance : Vec3(physicalExposure) * settingsWhiteBalance);
  if (cameraExposure && view.whiteBalance == Vec3(1.0f)) cam.scale = Vec3(view.exposure);
  clampUnit = 1.0f / std::max(luminance(cam.scale), 1e-20f);

  if (!pool) {
    int threads = static_cast<int>(std::thread::hardware_concurrency());
    pool = std::make_unique<ThreadPool>(threads > 0 ? threads : 4);
  }
  passesDone = 0;
  elapsedNs = 0;
  samplesTaken = 0;
  convergedFraction = 0.0f;
  finished = false;
  stopFlag = false;
  {
    std::lock_guard<std::mutex> lock(bucketMutex);
    activeBuckets.clear();
  }
}

// ---------------------------------------------------------------------------------------------
// Sampling

void Renderer::Impl::samplePixel(int x, int y, int sampleIndex, Rng& rng) {
  size_t pi = static_cast<size_t>(y) * w + x;
  float px = x + 0.5f + 0.75f * sampleTent(rng.next());
  float py = y + 0.5f + 0.75f * sampleTent(rng.next());
  Vec3 o, d;
  float weight, time, tNear, tFar;
  cameraRay(px, py, rng, o, d, weight, time, tNear, tFar);
  PathOutput out;
  trace(o, d, tNear, tFar, time, rng, out);
  Vec3 scale = cam.scale * weight;
  Vec3 L = out.L * scale;
  if (!isFiniteColor(L)) L = Vec3(0.0f);
  accum.add3(pi, L);
  float lum = luminance(L);
  lumSum.add(pi, lum);
  lumSq.add(pi, lum * lum);
  accumAlpha.add(pi, out.alpha);
  if (sampleIndex < kAovPasses) {
    accumAlbedo.add3(pi, out.albedo);
    accumNormal.add3(pi, out.normal);
    aovCounts.add(pi, 1.0f);
  }
  if (anyBeautyElement)
    for (int e = 0; e < kBeautyElements; ++e)
      if (!elemBuf[e].empty()) {
        Vec3 v = out.elem[e] * scale;
        if (isFiniteColor(v)) elemBuf[e].add3(pi, v);
      }
  if (!groupBuf.empty())
    for (size_t g = 0; g < groupBuf.size(); ++g) {
      Vec3 v = out.group[g] * scale;
      if (isFiniteColor(v)) groupBuf[g].add3(pi, v);
    }
  if (!zbuf.empty()) zbuf.add(pi, out.hit ? out.depth : 0.0f);
  if (!objId.empty() && out.hit && !idSet[pi]) {
    idSet[pi] = 1;
    auto idColor = [](uint32_t id) {
      if (id == 0) return Vec3(0.0f);
      uint32_t hsh = hashU32(id * 2654435761u + 17u);
      return hsvToRgb(Vec3(hashFloat(hsh), 0.55f + 0.4f * hashFloat(hsh + 1), 0.6f + 0.4f * hashFloat(hsh + 2)));
    };
    uint32_t oid = out.gBufferId > 0 ? static_cast<uint32_t>(out.gBufferId) * 7919u : out.nodeId;
    Vec3 oc = idColor(oid), mc = idColor(out.sceneMaterialId);
    for (int c = 0; c < 3; ++c) {
      objId.set(pi * 3 + c, oc[c]);
      matId.set(pi * 3 + c, mc[c]);
      wire.set(pi * 3 + c, out.hit ? out.wireColor[c] : 0.0f);
    }
  }
  counts.add(pi, 1.0f);
}

float Renderer::Impl::pixelNoise(size_t pi) const {
  float n = counts.get(pi);
  if (n < 2.0f) return 1e9f;
  float mean = lumSum.get(pi) / n;
  float var = std::max(0.0f, lumSq.get(pi) / n - mean * mean) * n / (n - 1.0f);
  float se = std::sqrt(var / n);
  return se / (mean + 0.05f);
}

void Renderer::Impl::updateConvergence(int x0, int y0, int x1, int y1) {
  if (noiseThreshold <= 0.0f) return;
  std::vector<float> noise(static_cast<size_t>(x1 - x0) * (y1 - y0));
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x < x1; ++x) noise[static_cast<size_t>(y - y0) * (x1 - x0) + (x - x0)] = pixelNoise(static_cast<size_t>(y) * w + x);
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x < x1; ++x) {
      size_t pi = static_cast<size_t>(y) * w + x;
      if (converged[pi] || counts.get(pi) < static_cast<float>(minSamples)) continue;
      float worst = 0.0f;
      for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
          int xx = std::clamp(x + dx, x0, x1 - 1), yy = std::clamp(y + dy, y0, y1 - 1);
          worst = std::max(worst, noise[static_cast<size_t>(yy - y0) * (x1 - x0) + (xx - x0)]);
        }
      if (worst < noiseThreshold) converged[pi] = 1;
    }
}

bool Renderer::Impl::timeUp() const {
  return timeLimit > 0.0 && static_cast<double>(nowNs() - startNs.load()) * 1e-9 >= timeLimit;
}

void Renderer::Impl::runProgressive() {
  int tilesX = (rx1 - rx0 + kTileSize - 1) / kTileSize, tilesY = (ry1 - ry0 + kTileSize - 1) / kTileSize;
  size_t regionPixels = static_cast<size_t>(rx1 - rx0) * (ry1 - ry0);
  bool natural = true;
  for (int pass = 0; pass < targetPasses; ++pass) {
    if (stopFlag) {
      natural = false;
      break;
    }
    std::atomic<bool> aborted{false};
    std::atomic<int64_t> taken{0};
    pool->run(tilesX * tilesY, [&](int tile, int) {
      if (stopFlag.load(std::memory_order_relaxed)) {
        aborted = true;
        return;
      }
      int x0 = rx0 + (tile % tilesX) * kTileSize, y0 = ry0 + (tile / tilesX) * kTileSize;
      int x1 = std::min(x0 + kTileSize, rx1), y1 = std::min(y0 + kTileSize, ry1);
      int64_t n = 0;
      for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
          size_t pi = static_cast<size_t>(y) * w + x;
          if (converged[pi]) continue;
          Rng rng(hash64((static_cast<uint64_t>(pass) << 32) ^ pi ^ 0x9E3779B97F4A7C15ULL), pi);
          samplePixel(x, y, pass, rng);
          ++n;
        }
      taken += n;
    });
    samplesTaken += taken.load();
    if (aborted) {
      natural = false;
      ++version;
      break;
    }
    passesDone = pass + 1;
    ++version;
    elapsedNs = nowNs() - startNs;
    if (noiseThreshold > 0.0f && pass + 1 >= minSamples && ((pass + 1) % 4 == 0 || pass + 1 == minSamples)) {
      updateConvergence(rx0, ry0, rx1, ry1);
      size_t done = 0;
      for (int y = ry0; y < ry1; ++y)
        for (int x = rx0; x < rx1; ++x) done += converged[static_cast<size_t>(y) * w + x];
      convergedFraction = regionPixels ? static_cast<float>(done) / regionPixels : 1.0f;
      if (done == regionPixels) break;
    }
    if (timeUp()) break;
  }
  finished = natural;
}

void Renderer::Impl::runBuckets() {
  int bs = std::max(bucketSize, 8);
  int bx = (rx1 - rx0 + bs - 1) / bs, by = (ry1 - ry0 + bs - 1) / bs;
  // Spiral order from the image center.
  std::vector<std::array<int, 2>> order;
  for (int y = 0; y < by; ++y)
    for (int x = 0; x < bx; ++x) order.push_back({x, y});
  float cx = (bx - 1) * 0.5f, cy = (by - 1) * 0.5f;
  std::stable_sort(order.begin(), order.end(), [&](const auto& a, const auto& b) {
    float da = std::max(std::fabs(a[0] - cx), std::fabs(a[1] - cy)), db = std::max(std::fabs(b[0] - cx), std::fabs(b[1] - cy));
    if (da != db) return da < db;
    return std::atan2(a[1] - cy, a[0] - cx) < std::atan2(b[1] - cy, b[0] - cx);
  });
  std::atomic<bool> aborted{false};
  std::atomic<int> bucketsDone{0};
  size_t regionPixels = static_cast<size_t>(rx1 - rx0) * (ry1 - ry0);
  std::atomic<size_t> convergedPixels{0};
  pool->run(static_cast<int>(order.size()), [&](int task, int) {
    if (stopFlag.load(std::memory_order_relaxed) || timeUp()) {
      aborted = true;
      return;
    }
    int x0 = rx0 + order[task][0] * bs, y0 = ry0 + order[task][1] * bs;
    int x1 = std::min(x0 + bs, rx1), y1 = std::min(y0 + bs, ry1);
    std::array<int, 4> box{x0, y0, x1, y1};
    {
      std::lock_guard<std::mutex> lock(bucketMutex);
      activeBuckets.insert(box);
    }
    int64_t n = 0;
    for (int s = 0; s < targetPasses; ++s) {
      if (stopFlag.load(std::memory_order_relaxed)) {
        aborted = true;
        break;
      }
      bool any = false;
      for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
          size_t pi = static_cast<size_t>(y) * w + x;
          if (converged[pi]) continue;
          Rng rng(hash64((static_cast<uint64_t>(s) << 32) ^ pi ^ 0x9E3779B97F4A7C15ULL), pi);
          samplePixel(x, y, s, rng);
          any = true;
          ++n;
        }
      if (!any) break;
      if (noiseThreshold > 0.0f && s + 1 >= minSamples && ((s + 1) % 4 == 0 || s + 1 == minSamples))
        updateConvergence(x0, y0, x1, y1);
      if ((s & 7) == 7) ++version;
    }
    size_t c = 0;
    for (int y = y0; y < y1; ++y)
      for (int x = x0; x < x1; ++x) c += converged[static_cast<size_t>(y) * w + x];
    convergedPixels += c;
    samplesTaken += n;
    {
      std::lock_guard<std::mutex> lock(bucketMutex);
      activeBuckets.erase(box);
    }
    ++bucketsDone;
    ++version;
    elapsedNs = nowNs() - startNs;
  });
  convergedFraction = regionPixels ? static_cast<float>(convergedPixels.load()) / regionPixels : 1.0f;
  int maxCount = 0;
  for (int y = ry0; y < ry1; ++y)
    for (int x = rx0; x < rx1; ++x) maxCount = std::max(maxCount, static_cast<int>(counts.get(static_cast<size_t>(y) * w + x)));
  passesDone = maxCount;
  finished = !aborted;
  ++version;
}

void Renderer::Impl::run() {
  startNs = nowNs();
  if (samplerType == 1) runBuckets();
  else runProgressive();
  elapsedNs = nowNs() - startNs;
  running = false;
}

// ---------------------------------------------------------------------------------------------
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

void Renderer::setRegion(bool enabled, float x0, float y0, float x1, float y1) {
  impl_->overRegion = enabled;
  impl_->overRegionBox[0] = x0;
  impl_->overRegionBox[1] = y0;
  impl_->overRegionBox[2] = x1;
  impl_->overRegionBox[3] = y1;
}

void Renderer::setRenderOnly(const std::vector<uint32_t>& nodeIds) {
  impl_->overSelected = nodeIds;
  std::sort(impl_->overSelected.begin(), impl_->overSelected.end());
}

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
  const Impl& I = *impl_;
  RenderStats s;
  s.passes = I.passesDone;
  s.targetPasses = I.targetPasses;
  s.running = I.running;
  s.finished = I.finished;
  s.triangles = I.triangles;
  int64_t ns = s.running ? nowNs() - I.startNs.load() : I.elapsedNs.load();
  s.seconds = static_cast<double>(ns) * 1e-9;
  s.converged = I.convergedFraction;
  s.samplesPerSecond = s.seconds > 0.0 ? static_cast<double>(I.samplesTaken.load()) / s.seconds : 0.0;
  {
    std::lock_guard<std::mutex> lock(I.bucketMutex);
    s.buckets.assign(I.activeBuckets.begin(), I.activeBuckets.end());
  }
  if (I.samplerType == 1 && s.running) {
    // Report progress as the highest sample count so far.
    int maxCount = 0;
    for (int y = I.ry0; y < I.ry1; y += 7)
      for (int x = I.rx0; x < I.rx1; x += 7)
        maxCount = std::max(maxCount, static_cast<int>(I.counts.get(static_cast<size_t>(y) * I.w + x)));
    s.passes = maxCount;
  }
  return s;
}

int Renderer::width() const { return impl_->w > 0 ? impl_->w : impl_->resW(); }
int Renderer::height() const { return impl_->h > 0 ? impl_->h : impl_->resH(); }

void Renderer::resolveHdr(std::vector<float>& rgb) const {
  const Impl& I = *impl_;
  size_t np = static_cast<size_t>(std::max(I.w, 0)) * std::max(I.h, 0);
  rgb.assign(np * 3, 0.0f);
  if (I.accum.empty()) return;
  for (size_t i = 0; i < np; ++i) {
    float n = I.counts.get(i);
    if (n <= 0.0f) continue;
    float inv = 1.0f / n;
    for (int c = 0; c < 3; ++c) rgb[i * 3 + c] = I.accum.get(i * 3 + c) * inv;
  }
}

void Renderer::resolveAlpha(std::vector<float>& alpha) const {
  const Impl& I = *impl_;
  size_t np = static_cast<size_t>(std::max(I.w, 0)) * std::max(I.h, 0);
  alpha.assign(np, 0.0f);
  if (I.accumAlpha.empty()) return;
  for (size_t i = 0; i < np; ++i) {
    float n = I.counts.get(i);
    alpha[i] = n > 0.0f ? clampf(I.accumAlpha.get(i) / n, 0.0f, 1.0f) : 0.0f;
  }
}

void Renderer::resolveSampleCounts(std::vector<float>& out) const {
  const Impl& I = *impl_;
  size_t np = static_cast<size_t>(std::max(I.w, 0)) * std::max(I.h, 0);
  out.assign(np, 0.0f);
  if (I.counts.empty()) return;
  for (size_t i = 0; i < np; ++i) out[i] = I.counts.get(i);
}

std::vector<std::string> Renderer::elementNames() const {
  const Impl& I = *impl_;
  std::vector<std::string> names;
  const auto& infos = renderElementInfos();
  for (size_t i = 0; i < infos.size() && i < 32; ++i)
    if (I.elemEnabled[i]) names.push_back(infos[i].key);
  return names;
}

bool Renderer::resolveElement(const std::string& name, std::vector<float>& rgb) const {
  const Impl& I = *impl_;
  const auto& infos = renderElementInfos();
  int idx = -1;
  for (size_t i = 0; i < infos.size(); ++i)
    if (name == infos[i].key) idx = static_cast<int>(i);
  if (idx < 0 || !I.elemEnabled[idx] || I.counts.empty()) return false;
  size_t np = static_cast<size_t>(I.w) * I.h;
  rgb.assign(np * 3, 0.0f);
  auto avg = [&](const AccumBuffer& b, size_t i, int c, bool aov) {
    float n = aov ? I.aovCounts.get(i) : I.counts.get(i);
    return n > 0.0f ? b.get(i * 3 + c) / n : 0.0f;
  };
  for (size_t i = 0; i < np; ++i) {
    float n = I.counts.get(i);
    if (n <= 0.0f) continue;
    if (!I.elemBuf[idx].empty()) {
      for (int c = 0; c < 3; ++c) rgb[i * 3 + c] = I.elemBuf[idx].get(i * 3 + c) / n;
      continue;
    }
    if (name == "DiffuseFilter") {
      for (int c = 0; c < 3; ++c) rgb[i * 3 + c] = avg(I.accumAlbedo, i, c, true);
    } else if (name == "Normals") {
      for (int c = 0; c < 3; ++c) rgb[i * 3 + c] = avg(I.accumNormal, i, c, true) * 0.5f + 0.5f;
    } else if (name == "Alpha") {
      float a = clampf(I.accumAlpha.get(i) / n, 0.0f, 1.0f);
      rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = a;
    } else if (name == "ZDepth" && !I.zbuf.empty()) {
      float z = I.zbuf.get(i) / n;
      float v = z <= 0.0f ? 0.0f : 1.0f - clampf((z - I.zMin) / std::max(I.zMax - I.zMin, 1e-3f), 0.0f, 1.0f);
      rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = v;
    } else if (name == "ObjectID" && !I.objId.empty()) {
      for (int c = 0; c < 3; ++c) rgb[i * 3 + c] = I.objId.get(i * 3 + c);
    } else if (name == "MaterialID" && !I.matId.empty()) {
      for (int c = 0; c < 3; ++c) rgb[i * 3 + c] = I.matId.get(i * 3 + c);
    } else if (name == "WireColor" && !I.wire.empty()) {
      for (int c = 0; c < 3; ++c) rgb[i * 3 + c] = I.wire.get(i * 3 + c);
    }
  }
  return true;
}

std::vector<std::string> Renderer::lightGroupNames() const {
  const Impl& I = *impl_;
  if (I.groupBuf.empty()) return {};
  return std::vector<std::string>(I.groupNames.begin(), I.groupNames.begin() + I.groupBuf.size());
}

bool Renderer::resolveLightMix(const std::vector<LightMixGroup>& mix, std::vector<float>& rgb) const {
  const Impl& I = *impl_;
  if (I.groupBuf.empty()) return false;
  size_t np = static_cast<size_t>(I.w) * I.h;
  rgb.assign(np * 3, 0.0f);
  for (size_t g = 0; g < I.groupBuf.size(); ++g) {
    Vec3 k(1.0f);
    bool on = true;
    for (const auto& m : mix)
      if (m.name == I.groupNames[g]) {
        k = m.color * m.intensity;
        on = m.enabled;
      }
    if (!on || isBlack(k)) continue;
    for (size_t i = 0; i < np; ++i) {
      float n = I.counts.get(i);
      if (n <= 0.0f) continue;
      for (int c = 0; c < 3; ++c) rgb[i * 3 + c] += I.groupBuf[g].get(i * 3 + c) / n * k[c];
    }
  }
  return true;
}

void Renderer::resolveFinalHdr(const DisplaySettings& display, std::vector<float>& color) const {
  const Impl& I = *impl_;
  int w = std::max(I.w, 0), h = std::max(I.h, 0);
  if (!display.element.empty() && resolveElement(display.element, color)) return;
  if (!(display.useLightMix && resolveLightMix(display.lightMix, color))) resolveHdr(color);
  if (w == 0 || h == 0) return;
  bool wantDenoise = display.denoise && denoiserAvailable() && display.element.empty();
  if (!wantDenoise || I.counts.empty()) return;
  int ver = I.version.load();
  std::lock_guard<std::mutex> lock(I.denoiseMutex);
  // The cached result is reused while nothing changed (and Light Mix is not being edited).
  if (I.denoisedVersion != ver || I.denoised.size() != color.size() || display.useLightMix) {
    size_t np = static_cast<size_t>(w) * h;
    std::vector<float> albedo(np * 3), normal(np * 3);
    for (size_t i = 0; i < np; ++i) {
      float n = std::max(I.aovCounts.get(i), 1.0f);
      for (int c = 0; c < 3; ++c) {
        albedo[i * 3 + c] = I.accumAlbedo.get(i * 3 + c) / n;
        normal[i * 3 + c] = I.accumNormal.get(i * 3 + c) / n;
      }
    }
    std::vector<float> out;
    std::string err;
    if (denoiseImage(color, albedo, normal, w, h, out, &err)) {
      I.denoised = std::move(out);
      I.denoisedVersion = ver;
    } else {
      I.denoised.clear();
      I.denoisedVersion = -1;
    }
  }
  if (I.denoisedVersion >= 0 && I.denoised.size() == color.size()) color = I.denoised;
}

void Renderer::resolve(const DisplaySettings& display, std::vector<uint8_t>& rgba) const {
  const Impl& I = *impl_;
  int w = std::max(I.w, 0), h = std::max(I.h, 0);
  if (w == 0 || h == 0) {
    rgba.clear();
    return;
  }
  std::vector<float> color;
  resolveFinalHdr(display, color);
  if (!display.element.empty()) {
    // Data elements are shown as they are (no tone mapping beyond sRGB encoding).
    std::string e = display.element;
    if (e == "Normals" || e == "ZDepth" || e == "ObjectID" || e == "MaterialID" || e == "WireColor" || e == "Alpha" ||
        e == "DiffuseFilter") {
      DisplaySettings flat;
      flat.tonemap = 2;
      tonemapToRgba(color.data(), w, h, flat, rgba);
      return;
    }
  }
  postProcess(color, w, h, display);
  std::vector<float> alpha;  // color mapping "affect background" off: keep the background linear
  if (!display.mapAffectBackground && display.element.empty()) resolveAlpha(alpha);
  tonemapToRgba(color.data(), w, h, display, rgba, alpha.size() == color.size() / 3 ? alpha.data() : nullptr);
}

void toneMapImage(const std::vector<float>& rgb, int width, int height, const DisplaySettings& display,
                  std::vector<uint8_t>& rgba) {
  std::vector<float> copy = rgb;
  postProcess(copy, width, height, display);
  tonemapToRgba(copy.data(), width, height, display, rgba);
}

bool Renderer::denoiserAvailable() {
  static const bool available = oidnAvailable();
  return available;
}

}  // namespace veraa
