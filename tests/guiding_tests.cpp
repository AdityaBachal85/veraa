// Sampling quality tests: the path guiding distributions (learning, sampling and densities), the
// light tree (pick probabilities, conservative bounds, unbiased against sampling every light),
// guided rendering (unbiased, less noise at equal samples) and the quality presets.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#include "guiding.h"
#include "lights.h"
#include "lighttree.h"
#include "renderer.h"
#include "rendersettings.h"
#include "scene.h"

using namespace veraa;
using namespace veraa::render;

static int g_failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++g_failures;                                                \
    }                                                              \
  } while (0)

static Vec3 uniformSphere(Rng& rng) { return sampleUniformSphere(rng.next(), rng.next()); }

// ---------------------------------------------------------------------------------------------
// Path guiding distributions

static void testGuideLearning() {
  PathGuide g;
  Aabb box;
  box.expand(Vec3(-100.0f));
  box.expand(Vec3(100.0f));
  g.reset(box);
  CHECK(!g.ready());
  // Radiance field: bright inside a 15 degree cone, dim elsewhere. Directions are sampled
  // uniformly, so each record is radiance * 4 pi.
  Vec3 target = normalize(Vec3(0.3f, -0.5f, 0.8f));
  float cosCone = std::cos(radians(15.0f));
  Rng rng(7, 3);
  for (int it = 0; it < 5; ++it) {
    int n = 20000 << it;
    for (int i = 0; i < n; ++i) {
      Vec3 p(rng.next() * 200.0f - 100.0f, rng.next() * 200.0f - 100.0f, rng.next() * 200.0f - 100.0f);
      Vec3 d = uniformSphere(rng);
      float L = dot(d, target) > cosCone ? 10.0f : 0.1f;
      g.record(p, d, L * 4.0f * kPi, rng);
    }
    g.update();
  }
  CHECK(g.ready());
  CHECK(g.regionCount() > 1);  // the records were enough to split space
  const PathGuide::Region* r = g.region(Vec3(10.0f, -20.0f, 5.0f));
  CHECK(PathGuide::canSample(r));
  float uniformPdf = 1.0f / (4.0f * kPi);
  CHECK(PathGuide::pdf(r, target) > 10.0f * uniformPdf);
  CHECK(PathGuide::pdf(r, -target) < uniformPdf);

  // The density integrates to one, and E[1/pdf] over its samples is the sphere's solid angle.
  double integral = 0.0, invPdf = 0.0, inCone = 0.0, coneMass = 0.0;
  const int N = 200000;
  for (int i = 0; i < N; ++i) {
    Vec3 d = uniformSphere(rng);
    float pd = PathGuide::pdf(r, d);
    integral += pd * 4.0 * kPi;
    if (dot(d, target) > cosCone) coneMass += pd * 4.0 * kPi;
    float ps;
    Vec3 s = PathGuide::sample(r, rng, ps);
    CHECK(ps > 0.0f);
    if (ps > 0.0f) invPdf += 1.0 / ps;
    if (dot(s, target) > cosCone) inCone += 1.0;
    if (i < 64) CHECK(std::fabs(ps - PathGuide::pdf(r, s)) <= 1e-3f * ps);  // sample() reports pdf()
  }
  integral /= N;
  invPdf /= N;
  inCone /= N;
  coneMass /= N;
  // Analytic share of the energy inside the cone: 10 * cone / (10 * cone + 0.1 * rest) = 0.634.
  std::printf("  guide: integral %.4f, E[1/pdf] %.3f (4 pi = %.3f), samples in cone %.3f (density %.3f, exact 0.634)\n",
              integral, invPdf, 4.0 * kPi, inCone, coneMass);
  CHECK(std::fabs(integral - 1.0) < 0.02);
  CHECK(std::fabs(invPdf / (4.0 * kPi) - 1.0) < 0.03);
  CHECK(std::fabs(inCone - coneMass) < 0.01);
  CHECK(inCone > 0.5 && inCone < 0.75);
}

// ---------------------------------------------------------------------------------------------
// Light tree

