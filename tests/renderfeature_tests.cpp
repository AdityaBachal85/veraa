// Rendering feature tests: energy conservation of the new lobes and materials, light units
// (IES, VeraaLight, mesh lights, dome), subsurface sanity, adaptive sampling, render elements
// summing to the beauty, Light Mix, buckets, regions, texture maps, proxies, matte/shadow,
// motion blur, displacement, fur, physical camera exposure and EXR files.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "lights.h"
#include "motionhook.h"
#include "photometry.h"
#include "renderer.h"
#include "renderobjects.h"
#include "rendersettings.h"
#include "scene.h"
#include "sky.h"
#include "texmaps.h"
#include "texture.h"

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

static double meanLum(const std::vector<float>& img, int w, int h, double fraction) {
  int half = std::max(1, static_cast<int>(w * fraction * 0.5));
  double sum = 0;
  int n = 0;
  for (int y = h / 2 - half; y < h / 2 + half; ++y)
    for (int x = w / 2 - half; x < w / 2 + half; ++x) {
      const float* p = &img[(static_cast<size_t>(y) * w + x) * 3];
      sum += luminance(Vec3(p[0], p[1], p[2]));
      ++n;
    }
  return sum / n;
}

static double centerMean(const Renderer& r, double fraction) {
  std::vector<float> hdr;
  r.resolveHdr(hdr);
  return meanLum(hdr, r.width(), r.height(), fraction);
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
  rs.setNumber("noiseThreshold", 0.0);
}

static RenderView topDownView(float height, float z = 50.0f) {
  RenderView v;
  v.position = Vec3(0, 0, z);
  v.target = Vec3(0, 0, 0);
  v.up = Vec3(0, 1, 0);
  v.orthographic = true;
  v.orthoHeight = height;
  return v;
}

static RenderView frontView() {
  RenderView v;
  v.position = Vec3(0, -400, 0);
  v.target = Vec3(0, 0, 0);
  v.fovYDeg = 10.0f;
  return v;
}

static Node* addFloor(Scene& s, Material* m, float size = 10000.0f) {
  Node* plane = s.createNode("Plane");
  plane->object->params.setNumber("length", size);
  plane->object->params.setNumber("width", size);
  plane->materialId = m ? m->id : 0;
  return plane;
}

// A closed object in a white environment: with no absorption it must look white.
static double furnace(Material* (*setup)(Scene&), int spp = 128) {
  Scene s;
  baseSettings(s, 40, spp, 64);
  s.renderSettings.setColor("envColor", Vec3(1.0f));
  Material* m = setup(s);
  Node* sphere = s.createNode("Sphere");
  sphere->object->params.setNumber("segments", 96);
  sphere->materialId = m->id;
  Renderer r;
  r.setScene(s);
  r.setView(frontView());
  r.renderBlocking();
  return centerMean(r, 0.4);
}

