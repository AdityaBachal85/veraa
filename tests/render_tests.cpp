// Renderer tests: energy conservation, light units, environment sampling, file output.
#include <cstdio>
#include <filesystem>

#include "environment.h"
#include "renderer.h"
#include "scene.h"

using namespace veraa;

static int g_failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++g_failures;                                                \
    }                                                              \
  } while (0)

#define CHECK_NEAR(value, expected, tol)                                                               \
  do {                                                                                                 \
    double v_ = (value), e_ = (expected);                                                              \
    if (!(std::fabs(v_ - e_) <= (tol))) {                                                              \
      std::printf("FAIL %s:%d: %s = %.5f, expected %.5f +- %.5f\n", __FILE__, __LINE__, #value, v_, e_, \
                  static_cast<double>(tol));                                                           \
      ++g_failures;                                                                                    \
    }                                                                                                  \
  } while (0)

// Mean of the HDR image's luminance in a centered square covering `fraction` of the width.
static double centerMean(const Renderer& r, double fraction) {
  std::vector<float> hdr;
  r.resolveHdr(hdr);
  int w = r.width(), h = r.height();
  int half = std::max(1, static_cast<int>(w * fraction * 0.5));
  double sum = 0;
  int n = 0;
  for (int y = h / 2 - half; y < h / 2 + half; ++y)
    for (int x = w / 2 - half; x < w / 2 + half; ++x) {
      const float* p = &hdr[(static_cast<size_t>(y) * w + x) * 3];
      sum += luminance(Vec3(p[0], p[1], p[2]));
      ++n;
    }
  return sum / n;
}

static void baseSettings(Scene& s, int size, int spp, int depth) {
  ParamBlock& rs = s.renderSettings;
  rs.setNumber("width", size);
  rs.setNumber("height", size);
  rs.setNumber("samples", spp);
  rs.setNumber("maxDepth", depth);
  rs.setNumber("clampIndirect", 0);
  rs.setNumber("envMode", 0);
  rs.setColor("envColor", Vec3(0.0f));
}

static RenderView topDownView(float height) {
  RenderView v;
  v.position = Vec3(0, 0, 50);
  v.target = Vec3(0, 0, 0);
  v.up = Vec3(0, 1, 0);
  v.orthographic = true;
  v.orthoHeight = height;
  return v;
}

// An albedo-1 diffuse sphere inside a uniform white environment must look exactly white.
static void testWhiteFurnace() {
  for (int variant = 0; variant < 2; ++variant) {
    Scene s;
    baseSettings(s, 48, 256, 64);
    s.renderSettings.setColor("envColor", Vec3(1.0f));
    Material* m = s.createMaterial("White");
    m->params.setColor("diffuse", Vec3(1.0f));
    if (variant == 1) {  // Fresnel-weighted glossy coat over white diffuse also conserves energy
      m->params.setColor("reflect", Vec3(1.0f));
      m->params.setNumber("reflectGlossiness", 0.7);
    }
    Node* sphere = s.createNode("Sphere");
    sphere->object->params.setNumber("segments", 96);
    sphere->materialId = m->id;
    Renderer r;
    CHECK(r.setScene(s));
    RenderView v;
    v.position = Vec3(0, -400, 0);
    v.target = Vec3(0, 0, 0);
    v.fovYDeg = 10.0f;
    r.setView(v);
    r.renderBlocking();
    double mean = centerMean(r, 0.4);
    if (variant == 0) CHECK_NEAR(mean, 1.0, 0.02);
    else CHECK(mean > 0.85 && mean < 1.02);
  }
}

// Omni: irradiance at 100 units = multiplier. A white Lambert floor reflects E / pi.
static void testOmniUnits() {
  Scene s;
  baseSettings(s, 16, 64, 1);
  Material* m = s.createMaterial("White");
  m->params.setColor("diffuse", Vec3(1.0f));
  Node* plane = s.createNode("Plane");
  plane->object->params.setNumber("length", 10000);
  plane->object->params.setNumber("width", 10000);
  plane->materialId = m->id;
  Node* omni = s.createNode("OmniLight");
  omni->transform.position = Vec3(0, 0, 100);
  omni->object->params.setNumber("intensity", 1.0);
  Renderer r;
  CHECK(r.setScene(s));
  r.setView(topDownView(1.0f));
  r.renderBlocking();
  CHECK_NEAR(centerMean(r, 0.5), 1.0 / kPi, 0.003);
}