static std::vector<Light> randomLights(int count, Rng& rng) {
  std::vector<Light> lights;
  for (int i = 0; i < count; ++i) {
    Light l;
    l.position = Vec3(rng.next() * 800.0f - 400.0f, rng.next() * 800.0f - 400.0f, 50.0f + rng.next() * 250.0f);
    l.color = Vec3(0.5f + rng.next() * 4.0f);
    Vec3 down = normalize(Vec3(rng.next() - 0.5f, rng.next() - 0.5f, -1.5f));
    switch (i % 6) {
      case 0:
      case 1: {
        l.type = i % 6 == 0 ? Light::Type::Rect : Light::Type::Disc;
        Frame f(down);
        l.ex = f.t * (5.0f + rng.next() * 30.0f);
        l.ey = f.b * (5.0f + rng.next() * 30.0f);
        l.n = down;
        l.area = (l.type == Light::Type::Disc ? kPi : 4.0f) * length(l.ex) * length(l.ey);
        l.doubleSided = rng.next() < 0.3f;
        break;
      }
      case 2:
        l.type = Light::Type::Sphere;
        l.radius = 3.0f + rng.next() * 20.0f;
        break;
      case 3:
        l.type = Light::Type::Omni;
        l.radius = rng.next() < 0.5f ? 0.0f : 5.0f;
        l.color *= 1e-3f;
        break;
      case 4:
        l.type = Light::Type::Spot;
        l.dir = down;
        l.cosHot = std::cos(radians(10.0f + rng.next() * 20.0f));
        l.cosFall = std::cos(std::acos(l.cosHot) + radians(5.0f + rng.next() * 15.0f));
        l.radius = rng.next() < 0.5f ? 0.0f : 4.0f;
        l.color *= 1e-3f;
        break;
      default: {
        // Small mesh light: two triangles of a tilted quad, one-sided.
        l.type = Light::Type::Mesh;
        Frame f(down);
        Vec3 a = l.position - f.t * 10.0f - f.b * 10.0f, b = l.position + f.t * 10.0f - f.b * 10.0f,
             c = l.position + f.t * 10.0f + f.b * 10.0f, d = l.position - f.t * 10.0f + f.b * 10.0f;
        // Winding so the emitting side (cross(b - a, c - a)) faces down.
        if (dot(cross(b - a, c - a), down) < 0.0f) std::swap(b, d);
        l.tri = {a, b, c, a, c, d};
        l.triCdf = {0.5f, 1.0f};
        l.area = 400.0f;
        break;
      }
    }
    lights.push_back(l);
  }
  return lights;
}

static void testLightTreePicks() {
  Rng rng(11, 5);
  std::vector<Light> lights = randomLights(36, rng);
  std::vector<int> idx;
  for (int i = 0; i < static_cast<int>(lights.size()); ++i) idx.push_back(i);
  LightTree tree;
  tree.build(lights, idx, nullptr);
  CHECK(!tree.empty());
  int conservativeFails = 0, probMismatch = 0;
  double worstFreq = 0.0;
  for (int t = 0; t < 12; ++t) {
    Vec3 p(rng.next() * 900.0f - 450.0f, rng.next() * 900.0f - 450.0f, rng.next() * 120.0f);
    Vec3 n = t % 4 == 3 ? Vec3(0.0f) : normalize(Vec3(rng.next() - 0.5f, rng.next() - 0.5f, 1.0f));
    bool twoSided = t % 3 == 2;
    // Every light that can reach the receiver must be pickable (bounds are conservative).
    for (int li = 0; li < static_cast<int>(lights.size()); ++li) {
      bool reaches = false;
      for (int k = 0; k < 64 && !reaches; ++k) {
        LightSample ls;
        if (!lights[li].sample(p, rng.next(), rng.next(), ls)) continue;
        float c = dot(ls.wi, n);
        reaches = length2(n) == 0.0f || c > 0.0f || (twoSided && c < 0.0f);
      }
      if (reaches && !(tree.pmf(p, n, twoSided, li) > 0.0f)) ++conservativeFails;
    }
    // Pick frequencies match pmf(), and picks report it.
    const int N = 40000;
    std::vector<double> freq(lights.size(), 0.0);
    LightPick picks[LightTree::kMaxPicks];
    for (int i = 0; i < N; ++i) {
      int np = tree.pick(p, n, twoSided, rng.next(), picks);
      for (int k = 0; k < np; ++k) {
        freq[picks[k].light] += 1.0;
        float pm = tree.pmf(p, n, twoSided, picks[k].light);
        if (std::fabs(pm - picks[k].prob) > 1e-4f * std::max(pm, 1e-3f)) ++probMismatch;
      }
    }
    for (size_t li = 0; li < lights.size(); ++li) {
      double pm = tree.pmf(p, n, twoSided, static_cast<int>(li));
      double f = freq[li] / N;
      double sigma = std::sqrt(std::max(pm * (1.0 - pm), 1e-9) / N);
      worstFreq = std::max(worstFreq, std::fabs(f - pm) / (4.0 * sigma + 2e-4));
    }
  }
  std::printf("  light tree: %d conservative-bound failures, %d pmf mismatches, worst frequency error %.2f (< 1)\n",
              conservativeFails, probMismatch, worstFreq);
  CHECK(conservativeFails == 0);
  CHECK(probMismatch == 0);
  CHECK(worstFreq < 1.0);
}