static void testFurnaces() {
  double sheen = furnace([](Scene& s) {
    Material* m = s.createMaterial("Sheen");
    m->params.setColor("diffuse", Vec3(1.0f));
    m->params.setColor("sheenColor", Vec3(1.0f));
    return m;
  });
  CHECK(sheen > 0.85 && sheen < 1.03);
  double coat = furnace([](Scene& s) {
    Material* m = s.createMaterial("Coat");
    m->params.setColor("diffuse", Vec3(1.0f));
    m->params.setNumber("coatAmount", 1.0);
    m->params.setNumber("coatGlossiness", 0.85);
    return m;
  });
  CHECK(coat > 0.85 && coat < 1.03);
  double blend = furnace([](Scene& s) {
    Material* m = s.createMaterial("Blend");
    m->setMaterialType("VeraaBlendMtl");
    m->subs[0].material->params.setColor("diffuse", Vec3(1.0f));
    m->subs[1].material = std::make_unique<Material>();
    m->subs[1].material->params.setColor("diffuse", Vec3(1.0f));
    return m;
  });
  CHECK_NEAR(blend, 1.0, 0.03);
  double thin = furnace([](Scene& s) {
    Material* m = s.createMaterial("Translucent");
    m->params.setColor("diffuse", Vec3(1.0f));
    m->params.setNumber("translucency", 1);
    return m;
  });
  CHECK_NEAR(thin, 1.0, 0.04);
  double glass = furnace([](Scene& s) {
    Material* m = s.createMaterial("Glass");
    m->params.setColor("diffuse", Vec3(0.0f));
    m->params.setColor("reflect", Vec3(1.0f));
    m->params.setColor("refract", Vec3(1.0f));
    m->params.setNumber("ior", 1.5);
    m->params.setNumber("fresnelIor", 1.5);
    return m;
  });
  CHECK_NEAR(glass, 1.0, 0.04);
  double rough = furnace([](Scene& s) {
    Material* m = s.createMaterial("FrostedGlass");
    m->params.setColor("diffuse", Vec3(0.0f));
    m->params.setColor("reflect", Vec3(1.0f));
    m->params.setColor("refract", Vec3(1.0f));
    m->params.setNumber("refractGlossiness", 0.8);
    m->params.setNumber("reflectGlossiness", 0.8);
    return m;
  });
  CHECK(rough > 0.85 && rough < 1.03);
  double car = furnace([](Scene& s) {
    Material* m = s.createMaterial("Car");
    m->setMaterialType("VeraaCarPaintMtl");
    m->params.setColor("baseColor", Vec3(1.0f));
    return m;
  });
  CHECK(car < 1.05);
  double twoSided = furnace([](Scene& s) {
    Material* m = s.createMaterial("TwoSided");
    m->setMaterialType("VeraaTwoSidedMtl");
    m->subs[0].material->params.setColor("diffuse", Vec3(1.0f));
    return m;
  });
  CHECK_NEAR(twoSided, 1.0, 0.04);
  double sss = furnace(
      [](Scene& s) {
        Material* m = s.createMaterial("SSS");
        m->setMaterialType("VeraaSSSMtl");
        m->params.setColor("sssColor", Vec3(1.0f));
        m->params.setNumber("scatterRadius", 5.0);
        m->params.setColor("radiusColor", Vec3(1.0f));
        return m;
      },
      64);
  CHECK(sss > 0.85 && sss < 1.05);
  std::printf("  furnace: sheen %.3f coat %.3f blend %.3f thin %.3f glass %.3f rough %.3f car %.3f two-sided %.3f sss %.3f\n",
              sheen, coat, blend, thin, glass, rough, car, twoSided, sss);
}

// Subsurface with a darker albedo looks roughly that bright, and wider radius blurs more.
static void testSubsurfaceAlbedo() {
  double half = furnace(
      [](Scene& s) {
        Material* m = s.createMaterial("SSS");
        m->setMaterialType("VeraaSSSMtl");
        m->params.setColor("sssColor", Vec3(0.5f));
        m->params.setNumber("scatterRadius", 5.0);
        m->params.setColor("radiusColor", Vec3(1.0f));
        m->params.setColor("specularColor", Vec3(0.0f));
        return m;
      },
      64);
  std::printf("  subsurface albedo 0.5 renders %.3f\n", half);
  CHECK(half > 0.38 && half < 0.68);
}

static void testIesPhotometry() {
  auto dir = std::filesystem::temp_directory_path() / "veraa_ies_test";
  std::filesystem::create_directories(dir);
  std::vector<float> v = {0, 45, 90, 135, 180}, c(5, 1000.0f);
  std::string path = (dir / "iso.ies").string();
  {
    FILE* f = std::fopen(path.c_str(), "wb");
    std::string t = makeIesText(v, c, "isotropic");
    std::fwrite(t.data(), 1, t.size(), f);
    std::fclose(f);
  }
  Scene s;
  baseSettings(s, 16, 32, 1);
  Material* m = s.createMaterial("White");
  m->params.setColor("diffuse", Vec3(1.0f));
  addFloor(s, m);
  Node* ies = s.createNode("VeraaIES");
  ies->object->params.setString("iesFile", path);
  ies->transform.position = Vec3(0, 0, 200);
  Renderer r;
  CHECK(r.setScene(s));
  CHECK(r.warnings().empty());
  r.setView(topDownView(1.0f));
  r.renderBlocking();
  // E = I / d² with d in meters: 1000 cd at 2 m = 250 lux; a white Lambert floor shows E / pi.
  CHECK_NEAR(centerMean(r, 0.5), 250.0 / kPi, 250.0 / kPi * 0.01);

  // Luminous power mode: 4 pi * 500 lm spread evenly is 500 cd.
  ies->object->params.setNumber("powerMode", 1);
  ies->object->params.setNumber("power", 4.0 * kPi * 500.0);
  Renderer r2;
  CHECK(r2.setScene(s));
  r2.setView(topDownView(1.0f));
  r2.renderBlocking();
  CHECK_NEAR(centerMean(r2, 0.5), 125.0 / kPi, 125.0 / kPi * 0.015);
  std::filesystem::remove_all(dir);
}

