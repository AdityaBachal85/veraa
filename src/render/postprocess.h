// Tone mapping, color corrections, lens effects and denoising of rendered HDR images.
#pragma once

#include <string>
#include <vector>

#include "renderer.h"

namespace veraa {
namespace render {

// Color mapping types: the values of DisplaySettings::tonemap and the "tonemap" render setting
// (stored in scene files, so the numbers never change).
enum ColorMappingType : int {
  kMapFilmicAces = 0,      // ACES RRT + sRGB ODT fit (Hill) with a mid-grey pre-gain
  kMapReinhard = 1,        // Reinhard on luminance; burn 1 = linear, 0 = full compression
  kMapLinear = 2,          // linear multiply (dark / bright multipliers), clipped at white
  kMapExponential = 3,     // 1 - exp(-x) per channel (desaturates highlights)
  kMapHsvExponential = 4,  // exponential on the HSV value (keeps hue and saturation)
  kMapIntensityExp = 5,    // exponential on the intensity (keeps the RGB ratios)
  kMapGamma = 6,           // gamma curve per channel (bright multiplier and gamma only)
  kMapIntensityGamma = 7,  // gamma curve on the intensity
  kMapAgx = 8,             // AgX-style filmic view: gamut inset, log encoding, sigmoid, outset
  kMapCount = 9
};

struct ColorMappingParams {
  int type = kMapFilmicAces;
  float dark = 1.0f;    // multiplier for dark values
  float bright = 1.0f;  // multiplier for bright values
  float gamma = 1.0f;   // output gamma (the curve's gamma for the gamma types)
  float burn = 0.0f;    // Reinhard burn value
};

ColorMappingParams colorMappingParams(const DisplaySettings& display);

// Exposed (already multiplied by the exposure) linear RGB to display-linear RGB, before the
// display transform. Results are >= 0 and mostly <= 1 (Linear and Gamma types can exceed 1).
Vec3 applyColorMapping(const Vec3& c, const ColorMappingParams& p);

// Lens effects (bloom, glare) and vignetting applied to linear (scene-referred) RGB in place.
void postProcess(std::vector<float>& rgb, int w, int h, const DisplaySettings& display);

// hdr: linear RGB floats (w*h*3). Writes w*h*4 sRGB bytes. The order is: exposure -> color
// mapping -> color corrections (white balance, tint, hue, highlight burn, saturation) ->
// display transform (sRGB OETF) -> display-encoded corrections (contrast, levels, curve, LUT).
// alpha (w*h, may be null) lets "affect background" keep the background out of the mapping.
void tonemapToRgba(const float* hdr, int w, int h, const DisplaySettings& display, std::vector<uint8_t>& out,
                   const float* alpha = nullptr);

// Parses "x,y;x,y;..." control points (sorted, clamped to [0,1]).
std::vector<Vec2> parseCurve(const std::string& text);

bool oidnAvailable();
// Intel Open Image Denoise "RT" filter with albedo and normal guides. Returns false on failure.
bool denoiseImage(const std::vector<float>& color, const std::vector<float>& albedo, const std::vector<float>& normal,
                  int w, int h, std::vector<float>& out, std::string* error);

}  // namespace render
}  // namespace veraa
