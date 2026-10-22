#include "postprocess.h"

#include <mutex>

#include "common.h"

#ifdef VERAA_HAVE_OIDN
#include <OpenImageDenoise/oidn.hpp>
#endif

namespace veraa {
namespace render {

namespace {

// ACES filmic curve, Stephen Hill's fit of the RRT + sRGB ODT.
Vec3 acesFitted(const Vec3& c) {
  Vec3 v(0.59719f * c.x + 0.35458f * c.y + 0.04823f * c.z, 0.07600f * c.x + 0.90834f * c.y + 0.01566f * c.z,
         0.02840f * c.x + 0.13383f * c.y + 0.83777f * c.z);
  auto rrt = [](float x) { return (x * (x + 0.0245786f) - 0.000090537f) / (x * (0.983729f * x + 0.4329510f) + 0.238081f); };
  v = Vec3(rrt(v.x), rrt(v.y), rrt(v.z));
  return {1.60475f * v.x - 0.53108f * v.y - 0.07367f * v.z, -0.10208f * v.x + 1.10813f * v.y - 0.00605f * v.z,
          -0.00327f * v.x - 0.07276f * v.y + 1.07602f * v.z};
}

}  // namespace

void tonemapToRgba(const float* hdr, int w, int h, const DisplaySettings& d, std::vector<uint8_t>& out) {
  size_t n = static_cast<size_t>(w) * h;
  out.resize(n * 4);
  float scale = std::pow(2.0f, d.exposureEV);
  // The filmic curve maps scene middle grey (0.18) to roughly display middle grey with this pre-gain.
  const float filmicGain = 1.8f;
  for (size_t i = 0; i < n; ++i) {
    Vec3 c(hdr[i * 3], hdr[i * 3 + 1], hdr[i * 3 + 2]);
    if (!isFiniteColor(c)) c = Vec3(0.0f);
    c = vmax(c * scale, Vec3(0.0f));
    if (d.saturation != 1.0f) c = vmax(lerp(Vec3(luminance(c)), c, d.saturation), Vec3(0.0f));
    switch (d.tonemap) {
      case 0: c = acesFitted(c * filmicGain); break;
      case 1: {
        float l = luminance(c);
        if (l > 0.0f) c *= (l / (1.0f + l)) / l;
        break;
      }
      default: break;
    }
    float rgb[3] = {linearToSrgb(c.x), linearToSrgb(c.y), linearToSrgb(c.z)};
    for (int k = 0; k < 3; ++k) {
      float v = rgb[k];
      if (d.contrast != 1.0f) v = clampf(0.5f + (v - 0.5f) * d.contrast, 0.0f, 1.0f);
      out[i * 4 + k] = static_cast<uint8_t>(std::lround(v * 255.0f));
    }
    out[i * 4 + 3] = 255;
  }
}

#ifdef VERAA_HAVE_OIDN
namespace {
std::mutex& oidnMutex() {
  static std::mutex m;
  return m;
}
oidn::DeviceRef* oidnDevice() {
  static oidn::DeviceRef device;
  static bool ok = [] {
    device = oidn::newDevice();
    device.commit();
    const char* msg = nullptr;
    return device.getError(msg) == oidn::Error::None;
  }();
  return ok ? &device : nullptr;
}
}  // namespace

bool oidnAvailable() {
  std::lock_guard<std::mutex> lock(oidnMutex());
  return oidnDevice() != nullptr;
}

bool denoiseImage(const std::vector<float>& color, const std::vector<float>& albedo, const std::vector<float>& normal,
                  int w, int h, std::vector<float>& out, std::string* error) {
  std::lock_guard<std::mutex> lock(oidnMutex());
  oidn::DeviceRef* dev = oidnDevice();
  if (!dev) {
    if (error) *error = "Denoiser device unavailable";
    return false;
  }
  size_t bytes = static_cast<size_t>(w) * h * 3 * sizeof(float);
  oidn::BufferRef colorBuf = dev->newBuffer(bytes);
  oidn::BufferRef albedoBuf = dev->newBuffer(bytes);
  oidn::BufferRef normalBuf = dev->newBuffer(bytes);
  oidn::BufferRef outBuf = dev->newBuffer(bytes);
  colorBuf.write(0, bytes, color.data());
  albedoBuf.write(0, bytes, albedo.data());
  normalBuf.write(0, bytes, normal.data());
  oidn::FilterRef filter = dev->newFilter("RT");
  filter.setImage("color", colorBuf, oidn::Format::Float3, w, h);
  filter.setImage("albedo", albedoBuf, oidn::Format::Float3, w, h);
  filter.setImage("normal", normalBuf, oidn::Format::Float3, w, h);
  filter.setImage("output", outBuf, oidn::Format::Float3, w, h);
  filter.set("hdr", true);
  filter.commit();
  filter.execute();
  const char* msg = nullptr;
  if (dev->getError(msg) != oidn::Error::None) {
    if (error) *error = msg ? msg : "Denoiser failed";
    return false;
  }
  out.resize(static_cast<size_t>(w) * h * 3);
  outBuf.read(0, bytes, out.data());
  return true;
}
#else
bool oidnAvailable() { return false; }
bool denoiseImage(const std::vector<float>&, const std::vector<float>&, const std::vector<float>&, int, int,
                  std::vector<float>&, std::string* error) {
  if (error) *error = "Built without Intel Open Image Denoise";
  return false;
}
#endif

}  // namespace render
}  // namespace veraa