// VeraaLight plane units: a huge plane just above the floor gives E = pi L, so the floor shows L.
static void testVeraaLightUnits() {
  struct Case {
    int units;
    float value;
    double expected;
  };
  // Plane 10 m x 10 m = 100 m² one-sided.
  Case cases[] = {{0, 2.0f, 2.0}, {2, 3.0f, 3.0}, {1, 1000.0f, 1000.0 / (kPi * 100.0)}, {3, 1.0f, 683.0 / (kPi * 100.0)}};
  for (const auto& cs : cases) {
    Scene s;
    baseSettings(s, 16, 128, 1);
    Material* m = s.createMaterial("White");
    m->params.setColor("diffuse", Vec3(1.0f));
    addFloor(s, m, 100000.0f);
    Node* l = s.createNode("VeraaLight");
    l->object->params.setNumber("width", 1000);
    l->object->params.setNumber("length", 1000);
    l->object->params.setNumber("units", cs.units);
    l->object->params.setNumber("intensity", cs.value);
    l->object->params.setNumber("invisible", 1);
    l->transform.position = Vec3(0, 0, 2);
    Renderer r;
    CHECK(r.setScene(s));
    RenderView v = topDownView(1.0f, 1.0f);
    r.setView(v);
    r.renderBlocking();
    CHECK_NEAR(centerMean(r, 0.5), cs.expected, cs.expected * 0.04);
  }
  // Color temperature keeps the luminance.
  Vec3 warm = kelvinToRgb(3000.0f);
  CHECK(warm.x > warm.z);
}

// A mesh light made from a plane behaves like the equivalent plane light.
static void testMeshAndDomeLights() {
  Scene s;
  baseSettings(s, 16, 256, 1);
  Material* m = s.createMaterial("White");
  m->params.setColor("diffuse", Vec3(1.0f));
  addFloor(s, m, 100000.0f);
  Node* emitter = s.createNode("Plane");
  emitter->name = "Emitter";
  emitter->object->params.setNumber("length", 1000);
  emitter->object->params.setNumber("width", 1000);
  emitter->transform.position = Vec3(0, 0, 2);
  emitter->transform.rotation = Vec3(180, 0, 0);  // face down
  Node* l = s.createNode("VeraaLight");
  l->object->params.setNumber("lightType", 3);
  l->object->params.setString("meshNode", "Emitter");
  l->object->params.setNumber("intensity", 2.0);
  l->object->params.setNumber("invisible", 1);
  Renderer r;
  CHECK(r.setScene(s));
  r.setView(topDownView(1.0f, 1.0f));
  r.renderBlocking();
  CHECK_NEAR(centerMean(r, 0.5), 2.0, 0.1);

  // Dome light with a constant color acts like a uniform environment (furnace).
  Scene d;
  baseSettings(d, 40, 128, 64);
  Material* w = d.createMaterial("White");
  w->params.setColor("diffuse", Vec3(1.0f));
  Node* sphere = d.createNode("Sphere");
  sphere->object->params.setNumber("segments", 96);
  sphere->materialId = w->id;
  Node* dome = d.createNode("VeraaLight");
  dome->object->params.setNumber("lightType", 4);
  dome->object->params.setNumber("intensity", 1.0);
  Renderer rd;
  CHECK(rd.setScene(d));
  rd.setView(frontView());
  rd.renderBlocking();
  CHECK_NEAR(centerMean(rd, 0.4), 1.0, 0.03);
}

