// Physically based daylight: a spectral model of the clear atmosphere (Rayleigh scattering by
// air, Mie scattering by aerosols, ozone absorption) with single scattering integrated along
// each view ray and multiple scattering from a precomputed transfer table (after Hillaire 2020),
// and the Hosek-Wilkie 2012 analytic sky. Radiance is linear sRGB with luminance in cd/m², so a
// sunny day looks right at EV100 ~15. The sun itself is a light (lights.cpp); the sky models
// here exclude the solar disk.
#pragma once

#include <memory>
#include <vector>

#include "common.h"

namespace veraa {
namespace render {

// Solar illuminance at the top of the atmosphere (lux).
constexpr float kSolarIlluminance = 128000.0f;
// Mean angular radius of the solar disk (degrees) and its solid angle (steradians).
constexpr float kSunAngularRadiusDeg = 0.265f;
constexpr float kSunSolidAngle = 6.7967e-5f;

// Sky models (the "skyModel" parameters; values are stored in files).
enum SkyModelType : int { kSkyPreetham = 0, kSkyPhysical = 1, kSkyHosekWilkie = 2 };

struct AtmosphereParams {
  float turbidity = 2.5f;  // Linke-style turbidity: 2 = very clear, 3 = clear, 6+ = hazy
  float ozone = 0.35f;     // ozone column in atm-cm (0.35 = 350 Dobson units)
  Vec3 groundAlbedo{0.2f, 0.2f, 0.2f};
};

// Sunlight reaching the ground from direction dirToSun (unit vector toward the sun), as linear
// RGB per lux of solar illuminance outside the atmosphere: luminance is the atmosphere's
// transmittance and the color is the sun's (warm at sunset). Zero once the sun has set.
Vec3 atmosphereTransmittance(const Vec3& dirToSun, const AtmosphereParams& p);

// Relative radiance across the solar disk (limb darkening), normalized to a disk average of 1.
// r: distance from the disk center in units of the disk radius (0..1).
Vec3 solarLimbDarkening(float r);

// Daylight sky (physical atmosphere or Hosek-Wilkie) for one sun position.
class PhysicalSky {
 public:
  PhysicalSky();
  ~PhysicalSky();
  PhysicalSky(const PhysicalSky&) = delete;
  PhysicalSky& operator=(const PhysicalSky&) = delete;

  // Physical atmosphere for a sun of illuminance sunE (lux, outside the atmosphere).
  void build(const Vec3& sunDir, const AtmosphereParams& p, float sunE);
  // model: kSkyPhysical or kSkyHosekWilkie. horizonBlurDeg: width of the blend from the
  // horizon haze to the lit ground below the horizon.
  void build(const Vec3& sunDir, const AtmosphereParams& p, float sunE, int model, float horizonBlurDeg);

  // Radiance (cd/m² luminance, linear sRGB) arriving from direction dir. Below the horizon:
  // the ground (albedo lit by sun and sky), blended into the horizon haze.
  Vec3 radiance(const Vec3& dir) const;
  // Bakes an equirectangular map (w x h, linear RGB, see equirectToDirection) using all cores.
  std::vector<float> bake(int w, int h) const;
  Vec3 groundRadiance() const;
  int model() const { return model_; }

 private:
  struct Impl;
  Vec3 skyRadiance(const Vec3& dir) const;  // above the horizon (dir.z clamped to >= 0)
  std::unique_ptr<Impl> impl_;
  int model_ = kSkyPhysical;
  Vec3 sun_{0, 0, 1};
  float blurDeg_ = 1.5f;
  Vec3 groundRadiance_;
};

}  // namespace render
}  // namespace veraa
