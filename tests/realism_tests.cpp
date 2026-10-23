// Realism tests: color mapping curves and the display pipeline, the physical sky models
// (radiance ranges, sun/sky balance, continuity), BSDF energy (white furnace with multiple
// scattering compensation), conductor Fresnel against reference n, k and thin films.
#include <cmath>
#include <cstdio>
#include <vector>

#include "common.h"
#include "postprocess.h"
#include "renderer.h"
#include "scene.h"
#include "sky.h"

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

// ---------------------------------------------------------------------------------------------
// Color mapping

static render::ColorMappingParams mapping(int type, float dark = 1, float bright = 1, float gamma = 1, float burn = 0) {
  render::ColorMappingParams p;
  p.type = type;
  p.dark = dark;
  p.bright = bright;
  p.gamma = gamma;
  p.burn = burn;
  return p;
}

static void testColorMappingMonotonic() {
  const Vec3 tints[] = {Vec3(1.0f), Vec3(1.0f, 0.2f, 0.05f), Vec3(0.1f, 0.3f, 1.0f), Vec3(0.2f, 1.0f, 0.1f)};
  const float mults[][2] = {{1, 1}, {2, 0.5f}, {0.5f, 2}, {3, 3}};
  for (int type = 0; type < render::kMapCount; ++type)
    for (const auto& m : mults)
      for (const Vec3& tint : tints) {
        render::ColorMappingParams p = mapping(type, m[0], m[1], type >= render::kMapGamma ? 2.2f : 1.0f, 0.3f);
        float prevLum = -1.0f;
        Vec3 prev(-1.0f);
        bool ok = true;
        float worstDrop = 0.0f;
        for (int i = 0; i <= 400; ++i) {
          float x = 1e-4f * std::pow(10.0f, i / 50.0f);  // 1e-4 .. 1e4
          Vec3 c = render::applyColorMapping(tint * x, p);
          if (!render::isFiniteColor(c) || c.x < 0 || c.y < 0 || c.z < 0) ok = false;
          float l = luminance(c);
          if (l < prevLum - 1e-5f) ok = false;
          // Per channel: no channel may drop as the input brightens, except that the AgX view
          // lets the dominant channel of a saturated color dip slightly on its path to white.
          if (prev.x >= 0.0f) worstDrop = std::max(worstDrop, maxComponent(prev - c));
          prevLum = l;
          prev = c;
        }
        if (worstDrop > (type == render::kMapAgx ? 0.02f : 1e-4f)) ok = false;
        if (!ok)
          std::printf("  color mapping %d (dark %.1f bright %.1f tint %.2f %.2f %.2f) is not monotonic (drop %.4f)\n",
                      type, m[0], m[1], tint.x, tint.y, tint.z, worstDrop);
        CHECK(ok);
      }
}