// Sun: irradiance = multiplier (at normal incidence).
static void testSunUnits() {
  Scene s;
  baseSettings(s, 16, 64, 1);
  Material* m = s.createMaterial("White");
  m->params.setColor("diffuse", Vec3(1.0f));
  Node* plane = s.createNode("Plane");
  plane->object->params.setNumber("length", 10000);
  plane->object->params.setNumber("width", 10000);
  plane->materialId = m->id;
  Node* sun = s.createNode("SunLight");
  sun->object->params.setColor("color", Vec3(1.0f));
  sun->object->params.setNumber("intensity", 2.0);
  sun->transform.position = Vec3(0, 0, 1000);  // rotation 0: shines straight down
  Renderer r;
  CHECK(r.setScene(s));
  r.setView(topDownView(1.0f));
  r.renderBlocking();
  CHECK_NEAR(centerMean(r, 0.5), 2.0 / kPi, 0.005);
}

// A huge plane light just above a white floor: irradiance pi * L, so the floor shows L.
static void testRectLightUnits() {
  Scene s;
  baseSettings(s, 16, 256, 1);
  Material* m = s.createMaterial("White");
  m->params.setColor("diffuse", Vec3(1.0f));
  Node* plane = s.createNode("Plane");
  plane->object->params.setNumber("length", 100000);
  plane->object->params.setNumber("width", 100000);
  plane->materialId = m->id;
  Node* light = s.createNode("RectLight");
  light->object->params.setNumber("width", 100000);
  light->object->params.setNumber("length", 100000);
  light->object->params.setNumber("intensity", 1.0);
  light->object->params.setNumber("invisible", 1);
  light->transform.position = Vec3(0, 0, 10);
  Renderer r;
  CHECK(r.setScene(s));
  RenderView v = topDownView(1.0f);
  v.position = Vec3(0, 0, 5);  // between the floor and the light
  r.setView(v);
  r.renderBlocking();
  CHECK_NEAR(centerMean(r, 0.5), 1.0, 0.03);
}

// Importance sampling an HDRI: E[L / pdf] must equal the integral of L, and pdf() must
// agree with the pdf returned by sample().
static void testEnvironmentSampling() {
  Scene s;
  s.filePath = std::string(VERAA_SOURCE_DIR) + "/scenes/test.veraa";
  s.renderSettings.setNumber("envMode", 2);
  s.renderSettings.setString("hdriPath", "../assets/hdri/royal_esplanade_1k.hdr");
  s.renderSettings.setNumber("hdriRotation", 33.0);
  render::TextureCache cache;
  render::Environment env;
  std::vector<std::string> warnings;
  env.build(s, cache, nullptr, 0.0f, &warnings);
  CHECK(warnings.empty());
  CHECK(!env.isBlack());

  // Reference integral by brute force over directions.
  double ref = 0.0;
  const int W = 1024, H = 512;
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      float u = (x + 0.5f) / W, v = (y + 0.5f) / H;
      Vec3 d = render::equirectToDirection(u, v, radians(33.0f));
      ref += luminance(env.eval(d)) * std::sin(v * kPi) * (kTwoPi / W) * (kPi / H);
    }
  render::Rng rng(7, 3);
  double est = 0.0;
  const int N = 400000;
  int pdfMismatch = 0;
  for (int i = 0; i < N; ++i) {
    Vec3 d, L;
    float pdf;
    if (!env.sample(rng.next(), rng.next(), d, L, pdf)) continue;
    est += luminance(L) / pdf;
    float p2 = env.pdf(d);
    if (std::fabs(p2 - pdf) > 1e-3f * pdf + 1e-6f) ++pdfMismatch;
  }
  est /= N;
  CHECK_NEAR(est / ref, 1.0, 0.02);
  CHECK(pdfMismatch < N / 1000);
}

static void testSceneFileRender() {
  Scene s;
  std::string err;
  CHECK(s.load(std::string(VERAA_SOURCE_DIR) + "/scenes/cornell_box.veraa", &err));
  Renderer r;
  r.setResolution(64, 64);
  r.setMaxSamples(8);
  CHECK(r.setScene(s, &err));
  CHECK(r.warnings().empty());
  const Node* cam = s.findNodeByName("Camera");
  CHECK(cam != nullptr);
  if (cam) r.setView(viewFromCamera(*cam, 1.0f));
  r.renderBlocking();
  RenderStats st = r.stats();
  CHECK(st.passes == 8);
  CHECK(st.finished);
  std::vector<float> hdr;
  r.resolveHdr(hdr);
  bool finite = true;
  double sum = 0;
  for (float f : hdr) {
    finite = finite && std::isfinite(f);
    sum += f;
  }
  CHECK(finite);
  CHECK(sum / hdr.size() > 0.05);
  std::vector<uint8_t> rgba;
  r.resolve(displaySettingsFromScene(s), rgba);
  CHECK(rgba.size() == 64u * 64u * 4u);

  auto path = (std::filesystem::temp_directory_path() / "veraa_render_test.png").u8string();
  CHECK(writePng(path, 64, 64, rgba));
  CHECK(std::filesystem::exists(path) && std::filesystem::file_size(path) > 100);
  std::filesystem::remove(path);
  auto hdrPath = (std::filesystem::temp_directory_path() / "veraa_render_test.hdr").u8string();
  CHECK(writeHdr(hdrPath, 64, 64, hdr));
  CHECK(std::filesystem::exists(hdrPath));
  std::filesystem::remove(hdrPath);
}

