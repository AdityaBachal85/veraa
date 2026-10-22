#include "sky.h"

#include "environment.h"

namespace veraa {
namespace render {

namespace {

constexpr double kEarthRadius = 6360e3;
constexpr double kAtmosphereRadius = 6420e3;
constexpr double kViewerHeight = 2.0;
constexpr double kRayleighScale = 8000.0;
constexpr double kMieScale = 1200.0;
constexpr float kMieG = 0.76f;

struct Coeffs {
  Vec3 rayleigh{5.802e-6f, 13.558e-6f, 33.1e-6f};
  float mieScatter = 3.996e-6f;
  float mieExtinction = 4.44e-6f;
  Vec3 ozone{0.650e-6f, 1.881e-6f, 0.085e-6f};
};

Coeffs coeffsFor(const AtmosphereParams& p) {
  Coeffs c;
  float k = std::max(p.turbidity, 1.0f) / 2.0f;
  c.mieScatter *= k;
  c.mieExtinction *= k;
  c.ozone *= p.ozone / 0.35f;
  return c;
}

struct Densities {
  double r, m, o;
};

Densities densityAt(double h) {
  h = std::max(h, 0.0);
  return {std::exp(-h / kRayleighScale), std::exp(-h / kMieScale), std::max(0.0, 1.0 - std::fabs(h - 25000.0) / 15000.0)};
}

// Distance along a ray from origin (height above the earth center = r0, cosine to zenith mu)
// to the sphere of radius R. Returns -1 when missed.
double raySphereFromInside(double r0, double mu, double R) {
  double b = r0 * mu;
  double c = r0 * r0 - R * R;
  double disc = b * b - c;
  if (disc < 0.0) return -1.0;
  double s = std::sqrt(disc);
  double t = -b + s;
  return t > 0.0 ? t : -1.0;
}

double hitGround(double r0, double mu) {
  if (mu >= 0.0) return -1.0;
  double b = r0 * mu;
  double c = r0 * r0 - kEarthRadius * kEarthRadius;
  double disc = b * b - c;
  if (disc < 0.0) return -1.0;
  double t = -b - std::sqrt(disc);
  return t > 0.0 ? t : -1.0;
}

// Optical depth (rayleigh, mie, ozone integrals of density) from a point toward a direction.
Densities opticalDepth(const Vec3& pos, const Vec3& dir, int steps) {
  double r0 = length(pos);
  double mu = dot(pos, dir) / r0;
  double tMax = raySphereFromInside(r0, mu, kAtmosphereRadius);
  Densities acc{0, 0, 0};
  if (tMax <= 0.0) return acc;
  double prevT = 0.0;
  for (int i = 0; i < steps; ++i) {
    double f = (i + 1.0) / steps;
    double t = tMax * f * f;
    double tm = 0.5 * (t + prevT);
    Vec3 x = pos + dir * static_cast<float>(tm);
    Densities d = densityAt(length(x) - kEarthRadius);
    double ds = t - prevT;
    acc.r += d.r * ds;
    acc.m += d.m * ds;
    acc.o += d.o * ds;
    prevT = t;
  }
  return acc;
}

Vec3 extinction(const Densities& d, const Coeffs& c) {
  Vec3 tau = c.rayleigh * static_cast<float>(d.r) + Vec3(c.mieExtinction * static_cast<float>(d.m)) +
             c.ozone * static_cast<float>(d.o);
  return Vec3(std::exp(-tau.x), std::exp(-tau.y), std::exp(-tau.z));
}

}  // namespace

Vec3 atmosphereTransmittance(const Vec3& dirToSun, const AtmosphereParams& p) {
  Vec3 pos(0.0f, 0.0f, static_cast<float>(kEarthRadius + kViewerHeight));
  Vec3 d = normalize(dirToSun);
  if (hitGround(length(pos), d.z) > 0.0) return Vec3(0.0f);
  return extinction(opticalDepth(pos, d, 48), coeffsFor(p));
}

void PhysicalSky::build(const Vec3& sunDir, const AtmosphereParams& p, float sunE) {
  sun_ = normalize(sunDir);
  p_ = p;
  sunE_ = sunE;
  // Ground: diffuse plane lit by the sun and the sky.
  Vec3 Esky(0.0f);
  const int nT = 12, nP = 24;
  for (int i = 0; i < nT; ++i) {
    float theta = (i + 0.5f) / nT * (kPi * 0.5f);
    for (int j = 0; j < nP; ++j) {
      float phi = (j + 0.5f) / nP * kTwoPi;
      Vec3 d(std::sin(theta) * std::cos(phi), std::sin(theta) * std::sin(phi), std::cos(theta));
      Esky += singleScattering(d) * (std::cos(theta) * std::sin(theta) * (kPi * 0.5f / nT) * (kTwoPi / nP));
    }
  }
  Vec3 Esun = atmosphereTransmittance(sun_, p) * (sunE * std::max(sun_.z, 0.0f));
  groundRadiance_ = p.groundAlbedo * (Esun + Esky) * kInvPi;
}

Vec3 PhysicalSky::singleScattering(const Vec3& dirIn) const {
  Coeffs c = coeffsFor(p_);
  Vec3 dir = normalize(dirIn);
  Vec3 pos(0.0f, 0.0f, static_cast<float>(kEarthRadius + kViewerHeight));
  double r0 = length(pos);
  double tMax = raySphereFromInside(r0, dir.z, kAtmosphereRadius);
  double tg = hitGround(r0, dir.z);
  if (tg > 0.0) tMax = tg;
  if (tMax <= 0.0) return Vec3(0.0f);
  float mu = dot(dir, sun_);
  float phaseR = 3.0f / (16.0f * kPi) * (1.0f + mu * mu);
  float g = kMieG, g2 = g * g;
  float phaseM = 3.0f / (8.0f * kPi) * ((1.0f - g2) * (1.0f + mu * mu)) /
                 ((2.0f + g2) * std::pow(std::max(1.0f + g2 - 2.0f * g * mu, 1e-6f), 1.5f));
  const int steps = 32;
  Densities view{0, 0, 0};
  Vec3 sum(0.0f);
  double prevT = 0.0;
  for (int i = 0; i < steps; ++i) {
    double f = (i + 1.0) / steps;
    double t = tMax * f * f;
    double tm = 0.5 * (t + prevT);
    double ds = t - prevT;
    Vec3 x = pos + dir * static_cast<float>(tm);
    double h = length(x) - kEarthRadius;
    Densities d = densityAt(h);
    // Optical depth from the viewer to the midpoint.
    Densities half{view.r + d.r * ds * 0.5, view.m + d.m * ds * 0.5, view.o + d.o * ds * 0.5};
    view.r += d.r * ds;
    view.m += d.m * ds;
    view.o += d.o * ds;
    double rx = length(x);
    if (hitGround(rx, dot(x, sun_) / static_cast<float>(rx)) > 0.0) {
      prevT = t;
      continue;  // in the earth's shadow
    }
    Densities toSun = opticalDepth(x, sun_, 8);
    Densities total{half.r + toSun.r, half.m + toSun.m, half.o + toSun.o};
    Vec3 T = extinction(total, c);
    Vec3 scatter = c.rayleigh * (static_cast<float>(d.r) * phaseR) + Vec3(c.mieScatter * static_cast<float>(d.m) * phaseM);
    sum += T * scatter * static_cast<float>(ds);
    prevT = t;
  }
  // Second-order scattering is not traced; a mild lift keeps the zenith from going too dark.
  return sum * (sunE_ * 1.12f);
}

Vec3 PhysicalSky::radiance(const Vec3& dirIn) const {
  Vec3 dir = normalize(dirIn);
  if (dir.z >= 0.0f) return singleScattering(Vec3(dir.x, dir.y, std::max(dir.z, 0.002f)));
  // Below the horizon: fade from the horizon haze into the lit ground.
  Vec3 horizonDir = normalize(Vec3(dir.x, dir.y, 0.002f));
  Vec3 horizon = singleScattering(horizonDir);
  float t = clampf(-dir.z / 0.06f, 0.0f, 1.0f);
  t = t * t * (3.0f - 2.0f * t);
  return lerp(horizon, groundRadiance_, t);
}

std::vector<float> PhysicalSky::bake(int w, int h) const {
  std::vector<float> rgb(static_cast<size_t>(w) * h * 3, 0.0f);
  // The sky is symmetric around the sun's azimuth, but baking every texel is simple and fast enough.
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      Vec3 d = equirectToDirection((x + 0.5f) / w, (y + 0.5f) / h, 0.0f);
      Vec3 c = radiance(d);
      size_t i = (static_cast<size_t>(y) * w + x) * 3;
      rgb[i] = c.x;
      rgb[i + 1] = c.y;
      rgb[i + 2] = c.z;
    }
  return rgb;
}

}  // namespace render
}  // namespace veraa