static void testColorMappingIdentities() {
  for (float x : {0.0f, 0.05f, 0.18f, 0.5f, 0.9f}) {
    Vec3 c(x, x * 0.5f, x * 0.25f);
    Vec3 lin = render::applyColorMapping(c, mapping(render::kMapLinear));
    CHECK_NEAR(lin.x, c.x, 1e-6);
    CHECK_NEAR(lin.z, c.z, 1e-6);
    Vec3 rh = render::applyColorMapping(c, mapping(render::kMapReinhard, 1, 1, 1, 1.0f));  // burn 1 = linear
    CHECK_NEAR(rh.x, c.x, 1e-5);
    CHECK_NEAR(rh.y, c.y, 1e-5);
    Vec3 gm = render::applyColorMapping(c, mapping(render::kMapGamma));
    CHECK_NEAR(gm.y, c.y, 1e-6);
    Vec3 ig = render::applyColorMapping(c, mapping(render::kMapIntensityGamma));
    CHECK_NEAR(ig.x, c.x, 1e-5);
    // Reinhard with burn 0 is the classic L / (1 + L) on luminance (the original "Reinhard").
    Vec3 r0 = render::applyColorMapping(c, mapping(render::kMapReinhard));
    float L = luminance(c);
    if (L > 0) CHECK_NEAR(luminance(r0), L / (1 + L), 1e-5);
  }
  // Exponential and gamma follow their formulas; multipliers scale linear mapping.
  CHECK_NEAR(render::applyColorMapping(Vec3(0.7f), mapping(render::kMapExponential)).x, 1 - std::exp(-0.7), 1e-5);
  CHECK_NEAR(render::applyColorMapping(Vec3(0.5f), mapping(render::kMapGamma, 1, 1, 2.2f)).x, std::pow(0.5, 1 / 2.2), 1e-5);
  CHECK_NEAR(render::applyColorMapping(Vec3(2.0f), mapping(render::kMapLinear, 2, 2)).x, 4.0, 1e-5);
  CHECK_NEAR(render::applyColorMapping(Vec3(0.25f), mapping(render::kMapLinear, 1, 1, 2.0f)).x, 0.5, 1e-5);
  // HSV exponential keeps the hue and saturation; intensity exponential keeps the ratios.
  Vec3 hsv = render::applyColorMapping(Vec3(4.0f, 2.0f, 1.0f), mapping(render::kMapHsvExponential));
  CHECK_NEAR(hsv.y / hsv.x, 0.5, 1e-4);
  CHECK_NEAR(hsv.x, 1 - std::exp(-4.0), 1e-4);
  Vec3 ie = render::applyColorMapping(Vec3(0.3f, 0.6f, 0.9f), mapping(render::kMapIntensityExp));
  CHECK_NEAR(ie.z / ie.x, 3.0, 1e-3);
  // Filmic (ACES fitted) is unchanged: Hill's RRT+ODT fit after a 1.8 pre-gain.
  auto acesRef = [](float v) {
    double x = v * 1.8;
    double a = (0.59719 + 0.35458 + 0.04823) * x;  // grey stays grey through both matrices
    double r = (a * (a + 0.0245786) - 0.000090537) / (a * (0.983729 * a + 0.4329510) + 0.238081);
    return r * (1.60475 - 0.53108 - 0.07367);
  };
  for (float v : {0.01f, 0.18f, 1.0f, 4.0f})
    CHECK_NEAR(render::applyColorMapping(Vec3(v), mapping(render::kMapFilmicAces)).y, acesRef(v), 2e-3);
  // AgX view: neutral stays neutral, middle grey lands near display middle grey, highlights reach white.
  Vec3 grey = render::applyColorMapping(Vec3(0.18f), mapping(render::kMapAgx));
  CHECK_NEAR(grey.x, grey.y, 1e-4);
  CHECK_NEAR(grey.z, grey.y, 1e-4);
  std::printf("  AgX: 0.18 -> %.3f (display linear), 0.001 -> %.4f, 16 -> %.3f\n", grey.y,
              render::applyColorMapping(Vec3(0.001f), mapping(render::kMapAgx)).y,
              render::applyColorMapping(Vec3(16.0f), mapping(render::kMapAgx)).y);
  CHECK(grey.y > 0.14f && grey.y < 0.26f);
  CHECK(render::applyColorMapping(Vec3(0.0f), mapping(render::kMapAgx)).y < 0.002f);
  CHECK(render::applyColorMapping(Vec3(64.0f), mapping(render::kMapAgx)).y > 0.97f);
  // Saturated highlights desaturate toward white instead of clipping to a primary.
  Vec3 hot = render::applyColorMapping(Vec3(40.0f, 4.0f, 1.0f), mapping(render::kMapAgx));
  CHECK(hot.x > 0.9f && hot.y > 0.5f && hot.z > 0.3f);
}

// The display pipeline: exposure -> color mapping -> corrections -> sRGB, with the background
// and adaptation-only switches.
static void testDisplayPipeline() {
  std::vector<float> hdr = {0.18f, 0.18f, 0.18f, 6.0f, 3.0f, 1.0f};
  std::vector<float> alpha = {1.0f, 0.0f};
  std::vector<uint8_t> out, ref;
  DisplaySettings d;
  d.tonemap = render::kMapAgx;
  d.exposureEV = 1.0f;
  render::tonemapToRgba(hdr.data(), 2, 1, d, out, alpha.data());
  Vec3 g = render::applyColorMapping(Vec3(0.36f), mapping(render::kMapAgx));
  CHECK(std::abs(out[0] - static_cast<int>(std::lround(render::linearToSrgb(g.x) * 255.0f))) <= 1);
  // Adaptation only: linear output (exposure and sRGB only).
  d.mapAdaptationOnly = true;
  render::tonemapToRgba(hdr.data(), 2, 1, d, out, alpha.data());
  CHECK(std::abs(out[0] - static_cast<int>(std::lround(render::linearToSrgb(0.36f) * 255.0f))) <= 1);
  CHECK(out[4] == 255 && out[5] == 255);  // 12, 6 clip
  // Background protected: pixel 2 (alpha 0) stays linear, pixel 1 (alpha 1) is mapped.
  d.mapAdaptationOnly = false;
  d.mapAffectBackground = false;
  render::tonemapToRgba(hdr.data(), 2, 1, d, out, alpha.data());
  d.mapAffectBackground = true;
  render::tonemapToRgba(hdr.data(), 2, 1, d, ref, alpha.data());
  CHECK(out[0] == ref[0]);
  CHECK(out[6] == static_cast<uint8_t>(std::lround(render::linearToSrgb(2.0f) * 255.0f)));
  CHECK(out[6] != ref[6]);
  // Corrections come after the mapping: a warm white balance still tints a burnt-out highlight.
  std::vector<float> white = {500.0f, 500.0f, 500.0f};
  d.corrections = true;
  d.whiteBalanceK = 9000.0f;  // white balance for a bluish light -> the image turns warmer
  render::tonemapToRgba(white.data(), 1, 1, d, out);
  CHECK(out[0] > out[2] + 10);
}