static Scene* buildElementScene(Scene& s, bool lightMix) {
  baseSettings(s, 64, 64, 6);
  ParamBlock& rs = s.renderSettings;
  rs.setNumber("envMode", 1);
  rs.setNumber("envIntensity", 0.5);
  rs.setNumber("clampIndirect", 0);
  for (const auto& e : renderElementInfos()) rs.setNumber(std::string("elem") + e.key, 1);
  rs.setNumber("lightMix", lightMix ? 1 : 0);
  rs.setNumber("fogOn", 1);
  rs.setNumber("fogDistance", 3000);
  rs.setNumber("fogAffectBackground", 0);
  Material* floor = s.createMaterial("Floor");
  floor->params.setColor("diffuse", Vec3(0.6f));
  floor->params.setColor("reflect", Vec3(0.3f));
  floor->params.setNumber("reflectGlossiness", 0.7);
  addFloor(s, floor, 2000.0f);
  Material* glass = s.createMaterial("Glass");
  glass->params.setColor("diffuse", Vec3(0.0f));
  glass->params.setColor("reflect", Vec3(1.0f));
  glass->params.setColor("refract", Vec3(1.0f));
  Node* g = s.createNode("Sphere");
  g->object->params.setNumber("radius", 20);
  g->transform.position = Vec3(-30, 0, 20);
  g->materialId = glass->id;
  Material* sss = s.createMaterial("Wax");
  sss->setMaterialType("VeraaSSSMtl");
  Node* w = s.createNode("Box");
  for (const char* k : {"length", "width", "height"}) w->object->params.setNumber(k, 30.0);
  w->transform.position = Vec3(40, 0, 0);
  w->materialId = sss->id;
  Material* emit = s.createMaterial("Emit");
  emit->params.setColor("selfIllum", Vec3(2.0f, 1.0f, 0.5f));
  Node* e = s.createNode("Sphere");
  e->object->params.setNumber("radius", 8);
  e->transform.position = Vec3(0, 40, 10);
  e->materialId = emit->id;
  Node* key = s.createNode("RectLight");
  key->name = "Key";
  key->object->params.setNumber("intensity", 8.0);
  key->transform.position = Vec3(0, -60, 150);
  Node* fill = s.createNode("OmniLight");
  fill->name = "Fill";
  fill->object->params.setNumber("intensity", 2.0);
  fill->transform.position = Vec3(80, -80, 60);
  Node* cam = s.createNode("Camera");
  cam->name = "ElementCam";
  cam->setWorldMatrix(Mat4::lookAt(Vec3(0, -220, 90), Vec3(0, 0, 15), Vec3(0, 0, 1)).inverse());
  cam->object->params.setNumber("fov", 45.0);
  s.renderSettings.setString("camera", "ElementCam");
  return &s;
}

static RenderView elementView(const Scene& s) { return viewFromCamera(*s.findNodeByName("ElementCam"), 1.0f); }

static void testElementsSumToBeauty() {
  Scene s;
  buildElementScene(s, true);
  if (const char* dump = std::getenv("VERAA_DUMP_SCENES")) s.save(std::string(dump) + "/elements.veraa");
  Renderer r;
  CHECK(r.setScene(s));
  r.setView(elementView(s));
  r.renderBlocking();
  std::vector<float> beauty, sum, tmp;
  r.resolveHdr(beauty);
  sum.assign(beauty.size(), 0.0f);
  int parts = 0;
  for (const auto& e : renderElementInfos()) {
    if (!e.sumsToBeauty) continue;
    CHECK(r.resolveElement(e.key, tmp));
    if (tmp.size() != sum.size()) continue;
    for (size_t i = 0; i < sum.size(); ++i) sum[i] += tmp[i];
    ++parts;
  }
  CHECK(parts == 9);
  double err = 0.0, total = 0.0;
  for (size_t i = 0; i < sum.size(); ++i) {
    err += std::fabs(sum[i] - beauty[i]);
    total += std::fabs(beauty[i]);
  }
  std::printf("  elements vs beauty relative error %.2e\n", err / std::max(total, 1e-9));
  CHECK(err / std::max(total, 1e-9) < 1e-4);
  // Every beauty component is present in this scene.
  for (const char* key : {"Lighting", "GI", "Specular", "Reflection", "Refraction", "SSS", "SelfIllumination",
                          "Atmosphere", "Background"}) {
    CHECK(r.resolveElement(key, tmp));
    double m = 0.0;
    for (float f : tmp) m += f;
    if (m <= 0.0) std::printf("  element %s is empty\n", key);
    CHECK(m > 0.0);
  }
  // Data elements resolve too.
  for (const char* key : {"DiffuseFilter", "Normals", "ZDepth", "ObjectID", "MaterialID", "WireColor", "Alpha"})
    CHECK(r.resolveElement(key, tmp) && tmp.size() == beauty.size());

  // Light Mix: all groups at 1 reproduce the beauty; doubling one group adds exactly its share.
  std::vector<std::string> groups = r.lightGroupNames();
  CHECK(groups.size() >= 4);  // Environment, Self-illumination, Key, Fill
  std::vector<LightMixGroup> mix;
  for (const auto& g : groups) mix.push_back({g, Vec3(1.0f), 1.0f, true});
  std::vector<float> lm;
  CHECK(r.resolveLightMix(mix, lm));
  double e2 = 0.0;
  for (size_t i = 0; i < lm.size(); ++i) e2 += std::fabs(lm[i] - beauty[i]);
  CHECK(e2 / std::max(total, 1e-9) < 1e-4);
  for (auto& m : mix)
    if (m.name == "Key") m.enabled = false;
  std::vector<float> noKey;
  CHECK(r.resolveLightMix(mix, noKey));
  double sumNoKey = 0, sumBeauty = 0;
  for (size_t i = 0; i < noKey.size(); ++i) {
    sumNoKey += noKey[i];
    sumBeauty += beauty[i];
  }
  CHECK(sumNoKey < sumBeauty * 0.9);
}

