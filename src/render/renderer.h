// Veraa's physically based path tracer (Intel Embree). Progressive or bucket rendering with
// adaptive sampling, render elements, Light Mix and optional Intel Open Image Denoise.
#pragma once

#include <array>
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

  // Physical camera properties (defaults describe a plain camera).
  float exposure = 1.0f;          // image multiplier (f-number, shutter and ISO)
  Vec3 whiteBalance{1.0f, 1.0f, 1.0f};
  float vignetting = 0.0f;        // 0..1+, cos^4 falloff amount
  float distortion = 0.0f;        // + barrel, - pincushion
  float shiftX = 0.0f, shiftY = 0.0f;  // lens shift (fractions of the half image height)
  int blades = 0;                 // aperture blades (0 = round)
  float bladeRotationDeg = 0.0f;
  float bokehAnisotropy = 0.0f;
  float nearClip = 0.0f, farClip = 0.0f;  // 0 = no clipping
  // Motion blur: shutter interval in frames around `frame` and the camera at the other end.
  bool motionBlur = false;
  double frame = 0.0;
  double shutterOpen = 0.0, shutterClose = 0.0;  // frames
  bool cameraMoves = false;
  Vec3 positionClose, targetClose, upClose;
};

// View of a camera node (its local -Z axis is the viewing direction) for an image aspect (w / h).
// Physical cameras also fill the exposure, lens and motion blur fields.
RenderView viewFromCamera(const Node& cameraNode, float aspect);
// Perspective view framing all geometry from the front-right, above (used when no camera is set).
RenderView defaultViewForScene(const Scene& scene, float aspect);

struct RenderStats {
  int passes = 0;          // completed samples per pixel (maximum over pixels)
  int targetPasses = 0;
  double seconds = 0.0;
  bool running = false;
  bool finished = false;   // reached the sample count, noise threshold or time limit
  size_t triangles = 0;
  float converged = 0.0f;  // fraction of pixels that reached the noise threshold
  double samplesPerSecond = 0.0;
  std::vector<std::array<int, 4>> buckets;  // buckets being rendered (x0, y0, x1, y1)
};

// One Light Mix group as shown in the frame buffer.
struct LightMixGroup {
  std::string name;
  Vec3 color{1.0f, 1.0f, 1.0f};
  float intensity = 1.0f;
  bool enabled = true;
};

// Post-processing applied when resolving the image; can change while rendering (like a VFB).
struct DisplaySettings {
  float exposureEV = 0.0f;
  int tonemap = 0;           // 0 Filmic (ACES), 1 Reinhard, 2 Linear
  float contrast = 1.0f;
  float saturation = 1.0f;
  bool denoise = false;      // ignored when Renderer::denoiserAvailable() is false

  // Color corrections.
  bool corrections = false;
  float whiteBalanceK = 6500.0f;
  float tint = 0.0f;
  float hueShift = 0.0f;
  float highlightBurn = 1.0f;
  std::vector<Vec2> curve;   // control points in [0,1]^2 (empty = identity)
  float levelsBlack = 0.0f, levelsWhite = 1.0f, levelsGamma = 1.0f;
  std::string lutFile;       // .cube 3D LUT
  float lutStrength = 1.0f;

  // Lens effects.
  bool lensEffects = false;
  float bloomIntensity = 0.15f, bloomSize = 0.04f;
  float glareIntensity = 0.08f, glareSize = 0.15f;
  int glareRays = 6;
  float glareRotation = 15.0f;
  float vignetting = 0.0f;

  // Light Mix: when enabled and the render has light groups, the image is recomposed from them.
  bool useLightMix = false;
  std::vector<LightMixGroup> lightMix;

  // Show a render element instead of the beauty ("" = beauty).
  std::string element;
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
  // Region override (fractions of the image, top-left origin). Disabled = scene setting.
  void setRegion(bool enabled, float x0, float y0, float x1, float y1);
  // Renders only these geometry nodes (empty = the scene setting). Takes effect in setScene().
  void setRenderOnly(const std::vector<uint32_t>& nodeIds);

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
  // Linear HDR image after denoising and Light Mix (what resolve() tone maps).
  void resolveFinalHdr(const DisplaySettings& display, std::vector<float>& rgb) const;
  // Alpha (coverage) per pixel.
  void resolveAlpha(std::vector<float>& alpha) const;
  // Samples taken per pixel (sample rate).
  void resolveSampleCounts(std::vector<float>& counts) const;

  // Render elements enabled for the last render ("Lighting", "Reflection", ...).
  std::vector<std::string> elementNames() const;
  // Linear RGB of one element; false when it was not rendered.
  bool resolveElement(const std::string& name, std::vector<float>& rgb) const;
  // Light Mix groups of the last render (empty when Light Mix is off).
  std::vector<std::string> lightGroupNames() const;
  // Recomposes the image from the light groups with the given colors and intensities.
  bool resolveLightMix(const std::vector<LightMixGroup>& mix, std::vector<float>& rgb) const;

  static bool denoiserAvailable();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Material editor swatch: a sphere on a checker floor under an area light and sky.
// sceneForPaths (may be null) resolves relative texture paths. Returns size*size RGBA8.
std::vector<uint8_t> renderMaterialPreview(const Material& material, const Scene* sceneForPaths, int size,
                                           int samples);
// Flat swatch of a texture map (material editor map previews). Returns size*size RGBA8.
std::vector<uint8_t> renderMapPreview(const TexMap& map, const Scene* sceneForPaths, int size);

// Applies lens effects, color corrections and the tone curve of `display` to a linear image.
void toneMapImage(const std::vector<float>& rgb, int width, int height, const DisplaySettings& display,
                  std::vector<uint8_t>& rgba);

bool writePng(const std::string& path, int width, int height, const std::vector<uint8_t>& rgba);
bool writeHdr(const std::string& path, int width, int height, const std::vector<float>& rgb);
// Saves by extension: .png .jpg/.jpeg .bmp .tga .tif/.tiff (8-bit) or .hdr .exr (float).
// rgba8 is the display image; hdr (linear RGB) and alpha are used by float formats and to add
// an alpha channel when withAlpha is set. Returns false and sets error on failure.
bool saveImage(const std::string& path, int width, int height, const std::vector<uint8_t>& rgba8,
               const std::vector<float>* hdr, const std::vector<float>* alpha, bool withAlpha,
               std::string* error = nullptr);

}  // namespace veraa