// Progressive rendering on background threads can be stopped and inspected while running.
static void testProgressive() {
  Scene s;
  CHECK(s.load(std::string(VERAA_SOURCE_DIR) + "/scenes/cornell_box.veraa"));
  Renderer r;
  r.setResolution(96, 96);
  r.setMaxSamples(100000);
  CHECK(r.setScene(s));
  r.setView(defaultViewForScene(s, 1.0f));
  r.start();
  std::vector<uint8_t> rgba;
  for (int i = 0; i < 20; ++i) r.resolve(DisplaySettings(), rgba);  // concurrent reads
  CHECK(r.isRunning());
  r.stop();
  CHECK(!r.isRunning());
  CHECK(!r.stats().finished);
  r.resolve(DisplaySettings(), rgba);
  CHECK(rgba.size() == 96u * 96u * 4u);
}

// Image textures: a two-color diffuse map and an opacity map with a transparent left half.
static void testTextures() {
  auto dir = std::filesystem::temp_directory_path() / "veraa_tex_test";
  std::filesystem::create_directories(dir);
  const int n = 32;
  std::vector<uint8_t> colorTex(n * n * 4), alphaTex(n * n * 4);
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x) {
      uint8_t* c = &colorTex[(y * n + x) * 4];
      bool left = x < n / 2;
      c[0] = left ? 255 : 0;
      c[1] = left ? 0 : 255;
      c[2] = 0;
      c[3] = 255;
      uint8_t* a = &alphaTex[(y * n + x) * 4];
      a[0] = a[1] = a[2] = 255;
      a[3] = left ? 0 : 255;
    }
  std::string colorPath = (dir / "color.png").u8string(), alphaPath = (dir / "alpha.png").u8string();
  CHECK(writePng(colorPath, n, n, colorTex));
  CHECK(writePng(alphaPath, n, n, alphaTex));

  for (int variant = 0; variant < 2; ++variant) {
    Scene s;
    baseSettings(s, 32, 64, 4);
    s.renderSettings.setColor("envColor", Vec3(1.0f));
    Material* m = s.createMaterial("Textured");
    if (variant == 0) {
      m->params.setColor("diffuse", Vec3(1.0f));
      m->params.setString("diffuseMap", colorPath);
    } else {
      m->params.setColor("diffuse", Vec3(0.0f));
      m->params.setString("opacityMap", alphaPath);
    }
    Node* plane = s.createNode("Plane");
    plane->materialId = m->id;
    Renderer r;
    CHECK(r.setScene(s));
    CHECK(r.warnings().empty());
    r.setView(topDownView(100.0f));
    r.renderBlocking();
    std::vector<float> hdr;
    r.resolveHdr(hdr);
    auto px = [&](int x, int y) {
      const float* p = &hdr[(static_cast<size_t>(y) * 32 + x) * 3];
      return Vec3(p[0], p[1], p[2]);
    };
    Vec3 left = px(8, 16), right = px(24, 16);
    if (variant == 0) {
      CHECK(left.x > 3 * left.y);   // red half
      CHECK(right.y > 3 * right.x); // green half
    } else {
      CHECK(left.x > 0.95f);        // cut out: the white environment shows through
      CHECK(right.x < 0.05f);       // black opaque half
    }
  }
  std::filesystem::remove_all(dir);
}

static void testMaterialPreview() {
  Material m;
  m.params.setColor("diffuse", Vec3(0.8f, 0.1f, 0.1f));
  m.params.setColor("reflect", Vec3(1.0f));
  std::vector<uint8_t> px = renderMaterialPreview(m, nullptr, 48, 8);
  CHECK(px.size() == 48u * 48u * 4u);
  // The sphere in the middle should be reddish.
  size_t c = (24 * 48 + 24) * 4;
  CHECK(px[c] > px[c + 1] && px[c] > px[c + 2]);
}

int main() {
  testWhiteFurnace();
  testOmniUnits();
  testSunUnits();
  testRectLightUnits();
  testEnvironmentSampling();
  testSceneFileRender();
  testProgressive();
  testTextures();
  testMaterialPreview();
  std::printf("Denoiser available: %s\n", Renderer::denoiserAvailable() ? "yes" : "no");
  if (g_failures) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("All render tests passed\n");
  return 0;
}
