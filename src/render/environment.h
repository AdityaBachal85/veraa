// Environment lighting: constant color, sky gradient, HDRI, physical skies (Preetham, the
// spectral physical atmosphere or Hosek-Wilkie, see sky.h) and dome lights, with importance
// sampling for map-based environments.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common.h"
#include "texture.h"

namespace veraa {
class Scene;

namespace render {

struct LightBuildResult;

// Piecewise-constant 2D distribution over a w x h grid (rows = v, columns = u).
class Distribution2D {
 public:
  void build(const std::vector<float>& weights, int w, int h);
  bool valid() const { return total_ > 0.0f; }
  // Returns continuous (u, v) in [0,1)^2 and the pdf with respect to (u, v).
  Vec2 sample(float u1, float u2, float& pdf) const;
  float pdf(float u, float v) const;

 private:
  int w_ = 0, h_ = 0;
  float total_ = 0.0f;
  std::vector<float> func_;          // w*h
  std::vector<float> condCdf_;       // h * (w+1)
  std::vector<float> rowIntegral_;   // h
  std::vector<float> margCdf_;       // h+1
};

// Equirectangular mapping for Z-up worlds. u = 0.5 looks along +Y (the 3ds Max front-view
// direction), u grows toward +X (turning right); v = 0 is the zenith, v = 1 the nadir.
Vec2 directionToEquirect(const Vec3& dir, float rotationRad);
Vec3 equirectToDirection(float u, float v, float rotationRad);

// Color of sunlight after passing through the atmosphere, relative to the sun at the zenith
// (1 there). skyModel 0 (Preetham) uses the original approximation, the other models the
// physical atmosphere's transmittance.
Vec3 sunAtmosphereTint(float elevationRad, float turbidity, int skyModel = 0);

class Environment {
 public:
  // Environment from the render settings. sunDir: unit vector toward the (standard) sun or null;
  // sunIrradiance: the sun's luminance irradiance.
  void build(const Scene& scene, TextureCache& cache, const Vec3* sunDir, float sunIrradiance,
             std::vector<std::string>* warnings);
  // Same, using the lights collected for rendering (physical sun drives the physical sky).
  void build(const Scene& scene, TextureCache& cache, const LightBuildResult& lights, std::vector<std::string>* warnings);
  // Dome light: an HDRI (or a constant color without texture) times color.
  void buildDome(const Scene& scene, TextureCache& cache, const LightBuildResult& lights,
                 std::vector<std::string>* warnings);
  // Uniform radiance environment (tests and material previews).
  void buildConstant(const Vec3& radiance);
  void buildGradient(const Vec3& zenith, const Vec3& horizon, const Vec3& ground, float intensity);

  // Radiance arriving from world direction dir.
  Vec3 eval(const Vec3& dir) const;
  // Samples a direction proportional to the environment's brightness.
  bool sample(float u1, float u2, Vec3& dir, Vec3& radiance, float& pdf) const;
  float pdf(const Vec3& dir) const;
  bool isBlack() const { return black_; }
  // Average radiance (luminance) for light selection.
  float averageLuminance() const { return average_; }

  // Visible sun disk (physical sun): radiance when dir falls inside the disk.
  Vec3 sunDisk(const Vec3& dir) const;

  bool visibleToCamera = true;
  Vec3 background;

 private:
  void buildFromMap(const Texture* map, float rotationRad, const Vec3& tint);

  bool black_ = true;
  bool useMap_ = false;
  bool hemisphere_ = false;
  Vec3 constant_;
  const Texture* map_ = nullptr;
  std::unique_ptr<Texture> baked_;
  float rotation_ = 0.0f;
  Vec3 tint_{1, 1, 1};
  float average_ = 0.0f;
  Distribution2D dist_;
  bool hasSunDisk_ = false;
  Vec3 sunDir_;
  float sunCos_ = 1.0f;
  float sunAngle_ = 0.0f;  // angular radius of the disk (radians)
  Vec3 sunRadiance_;       // average radiance of the disk (limb darkening is applied around it)
};

}  // namespace render
}  // namespace veraa
