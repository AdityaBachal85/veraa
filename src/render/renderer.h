// Veraa's physically based path tracer (Intel Embree). Progressive, multithreaded,
// with optional Intel Open Image Denoise.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "scene.h"

namespace veraa {

// Where the image is seen from. Z-up world.
struct RenderView {
  Vec3 position{0.0f, -300.0f, 150.0f};
  Vec3 target{0.0f, 0.0f, 50.0f};
  Vec3 up{0.0f, 0.0f, 1.0f};
  float fovYDeg = 40.0f;        // vertical field of view (perspective)
  bool orthographic = false;
  float orthoHeight = 200.0f;   // world units visible vertically (orthographic)
  float apertureRadius = 0.0f;  // depth of field; 0 = pinhole
  float focusDistance = 300.0f;
};

// View of a camera node (its local -Z axis is the viewing direction) for an image aspect (w / h).
RenderView viewFromCamera(const Node& cameraNode, float aspect);
// Perspective view framing all geometry from the front-right, above (used when no camera is set).
RenderView defaultViewForScene(const Scene& scene, float aspect);

struct RenderStats {
  int passes = 0;          // completed samples per pixel
  int targetPasses = 0;
  double seconds = 0.0;
  bool running = false;
  bool finished = false;   // reached the sample count or time limit
  size_t triangles = 0;
};

// Post-processing applied when resolving the image; can change while rendering (like a VFB).
struct DisplaySettings {
  float exposureEV = 0.0f;
  int tonemap = 0;           // 0 Filmic (ACES), 1 Reinhard, 2 Linear
  float contrast = 1.0f;
  float saturation = 1.0f;
  bool denoise = false;      // ignored when Renderer::denoiserAvailable() is false
};

DisplaySettings displaySettingsFromScene(const Scene& scene);

class Renderer {
 public:
  Renderer();
  ~Renderer();  // stops rendering
  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;

  // Copies everything needed from the scene: world-space geometry, materials, textures, lights,
  // environment and render settings. Call while not rendering, from the thread that owns the scene.
  // Returns false and sets error on fatal problems. Missing files become warnings().
  bool setScene(const Scene& scene, std::string* error = nullptr);
  std::vector<std::string> warnings() const;

  void setView(const RenderView& view);
  // Overrides of the scene's render settings (0 or negative = use the scene value).
  void setResolution(int width, int height);
  void setMaxSamples(int samplesPerPixel);
  void setTimeLimit(double seconds);

  // Progressive rendering on background threads; each pass adds one sample per pixel.
  void start();
  void stop();  // requests a stop and waits for the worker threads
  bool isRunning() const;
  // Renders synchronously until the sample count or time limit is reached (CLI, previews).
  void renderBlocking();

  RenderStats stats() const;
  int width() const;
  int height() const;

  // 8-bit sRGB RGBA of the current accumulation. Safe to call while rendering.
  // Denoising is slow (tens to hundreds of ms); request it on demand or when finished.
  void resolve(const DisplaySettings& display, std::vector<uint8_t>& rgba) const;
  // Linear HDR RGB floats (averaged samples, no tone mapping).
  void resolveHdr(std::vector<float>& rgb) const;

  static bool denoiserAvailable();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Material editor swatch: a sphere on a checker floor under an area light and sky.
// sceneForPaths (may be null) resolves relative texture paths. Returns size*size RGBA8.
std::vector<uint8_t> renderMaterialPreview(const Material& material, const Scene* sceneForPaths, int size,
                                           int samples);

bool writePng(const std::string& path, int width, int height, const std::vector<uint8_t>& rgba);
bool writeHdr(const std::string& path, int width, int height, const std::vector<float>& rgb);

}  // namespace veraa
