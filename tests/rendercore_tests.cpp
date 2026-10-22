// Core tests of the rendering module: texture maps, material types, legacy scene upgrade,
// photometry (color temperature, IES), solar position, proxies, fur and the physical camera.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "photometry.h"
#include "renderobjects.h"
#include "rendersettings.h"
#include "scene.h"

using namespace veraa;

static int g_failures = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
      ++g_failures;                                                          \
    }                                                                        \
  } while (0)

static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

static void testTexMaps() {
  CHECK(texMapTypes().size() >= 16);
  for (const auto& t : texMapTypes()) {
    auto m = createTexMap(t.type);
    CHECK(m != nullptr);
    for (const auto& key : t.mappable) (void)key;
    json j = m->toJson();
    auto back = TexMap::fromJson(j);
    CHECK(back && back->type == t.type);
    CHECK(back && back->params.toJson() == m->params.toJson());
  }
  // A tree of maps survives a round trip and deep copies.
  auto mix = createTexMap("Mix");
  MapSlot s;
  s.map = createTexMap("Checker");
  s.map->params.setNumber("uTiling", 8);
  s.amount = 0.5f;
  mix->maps["color1"] = s;
  mix->maps["color2"].map = createTexMap("Noise");
  TexMap copy = *mix;
  copy.maps["color1"].map->params.setNumber("uTiling", 2);
  CHECK(mix->mapFor("color1")->params.getFloat("uTiling") == 8.0f);
  auto back = TexMap::fromJson(mix->toJson());
  CHECK(back && back->mapFor("color1") && back->mapFor("color1")->type == "Checker");
  CHECK(back && back->maps.at("color1").amount == 0.5f);
  CHECK(back && back->mapFor("color2") && back->mapFor("color2")->type == "Noise");
  CHECK(TexMap::fromJson(json{{"type", "NoSuchMap"}}) == nullptr);
}

static void testMaterials() {
  CHECK(materialTypes().size() >= 9);
  for (const auto& t : materialTypes()) {
    Material m(t.type);
    CHECK(m.materialType == t.type);
    Material back;
    back.fromJson(m.toJson());
    CHECK(back.materialType == t.type);
    CHECK(back.params.toJson() == m.params.toJson());
    CHECK(back.subs.size() == m.subs.size());
  }
  // VeraaMtl keeps the original keys used by older scenes and the renderer.
  Material v;
  for (const char* k : {"diffuse", "roughness", "reflect", "reflectGlossiness", "fresnel", "fresnelIor", "metalness",
                        "refract", "refractGlossiness", "ior", "fogColor", "fogMultiplier", "thinWalled",
                        "affectShadows", "selfIllum", "selfIllumMultiplier", "opacity", "bumpAmount", "coatAmount",
                        "sheenColor", "anisotropy", "abbe", "translucency"})
    CHECK(v.params.has(k));

  // Legacy file maps load as Bitmap slots with the old tiling.
  json legacy{{"id", 3},
              {"name", "Old"},
              {"type", "VeraaMtl"},
              {"params",
               {{"diffuse", {0.2, 0.3, 0.4}},
                {"diffuseMap", "wood.jpg"},
                {"diffuseMapAmount", 0.75},
                {"bumpMap", "bump.png"},
                {"mapTileU", 4.0},
                {"mapTileV", 2.0}}}};
  Material old;
  old.fromJson(legacy);
  CHECK(old.id == 3);
  CHECK(old.params.getString("diffuseMap").empty());
  const TexMap* d = old.mapFor("diffuse");
  CHECK(d && d->type == "Bitmap" && d->params.getString("file") == "wood.jpg");
  CHECK(d && d->params.getFloat("uTiling") == 4.0f && d->params.getFloat("vTiling") == 2.0f);
  CHECK(old.maps.at("diffuse").amount == 0.75f);
  CHECK(old.mapFor("bump") && old.mapFor("bump")->params.getString("file") == "bump.png");
  CHECK(old.viewportTexture() == "wood.jpg");

  // Compound materials copy deeply and survive files.
  Material blend("VeraaBlendMtl");
  CHECK(blend.subs.size() == 10);
  blend.subs[1].material = std::make_unique<Material>("VeraaMtl");
  blend.subs[1].material->params.setColor("diffuse", Vec3(1, 0, 0));
  Material blendCopy = blend;
  blendCopy.subs[1].material->params.setColor("diffuse", Vec3(0, 1, 0));
  CHECK(blend.subs[1].material->params.getColor("diffuse") == Vec3(1, 0, 0));
  Material blendBack;
  blendBack.fromJson(blend.toJson());
  CHECK(blendBack.subs.size() == 10 && blendBack.subs[1].material &&
        blendBack.subs[1].material->params.getColor("diffuse") == Vec3(1, 0, 0));
  CHECK(blendBack.subs[2].material == nullptr);

  Material multi("MultiSubObject");
  CHECK(multi.subs.size() == 4);
  multi.subs.push_back(SubMaterialSlot());
  multi.subs.back().matId = 7;
  multi.subs.back().material = std::make_unique<Material>("VeraaLightMtl");
  Material multiBack;
  multiBack.fromJson(multi.toJson());
  CHECK(multiBack.subs.size() == 5);
  CHECK(multiBack.subForFaceId(6) && multiBack.subForFaceId(6)->materialType == "VeraaLightMtl");
  CHECK(multiBack.subForFaceId(0) == multiBack.subs[0].material.get());
  CHECK(multiBack.subForFaceId(11) != nullptr);  // wraps around

  // Changing the type keeps shared parameters and maps.
  Material conv;
  conv.params.setColor("diffuse", Vec3(0.1f, 0.2f, 0.3f));
  conv.maps["diffuse"].map = createTexMap("Checker");
  conv.setMaterialType("VeraaBlendMtl");
  CHECK(conv.materialType == "VeraaBlendMtl" && conv.maps.empty());
  conv.setMaterialType("VeraaMtl");
  CHECK(conv.params.getColor("diffuse") == Vec3(0.5f));

  // Scenes save and load material trees.
  Scene s;
  Material* sm = s.createMaterial("Tree");
  sm->setMaterialType("VeraaTwoSidedMtl");
  sm->subs[1].material = std::make_unique<Material>("VeraaCarPaintMtl");
  json sj = s.toJson();
  Scene s2;
  CHECK(s2.fromJson(sj));
  CHECK(s2.materials.size() == 1 && s2.materials[0]->materialType == "VeraaTwoSidedMtl");
  CHECK(s2.materials[0]->subs[1].material && s2.materials[0]->subs[1].material->materialType == "VeraaCarPaintMtl");
}

