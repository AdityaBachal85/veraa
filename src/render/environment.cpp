#include "environment.h"

#include <algorithm>
#include <iterator>
#include <mutex>

#include "lights.h"
#include "scene.h"
#include "sky.h"

namespace veraa {
namespace render {

// ---------------------------------------------------------------------------------------------
// Distribution2D

void Distribution2D::build(const std::vector<float>& weights, int w, int h) {
  w_ = w;
  h_ = h;
  func_.resize(static_cast<size_t>(w) * h);
  for (size_t i = 0; i < func_.size(); ++i) {
    float f = weights[i];
    func_[i] = (std::isfinite(f) && f > 0.0f) ? f : 0.0f;
  }
  condCdf_.assign(static_cast<size_t>(h) * (w + 1), 0.0f);
  rowIntegral_.assign(h, 0.0f);
  margCdf_.assign(h + 1, 0.0f);
  for (int y = 0; y < h; ++y) {
    float* cdf = &condCdf_[static_cast<size_t>(y) * (w + 1)];
    const float* f = &func_[static_cast<size_t>(y) * w];
    double acc = 0.0;
    cdf[0] = 0.0f;
    for (int x = 0; x < w; ++x) {
      acc += f[x] / static_cast<double>(w);
      cdf[x + 1] = static_cast<float>(acc);
    }
    rowIntegral_[y] = static_cast<float>(acc);
    for (int x = 1; x <= w; ++x) cdf[x] = acc > 0.0 ? static_cast<float>(cdf[x] / acc) : static_cast<float>(x) / w;
  }
  double acc = 0.0;
  for (int y = 0; y < h; ++y) {
    acc += rowIntegral_[y] / static_cast<double>(h);
    margCdf_[y + 1] = static_cast<float>(acc);
  }
  total_ = static_cast<float>(acc);
  for (int y = 1; y <= h; ++y)
    margCdf_[y] = acc > 0.0 ? static_cast<float>(margCdf_[y] / acc) : static_cast<float>(y) / h;
}

namespace {
// Index i with cdf[i] <= u < cdf[i+1].
int findInterval(const float* cdf, int n, float u) {
  const float* it = std::upper_bound(cdf, cdf + n + 1, u);
  int i = static_cast<int>(it - cdf) - 1;
  return std::min(std::max(i, 0), n - 1);
}
}  // namespace

Vec2 Distribution2D::sample(float u1, float u2, float& pdf) const {
  int y = findInterval(margCdf_.data(), h_, u2);
  float dy = margCdf_[y + 1] - margCdf_[y];
  float fy = dy > 0.0f ? (u2 - margCdf_[y]) / dy : 0.5f;
  const float* cdf = &condCdf_[static_cast<size_t>(y) * (w_ + 1)];
  int x = findInterval(cdf, w_, u1);
  float dx = cdf[x + 1] - cdf[x];
  float fx = dx > 0.0f ? (u1 - cdf[x]) / dx : 0.5f;
  pdf = total_ > 0.0f ? func_[static_cast<size_t>(y) * w_ + x] / total_ : 0.0f;
  return {(x + clampf(fx, 0.0f, 0.9999f)) / w_, (y + clampf(fy, 0.0f, 0.9999f)) / h_};
}

float Distribution2D::pdf(float u, float v) const {
  if (total_ <= 0.0f) return 0.0f;
  int x = std::min(std::max(static_cast<int>(u * w_), 0), w_ - 1);
  int y = std::min(std::max(static_cast<int>(v * h_), 0), h_ - 1);
  return func_[static_cast<size_t>(y) * w_ + x] / total_;
}

// ---------------------------------------------------------------------------------------------
// Mapping

Vec2 directionToEquirect(const Vec3& d, float rotationRad) {
  float phi = std::atan2(d.x, d.y) - rotationRad;
  float u = 0.5f + phi / kTwoPi;
  u -= std::floor(u);
  float v = std::acos(clampf(d.z, -1.0f, 1.0f)) / kPi;
  return {u, v};
}

Vec3 equirectToDirection(float u, float v, float rotationRad) {
  float theta = v * kPi;
  float phi = (u - 0.5f) * kTwoPi + rotationRad;
  float s = std::sin(theta);
  return {s * std::sin(phi), s * std::cos(phi), std::cos(theta)};
}

Vec3 sunAtmosphereTint(float elevationRad, float turbidity, int skyModel) {
  if (skyModel != kSkyPreetham) {
    // Transmittance of the physical atmosphere, relative to the sun at the zenith per channel
    // (a standard sun keeps its own color and intensity at noon).
    AtmosphereParams ap;
    ap.turbidity = turbidity;
    Vec3 dir(std::cos(elevationRad), 0.0f, std::sin(elevationRad));
    Vec3 t = atmosphereTransmittance(dir, ap), z = atmosphereTransmittance(Vec3(0, 0, 1), ap);
    return Vec3(t.x / std::max(z.x, 1e-6f), t.y / std::max(z.y, 1e-6f), t.z / std::max(z.z, 1e-6f));
  }
  float e = std::max(degrees(elevationRad), -1.0f);
  float s = std::sin(radians(std::max(e, 0.0f)));
  // Kasten-Young relative air mass.
  float m = 1.0f / (s + 0.50572f * std::pow(std::max(e, 0.0f) + 6.07995f, -1.6364f));
  float k = 0.5f + turbidity / 6.0f;
  Vec3 beta = Vec3(0.02f, 0.045f, 0.11f) * k;
  float extra = std::max(m - 1.0f, 0.0f);
  Vec3 t(std::exp(-beta.x * extra), std::exp(-beta.y * extra), std::exp(-beta.z * extra));
  if (e < 0.0f) t *= std::max(0.0f, 1.0f + e);  // fade out once below the horizon
  return t;
}

// ---------------------------------------------------------------------------------------------
// Sky models

namespace {

Vec3 gradientSky(const Vec3& d, const Vec3& zenith, const Vec3& horizon, const Vec3& ground) {
  float z = d.z;
  if (z >= 0.0f) return lerp(horizon, zenith, std::sqrt(z));
  float t = clampf(-z / 0.08f, 0.0f, 1.0f);
  t = t * t * (3.0f - 2.0f * t);
  return lerp(horizon, ground, t);
}

struct Perez {
  float A, B, C, D, E;
  float operator()(float cosTheta, float gamma) const {
    float cg = std::cos(gamma);
    return (1.0f + A * std::exp(B / std::max(cosTheta, 0.001f))) * (1.0f + C * std::exp(D * gamma) + E * cg * cg);
  }
};

Vec3 xyYToLinearSrgb(float x, float y, float Y) {
  if (y <= 1e-6f) return Vec3(0.0f);
  float X = x / y * Y, Z = (1.0f - x - y) / y * Y;
  return {3.2406f * X - 1.5372f * Y - 0.4986f * Z, -0.9689f * X + 1.8758f * Y + 0.0415f * Z,
          0.0557f * X - 0.2040f * Y + 1.0570f * Z};
}

// Parameters of the physical sun that drives the sky: the first enabled VeraaSun or Sun
// Positioner, as chosen by collectLights().
const ParamBlock* physicalSunParams(const Scene& scene) {
  for (const auto& node : scene.nodes) {
    const LightObject* lo = node->light();
    if (!lo || !lo->params.getBool("enabled")) continue;
    std::string type = lo->type();
    if (type == "VeraaSun" || type == "SunPositioner") return &lo->params;
  }
  return nullptr;
}

// Baked physical skies are kept for re-renders of the same sun and atmosphere.
struct SkyKey {
  float v[12];
  int model, w, h;
  bool operator==(const SkyKey& o) const {
    return model == o.model && w == o.w && h == o.h && std::equal(std::begin(v), std::end(v), std::begin(o.v));
  }
};

std::shared_ptr<const std::vector<float>> bakedSky(const Vec3& sunDir, const AtmosphereParams& ap, float sunE,
                                                   int model, float blurDeg, int w, int h) {
  static std::mutex mutex;
  static std::vector<std::pair<SkyKey, std::shared_ptr<const std::vector<float>>>> cache;
  SkyKey key{{sunDir.x, sunDir.y, sunDir.z, ap.turbidity, ap.ozone, ap.groundAlbedo.x, ap.groundAlbedo.y,
              ap.groundAlbedo.z, sunE, blurDeg, 0.0f, 0.0f},
             model, w, h};
  {
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& e : cache)
      if (e.first == key) return e.second;
  }
  PhysicalSky sky;
  sky.build(sunDir, ap, sunE, model, blurDeg);
  auto map = std::make_shared<const std::vector<float>>(sky.bake(w, h));
  std::lock_guard<std::mutex> lock(mutex);
  cache.insert(cache.begin(), {key, map});
  if (cache.size() > 3) cache.pop_back();
  return map;
}

// Preetham et al. 1999 clear-sky model. Returns relative radiance (arbitrary scale).
class PreethamSky {
 public:
  PreethamSky(const Vec3& sunDir, float T) {
    sun_ = sunDir;
    thetaS_ = std::acos(clampf(sunDir.z, 0.0f, 1.0f));
    thetaS_ = std::min(thetaS_, radians(89.5f));
    Y_ = {0.1787f * T - 1.4630f, -0.3554f * T + 0.4275f, -0.0227f * T + 5.3251f, 0.1206f * T - 2.5771f,
          -0.0670f * T + 0.3703f};
    x_ = {-0.0193f * T - 0.2592f, -0.0665f * T + 0.0008f, -0.0004f * T + 0.2125f, -0.0641f * T - 0.8989f,
          -0.0033f * T + 0.0452f};
    y_ = {-0.0167f * T - 0.2608f, -0.0950f * T + 0.0092f, -0.0079f * T + 0.2102f, -0.0441f * T - 1.6537f,
          -0.0109f * T + 0.0529f};
    float ts = thetaS_, ts2 = ts * ts, ts3 = ts2 * ts, T2 = T * T;
    float chi = (4.0f / 9.0f - T / 120.0f) * (kPi - 2.0f * ts);
    zY_ = std::max((4.0453f * T - 4.9710f) * std::tan(chi) - 0.2155f * T + 2.4192f, 0.01f);
    zx_ = T2 * (0.00166f * ts3 - 0.00375f * ts2 + 0.00209f * ts) +
          T * (-0.02903f * ts3 + 0.06377f * ts2 - 0.03202f * ts + 0.00394f) +
          (0.11693f * ts3 - 0.21196f * ts2 + 0.06052f * ts + 0.25886f);
    zy_ = T2 * (0.00275f * ts3 - 0.00610f * ts2 + 0.00317f * ts) +
          T * (-0.04214f * ts3 + 0.08970f * ts2 - 0.04153f * ts + 0.00516f) +
          (0.15346f * ts3 - 0.26756f * ts2 + 0.06670f * ts + 0.26688f);
    normY_ = Y_(1.0f, thetaS_);
    normx_ = x_(1.0f, thetaS_);
    normy_ = y_(1.0f, thetaS_);
  }
  Vec3 radiance(const Vec3& d) const {
    float cosT = std::max(d.z, 0.001f);
    float gamma = std::acos(clampf(dot(d, sun_), -1.0f, 1.0f));
    float Y = zY_ * Y_(cosT, gamma) / normY_;
    float x = zx_ * x_(cosT, gamma) / normx_;
    // Preetham's chromaticities sit slightly below the white point (a magenta cast near the
    // horizon); a small shift toward green makes the horizon neutral, as in real skies.
    float y = zy_ * y_(cosT, gamma) / normy_ + 0.012f;
    Vec3 c = xyYToLinearSrgb(x, y, Y);
    return vmax(c, Vec3(0.0f));
  }

