// Physical light helpers shared by the renderer and the app: color temperature, IES
// photometric profiles (IESNA LM-63), solar position (NOAA algorithm) and light units.
//
// Units: scene units are centimeters (100 units = 1 m). Image radiance is luminance in cd/m²,
// irradiance is illuminance in lux, point light intensity is candela.
#pragma once

#include <string>
#include <vector>

#include "vmath.h"

namespace veraa {

// Linear sRGB color of a blackbody radiator, scaled so 6500 K is (1, 1, 1).
Vec3 kelvinToRgb(float kelvin);

// IESNA LM-63 photometric web (type C photometry).
struct IesProfile {
  std::vector<float> vAngles;  // vertical angles in degrees, 0 = nadir (straight down), 180 = zenith
  std::vector<float> hAngles;  // horizontal angles in degrees
  std::vector<float> candela;  // candela[h * vAngles.size() + v], multipliers applied
  float ratedLumens = 0.0f;    // lamp lumens from the file (-1 = absolute photometry)
  float width = 0.0f, length = 0.0f, height = 0.0f;  // luminous opening, meters
  float maxCandela = 0.0f;

  bool load(const std::string& path, std::string* error = nullptr);
  bool parse(const std::string& text, std::string* error = nullptr);
  bool valid() const { return !vAngles.empty() && !hAngles.empty() && !candela.empty(); }
  // Luminous intensity (cd) at a vertical angle from nadir and a horizontal angle, in degrees.
  float intensity(float verticalDeg, float horizontalDeg) const;
  // Luminous flux (lm) integrated over the sphere.
  float totalFlux() const;
};

// Writes a rotationally symmetric profile (used to create sample files and in tests).
std::string makeIesText(const std::vector<float>& vAngles, const std::vector<float>& candela,
                        const std::string& description);

struct SolarInput {
  int year = 2024, month = 6, day = 21;
  float hour = 12.0f;       // local clock time, hours (fraction allowed)
  float timezone = 0.0f;    // hours east of UTC (e.g. +1 for Central Europe)
  bool daylightSaving = false;
  float latitude = 0.0f;    // degrees, north positive
  float longitude = 0.0f;   // degrees, east positive
};

struct SolarPosition {
  float azimuthDeg = 0.0f;    // clockwise from north (east = 90)
  float elevationDeg = 0.0f;  // above the horizon, without atmospheric refraction
  float declinationDeg = 0.0f;
  float equationOfTimeMin = 0.0f;
};

SolarPosition solarPosition(const SolarInput& in);
// Unit vector toward the sun in a Z-up world whose north is +Y rotated by northDeg around +Z
// (counterclockwise seen from above).
Vec3 solarDirection(float azimuthDeg, float elevationDeg, float northDeg);

// ---- Light units ----
enum class LightUnits { Image = 0, Lumens = 1, Luminance = 2, Watts = 3, Radiance = 4 };
constexpr float kLumensPerWatt = 683.0f;
// Converts a light's multiplier to emitted luminance (cd/m²) for a Lambertian emitter of
// area areaM2 (square meters) that emits from `sides` sides.
float luminanceFromUnits(LightUnits units, float value, float areaM2, int sides);

// Exposure of a camera: EV100 = log2(N²/t) - log2(ISO/100). The image is scaled by
// 1 / (1.2 * 2^EV100) so a mid-gray card at the matching scene brightness renders mid-gray.
float ev100(float fNumber, float shutterSeconds, float iso);
float exposureScale(float ev100Value);

}  // namespace veraa