static void testColorTemperature() {
  Vec3 w = kelvinToRgb(6500.0f);
  CHECK(near(w.x, 1.0, 0.02) && near(w.y, 1.0, 0.02) && near(w.z, 1.0, 0.02));
  Vec3 warm = kelvinToRgb(2700.0f), cool = kelvinToRgb(10000.0f);
  CHECK(warm.x > warm.y && warm.y > warm.z);
  CHECK(cool.z > cool.x);
  CHECK(near(luminance(warm), 1.0, 1e-3));
}

static void testIes() {
  // Isotropic 1000 cd source: flux = 4 pi 1000 lm.
  std::vector<float> v = {0, 45, 90, 135, 180}, c(5, 1000.0f);
  IesProfile p;
  std::string err;
  CHECK(p.parse(makeIesText(v, c, "isotropic"), &err));
  CHECK(p.valid());
  CHECK(near(p.intensity(30, 77), 1000.0, 1e-3));
  CHECK(near(p.totalFlux(), 4.0 * kPi * 1000.0, 4.0 * kPi * 1000.0 * 0.002));
  // Downlight: cos falloff in the lower hemisphere: flux = pi * I0.
  std::vector<float> va, ca;
  for (int a = 0; a <= 180; a += 5) {
    va.push_back(static_cast<float>(a));
    ca.push_back(a <= 90 ? 2000.0f * std::cos(radians(static_cast<float>(a))) : 0.0f);
  }
  IesProfile d;
  CHECK(d.parse(makeIesText(va, ca, "cosine"), &err));
  CHECK(near(d.intensity(0, 0), 2000.0, 1e-2));
  CHECK(near(d.intensity(60, 200), 1000.0, 5.0));
  CHECK(near(d.intensity(120, 0), 0.0, 1e-3));
  CHECK(near(d.totalFlux(), kPi * 2000.0, kPi * 2000.0 * 0.01));
  // Quadrant symmetric data with keyword lines and TILT=NONE parses.
  std::string quad =
      "IESNA:LM-63-1995\n[TEST] quad\nTILT=NONE\n1 1000 1 3 3 1 2 0.1 0.1 0\n1 1 50\n0 45 90\n0 45 90\n"
      "100 50 0\n200 100 0\n300 150 0\n";
  IesProfile q;
  CHECK(q.parse(quad, &err));
  CHECK(near(q.intensity(0, 90), 300.0, 1e-3));
  CHECK(near(q.intensity(0, 270), 300.0, 1e-3));   // folded into 0..90
  CHECK(near(q.intensity(0, 135), 200.0, 1e-3));
  CHECK(!q.parse("garbage", &err));
  // The sample profiles shipped with Veraa load.
  for (const char* f : {"downlight_narrow.ies", "downlight_wide.ies", "wallwasher.ies", "pendant_uplight.ies"}) {
    IesProfile sp;
    std::string path = std::string(VERAA_SOURCE_DIR) + "/assets/ies/" + f;
    bool ok = sp.load(path, &err);
    if (!ok) std::printf("  %s: %s\n", f, err.c_str());
    CHECK(ok && sp.totalFlux() > 10.0f);
  }
}

