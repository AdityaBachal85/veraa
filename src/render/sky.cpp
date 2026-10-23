#include "sky.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <thread>

#include "environment.h"

extern "C" {
#include "ArHosekSkyModel.h"
}

namespace veraa {
namespace render {

namespace {

// ---------------------------------------------------------------------------------------------
// Atmosphere (meters). Air density falls off exponentially with an 8 km scale height, aerosols
// with 1.2 km; ozone sits in a layer centered at 25 km.

constexpr double kEarthRadius = 6360e3;
constexpr double kTopRadius = 6460e3;  // 100 km of atmosphere
constexpr double kViewerHeight = 2.0;
constexpr double kRayleighScale = 8000.0;
constexpr double kMieScale = 1200.0;
constexpr double kOzoneCenter = 25000.0, kOzoneHalfWidth = 15000.0;
constexpr float kMieG = 0.8f;            // aerosol phase asymmetry
constexpr float kAerosolAlbedo = 0.92f;  // single scattering albedo of continental aerosols
constexpr float kAngstromAlpha = 1.3f;   // wavelength exponent of the aerosol optical depth

// ---------------------------------------------------------------------------------------------
// Spectral sampling: 16 bands of 25 nm from 377.5 to 777.5 nm.

constexpr int kBands = 16;
constexpr float kBand0 = 390.0f, kBandStep = 25.0f;
using Spec = std::array<float, kBands>;

float bandCenter(int i) { return kBand0 + kBandStep * static_cast<float>(i); }

// Reference tables at 10 nm steps from 380 to 780 nm.
constexpr int kTableSize = 41;
// Extraterrestrial solar spectral irradiance (W m^-2 nm^-1), smoothed to 10 nm, after the
// ASTM E-490 / Wehrli 1985 reference spectra. Only its shape is used: the sun is rescaled to
// kSolarIlluminance.
const float kSolarTable[kTableSize] = {1.12f, 1.10f, 1.55f, 1.72f, 1.75f, 1.60f, 1.80f, 2.01f, 2.05f, 2.02f, 2.05f,
                                       1.95f, 1.94f, 1.90f, 1.85f, 1.88f, 1.86f, 1.86f, 1.80f, 1.82f, 1.80f, 1.76f,
                                       1.75f, 1.71f, 1.68f, 1.64f, 1.62f, 1.55f, 1.53f, 1.51f, 1.48f, 1.45f, 1.42f,
                                       1.39f, 1.36f, 1.33f, 1.30f, 1.27f, 1.25f, 1.22f, 1.19f};
// Ozone absorption cross section (1e-21 cm^2), Chappuis band, 10 nm averages of the 293 K
// measurements of Serdyuchenko et al. 2014.
const float kOzoneTable[kTableSize] = {0.0028f, 0.0066f, 0.015f, 0.028f, 0.055f, 0.085f, 0.158f, 0.232f, 0.367f,
                                       0.492f,  0.775f,  0.902f, 1.48f,  1.60f,  2.14f,  2.76f,  3.09f,  3.50f,
                                       4.27f,   4.67f,   4.40f,  4.70f,  5.02f,  4.31f,  3.74f,  3.22f,  2.66f,
                                       2.24f,   1.85f,   1.47f,  1.21f,  0.94f,  0.75f,  0.66f,  0.51f,  0.42f,
                                       0.42f,   0.32f,   0.25f,  0.28f,  0.25f};

float smoothStep(float a, float b, float x) {
  float t = clampf((x - a) / (b - a), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

float tableAt(const float* t, float lambda) {
  float f = clampf((lambda - 380.0f) / 10.0f, 0.0f, static_cast<float>(kTableSize - 1));
  int i = std::min(static_cast<int>(f), kTableSize - 2);
  float a = f - static_cast<float>(i);
  return t[i] * (1.0f - a) + t[i + 1] * a;
}

float bandAverage(const float* t, int band) {
  float c = bandCenter(band), sum = 0.0f;
  for (int k = 0; k < 25; ++k) sum += tableAt(t, c - 12.0f + static_cast<float>(k));
  return sum / 25.0f;
}

// CIE 1931 2-degree color matching functions: the multi-lobe Gaussian fit of Wyman, Sloan and
// Shirley, "Simple Analytic Approximations to the CIE XYZ Color Matching Functions" (JCGT 2013).
float lobe(float x, float mu, float s1, float s2) {
  float t = (x - mu) / (x < mu ? s1 : s2);
  return std::exp(-0.5f * t * t);
}

Vec3 cie1931(float l) {
  return {1.056f * lobe(l, 599.8f, 37.9f, 31.0f) + 0.362f * lobe(l, 442.0f, 16.0f, 26.7f) -
              0.065f * lobe(l, 501.1f, 20.4f, 26.2f),
          0.821f * lobe(l, 568.8f, 46.9f, 40.5f) + 0.286f * lobe(l, 530.9f, 16.3f, 31.1f),
          1.217f * lobe(l, 437.0f, 11.8f, 36.0f) + 0.681f * lobe(l, 459.0f, 26.0f, 13.8f)};
}

Vec3 xyzToLinearSrgb(const Vec3& c) {
  return {3.2404542f * c.x - 1.5371385f * c.y - 0.4985314f * c.z,
          -0.9692660f * c.x + 1.8760108f * c.y + 0.0415560f * c.z,
          0.0556434f * c.x - 0.2040259f * c.y + 1.0572252f * c.z};
}

struct SpectralData {
  Spec sun{};       // solar spectral irradiance outside the atmosphere, scaled to kSolarIlluminance
  Spec rayleigh{};  // Rayleigh scattering coefficient at sea level (1/m)
  Spec ozone{};     // ozone absorption cross section (m^2)
  std::array<Vec3, kBands> toRgb{};  // band radiance (W m^-2 sr^-1 nm^-1) -> linear sRGB in cd/m²
};

const SpectralData& spectral() {
  static const SpectralData data = [] {
    SpectralData s;
    for (int i = 0; i < kBands; ++i) {
      float lambda = bandCenter(i);
      Vec3 xyz(0.0f);
      for (int k = 0; k < 25; ++k) xyz += cie1931(lambda - 12.0f + static_cast<float>(k));  // 1 nm steps
      s.toRgb[i] = xyzToLinearSrgb(xyz * 683.0f);
      s.sun[i] = bandAverage(kSolarTable, i);
      // Rayleigh optical depth of the whole atmosphere (Hansen & Travis 1974), lambda in um.
      double um2 = 1.0 / (lambda * 1e-3 * lambda * 1e-3);
      double tau = 0.008569 * um2 * um2 * (1.0 + 0.0113 * um2 + 0.00013 * um2 * um2);
      s.rayleigh[i] = static_cast<float>(tau / kRayleighScale);
      s.ozone[i] = bandAverage(kOzoneTable, i) * 1e-25f;  // 1e-21 cm^2 -> m^2
    }
    double lum = 0.0;
    for (int i = 0; i < kBands; ++i) lum += s.sun[i] * luminance(s.toRgb[i]);
    float k = static_cast<float>(kSolarIlluminance / lum);
    for (float& v : s.sun) v *= k;
    return s;
  }();
  return data;
}

// Coefficients of one atmosphere (turbidity and ozone).
struct Coeffs {
  Spec scatR{}, scatM{}, extM{}, absO{};  // per unit density (1/m)
};

Coeffs coeffsFor(const AtmosphereParams& p) {
  const SpectralData& sd = spectral();
  Coeffs c;
  // Angstrom turbidity from the Linke-style turbidity (Preetham et al. 1999, appendix).
  float beta = std::max(0.04608f * clampf(p.turbidity, 1.0f, 30.0f) - 0.04586f, 0.002f);
  // Ozone: column (atm-cm -> molecules/m^2) spread over the 30 km wide layer.
  double ozonePeak = std::max(p.ozone, 0.0f) * 2.687e23 / kOzoneHalfWidth;
  for (int i = 0; i < kBands; ++i) {
    float lambdaUm = bandCenter(i) * 1e-3f;
    float tauA = beta * std::pow(lambdaUm, -kAngstromAlpha);
    c.scatR[i] = sd.rayleigh[i];
    c.extM[i] = static_cast<float>(tauA / kMieScale);
    c.scatM[i] = c.extM[i] * kAerosolAlbedo;
    c.absO[i] = static_cast<float>(sd.ozone[i] * ozonePeak);
  }
  return c;
}

struct Density {
  double r, m, o;
};

Density densityAt(double h) {
  h = std::max(h, 0.0);
  return {std::exp(-h / kRayleighScale), std::exp(-h / kMieScale),
          std::max(0.0, 1.0 - std::fabs(h - kOzoneCenter) / kOzoneHalfWidth)};
}

// Distance from a point at radius r (direction cosine mu to the local zenith) to the sphere of
// radius R around the earth's center, from inside. -1 when missed.
double toSphere(double r, double mu, double R) {
  double disc = r * r * (mu * mu - 1.0) + R * R;
  if (disc < 0.0) return -1.0;
  double t = -r * mu + std::sqrt(disc);
  return t > 0.0 ? t : -1.0;
}

// Distance to the ground, or -1 when the ray does not hit it.
double toGround(double r, double mu) {
  if (mu >= 0.0) return -1.0;
  double disc = r * r * (mu * mu - 1.0) + kEarthRadius * kEarthRadius;
  if (disc < 0.0) return -1.0;
  double t = -r * mu - std::sqrt(disc);
  return t > 0.0 ? t : -1.0;
}

// Cosine of the horizon (the ray grazing the ground) seen from radius r.
double horizonMu(double r) {
  double s = kEarthRadius / std::max(r, kEarthRadius);
  return -std::sqrt(std::max(0.0, 1.0 - s * s));
}

// ---------------------------------------------------------------------------------------------
// Optical depth table: integrals of the three normalized densities (meters) from a point at
// height h toward a direction with zenith cosine mu, out of the atmosphere. Depends only on the
// geometry, so it is computed once. mu is parameterized between the horizon and the zenith.

struct DepthTable {
  static constexpr int kH = 64, kMu = 256;
  std::vector<Density> data;

  static double heightOf(int i) {
    double f = static_cast<double>(i) / (kH - 1);
    return (kTopRadius - kEarthRadius) * f * f;
  }

  void build() {
    data.resize(static_cast<size_t>(kH) * kMu);
    for (int ih = 0; ih < kH; ++ih) {
      double r = kEarthRadius + heightOf(ih);
      double muH = horizonMu(r);
      for (int iu = 0; iu < kMu; ++iu) {
        double u = static_cast<double>(iu) / (kMu - 1);
        double mu = muH + (1.0 - muH) * u * u;
        double sinT = std::sqrt(std::max(0.0, 1.0 - mu * mu));
        double tMax = std::max(toSphere(r, mu, kTopRadius), 0.0);
        const int steps = 256;
        double dt = tMax / steps;
        Density acc{0, 0, 0};
        for (int k = 0; k < steps; ++k) {
          double t = (k + 0.5) * dt;
          double x = sinT * t, z = r + mu * t;
          Density d = densityAt(std::sqrt(x * x + z * z) - kEarthRadius);
          acc.r += d.r * dt;
          acc.m += d.m * dt;
          acc.o += d.o * dt;
        }
        data[static_cast<size_t>(ih) * kMu + iu] = acc;
      }
    }
  }

  // Table cell and weights for a lookup. Directions below the horizon use the grazing value
  // (visibility is separate).
  static void cell(double h, double mu, int& h0, int& u0, double& th, double& tu) {
    double r = kEarthRadius + std::max(h, 0.0);
    double muH = horizonMu(r);
    double fh = std::sqrt(clampf(static_cast<float>(h / (kTopRadius - kEarthRadius)), 0.0f, 1.0f)) * (kH - 1);
    double u = std::sqrt(std::max(0.0, (mu - muH) / (1.0 - muH)));
    double fu = std::min(u, 1.0) * (kMu - 1);
    h0 = std::min(static_cast<int>(fh), kH - 2);
    u0 = std::min(static_cast<int>(fu), kMu - 2);
    th = fh - h0;
    tu = fu - u0;
  }

  // Bilinear lookup.
  Density lookup(double h, double mu) const {
    int h0, u0;
    double th, tu;
    cell(h, mu, h0, u0, th, tu);
    auto at = [&](int a, int b) { return data[static_cast<size_t>(a) * kMu + b]; };
    Density a = at(h0, u0), b = at(h0, u0 + 1), c = at(h0 + 1, u0), d = at(h0 + 1, u0 + 1);
    auto mix = [&](double p, double q, double s, double t) {
      return (p * (1.0 - tu) + q * tu) * (1.0 - th) + (s * (1.0 - tu) + t * tu) * th;
    };
    return {mix(a.r, b.r, c.r, d.r), mix(a.m, b.m, c.m, d.m), mix(a.o, b.o, c.o, d.o)};
  }
};

const DepthTable& depthTable() {
  static const DepthTable table = [] {
    DepthTable t;
    t.build();
    return t;
  }();
  return table;
}

// Fraction of the solar disk above the horizon seen from radius r for a sun at zenith cosine mu.
float sunVisibility(double r, double mu) {
  double muH = horizonMu(r);
  if (mu > muH + 0.02) return 1.0f;  // well above or below the horizon (the disk is ~0.01 wide)
  if (mu < muH - 0.02) return 0.0f;
  double above = std::asin(clampf(static_cast<float>(mu), -1.0f, 1.0f)) - std::asin(muH);
  double rad = radians(kSunAngularRadiusDeg);
  return smoothStep(static_cast<float>(-rad), static_cast<float>(rad), static_cast<float>(above));
}

// Spectral transmittance toward the sun for one atmosphere, tabulated on the depth table's grid.
struct SunTable {
  std::vector<Spec> data;

  void build(const Coeffs& c) {
    const DepthTable& dt = depthTable();
    data.resize(dt.data.size());
    for (size_t k = 0; k < data.size(); ++k) {
      const Density& d = dt.data[k];
      for (int i = 0; i < kBands; ++i)
        data[k][i] = static_cast<float>(std::exp(-(c.scatR[i] * d.r + c.extM[i] * d.m + c.absO[i] * d.o)));
    }
  }

  void lookup(double h, double mu, Spec& T) const {
    float vis = sunVisibility(kEarthRadius + std::max(h, 0.0), mu);
    if (vis <= 0.0f) {
      T.fill(0.0f);
      return;
    }
    int h0, u0;
    double th, tu;
    DepthTable::cell(h, mu, h0, u0, th, tu);
    const int n = DepthTable::kMu;
    const Spec& a = data[static_cast<size_t>(h0) * n + u0];
    const Spec& b = data[static_cast<size_t>(h0) * n + u0 + 1];
    const Spec& c = data[static_cast<size_t>(h0 + 1) * n + u0];
    const Spec& d = data[static_cast<size_t>(h0 + 1) * n + u0 + 1];
    float wa = static_cast<float>((1 - tu) * (1 - th)) * vis, wb = static_cast<float>(tu * (1 - th)) * vis;
    float wc = static_cast<float>((1 - tu) * th) * vis, wd = static_cast<float>(tu * th) * vis;
    for (int i = 0; i < kBands; ++i) T[i] = a[i] * wa + b[i] * wb + c[i] * wc + d[i] * wd;
  }
};

// Spectral transmittance toward the sun from height h (sun at zenith cosine mu).
void sunTransmittance(const Coeffs& c, double h, double mu, Spec& T) {
  float vis = sunVisibility(kEarthRadius + h, mu);
  if (vis <= 0.0f) {
    T.fill(0.0f);
    return;
  }
  Density d = depthTable().lookup(h, mu);
  for (int i = 0; i < kBands; ++i) {
    double tau = c.scatR[i] * d.r + c.extM[i] * d.m + c.absO[i] * d.o;
    T[i] = vis * static_cast<float>(std::exp(-tau));
  }
}

float rayleighPhase(float nu) { return 3.0f / (16.0f * kPi) * (1.0f + nu * nu); }

// Cornette-Shanks phase function (a normalized Henyey-Greenstein variant with a Rayleigh-like lobe).
float miePhase(float nu) {
  float g = kMieG, g2 = g * g;
  return 3.0f / (8.0f * kPi) * ((1.0f - g2) * (1.0f + nu * nu)) /
         ((2.0f + g2) * std::pow(std::max(1.0f + g2 - 2.0f * g * nu, 1e-6f), 1.5f));
}

// Spectral reflectance of the ground from an RGB albedo (smooth blend of the three channels).
Spec spectralAlbedo(const Vec3& a) {
  Spec s{};
  for (int i = 0; i < kBands; ++i) {
    float l = bandCenter(i);
    float wb = clampf((520.0f - l) / 70.0f, 0.0f, 1.0f), wr = clampf((l - 560.0f) / 60.0f, 0.0f, 1.0f);
    float wg = std::max(0.0f, 1.0f - wb - wr);
    s[i] = clampf(a.x * wr + a.y * wg + a.z * wb, 0.0f, 1.0f);
  }
  return s;
}

Vec3 spectrumToRgb(const Spec& L) {
  const SpectralData& sd = spectral();
  Vec3 c(0.0f);
  for (int i = 0; i < kBands; ++i) c += sd.toRgb[i] * L[i];
  return c;
}

// Runs fn(i) for i in [0, n) on all cores.
template <typename Fn>
void parallelFor(int n, const Fn& fn) {
  int threads = static_cast<int>(std::min<unsigned>(std::max(1u, std::thread::hardware_concurrency()), 32u));
  threads = std::min(threads, n);
  if (threads <= 1) {
    for (int i = 0; i < n; ++i) fn(i);
    return;
  }
  std::atomic<int> next{0};
  auto work = [&] {
    for (int i = next++; i < n; i = next++) fn(i);
  };
  std::vector<std::thread> pool;
  for (int t = 1; t < threads; ++t) pool.emplace_back(work);
  work();
  for (auto& t : pool) t.join();
}

}  // namespace

// ---------------------------------------------------------------------------------------------

Vec3 atmosphereTransmittance(const Vec3& dirToSun, const AtmosphereParams& p) {
  Vec3 d = normalize(dirToSun);
  Spec T;
  sunTransmittance(coeffsFor(p), kViewerHeight, d.z, T);
  const SpectralData& sd = spectral();
  Spec E;
  for (int i = 0; i < kBands; ++i) E[i] = sd.sun[i] * T[i];
  return vmax(spectrumToRgb(E) * (1.0f / kSolarIlluminance), Vec3(0.0f));
}

Vec3 solarLimbDarkening(float r) {
  // Linear limb darkening law I(mu) = 1 - u (1 - mu) with coefficients for red, green and blue
  // light (the limb of the sun is darker and redder than its center); the disk average of the
  // law is 1 - u / 3.
  const Vec3 u(0.62f, 0.72f, 0.82f);
  float mu = std::sqrt(std::max(0.0f, 1.0f - clampf(r, 0.0f, 1.0f) * clampf(r, 0.0f, 1.0f)));
  Vec3 out;
  for (int k = 0; k < 3; ++k) out[k] = (1.0f - u[k] * (1.0f - mu)) / (1.0f - u[k] / 3.0f);
  return out;
}

// ---------------------------------------------------------------------------------------------
// Sky implementation

struct PhysicalSky::Impl {
  // Physical atmosphere.
  bool physical = false;
  Coeffs coeffs;
  Vec3 sunDir{0, 0, 1};
  float sunScale = 1.0f;  // sun illuminance relative to kSolarIlluminance
  SunTable sunT;
  // Multiple scattering: Psi(h, mu_s) per band, per unit solar irradiance (Hillaire 2020).
  static constexpr int kMsH = 32, kMsMu = 64;
  std::vector<Spec> ms;
  // The sky depends only on the view zenith angle and the angle to the sun, so it is integrated
  // on that grid (cos(zenith) = u^2 is dense at the horizon, angle = pi v^2 dense at the sun) and
  // interpolated.
  static constexpr int kGridZ = 128, kGridSun = 192;
  std::vector<Vec3> grid;

  // Hosek-Wilkie (XYZ version).
  bool hosek = false;
  ArHosekSkyModelState* hw[3] = {nullptr, nullptr, nullptr};
  float hosekWeight = 1.0f;  // blend toward the physical model once the sun has set
  Vec3 hosekSun{0, 0, 1};

  ~Impl() {
    for (auto* s : hw)
      if (s) arhosekskymodelstate_free(s);
  }

  void lookupMs(double h, double mu, Spec& out) const {
    double fh = std::sqrt(clampf(static_cast<float>(h / (kTopRadius - kEarthRadius)), 0.0f, 1.0f)) * (kMsH - 1);
    double fm = (clampf(static_cast<float>(mu), -1.0f, 1.0f) * 0.5 + 0.5) * (kMsMu - 1);
    int h0 = std::min(static_cast<int>(fh), kMsH - 2), m0 = std::min(static_cast<int>(fm), kMsMu - 2);
    float th = static_cast<float>(fh - h0), tm = static_cast<float>(fm - m0);
    const Spec& a = ms[static_cast<size_t>(h0) * kMsMu + m0];
    const Spec& b = ms[static_cast<size_t>(h0) * kMsMu + m0 + 1];
    const Spec& c = ms[static_cast<size_t>(h0 + 1) * kMsMu + m0];
    const Spec& d = ms[static_cast<size_t>(h0 + 1) * kMsMu + m0 + 1];
    for (int i = 0; i < kBands; ++i)
      out[i] = (a[i] * (1 - tm) + b[i] * tm) * (1 - th) + (c[i] * (1 - tm) + d[i] * tm) * th;
  }

  void buildMultipleScattering(const Spec& albedo) {
    ms.assign(static_cast<size_t>(kMsH) * kMsMu, Spec{});
    parallelFor(kMsH * kMsMu, [&](int idx) {
      int ih = idx / kMsMu, im = idx % kMsMu;
      double f = static_cast<double>(ih) / (kMsH - 1);
      double h = (kTopRadius - kEarthRadius) * f * f;
      double muS = -1.0 + 2.0 * im / (kMsMu - 1);
      double r0 = kEarthRadius + h;
      double sx = std::sqrt(std::max(0.0, 1.0 - muS * muS)), sz = muS;
      Spec L2{}, F{};
      const int nTheta = 8, nPhi = 8, steps = 20;
      for (int a = 0; a < nTheta; ++a)
        for (int b = 0; b < nPhi; ++b) {
          double ct = -1.0 + 2.0 * (a + 0.5) / nTheta, st = std::sqrt(std::max(0.0, 1.0 - ct * ct));
          double ph = kTwoPi * (b + 0.5) / nPhi;
          double wx = st * std::cos(ph), wy = st * std::sin(ph), wz = ct;
          double tG = toGround(r0, wz);
          double tMax = tG > 0.0 ? tG : std::max(toSphere(r0, wz, kTopRadius), 0.0);
          double dt = tMax / steps;
          Spec Lw{}, Fw{}, Tw;
          Tw.fill(1.0f);
          Spec Ts;
          for (int k = 0; k < steps; ++k) {
            double t = (k + 0.5) * dt;
            double px = wx * t, py = wy * t, pz = r0 + wz * t;
            double r = std::sqrt(px * px + py * py + pz * pz);
            Density d = densityAt(r - kEarthRadius);
            sunT.lookup(r - kEarthRadius, (px * sx + pz * sz) / r, Ts);
            for (int i = 0; i < kBands; ++i) {
              float sS = static_cast<float>(coeffs.scatR[i] * d.r + coeffs.scatM[i] * d.m);
              float ext = static_cast<float>(coeffs.scatR[i] * d.r + coeffs.extM[i] * d.m + coeffs.absO[i] * d.o);
              float tSeg = std::exp(-ext * static_cast<float>(dt));
              float integ = ext > 1e-12f ? (1.0f - tSeg) / ext : static_cast<float>(dt);
              Lw[i] += Tw[i] * sS * Ts[i] * (1.0f / (4.0f * kPi)) * integ;
              Fw[i] += Tw[i] * sS * integ;
              Tw[i] *= tSeg;
            }
          }
          if (tG > 0.0) {
            // Light reflected by the (Lambertian) ground toward the point.
            double px = wx * tG, py = wy * tG, pz = r0 + wz * tG;
            double r = std::sqrt(px * px + py * py + pz * pz);
            double muG = (px * sx + pz * sz) / r;
            sunT.lookup(0.0, muG, Ts);
            float cosG = static_cast<float>(std::max(muG, 0.0));
            for (int i = 0; i < kBands; ++i) Lw[i] += Tw[i] * albedo[i] * kInvPi * Ts[i] * cosG;
          }
          for (int i = 0; i < kBands; ++i) {
            L2[i] += Lw[i] / (nTheta * nPhi);
            F[i] += Fw[i] / (nTheta * nPhi);
          }
        }
      Spec& out = ms[static_cast<size_t>(idx)];
      for (int i = 0; i < kBands; ++i) out[i] = L2[i] / std::max(1.0f - F[i], 1e-3f);
    });
  }

  void buildGrid() {
    grid.assign(static_cast<size_t>(kGridZ) * kGridSun, Vec3(0.0f));
    // Frame around the sun's azimuth (any azimuth when the sun is at the zenith).
    Vec3 z(0, 0, 1), a(sunDir.x, sunDir.y, 0.0f);
    a = length2(a) > 1e-12f ? normalize(a) : Vec3(1, 0, 0);
    Vec3 b = cross(z, a);
    float cosS = clampf(sunDir.z, -1.0f, 1.0f), sinS = std::sqrt(std::max(0.0f, 1.0f - cosS * cosS));
    parallelFor(kGridZ, [&](int iz) {
      float u = static_cast<float>(iz) / (kGridZ - 1);
      float cosT = u * u, sinT = std::sqrt(std::max(0.0f, 1.0f - cosT * cosT));
      for (int ig = 0; ig < kGridSun; ++ig) {
        float v = static_cast<float>(ig) / (kGridSun - 1);
        float cosG = std::cos(kPi * v * v);
        // Azimuth from the sun with this angle to the sun (the nearest one when unreachable).
        float cosP = sinT * sinS > 1e-6f ? clampf((cosG - cosT * cosS) / (sinT * sinS), -1.0f, 1.0f) : 1.0f;
        float sinP = std::sqrt(std::max(0.0f, 1.0f - cosP * cosP));
        Vec3 d = a * (sinT * cosP) + b * (sinT * sinP) + z * cosT;
        grid[static_cast<size_t>(iz) * kGridSun + ig] = march(d);
      }
    });
  }

  Vec3 physicalRadiance(const Vec3& dir) const {
    float u = std::sqrt(clampf(dir.z, 0.0f, 1.0f));
    float gamma = std::atan2(length(cross(dir, sunDir)), dot(dir, sunDir));
    float v = std::sqrt(clampf(gamma / kPi, 0.0f, 1.0f));
    float fz = u * (kGridZ - 1), fg = v * (kGridSun - 1);
    int z0 = std::min(static_cast<int>(fz), kGridZ - 2), g0 = std::min(static_cast<int>(fg), kGridSun - 2);
    float tz = fz - static_cast<float>(z0), tg = fg - static_cast<float>(g0);
    auto at = [&](int i, int j) { return grid[static_cast<size_t>(i) * kGridSun + j]; };
    return lerp(lerp(at(z0, g0), at(z0, g0 + 1), tg), lerp(at(z0 + 1, g0), at(z0 + 1, g0 + 1), tg), tz);
  }

  // Single + multiple scattering along the view ray from the viewer (dir.z >= 0).
  Vec3 march(const Vec3& dir) const {
    const SpectralData& sd = spectral();
    double r0 = kEarthRadius + kViewerHeight;
    double dx = dir.x, dy = dir.y, dz = std::max(dir.z, 0.0f);
    double len = std::sqrt(dx * dx + dy * dy + dz * dz);
    dx /= len;
    dy /= len;
    dz /= len;
    double tMax = std::max(toSphere(r0, dz, kTopRadius), 0.0);
    float nu = static_cast<float>(dx * sunDir.x + dy * sunDir.y + dz * sunDir.z);
    float pR = rayleighPhase(nu), pM = miePhase(nu);
    Spec L{}, Tv, Ts, psi;
    Tv.fill(1.0f);
    const int steps = 40;
    double prev = 0.0;
    for (int k = 0; k < steps; ++k) {
      double f = (k + 1.0) / steps;
      double t = tMax * f * f;
      double dt = t - prev, tm = prev + 0.5 * dt;
      prev = t;
      double px = dx * tm, py = dy * tm, pz = r0 + dz * tm;
      double r = std::sqrt(px * px + py * py + pz * pz);
      double h = r - kEarthRadius;
      double muS = (px * sunDir.x + py * sunDir.y + pz * sunDir.z) / r;
      Density d = densityAt(h);
      sunT.lookup(h, muS, Ts);
      lookupMs(h, muS, psi);
      for (int i = 0; i < kBands; ++i) {
        float sR = static_cast<float>(coeffs.scatR[i] * d.r), sM = static_cast<float>(coeffs.scatM[i] * d.m);
        float ext = sR + static_cast<float>(coeffs.extM[i] * d.m + coeffs.absO[i] * d.o);
        float S = sd.sun[i] * (sR * (pR * Ts[i] + psi[i]) + sM * (pM * Ts[i] + psi[i]));
        float tSeg = std::exp(-ext * static_cast<float>(dt));
        float integ = ext > 1e-12f ? (1.0f - tSeg) / ext : static_cast<float>(dt);
        L[i] += Tv[i] * S * integ;
        Tv[i] *= tSeg;
      }
    }
    return vmax(spectrumToRgb(L) * sunScale, Vec3(0.0f));
  }

  Vec3 hosekRadiance(const Vec3& dir) const {
    float cosT = clampf(dir.z, 0.0f, 1.0f);
    double theta = std::acos(cosT);
    double gamma = std::acos(clampf(dot(dir, hosekSun), -1.0f, 1.0f));
    Vec3 xyz(static_cast<float>(arhosek_tristim_skymodel_radiance(hw[0], theta, gamma, 0)),
             static_cast<float>(arhosek_tristim_skymodel_radiance(hw[1], theta, gamma, 1)),
             static_cast<float>(arhosek_tristim_skymodel_radiance(hw[2], theta, gamma, 2)));
    // The XYZ data are radiometric (CIE observer without the 683 lm/W factor).
    return vmax(xyzToLinearSrgb(xyz * 683.0f), Vec3(0.0f)) * sunScale;
  }
};

PhysicalSky::PhysicalSky() = default;
PhysicalSky::~PhysicalSky() = default;

void PhysicalSky::build(const Vec3& sunDir, const AtmosphereParams& p, float sunE) {
  build(sunDir, p, sunE, kSkyPhysical, 1.5f);
}

void PhysicalSky::build(const Vec3& sunDir, const AtmosphereParams& p, float sunE, int model, float horizonBlurDeg) {
  impl_ = std::make_unique<Impl>();
  Impl& I = *impl_;
  sun_ = normalize(sunDir);
  model_ = model == kSkyHosekWilkie ? kSkyHosekWilkie : kSkyPhysical;
  blurDeg_ = std::max(horizonBlurDeg, 0.0f);
  I.sunDir = sun_;
  I.sunScale = std::max(sunE, 0.0f) / kSolarIlluminance;
  I.coeffs = coeffsFor(p);
  float elevation = std::asin(clampf(sun_.z, -1.0f, 1.0f));
  if (model_ == kSkyHosekWilkie) {
    // The Hosek-Wilkie fit covers solar elevations from 0 to 90 degrees; below the horizon the
    // sky fades into the physical model, which handles twilight.
    I.hosek = true;
    I.hosekWeight = smoothStep(radians(-1.0f), 0.0f, elevation);
    I.hosekSun = normalize(Vec3(sun_.x, sun_.y, std::max(sun_.z, 0.0f)));
    double turb = clampf(p.turbidity, 1.0f, 10.0f), elev = std::max(elevation, 0.0f);
    Vec3 a = clampColor(p.groundAlbedo, 0.0f, 1.0f);
    // Ground albedo per XYZ channel (the albedo's reflectance in X, Y and Z).
    double alb[3] = {(0.4124 * a.x + 0.3576 * a.y + 0.1805 * a.z) / 0.9505, luminance(a),
                     (0.0193 * a.x + 0.1192 * a.y + 0.9505 * a.z) / 1.0890};
    for (int c = 0; c < 3; ++c) I.hw[c] = arhosek_xyz_skymodelstate_alloc_init(turb, std::min(alb[c], 1.0), elev);
  }
  I.physical = !I.hosek || I.hosekWeight < 1.0f;
  if (I.physical) {
    I.sunT.build(I.coeffs);
    I.buildMultipleScattering(spectralAlbedo(p.groundAlbedo));
    I.buildGrid();
  }

  // Ground below the horizon: diffuse albedo lit by the sun and the sky.
  Vec3 Esky(0.0f);
  const int nT = 12, nP = 24;
  for (int i = 0; i < nT; ++i) {
    float theta = (i + 0.5f) / nT * (kPi * 0.5f);
    for (int j = 0; j < nP; ++j) {
      float phi = (j + 0.5f) / nP * kTwoPi;
      Vec3 d(std::sin(theta) * std::cos(phi), std::sin(theta) * std::sin(phi), std::cos(theta));
      Esky += skyRadiance(d) * (std::cos(theta) * std::sin(theta) * (kPi * 0.5f / nT) * (kTwoPi / nP));
    }
  }
  Vec3 Esun = atmosphereTransmittance(sun_, p) * (sunE * std::max(sun_.z, 0.0f));
  groundRadiance_ = clampColor(p.groundAlbedo, 0.0f, 1.0f) * (Esun + Esky) * kInvPi;
}

Vec3 PhysicalSky::skyRadiance(const Vec3& dir) const {
  if (!impl_) return Vec3(0.0f);
  const Impl& I = *impl_;
  Vec3 d = normalize(Vec3(dir.x, dir.y, std::max(dir.z, 0.0f)));
  if (I.hosek && I.hosekWeight >= 1.0f) return I.hosekRadiance(d);
  if (!I.hosek) return I.physicalRadiance(d);
  return lerp(I.physicalRadiance(d), I.hosekRadiance(d), I.hosekWeight);
}

Vec3 PhysicalSky::radiance(const Vec3& dirIn) const {
  Vec3 dir = normalize(dirIn);
  if (dir.z >= 0.0f) return skyRadiance(dir);
  // Below the horizon: fade from the horizon haze into the lit ground.
  float below = degrees(std::asin(clampf(-dir.z, 0.0f, 1.0f)));
  if (below >= blurDeg_) return groundRadiance_;
  float t = smoothStep(0.0f, std::max(blurDeg_, 1e-3f), below);
  return lerp(skyRadiance(Vec3(dir.x, dir.y, 0.0f)), groundRadiance_, t);
}

Vec3 PhysicalSky::groundRadiance() const { return groundRadiance_; }

std::vector<float> PhysicalSky::bake(int w, int h) const {
  std::vector<float> rgb(static_cast<size_t>(w) * h * 3, 0.0f);
  parallelFor(h, [&](int y) {
    for (int x = 0; x < w; ++x) {
      Vec3 d = equirectToDirection((x + 0.5f) / w, (y + 0.5f) / h, 0.0f);
      Vec3 c = radiance(d);
      size_t i = (static_cast<size_t>(y) * w + x) * 3;
      rgb[i] = c.x;
      rgb[i + 1] = c.y;
      rgb[i + 2] = c.z;
    }
  });
  return rgb;
}

}  // namespace render
}  // namespace veraa