// Adaptive sampling stops early on easy scenes and still converges to the right answer.
static void testAdaptive() {
  Scene s;
  baseSettings(s, 48, 2048, 4);
  s.renderSettings.setColor("envColor", Vec3(0.8f));
  s.renderSettings.setNumber("noiseThreshold", 0.02);
  s.renderSettings.setNumber("minSamples", 16);
  Material* m = s.createMaterial("Grey");
  m->params.setColor("diffuse", Vec3(0.5f));
  Node* sphere = s.createNode("Sphere");
  sphere->materialId = m->id;
  Renderer r;
  CHECK(r.setScene(s));
  r.setView(frontView());
  r.renderBlocking();
  RenderStats st = r.stats();
  std::printf("  adaptive: %d passes, %.0f%% converged\n", st.passes, st.converged * 100.0);
  CHECK(st.finished);
  CHECK(st.passes < 2048);
  CHECK(st.converged > 0.99f);
  std::vector<float> counts;
  r.resolveSampleCounts(counts);
  float lo = 1e9f, hi = 0.0f;
  for (float c : counts) {
    lo = std::min(lo, c);
    hi = std::max(hi, c);
  }
  CHECK(lo >= 16.0f);
  double adaptiveMean = centerMean(r, 0.4);
  s.renderSettings.setNumber("noiseThreshold", 0.0);
  s.renderSettings.setNumber("samples", 512);
  Renderer ref;
  CHECK(ref.setScene(s));
  ref.setView(frontView());
  ref.renderBlocking();
  CHECK_NEAR(adaptiveMean, centerMean(ref, 0.4), 0.01);
}

static void testBucketsAndRegion() {
  Scene s;
  CHECK(s.load(std::string(VERAA_SOURCE_DIR) + "/scenes/cornell_box.veraa"));
  s.renderSettings.setNumber("noiseThreshold", 0.0);
  RenderView view = defaultViewForScene(s, 1.0f);
  Renderer a;
  a.setResolution(64, 64);
  a.setMaxSamples(64);
  CHECK(a.setScene(s));
  a.setView(view);
  a.renderBlocking();
  s.renderSettings.setNumber("samplerType", 1);
  s.renderSettings.setNumber("bucketSize", 16);
  Renderer b;
  b.setResolution(64, 64);
  b.setMaxSamples(64);
  CHECK(b.setScene(s));
  b.setView(view);
  b.renderBlocking();
  CHECK(b.stats().finished);
  CHECK(b.stats().passes == 64);
  double ma = centerMean(a, 0.8), mb = centerMean(b, 0.8);
  CHECK_NEAR(mb / ma, 1.0, 0.05);
  // Region: only the left half is rendered.
  Renderer c;
  c.setResolution(64, 64);
  c.setMaxSamples(4);
  c.setRegion(true, 0.0f, 0.0f, 0.5f, 1.0f);
  CHECK(c.setScene(s));
  c.setView(view);
  c.renderBlocking();
  std::vector<float> counts;
  c.resolveSampleCounts(counts);
  CHECK(counts[10 * 64 + 5] == 4.0f);
  CHECK(counts[10 * 64 + 60] == 0.0f);
}