static void testSolarPosition() {
  // Solar noon at an equinox: elevation = 90 - latitude, sun due south (northern hemisphere).
  SolarInput in;
  in.year = 2023;
  in.month = 3;
  in.day = 20;
  in.latitude = 40.0f;
  in.longitude = 0.0f;
  in.timezone = 0.0f;
  in.hour = 12.0f + 7.5f / 60.0f;  // equation of time is about -7.5 min on March 20
  SolarPosition p = solarPosition(in);
  CHECK(near(p.elevationDeg, 50.0, 0.5));
  CHECK(near(p.azimuthDeg, 180.0, 0.6));
  CHECK(near(p.declinationDeg, 0.0, 0.5));
  // June solstice at 40N: noon elevation 73.44, declination 23.44.
  in.month = 6;
  in.day = 21;
  in.hour = 12.0f + 1.7f / 60.0f;
  p = solarPosition(in);
  CHECK(near(p.declinationDeg, 23.44, 0.1));
  CHECK(near(p.elevationDeg, 73.44, 0.5));
  // Equation of time extremes: about +16.4 min early November, -14.2 min mid February.
  in.month = 11;
  in.day = 3;
  CHECK(near(solarPosition(in).equationOfTimeMin, 16.4, 0.3));
  in.month = 2;
  in.day = 11;
  CHECK(near(solarPosition(in).equationOfTimeMin, -14.2, 0.3));
  // Equinox sunrise on the equator: due east, at the horizon (6:00 local mean solar time + EoT).
  in = SolarInput();
  in.year = 2023;
  in.month = 3;
  in.day = 20;
  in.latitude = 0.0f;
  in.longitude = 0.0f;
  in.hour = 6.0f + 7.5f / 60.0f;
  p = solarPosition(in);
  CHECK(near(p.azimuthDeg, 90.0, 0.6));
  CHECK(near(p.elevationDeg, 0.0, 0.5));
  // Time zones shift clock time: 13:00 UTC+1 equals 12:00 UTC at longitude 0.
  SolarInput a = in, b = in;
  a.hour = 12.0f;
  b.hour = 13.0f;
  b.timezone = 1.0f;
  CHECK(near(solarPosition(a).elevationDeg, solarPosition(b).elevationDeg, 1e-3));
  // Direction: azimuth 90 (east) with north = +Y points along +X.
  Vec3 e = solarDirection(90.0f, 0.0f, 0.0f);
  CHECK(near(e.x, 1.0, 1e-4) && near(e.z, 0.0, 1e-4));
  Vec3 s = solarDirection(180.0f, 45.0f, 0.0f);
  CHECK(s.y < -0.7f && near(s.z, std::sqrt(0.5), 1e-3));
}

static void testUnitsAndExposure() {
  CHECK(near(ev100(1.0f, 1.0f, 100.0f), 0.0, 1e-5));
  CHECK(near(ev100(16.0f, 1.0f / 100.0f, 100.0f), 14.64, 0.01));  // "sunny 16"
  CHECK(near(exposureScale(0.0f), 1.0 / 1.2, 1e-5));
  // 1000 lm from a 1 m² one-sided Lambertian panel: L = 1000 / pi.
  CHECK(near(luminanceFromUnits(LightUnits::Lumens, 1000.0f, 1.0f, 1), 1000.0 / kPi, 1e-2));
  CHECK(near(luminanceFromUnits(LightUnits::Watts, 1.0f, 1.0f, 2), 683.0 / (2 * kPi), 1e-2));
}