// Many lights over a floor, direct light only: the light tree converges to the same image as
// sampling every light.
static void testLightTreeUnbiased() {
  auto build = [](Scene& s, int mode, int spp) {
    ParamBlock& rs = s.renderSettings;
    rs.setNumber("width", 48);
    rs.setNumber("height", 48);
    rs.setNumber("samples", spp);
    rs.setNumber("maxDepth", 1);
    rs.setNumber("clampIndirect", 0);
    rs.setNumber("envMode", 0);
    rs.setColor("envColor", Vec3(0.0f));
    rs.setNumber("noiseThreshold", 0.0);
    rs.setNumber("pathGuiding", 0);
    rs.setNumber("lightSampling", mode);
    Node* floor = s.createNode("Plane");
    floor->object->params.setNumber("length", 1200);
    floor->object->params.setNumber("width", 1200);
    Rng rng(5, 9);
    const char* types[] = {"VeraaLight", "VeraaLight", "VeraaLight", "OmniLight", "SpotLight", "RectLight", "SphereLight"};
    for (int i = 0; i < 28; ++i) {
      Node* l = s.createNode(types[i % 7]);
      ParamBlock& p = l->object->params;
      l->transform.position = Vec3(rng.next() * 1000.0f - 500.0f, rng.next() * 1000.0f - 500.0f, 40.0f + rng.next() * 200.0f);
      l->transform.rotation = Vec3((rng.next() - 0.5f) * 60.0f, (rng.next() - 0.5f) * 60.0f, rng.next() * 360.0f);
      p.setNumber("intensity", 0.5 + rng.next() * 3.0);
      if (i % 7 < 3) {
        p.setNumber("lightType", i % 7);  // plane, disc, sphere
        p.setNumber("width", 20 + rng.next() * 60);
        p.setNumber("length", 20 + rng.next() * 60);
        p.setNumber("lightRadius", 5 + rng.next() * 15);
        p.setNumber("invisible", 1);
      }
      if (i % 7 == 5) p.setNumber("invisible", 1);
      if (i % 7 == 6) {
        p.setNumber("radius", 4 + rng.next() * 10);
        p.setNumber("invisible", 1);
      }
      if (i % 7 == 3 || i % 7 == 4) p.setNumber("intensity", 2000.0 + rng.next() * 6000.0);
    }
    // Emissive LED strips: a long segmented mesh (split into chunks for the light tree).
    Material* led = s.createMaterial("LED");
    led->setMaterialType("VeraaLightMtl");
    led->params.setColor("color", Vec3(1.0f, 0.9f, 0.7f));
    led->params.setNumber("multiplier", 6.0);
    led->params.setNumber("directIllumination", 1);
    for (int k = 0; k < 2; ++k) {
      Node* strip = s.createNode("Plane");
      strip->object->params.setNumber("length", 6);
      strip->object->params.setNumber("width", 700);
      strip->object->params.setNumber("lengthSegs", 1);
      strip->object->params.setNumber("widthSegs", 60);
      strip->transform.position = Vec3(0, k ? 250.0f : -150.0f, 60.0f + 40.0f * k);
      strip->transform.rotation = Vec3(180, 0, 0);  // emit downward
      strip->materialId = led->id;
      strip->visibleToCamera = false;
    }
  };
  auto render = [&](int mode, int spp, double& seconds) {
    Scene s;
    build(s, mode, spp);
    Renderer r;
    r.setScene(s);
    RenderView v;
    v.position = Vec3(0, 0, 500);
    v.target = Vec3(0, 0, 0);
    v.up = Vec3(0, 1, 0);
    v.orthographic = true;
    v.orthoHeight = 1100.0f;
    r.setView(v);
    auto t0 = std::chrono::steady_clock::now();
    r.renderBlocking();
    seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::vector<float> hdr;
    r.resolveHdr(hdr);
    return hdr;
  };
  double tAll, tTree, tPower;
  std::vector<float> all = render(1, 64, tAll), tree = render(0, 256, tTree), power = render(2, 256, tPower);
  auto quadMeans = [](const std::vector<float>& img) {
    std::vector<double> q(5, 0.0);
    for (int y = 0; y < 48; ++y)
      for (int x = 0; x < 48; ++x) {
        const float* p = &img[(static_cast<size_t>(y) * 48 + x) * 3];
        double l = luminance(Vec3(p[0], p[1], p[2]));
        q[(y / 24) * 2 + x / 24] += l / 576.0;
        q[4] += l / 2304.0;
      }
    return q;
  };
  std::vector<double> qa = quadMeans(all), qt = quadMeans(tree), qp = quadMeans(power);
  std::printf("  many lights: mean all %.4f (%.2fs, 64 spp), tree %.4f (%.2fs, 256 spp), power %.4f (%.2fs, 256 spp)\n",
              qa[4], tAll, qt[4], tTree, qp[4], tPower);
  CHECK(qa[4] > 0.0);
  for (int i = 0; i < 5; ++i) {
    CHECK(std::fabs(qt[i] / qa[i] - 1.0) < (i == 4 ? 0.02 : 0.04));
    CHECK(std::fabs(qp[i] / qa[i] - 1.0) < (i == 4 ? 0.02 : 0.04));
  }
}

