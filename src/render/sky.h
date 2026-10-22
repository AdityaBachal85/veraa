// Physically based clear sky: single scattering through an atmosphere of air molecules
// (Rayleigh), aerosols (Mie) and ozone absorption, in photometric units (cd/m²).
#pragma once

#include <vector>

#include "common.h"

namespace veraa {
namespace render {

// Solar illuminance at the top of the atmosphere (lux).
constexpr float kSolarIlluminance = 128000.0f;
// Solid angle of the solar disk (steradians) at its mean angular radius of 0.265°.
constexpr float kSunSolidAngle = 6.7967e-5f;

struct AtmosphereParams {
  float turbidity = 2.5f;  // aerosol amount (2 = very clear, 10 = hazy)
  float ozone = 0.35f;     // ozone column relative to the standard 0.35 cm
  Vec3 groundAlbedo{0.2f, 0.2f, 0.2f};
};

// Fraction of sunlight reaching the ground from direction dir (unit vector toward the sun).
Vec3 atmosphereTransmittance(const Vec3& dirToSun, const AtmosphereParams& p);

// Sky radiance in cd/m² for a sun of illuminance sunE (lux, outside the atmosphere) in
// direction sunDir. Directions below the horizon see the ground (albedo lit by sun and sky).
class PhysicalSky {
 public:
  void build(const Vec3& sunDir, const AtmosphereParams& p, float sunE);
  Vec3 radiance(const Vec3& dir) const;
  // Bakes an equirectangular map (w x h, linear RGB) of the sky and ground.
  std::vector<float> bake(int w, int h) const;

 private:
  Vec3 singleScattering(const Vec3& dir) const;
  Vec3 sun_{0, 0, 1};
  AtmosphereParams p_;
  float sunE_ = kSolarIlluminance;
  Vec3 groundRadiance_;
};

}  // namespace render
}  // namespace veraa