 private:
  Vec3 sun_;
  float thetaS_;
  Perez Y_, x_, y_;
  float zY_, zx_, zy_, normY_, normx_, normy_;
};

}  // namespace

// ---------------------------------------------------------------------------------------------
// Environment

void Environment::buildConstant(const Vec3& radiance) {
  useMap_ = false;
  map_ = nullptr;
  baked_.reset();
  hemisphere_ = false;
  tint_ = Vec3(1.0f);
  constant_ = vmax(radiance, Vec3(0.0f));
  black_ = veraa::isBlack(constant_);
  average_ = luminance(constant_);
}

void Environment::buildFromMap(const Texture* map, float rotationRad, const Vec3& tint) {
  useMap_ = true;
  map_ = map;
  rotation_ = rotationRad;
  tint_ = tint;
  int w = map->width(), h = map->height();
  // Importance distribution at up to 1024 x 512 (box-filtered from larger maps).
  int dw = std::min(w, 1024), dh = std::min(h, 512);
  std::vector<float> weights(static_cast<size_t>(dw) * dh, 0.0f);
  double avg = 0.0, solid = 0.0;
  for (int y = 0; y < dh; ++y) {
    int y0 = y * h / dh, y1 = std::max(y0 + 1, (y + 1) * h / dh);
    float sinT = std::sin((y + 0.5f) / dh * kPi);
    bool below = hemisphere_ && (y + 0.5f) / dh > 0.5f;
    for (int x = 0; x < dw; ++x) {
      int x0 = x * w / dw, x1 = std::max(x0 + 1, (x + 1) * w / dw);
      float acc = 0.0f;
      for (int yy = y0; yy < y1; ++yy)
        for (int xx = x0; xx < x1; ++xx) acc += luminance(map->texel(xx, yy, true) * tint);
      float l = below ? 0.0f : acc / ((y1 - y0) * (x1 - x0));
      weights[static_cast<size_t>(y) * dw + x] = l * sinT;
      avg += l * sinT;
      solid += sinT;
    }
  }
  dist_.build(weights, dw, dh);
  average_ = solid > 0.0 ? static_cast<float>(avg / solid) : 0.0f;
  black_ = !dist_.valid() || maxComponent(tint) <= 0.0f;
}

void Environment::buildGradient(const Vec3& zenith, const Vec3& horizon, const Vec3& ground, float intensity) {
  const int w = 64, h = 128;
  std::vector<float> rgb(static_cast<size_t>(w) * h * 3);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      Vec3 d = equirectToDirection((x + 0.5f) / w, (y + 0.5f) / h, 0.0f);
      Vec3 c = gradientSky(d, zenith, horizon, ground) * intensity;
      size_t i = (static_cast<size_t>(y) * w + x) * 3;
      rgb[i] = c.x;
      rgb[i + 1] = c.y;
      rgb[i + 2] = c.z;
    }
  baked_ = Texture::fromFloatRgb(w, h, std::move(rgb));
  hemisphere_ = false;
  buildFromMap(baked_.get(), 0.0f, Vec3(1.0f));
}