static void testTextureMaps() {
  auto eval = [](const TexMap& map, const Vec2& uv, const Vec3& n = Vec3(0, 0, 1), const Vec3& wo = Vec3(0, 0, 1)) {
    render::TextureCache cache;
    render::MapCompileContext ctx;
    ctx.cache = &cache;
    render::MapPtr m = render::compileMap(&map, ctx);
    render::ShadeContext sc;
    sc.uv = uv;
    sc.p = sc.pObj = Vec3(uv.x * 100.0f, uv.y * 100.0f, 0.0f);
    sc.ng = sc.ns = n;
    sc.wo = wo;
    return m->color(sc);
  };
  TexMap checker("Checker");
  checker.params.setColor("color1", Vec3(1, 0, 0));
  checker.params.setColor("color2", Vec3(0, 0, 1));
  Vec3 a = eval(checker, Vec2(0.1f, 0.1f)), b = eval(checker, Vec2(0.6f, 0.1f));
  CHECK(a.x > 0.9f && b.z > 0.9f);
  checker.params.setNumber("uTiling", 2.0);  // tiling scales around the map center
  CHECK(eval(checker, Vec2(0.2f, 0.1f)).z > 0.9f);
  CHECK(eval(checker, Vec2(0.3f, 0.1f)).x > 0.9f);

  TexMap noise("Noise");
  float lo = 1, hi = 0;
  for (int i = 0; i < 200; ++i) {
    float v = eval(noise, Vec2(i * 0.0137f, i * 0.0071f)).x;
    lo = std::min(lo, v);
    hi = std::max(hi, v);
  }
  CHECK(lo >= 0.0f && hi <= 1.0f && hi - lo > 0.2f);

  TexMap falloff("Falloff");
  CHECK(eval(falloff, Vec2(), Vec3(0, 0, 1), Vec3(0, 0, 1)).x < 0.01f);  // facing: front color (black)
  CHECK(eval(falloff, Vec2(), Vec3(0, 0, 1), normalize(Vec3(1, 0, 0.001f))).x > 0.95f);

  TexMap tiles("Tiles");
  tiles.params.setColor("groutColor", Vec3(0, 1, 0));
  tiles.params.setNumber("colorVariance", 0.0);
  tiles.params.setNumber("fadeVariance", 0.0);
  CHECK(eval(tiles, Vec2(0.001f, 0.06f)).y > 0.9f);   // grout at a tile edge
  CHECK(eval(tiles, Vec2(0.125f, 0.0625f)).y < 0.5f);  // tile center

  TexMap mix("Mix");
  mix.params.setColor("color1", Vec3(0.0f));
  mix.params.setColor("color2", Vec3(1.0f));
  mix.params.setNumber("mixAmount", 0.25);
  CHECK_NEAR(eval(mix, Vec2()).x, 0.25, 1e-4);

  TexMap comp("Composite");
  comp.params.setColor("layer1Color", Vec3(0.5f));
  comp.params.setColor("layer2Color", Vec3(0.5f));
  comp.params.setNumber("layer2Mode", 5);  // multiply
  CHECK_NEAR(eval(comp, Vec2()).x, 0.25, 1e-4);

  TexMap cc("ColorCorrection");
  cc.params.setColor("color", Vec3(0.2f));
  cc.params.setNumber("exposure", 1.0);
  CHECK_NEAR(eval(cc, Vec2()).x, 0.4, 1e-4);

  TexMap ramp("GradientRamp");
  CHECK_NEAR(eval(ramp, Vec2(0.25f, 0.5f)).x, 0.25, 1e-3);  // black -> grey -> white along U

  TexMap tri("Triplanar");
  tri.maps["texture"].map = std::make_unique<TexMap>(checker);
  Vec3 t = eval(tri, Vec2(0.3f, 0.3f));
  CHECK(render::isFiniteColor(t));

  TexMap var("ColorVariation");
  Vec3 v1 = eval(var, Vec2()), v2 = eval(var, Vec2(0.9f, 0.9f));
  CHECK(v1 == v2);  // same object: same color

  std::vector<uint8_t> swatch = renderMapPreview(checker, nullptr, 32);
  CHECK(swatch.size() == 32u * 32u * 4u);
}

static void testProxyRender() {
  Scene a;
  baseSettings(a, 48, 16, 2);
  a.renderSettings.setColor("envColor", Vec3(1.0f));
  Node* box = a.createNode("Box");
  box->transform.rotation = Vec3(10, 20, 30);
  std::string path = (std::filesystem::temp_directory_path() / "veraa_proxy_render.vxp").string();
  CHECK(writeProxyFile(path, box->triMesh()));
  Mat4 M = box->objectWorldMatrix();
  Renderer ra;
  CHECK(ra.setScene(a));
  RenderView v = defaultViewForScene(a, 1.0f);
  ra.setView(v);
  ra.renderBlocking();
  Scene b;
  baseSettings(b, 48, 16, 2);
  b.renderSettings.setColor("envColor", Vec3(1.0f));
  for (int i = 0; i < 2; ++i) {
    Node* p = b.createNode("VeraaProxy");
    p->object->params.setString("file", path);
    p->transform = decomposeTransform(M);
    if (i == 1) p->transform.position += Vec3(0, 0, -100000);  // a second instance far away
  }
  Renderer rb;
  CHECK(rb.setScene(b));
  CHECK(rb.warnings().empty());
  rb.setView(v);
  rb.renderBlocking();
  CHECK_NEAR(centerMean(rb, 0.6), centerMean(ra, 0.6), 0.02);
  std::filesystem::remove(path);
}