// ---------------------------------------------------------------------------------------------
// Guided rendering: a closed room lit through a small window.

static void buildWindowRoom(Scene& s, int spp, bool guiding) {
  ParamBlock& rs = s.renderSettings;
  rs.setNumber("width", 96);
  rs.setNumber("height", 72);
  rs.setNumber("samples", spp);
  rs.setNumber("maxDepth", 2);
  rs.setNumber("clampIndirect", 0);
  rs.setNumber("filterGlossy", 0);
  rs.setNumber("noiseThreshold", 0.0);
  rs.setNumber("envMode", 0);
  rs.setColor("envColor", Vec3(1.0f, 0.95f, 0.9f));
  rs.setNumber("envIntensity", 4.0);
  rs.setNumber("pathGuiding", guiding ? 1 : 0);
  rs.setNumber("guidingTrainingSamples", 32);
  Material* white = s.createMaterial("White");
  white->params.setColor("diffuse", Vec3(0.3f));  // dark walls: light through the window dominates
  auto box = [&](Vec3 base, Vec3 size) {
    Node* b = s.createNode("Box");
    b->object->params.setNumber("width", size.x);
    b->object->params.setNumber("length", size.y);
    b->object->params.setNumber("height", size.z);
    b->transform.position = base;
    b->materialId = white->id;
  };
  // Interior 200 x 200 x 160; a 50 x 40 window in the -X wall.
  box(Vec3(0, 0, -10), Vec3(220, 220, 10));    // floor
  box(Vec3(0, 0, 160), Vec3(220, 220, 10));    // ceiling
  box(Vec3(0, 105, 0), Vec3(220, 10, 160));    // +Y
  box(Vec3(0, -105, 0), Vec3(220, 10, 160));   // -Y
  box(Vec3(105, 0, 0), Vec3(10, 220, 160));    // +X
  box(Vec3(-105, 0, 0), Vec3(10, 220, 80));    // -X below the window
  box(Vec3(-105, 0, 120), Vec3(10, 220, 40));  // above
  box(Vec3(-105, -67.5f, 80), Vec3(10, 85, 40));
  box(Vec3(-105, 67.5f, 80), Vec3(10, 85, 40));
  box(Vec3(30, 20, 0), Vec3(40, 40, 50));      // a block on the floor
}