void Environment::build(const Scene& scene, TextureCache& cache, const Vec3* sunDir, float sunIrradiance,
                        std::vector<std::string>* warnings) {
  LightBuildResult lights;
  if (sunDir) {
    lights.hasSun = true;
    lights.sunDir = *sunDir;
    lights.sunIrradiance = sunIrradiance;
  }
  build(scene, cache, lights, warnings);
}

void Environment::build(const Scene& scene, TextureCache& cache, const LightBuildResult& lights,
                        std::vector<std::string>* warnings) {
  const ParamBlock& rs = scene.renderSettings;
  visibleToCamera = rs.getBool("envVisible");
  background = rs.getColor("backgroundColor");
  hasSunDisk_ = false;
  hemisphere_ = false;
  float envIntensity = rs.getFloat("envIntensity");
  int mode = rs.getInt("envMode");
  Vec3 zenith = rs.getColor("skyZenith"), horizon = rs.getColor("skyHorizon"), ground = rs.getColor("groundColor");

  if (mode == 0) {
    buildConstant(rs.getColor("envColor") * envIntensity);
    return;
  }
  if (mode == 2) {
    std::string path = scene.resolvePath(rs.getString("hdriPath"));
    const Texture* tex = nullptr;
    if (path.empty()) {
      if (warnings) warnings->push_back("Environment is set to HDRI but no HDRI file is chosen; using the sky gradient");
    } else {
      tex = cache.get(path, warnings);
    }
    if (tex) {
      buildFromMap(tex, radians(rs.getFloat("hdriRotation")), Vec3(rs.getFloat("hdriIntensity") * envIntensity));
      return;
    }
    buildGradient(zenith, horizon, ground, envIntensity);
    return;
  }
  if (mode == 3) {
    bool physical = lights.hasSun && lights.physicalSun;
    Vec3 sd = lights.hasSun ? normalize(lights.sunDir) : normalize(Vec3(0.0f, 0.7071f, 0.7071f));
    int skyModel = physical ? lights.sunSkyModel : rs.getInt("skyModel");
    if (skyModel == kSkyPhysical || skyModel == kSkyHosekWilkie) {
      AtmosphereParams ap;
      float sunE = 0.0f, blur = rs.getFloat("skyHorizonBlur");
      if (physical) {
        // Photometric: the sun's illuminance outside the atmosphere drives the sky.
        ap.turbidity = lights.sunTurbidity;
        ap.ozone = lights.sunOzone;
        ap.groundAlbedo = lights.sunGroundAlbedo;
        float skyMult = 1.0f;
        if (const ParamBlock* sp = physicalSunParams(scene)) {
          if (sp->has("skyIntensity")) skyMult = sp->getFloat("skyIntensity");
          if (sp->has("horizonBlur")) blur = sp->getFloat("horizonBlur");
        }
        sunE = kSolarIlluminance * lights.sunIntensity * skyMult;
      } else {
        // A standard sun's intensity is its irradiance at noon (sun at the zenith): the sky is
        // scaled to match, so the sun/sky balance stays physical.
        ap.turbidity = rs.getFloat("skyTurbidity");
        ap.groundAlbedo = ground;
        float userE = lights.hasSun ? lights.sunIrradiance : 3.0f * luminance(Vec3(1.0f, 0.95f, 0.88f));
        sunE = userE / std::max(luminance(atmosphereTransmittance(Vec3(0, 0, 1), ap)), 1e-3f);
      }
      const int w = 1024, h = 512;
      std::vector<float> rgb = *bakedSky(sd, ap, sunE, skyModel, blur, w, h);
      for (float& f : rgb) f *= envIntensity;
      baked_ = Texture::fromFloatRgb(w, h, std::move(rgb));
      buildFromMap(baked_.get(), 0.0f, Vec3(1.0f));
    } else {
      float sunE = lights.hasSun ? lights.sunIrradiance : 3.0f * luminance(Vec3(1.0f, 0.95f, 0.88f));
      float turbidity = physical ? lights.sunTurbidity : rs.getFloat("skyTurbidity");
      Vec3 groundAlbedo = physical ? lights.sunGroundAlbedo : ground;
      // Keep the model valid while the sun is near or below the horizon; dim the sky instead.
      float elevation = std::asin(clampf(sd.z, -1.0f, 1.0f));
      Vec3 modelSun = sd;
      if (modelSun.z < 0.02f) {
        modelSun.z = 0.02f;
        modelSun = normalize(modelSun);
      }
      PreethamSky sky(modelSun, clampf(turbidity, 1.7f, 10.0f));
      const int w = 256, h = 128;
      std::vector<float> rgb(static_cast<size_t>(w) * h * 3, 0.0f);
      double eRaw = 0.0;
      for (int y = 0; y < h / 2; ++y)
        for (int x = 0; x < w; ++x) {
          Vec3 d = equirectToDirection((x + 0.5f) / w, (y + 0.5f) / h, 0.0f);
          Vec3 c = sky.radiance(d);
          size_t i = (static_cast<size_t>(y) * w + x) * 3;
          rgb[i] = c.x;
          rgb[i + 1] = c.y;
          rgb[i + 2] = c.z;
          float sinT = std::sin((y + 0.5f) / h * kPi);
          eRaw += luminance(c) * d.z * sinT * (kTwoPi / w) * (kPi / h);
        }
      // Physical sky brightness tied to the sun: diffuse sky irradiance on the ground is
      // about 8% of the sun's irradiance plus 25% of its horizontal component (clear-sky ratios).
      float cosS = std::max(sd.z, 0.0f);
      float target = sunE * (0.08f + 0.25f * cosS);
      if (physical) {
        target = kSolarIlluminance * lights.sunIntensity * (0.02f + 0.1f * cosS);
        const ParamBlock* sp = physicalSunParams(scene);
        if (sp && sp->has("skyIntensity")) target *= sp->getFloat("skyIntensity");
      }
      if (elevation < 0.0f) target *= std::max(0.0f, 1.0f + degrees(elevation) / 6.0f);
      float scale = eRaw > 0.0 ? static_cast<float>(target / eRaw) * envIntensity : 0.0f;
      for (size_t i = 0; i < static_cast<size_t>(w) * (h / 2) * 3; ++i) rgb[i] *= scale;
      // Ground: a diffuse plane lit by the sun and sky, blended into the horizon.
      Vec3 groundL = groundAlbedo * ((sunE * cosS + target) * kInvPi) * envIntensity;
      for (int y = h / 2; y < h; ++y) {
        float below = ((y + 0.5f) / h - 0.5f) * 180.0f;  // degrees below the horizon
        float t = clampf(below / 4.0f, 0.0f, 1.0f);
        t = t * t * (3.0f - 2.0f * t);
        for (int x = 0; x < w; ++x) {
          size_t hi = (static_cast<size_t>(h / 2 - 1) * w + x) * 3;
          size_t i = (static_cast<size_t>(y) * w + x) * 3;
          Vec3 horizonC(rgb[hi], rgb[hi + 1], rgb[hi + 2]);
          Vec3 c = lerp(horizonC, groundL, t);
          rgb[i] = c.x;
          rgb[i + 1] = c.y;
          rgb[i + 2] = c.z;
        }
      }
      baked_ = Texture::fromFloatRgb(w, h, std::move(rgb));
      buildFromMap(baked_.get(), 0.0f, Vec3(1.0f));
    }
    if (physical && !lights.sunInvisible && lights.sunCos < 1.0f && sd.z > -0.01f) {
      hasSunDisk_ = true;
      sunDir_ = sd;
      sunCos_ = lights.sunCos;
      sunAngle_ = std::acos(clampf(lights.sunCos, -1.0f, 1.0f));
      sunRadiance_ = lights.sunColor / (kTwoPi * (1.0f - lights.sunCos));
    }
    return;
  }
  buildGradient(zenith, horizon, ground, envIntensity);
}