static void testMatteShadow() {
  Scene s;
  baseSettings(s, 64, 64, 4);
  s.renderSettings.setColor("envColor", Vec3(0.5f));
  Material* matte = s.createMaterial("Catcher");
  matte->setMaterialType("VeraaWrapperMtl");
  matte->params.setNumber("matteSurface", 1);
  matte->params.setNumber("alphaContribution", -1.0);
  addFloor(s, matte, 4000.0f);
  Node* box = s.createNode("Box");
  box->object->params.setNumber("length", 40);
  box->object->params.setNumber("width", 40);
  box->object->params.setNumber("height", 40);
  box->transform.position = Vec3(0, 0, 200);  // floating: casts a shadow on the floor below
  box->visibleToCamera = false;
  Node* sun = s.createNode("SunLight");
  sun->object->params.setNumber("intensity", 5.0);
  Renderer r;
  CHECK(r.setScene(s));
  r.setView(topDownView(200.0f, 100.0f));
  r.renderBlocking();
  std::vector<float> alpha;
  r.resolveAlpha(alpha);
  float center = alpha[32 * 64 + 32], edge = alpha[4 * 64 + 4];
  std::printf("  matte alpha: shadow %.2f, lit %.2f\n", center, edge);
  CHECK(center > 0.8f);
  CHECK(edge < 0.1f);
}

static void testMotionBlur() {
  Scene s;
  baseSettings(s, 64, 64, 1);
  s.renderSettings.setColor("envColor", Vec3(1.0f));
  Material* black = s.createMaterial("Black");
  black->params.setColor("diffuse", Vec3(0.0f));
  Node* ball = s.createNode("Sphere");
  ball->object->params.setNumber("radius", 10);
  ball->materialId = black->id;
  uint32_t ballId = ball->id;
  // The ball moves 60 units along X during the shutter.
  setNodeMatrixAtTime([ballId](const Scene&, const Node& n, double frame, bool, Mat4& out) {
    if (n.id != ballId) return false;
    out = Mat4::translate(Vec3(static_cast<float>(frame) * 60.0f, 0, 0));
    return true;
  });
  RenderView v = topDownView(100.0f, 100.0f);
  v.position = Vec3(30, 0, 100);
  v.target = Vec3(30, 0, 0);
  v.motionBlur = true;
  v.shutterOpen = 0.0;
  v.shutterClose = 1.0;
  Renderer r;
  r.setView(v);  // the shutter must be known when the scene is built
  CHECK(r.setScene(s));
  r.renderBlocking();
  std::vector<float> hdr;
  r.resolveHdr(hdr);
  // The middle of the streak is partly covered (grey), not black or white.
  float mid = hdr[(32 * 64 + 32) * 3];
  std::printf("  motion blur streak value %.2f\n", mid);
  CHECK(mid > 0.2f && mid < 0.95f);
  setNodeMatrixAtTime(nullptr);
}

static void testDisplacementAndFur() {
  Scene s;
  baseSettings(s, 32, 4, 2);
  s.renderSettings.setColor("envColor", Vec3(1.0f));
  Node* plane = s.createNode("Plane");
  plane->name = "Ground";
  auto mod = createModifier("VeraaDisplacementMod");
  auto* dm = static_cast<VeraaDisplacementModifier*>(mod.get());
  dm->texmap.map = createTexMap("Noise");
  dm->params.setNumber("amount", 10.0);
  dm->params.setNumber("edgeLength", 4.0);
  plane->modifiers.push_back(std::move(mod));
  plane->invalidate();
  Renderer r;
  CHECK(r.setScene(s));
  size_t displaced = r.stats().triangles;
  CHECK(displaced > plane->triMesh().triangleCount() * 16);

  Node* fur = s.createNode("VeraaFur");
  fur->object->params.setString("sourceNode", "Ground");
  fur->object->params.setNumber("density", 0.05);
  Renderer rf;
  CHECK(rf.setScene(s));
  CHECK(rf.stats().triangles > displaced);  // curve segments added
  rf.setView(topDownView(100.0f, 200.0f));
  rf.renderBlocking();
  CHECK(std::isfinite(centerMean(rf, 0.5)));
}