static void testObjects() {
  for (const char* t : {"VeraaLight", "VeraaIES", "VeraaSun", "SunPositioner", "VeraaPhysicalCamera", "VeraaProxy",
                        "VeraaFur", "VeraaInfinitePlane"}) {
    auto o = createObject(t);
    CHECK(o != nullptr);
    if (o) CHECK(o->type() == t);
  }
  CHECK(createModifier("VeraaDisplacementMod") != nullptr);
  // Physical camera: 36 mm gate, 36 mm lens = 53.13 deg horizontal.
  VeraaPhysicalCameraObject cam;
  cam.params.setNumber("focalLength", 36.0);
  CHECK(near(cam.horizontalFovDeg(), 53.13, 0.01));
  CHECK(near(cam.verticalFovDeg(1.0f), 53.13, 0.01));
  cam.params.setNumber("fNumber", 2.0);
  CHECK(near(cam.apertureRadius(), 0.9, 1e-4));  // 36 mm / 2 / 2 = 9 mm
  CHECK(near(cam.exposureMultiplier(30.0f), exposureScale(ev100(2.0f, 1.0f / 200.0f, 100.0f)), 1e-9));
  cam.params.setNumber("exposure", 0);
  CHECK(cam.exposureMultiplier(30.0f) == 1.0f);
  // Scene round trip keeps the camera type (and its overridden FOV).
  Scene s;
  Node* n = s.createNode("VeraaPhysicalCamera");
  CHECK(n && n->camera());
  n->object->params.setNumber("focalLength", 18.0);
  Scene s2;
  CHECK(s2.fromJson(s.toJson()));
  CHECK(s2.nodes.size() == 1 && s2.nodes[0]->camera() &&
        near(s2.nodes[0]->camera()->verticalFovDeg(1.0f), 90.0, 0.1));

  // Sun positioner direction follows the date and time.
  VeraaSunObject sun(true);
  sun.params.setNumber("latitude", 40.0);
  sun.params.setNumber("longitude", 0.0);
  sun.params.setNumber("timezone", 0.0);
  sun.params.setNumber("daylightSaving", 0);
  sun.params.setNumber("month", 6);
  sun.params.setNumber("day", 21);
  sun.params.setNumber("hour", 12.03);
  Vec3 d = sun.sunDirection(Vec3(0, 0, 1));
  CHECK(near(std::asin(d.z) * 180.0 / kPi, 73.44, 0.5));
  CHECK(d.y < 0.0f);  // south

  // Render settings from the module exist in every scene.
  CHECK(s.renderSettings.has("noiseThreshold") && s.renderSettings.has("fogOn") && s.renderSettings.has("elemReflection"));
  for (const auto& e : renderElementInfos()) CHECK(s.renderSettings.has(std::string("elem") + e.key));
}

static void testProxyAndFur() {
  auto box = createObject("Box");
  auto* g = dynamic_cast<GeometryObject*>(box.get());
  TriMesh tm = triangulate(g->build());
  std::string path = (std::filesystem::temp_directory_path() / "veraa_test_proxy.vxp").string();
  std::string err;
  CHECK(writeProxyFile(path, tm, &err));
  auto back = loadProxyFile(path, &err);
  CHECK(back && back->positions.size() == tm.positions.size() && back->indices == tm.indices);
  VeraaProxyObject proxy;
  proxy.params.setString("file", path);
  proxy.params.setNumber("displayMode", 2);
  CHECK(proxy.build().faces.size() == tm.triangleCount());
  proxy.params.setNumber("displayMode", 0);
  CHECK(proxy.build().faces.size() == 6);
  std::filesystem::remove(path);
  CHECK(loadProxyFile(path, &err) == nullptr);

  VeraaFurObject fur;
  fur.params.setNumber("density", 0.5);
  auto strands = generateFur(fur.params, tm, 0);
  float area = 0.0f;
  for (size_t t = 0; t < tm.triangleCount(); ++t)
    area += 0.5f * length(cross(tm.positions[tm.indices[t * 3 + 1]] - tm.positions[tm.indices[t * 3]],
                                tm.positions[tm.indices[t * 3 + 2]] - tm.positions[tm.indices[t * 3]]));
  CHECK(near(static_cast<double>(strands.size()), area * 0.5, 2.0));
  CHECK(!strands.empty() && strands[0].points.size() == static_cast<size_t>(fur.params.getInt("knots")));
  CHECK(generateFur(fur.params, tm, 10).size() == 10);

  VeraaDisplacementModifier dm;
  dm.texmap.map = createTexMap("Noise");
  json j = dm.toJson();
  VeraaDisplacementModifier dm2;
  dm2.fromJson(j);
  CHECK(dm2.texmap.map && dm2.texmap.map->type == "Noise");
}

int main() {
  testTexMaps();
  testMaterials();
  testColorTemperature();
  testIes();
  testSolarPosition();
  testUnitsAndExposure();
  testObjects();
  testProxyAndFur();
  if (g_failures == 0) std::printf("All render core tests passed\n");
  return g_failures == 0 ? 0 : 1;
}