void Environment::buildDome(const Scene& scene, TextureCache& cache, const LightBuildResult& lights,
                            std::vector<std::string>* warnings) {
  visibleToCamera = !lights.domeInvisible;
  background = scene.renderSettings.getColor("backgroundColor");
  hasSunDisk_ = false;
  hemisphere_ = !lights.domeSpherical;
  const Texture* tex = nullptr;
  if (!lights.domeTexture.empty()) tex = cache.get(scene.resolvePath(lights.domeTexture), warnings);
  if (tex) {
    buildFromMap(tex, radians(lights.domeRotation), lights.domeColor);
    return;
  }
  if (hemisphere_) {
    // A constant hemisphere is still importance sampled through a small baked map.
    const int w = 8, h = 8;
    std::vector<float> rgb(static_cast<size_t>(w) * h * 3, 1.0f);
    baked_ = Texture::fromFloatRgb(w, h, std::move(rgb));
    buildFromMap(baked_.get(), 0.0f, lights.domeColor);
    return;
  }
  buildConstant(lights.domeColor);
}

Vec3 Environment::eval(const Vec3& dir) const {
  if (black_) return Vec3(0.0f);
  if (hemisphere_ && dir.z < 0.0f) return Vec3(0.0f);
  if (!useMap_) return constant_;
  Vec2 uv = directionToEquirect(dir, rotation_);
  float h = static_cast<float>(map_->height());
  float v = clampf(uv.y, 0.5f / h, 1.0f - 0.5f / h);  // no wrapping across the poles
  return map_->color(Vec2(uv.x, 1.0f - v), true) * tint_;
}