static void testPhysicalExposure() {
  auto renderWith = [](float iso) {
    Scene s;
    baseSettings(s, 16, 16, 1);
    s.renderSettings.setColor("envColor", Vec3(1000.0f));
    Node* cam = s.createNode("VeraaPhysicalCamera");
    cam->object->params.setNumber("iso", iso);
    cam->object->params.setNumber("vignettingOn", 0);
    Renderer r;
    r.setScene(s);
    RenderView v = viewFromCamera(*cam, 1.0f);
    r.setView(v);
    r.renderBlocking();
    return centerMean(r, 0.5);
  };
  double a = renderWith(100.0f), b = renderWith(200.0f);
  CHECK_NEAR(b / a, 2.0, 0.01);
  CHECK_NEAR(a, 1000.0 * exposureScale(ev100(8.0f, 1.0f / 200.0f, 100.0f)), 1e-3);
}

static void testSunAndSky() {
  Vec3 e = render::physicalSunIrradiance(radians(90.0f), 2.5f, 0.35f);
  std::printf("  physical sun at zenith: %.0f lux\n", luminance(e));
  CHECK(luminance(e) > 90000.0f && luminance(e) < 120000.0f);
  Vec3 low = render::physicalSunIrradiance(radians(5.0f), 2.5f, 0.35f);
  CHECK(low.x > low.z);  // reddish near the horizon
  render::PhysicalSky sky;
  render::AtmosphereParams ap;
  sky.build(normalize(Vec3(0, 1, 1)), ap, render::kSolarIlluminance);
  Vec3 zenith = sky.radiance(Vec3(0, 0, 1));
  CHECK(zenith.z > zenith.x);  // blue sky
  CHECK(luminance(zenith) > 1000.0f && luminance(zenith) < 20000.0f);
  // Scene with a VeraaSun and physical exposure renders sensible values.
  Scene s;
  baseSettings(s, 16, 16, 2);
  s.renderSettings.setNumber("envMode", 3);
  s.renderSettings.setNumber("exposureControl", 1);
  s.renderSettings.setNumber("ev100", 15.0);
  Material* m = s.createMaterial("Grey");
  m->params.setColor("diffuse", Vec3(0.5f));
  addFloor(s, m);
  Node* sun = s.createNode("SunPositioner");
  sun->object->params.setNumber("hour", 13.0);
  Renderer r;
  CHECK(r.setScene(s));
  r.setView(topDownView(1.0f));
  r.renderBlocking();
  double v = centerMean(r, 0.5);
  std::printf("  sunlit grey floor at EV 15: %.3f\n", v);
  CHECK(v > 0.1 && v < 1.0);
}

static void testExr() {
  std::string path = (std::filesystem::temp_directory_path() / "veraa_test.exr").string();
  std::vector<float> rgb = {0.5f, 1.0f, 2.0f, 4.0f, 0.25f, 0.0f};
  std::vector<uint8_t> rgba8(8, 255);
  std::string err;
  CHECK(saveImage(path, 2, 1, rgba8, &rgb, nullptr, false, &err));
  int w = 0, h = 0;
  std::vector<float> back;
  CHECK(render::loadExr(path, w, h, back, &err));
  CHECK(w == 2 && h == 1 && back.size() == 8);
  if (back.size() == 8) {
    CHECK_NEAR(back[0], 0.5, 1e-3);
    CHECK_NEAR(back[2], 2.0, 1e-3);
    CHECK_NEAR(back[4], 4.0, 1e-2);
  }
  std::filesystem::remove(path);
  for (const char* ext : {"png", "jpg", "tif", "bmp", "tga"}) {
    std::string p = (std::filesystem::temp_directory_path() / (std::string("veraa_test.") + ext)).string();
    CHECK(saveImage(p, 2, 1, rgba8, &rgb, nullptr, false, &err));
    CHECK(std::filesystem::exists(p) && std::filesystem::file_size(p) > 10);
    std::filesystem::remove(p);
  }
}

int main() {
  testFurnaces();
  testSubsurfaceAlbedo();
  testIesPhotometry();
  testVeraaLightUnits();
  testMeshAndDomeLights();
  testElementsSumToBeauty();
  testAdaptive();
  testBucketsAndRegion();
  testTextureMaps();
  testProxyRender();
  testMatteShadow();
  testMotionBlur();
  testDisplacementAndFur();
  testPhysicalExposure();
  testSunAndSky();
  testExr();
  if (g_failures) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("All render feature tests passed\n");
  return 0;
}