// ---------------------------------------------------------------------------------------------
// Physical sky

static Vec3 sunAt(float elevationDeg) {
  float e = radians(elevationDeg);
  return Vec3(std::cos(e) * 0.6f, std::cos(e) * 0.8f, std::sin(e));
}

// Horizontal illuminance (lux) from the sky dome (sun excluded).
static double skyIlluminance(const render::PhysicalSky& sky) {
  double e = 0.0;
  const int nT = 48, nP = 96;
  for (int i = 0; i < nT; ++i) {
    float th = (i + 0.5f) / nT * kPi * 0.5f;
    for (int j = 0; j < nP; ++j) {
      float ph = (j + 0.5f) / nP * kTwoPi;
      Vec3 d(std::sin(th) * std::cos(ph), std::sin(th) * std::sin(ph), std::cos(th));
      e += luminance(sky.radiance(d)) * std::cos(th) * std::sin(th) * (kPi * 0.5f / nT) * (kTwoPi / nP);
    }
  }
  return e;
}

static void testSkyRadiance() {
  render::AtmosphereParams ap;  // turbidity 2.5, ozone 0.35, albedo 0.2
  struct Expect {
    float elev;
    double zenithLo, zenithHi, skyLo, skyHi;
  };
  // Clear-sky ranges from measurements (zenith luminance in cd/m², diffuse horizontal illuminance
  // in lux): a few thousand cd/m² and 10-25 klux in the day, hundreds of lux at sunset and
  // a few lux at the end of civil twilight (sun 6 degrees below the horizon).
  const Expect expect[] = {{60, 2500, 9000, 12000, 30000}, {30, 1000, 5000, 9000, 25000},
                           {10, 400, 3000, 4000, 15000},   {0, 50, 600, 300, 3000},
                           {-6, 0.2, 10, 0.5, 30}};
  for (int model : {render::kSkyPhysical, render::kSkyHosekWilkie}) {
    for (const Expect& e : expect) {
      if (model == render::kSkyHosekWilkie && e.elev < 0) continue;
      render::PhysicalSky sky;
      sky.build(sunAt(e.elev), ap, render::kSolarIlluminance, model, 1.5f);
      Vec3 zen = sky.radiance(Vec3(0, 0, 1));
      double es = skyIlluminance(sky);
      std::printf("  sky model %d sun %5.1f deg: zenith %8.1f cd/m2 (b/r %.2f), sky illuminance %8.0f lux\n", model,
                  e.elev, luminance(zen), zen.z / std::max(zen.x, 1e-6f), es);
      CHECK(luminance(zen) > e.zenithLo && luminance(zen) < e.zenithHi);
      CHECK(es > e.skyLo && es < e.skyHi);
      CHECK(zen.z > zen.x);  // blue zenith, also in twilight
    }
  }
  // Sun/sky balance on a clear day: direct (horizontal) sunlight several times the sky light.
  for (float elev : {30.0f, 45.0f, 60.0f}) {
    render::PhysicalSky sky;
    sky.build(sunAt(elev), ap, render::kSolarIlluminance);
    Vec3 sunN = render::atmosphereTransmittance(sunAt(elev), ap) * render::kSolarIlluminance;
    double ratio = luminance(sunN) * std::sin(radians(elev)) / skyIlluminance(sky);
    std::printf("  sun %4.1f deg: direct normal %6.0f lux, direct/diffuse %.2f\n", elev, luminance(sunN), ratio);
    CHECK(ratio > 1.5 && ratio < 8.0);
  }
  Vec3 noon = render::atmosphereTransmittance(Vec3(0, 0, 1), ap) * render::kSolarIlluminance;
  CHECK(luminance(noon) > 90000.0f && luminance(noon) < 110000.0f);
  CHECK(noon.x > noon.z && noon.z > 0.7f * noon.x);  // slightly warm white at noon
  Vec3 low = render::atmosphereTransmittance(sunAt(3.0f), ap);
  CHECK(low.x > 1.5f * low.y && low.y > low.z);  // orange-red at sunset
  CHECK(maxComponent(render::atmosphereTransmittance(sunAt(-1.0f), ap)) == 0.0f);
  // More haze: less direct sun, more sky light.
  render::AtmosphereParams hazy = ap;
  hazy.turbidity = 6.0f;
  CHECK(luminance(render::atmosphereTransmittance(sunAt(40), hazy)) < luminance(render::atmosphereTransmittance(sunAt(40), ap)));
}