Vec3 Environment::sunDisk(const Vec3& dir) const {
  if (!hasSunDisk_) return Vec3(0.0f);
  float c = dot(dir, sunDir_);
  if (c < sunCos_) return Vec3(0.0f);
  // Limb darkening around the disk's average radiance (the sun light's irradiance is unchanged).
  float angle = std::asin(std::min(length(cross(dir, sunDir_)), 1.0f));  // precise for tiny angles
  float r = sunAngle_ > 0.0f ? angle / sunAngle_ : 0.0f;
  return sunRadiance_ * solarLimbDarkening(r);
}

bool Environment::sample(float u1, float u2, Vec3& dir, Vec3& radiance, float& pdf) const {
  if (black_) return false;
  if (!useMap_) {
    dir = sampleUniformSphere(u1, u2);
    radiance = constant_;
    pdf = 1.0f / (4.0f * kPi);
    return true;
  }
  float pdfUv = 0.0f;
  Vec2 uv = dist_.sample(u1, u2, pdfUv);
  float sinT = std::sin(uv.y * kPi);
  if (pdfUv <= 0.0f || sinT <= 1e-6f) return false;
  dir = equirectToDirection(uv.x, uv.y, rotation_);
  pdf = pdfUv / (2.0f * kPi * kPi * sinT);
  radiance = eval(dir);
  return true;
}

float Environment::pdf(const Vec3& dir) const {
  if (black_) return 0.0f;
  if (!useMap_) return 1.0f / (4.0f * kPi);
  if (hemisphere_ && dir.z < 0.0f) return 0.0f;
  Vec2 uv = directionToEquirect(dir, rotation_);
  float sinT = std::sin(uv.y * kPi);
  if (sinT <= 1e-6f) return 0.0f;
  return dist_.pdf(uv.x, uv.y) / (2.0f * kPi * kPi * sinT);
}

}  // namespace render
}  // namespace veraa
