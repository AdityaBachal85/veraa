// Tone mapping, color correction and denoising of rendered HDR images.
#pragma once

#include <string>
#include <vector>

#include "renderer.h"

namespace veraa {
namespace render {

// hdr: linear RGB floats (w*h*3). Writes w*h*4 sRGB bytes.
void tonemapToRgba(const float* hdr, int w, int h, const DisplaySettings& display, std::vector<uint8_t>& out);

bool oidnAvailable();
// Intel Open Image Denoise "RT" filter with albedo and normal guides. Returns false on failure.
bool denoiseImage(const std::vector<float>& color, const std::vector<float>& albedo, const std::vector<float>& normal,
                  int w, int h, std::vector<float>& out, std::string* error);

}  // namespace render
}  // namespace veraa