static std::vector<float> renderWindowRoom(int spp, bool guiding, double& seconds) {
  Scene s;
  buildWindowRoom(s, spp, guiding);
  Renderer r;
  r.setScene(s);
  RenderView v;
  v.position = Vec3(-80, -80, 120);
  v.target = Vec3(90, 60, 30);
  v.fovYDeg = 60.0f;
  r.setView(v);
  auto t0 = std::chrono::steady_clock::now();
  r.renderBlocking();
  seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::vector<float> hdr;
  r.resolveHdr(hdr);
  return hdr;
}

static void testGuidingReducesNoise() {
  double tRef, tOff, tOn;
  std::vector<float> ref = renderWindowRoom(2048, false, tRef);
  std::vector<float> off = renderWindowRoom(128, false, tOff);
  std::vector<float> on = renderWindowRoom(128, true, tOn);
  auto stats = [&](const std::vector<float>& img, double& mse, double& mean) {
    mse = 0.0;
    mean = 0.0;
    double refMean = 0.0;
    for (size_t i = 0; i < img.size(); ++i) {
      double d = img[i] - ref[i];
      mse += d * d;
      mean += img[i];
      refMean += ref[i];
    }
    mse /= static_cast<double>(img.size());
    mean /= refMean;
  };
  double mseOff, mseOn, meanOff, meanOn;
  stats(off, mseOff, meanOff);
  stats(on, mseOn, meanOn);
  std::printf("  window room at 128 spp: MSE without guiding %.5f (%.2fs), with %.5f (%.2fs): %.2fx less; "
              "mean vs reference %.4f / %.4f\n",
              mseOff, tOff, mseOn, tOn, mseOff / std::max(mseOn, 1e-12), meanOff, meanOn);
  CHECK(mseOn < mseOff / 1.15);
  CHECK(std::fabs(meanOn - 1.0) < 0.05);
  CHECK(std::fabs(meanOff - 1.0) < 0.05);
}

// ---------------------------------------------------------------------------------------------
// Quality presets

static void testPresets() {
  const auto& names = renderQualityPresetNames();
  CHECK(names.size() == 5 && names.front() == "Draft" && names.back() == "Supreme");
  CHECK(renderQualityPresetIndex("supreme") == 4 && renderQualityPresetIndex("Very High") == 3 &&
        renderQualityPresetIndex("veryhigh") == 3 && renderQualityPresetIndex("ultra") == -1);
  Scene s;
  ParamBlock& rs = s.renderSettings;
  CHECK(rs.has("pathGuiding") && rs.has("lightSampling") && rs.has("filterGlossy") && rs.has("guidingTrainingSamples"));
  CHECK(matchingRenderQualityPreset(rs) == -1);
  for (int i = 0; i < 5; ++i) {
    CHECK(applyRenderQualityPreset(rs, i));
    CHECK(matchingRenderQualityPreset(rs) == i);
  }
  CHECK(rs.getBool("pathGuiding") && rs.getInt("maxDepth") >= 16 && rs.getFloat("noiseThreshold") < 0.005f &&
        rs.getBool("denoise"));
  applyRenderQualityPreset(rs, 0);
  CHECK(!rs.getBool("pathGuiding") && rs.getInt("samples") < 256);
  rs.setNumber("maxDepth", 5);
  CHECK(matchingRenderQualityPreset(rs) == -1);
}

int main() {
  auto t0 = std::chrono::steady_clock::now();
  testPresets();
  testGuideLearning();
  testLightTreePicks();
  testLightTreeUnbiased();
  testGuidingReducesNoise();
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  if (g_failures == 0) std::printf("All guiding tests passed (%.1fs)\n", secs);
  else std::printf("%d guiding test failure(s)\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