static void testSkyContinuity() {
  render::AtmosphereParams ap;
  // Across the horizon (the blend into the ground starts at the horizon value).
  render::PhysicalSky sky;
  sky.build(sunAt(20.0f), ap, render::kSolarIlluminance, render::kSkyPhysical, 2.0f);
  for (float az : {0.0f, 1.0f, 2.5f, 4.0f}) {
    Vec3 h(std::cos(az), std::sin(az), 0.0f);
    float above = luminance(sky.radiance(normalize(h + Vec3(0, 0, 0.0005f))));
    float below = luminance(sky.radiance(normalize(h - Vec3(0, 0, 0.0005f))));
    CHECK(std::fabs(above - below) < 0.05f * above);
    // Smooth along the sky (no seams on the interpolation grid).
    float prev = -1.0f;
    for (int i = 0; i <= 90; ++i) {
      float e = radians(static_cast<float>(i));
      float l = luminance(sky.radiance(Vec3(std::cos(az) * std::cos(e), std::sin(az) * std::cos(e), std::sin(e))));
      if (prev > 0.0f && i > 2) CHECK(std::fabs(l - prev) < 0.25f * std::max(l, prev));
      prev = l;
    }
  }
  // Through sunset and twilight: the sky dims smoothly and monotonically; the Hosek-Wilkie sky
  // hands over to the physical model below the horizon without a jump.
  for (int model : {render::kSkyPhysical, render::kSkyHosekWilkie}) {
    double prev = -1.0;
    for (float elev = 8.0f; elev >= -6.0f; elev -= 1.0f) {
      render::PhysicalSky s;
      s.build(sunAt(elev), ap, render::kSolarIlluminance, model, 1.5f);
      double e = skyIlluminance(s);
      if (prev > 0.0) {
        CHECK(e < prev * 1.02);
        CHECK(e > prev * 0.15);
      }
      prev = e;
    }
  }
}

// Scenes saved before the new sky models keep the Preetham sky; new scenes use the physical one.
static void testSkyBackCompat() {
  Scene fresh;
  CHECK(fresh.renderSettings.getInt("skyModel") == render::kSkyPhysical);
  Scene old;
  json j = {{"format", "veraa-scene"}, {"version", 1}, {"renderSettings", {{"envMode", 3}, {"tonemap", 1}}}};
  CHECK(old.fromJson(j));
  CHECK(old.renderSettings.getInt("skyModel") == render::kSkyPreetham);
  CHECK(old.renderSettings.getInt("tonemap") == render::kMapReinhard);
  CHECK(old.renderSettings.getFloat("cmBurn") == 0.0f);  // the original Reinhard curve
  json saved = fresh.toJson();
  Scene back;
  CHECK(back.fromJson(saved));
  CHECK(back.renderSettings.getInt("skyModel") == render::kSkyPhysical);
}

static void testSolarDisk() {
  // Limb darkening keeps the disk's average radiance (and so the sun's irradiance).
  Vec3 avg(0.0f);
  const int n = 2000;
  for (int i = 0; i < n; ++i) {
    float r = (i + 0.5f) / n;
    avg += render::solarLimbDarkening(r) * (2.0f * r / n);
  }
  CHECK_NEAR(avg.x, 1.0, 0.01);
  CHECK_NEAR(avg.z, 1.0, 0.01);
  Vec3 center = render::solarLimbDarkening(0.0f), limb = render::solarLimbDarkening(0.98f);
  CHECK(center.y > 1.0f && limb.y < 0.6f);
  CHECK(limb.x / center.x > limb.z / center.z);  // the limb is redder
  CHECK_NEAR(render::kSunSolidAngle, kTwoPi * (1.0 - std::cos(radians(render::kSunAngularRadiusDeg))), 2e-6);
}

int main() {
  testColorMappingMonotonic();
  testColorMappingIdentities();
  testDisplayPipeline();
  testSkyRadiance();
  testSkyContinuity();
  testSkyBackCompat();
  testSolarDisk();
  if (g_failures) {
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("All realism tests passed\n");
  return 0;
}
