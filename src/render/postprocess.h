// Tone mapping, color corrections, lens effects and denoising of rendered HDR images.
#pragma once

#include <string>
#include <vector>

#include "renderer.h"

namespace veraa {
namespace render {

// Lens effects (bloom, glare, vignetting) and the scene-referred color corrections (white
// balance, tint, hue, highlight burn) applied to linear RGB in place.
void postProcess(std::vector<float>& rgb, int w, int h, const DisplaySettings& display);

// hdr: linear RGB floats (w*h*3). Writes w*h*4 sRGB bytes. Display-referred corrections
// (levels, curve, LUT) are applied after the tone curve.
void tonemapToRgba(const float* hdr, int w, int h, const DisplaySettings& display, std::vector<uint8_t>& out);

// Parses "x,y;x,y;..." control points (sorted, clamped to [0,1]).
std::vector<Vec2> parseCurve(const std::string& text);

bool oidnAvailable();
// Intel Open Image Denoise "RT" filter with albedo and normal guides. Returns false on failure.
bool denoiseImage(const std::vector<float>& color, const std::vector<float>& albedo, const std::vector<float>& normal,
                  int w, int h, std::vector<float>& out, std::string* error);

}  // namespace render
}  // namespace veraa
