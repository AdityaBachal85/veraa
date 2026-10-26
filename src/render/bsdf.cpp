#include "bsdf.h"

#include <algorithm>
#include <atomic>
#include <complex>
#include <thread>
#include <vector>

namespace veraa {
namespace render {

namespace {

constexpr float kMinAlpha = 0.0015f;

float ggxG1(const Vec3& w, float ax, float ay) { return 1.0f / (1.0f + ggxLambda(w, ax, ay)); }
float ggxG2(const Vec3& wo, const Vec3& wi, float ax, float ay) {
  return 1.0f / (1.0f + ggxLambda(wo, ax, ay) + ggxLambda(wi, ax, ay));
}

// "Charlie" sheen distribution (Estevez & Kulla 2017).
float charlieD(float cosH, float alpha) {
  float inv = 1.0f / alpha;
  float sin2 = std::max(0.0f, 1.0f - cosH * cosH);
  return (2.0f + inv) * std::pow(sin2, inv * 0.5f) / kTwoPi;
}

// Shadowing of the Charlie sheen: the fitted Lambda of Estevez & Kulla, "Production Friendly
// Microfacet Sheen BRDF" (2017), with their terminator softening on the light side.
float sheenL(float x, float r) {
  float t = (1.0f - r) * (1.0f - r);
  float a = 21.5473f + (25.3245f - 21.5473f) * t, b = 3.82987f + (3.32435f - 3.82987f) * t;
  float c = 0.19823f + (0.16801f - 0.19823f) * t, d = -1.97760f + (-1.27393f + 1.97760f) * t;
  float e = -4.32054f + (-4.85967f + 4.32054f) * t;
  return a / (1.0f + b * std::pow(std::max(x, 0.0f), c)) + d * x + e;
}

float sheenLambda(float cosT, float r) {
  return cosT < 0.5f ? std::exp(sheenL(cosT, r)) : std::exp(2.0f * sheenL(0.5f, r) - sheenL(1.0f - cosT, r));
}

// f * cos(wi) of the sheen lobe for unit weight.
float sheenEval(float cO, float cI, float cosH, float r) {
  float lo = sheenLambda(cO, r), li = sheenLambda(cI, r);
  li = std::pow(li, 1.0f + 2.0f * std::pow(1.0f - cI, 8.0f));
  float G = 1.0f / (1.0f + lo + li);
  return charlieD(cosH, r) * G / (4.0f * std::max(cO, 1e-4f));
}

bool isReflective(LobeKind k) {
  return k == LobeKind::Lambert || k == LobeKind::OrenNayar || k == LobeKind::Sheen || k == LobeKind::GgxReflect ||
         k == LobeKind::SpecularReflect;
}

bool isDelta(LobeKind k) {
  return k == LobeKind::SpecularReflect || k == LobeKind::SpecularRefract || k == LobeKind::ThinTransmit ||
         k == LobeKind::Subsurface;
}

Vec3 clampUpper(Vec3 w) {
  if (w.z < 1e-4f) {
    w.z = 1e-4f;
    w = normalize(w);
  }
  return w;
}

}  // namespace

float ggxD(const Vec3& h, float ax, float ay) {
  if (h.z <= 0.0f) return 0.0f;
  float x = h.x / ax, y = h.y / ay;
  float d = x * x + y * y + h.z * h.z;
  return 1.0f / (kPi * ax * ay * d * d);
}

float ggxLambda(const Vec3& w, float ax, float ay) {
  float c2 = w.z * w.z;
  if (c2 >= 1.0f) return 0.0f;
  float a2t2 = (ax * ax * w.x * w.x + ay * ay * w.y * w.y) / std::max(c2, 1e-12f);
  return 0.5f * (-1.0f + std::sqrt(1.0f + a2t2));
}

// Visible-normal sampling (Heitz 2018), anisotropic.
Vec3 sampleGgxVndf(const Vec3& wo, float ax, float ay, float u1, float u2) {
  Vec3 vh = normalize(Vec3(ax * wo.x, ay * wo.y, wo.z));
  float lensq = vh.x * vh.x + vh.y * vh.y;
  Vec3 t1 = lensq > 0.0f ? Vec3(-vh.y, vh.x, 0.0f) / std::sqrt(lensq) : Vec3(1.0f, 0.0f, 0.0f);
  Vec3 t2 = cross(vh, t1);
  float r = std::sqrt(u1);
  float phi = kTwoPi * u2;
  float p1 = r * std::cos(phi);
  float p2 = r * std::sin(phi);
  float s = 0.5f * (1.0f + vh.z);
  p2 = (1.0f - s) * std::sqrt(std::max(0.0f, 1.0f - p1 * p1)) + s * p2;
  Vec3 nh = t1 * p1 + t2 * p2 + vh * std::sqrt(std::max(0.0f, 1.0f - p1 * p1 - p2 * p2));
  return normalize(Vec3(ax * nh.x, ay * nh.y, std::max(1e-6f, nh.z)));
}

float sheenAlbedo(float cosO, float alpha) {
  // 32 x 32 table over (cos theta_o, alpha), integrated numerically once.
  static const std::vector<float> table = [] {
    std::vector<float> t(32 * 32);
    for (int ia = 0; ia < 32; ++ia) {
      float a = 0.07f + (1.0f - 0.07f) * ia / 31.0f;
      for (int ic = 0; ic < 32; ++ic) {
        float c = std::max((ic + 0.5f) / 32.0f, 0.02f);
        Vec3 wo(std::sqrt(1.0f - c * c), 0.0f, c);
        double sum = 0.0;
        const int nT = 48, nP = 48;
        for (int i = 0; i < nT; ++i) {
          float th = (i + 0.5f) / nT * (kPi * 0.5f);
          for (int j = 0; j < nP; ++j) {
            float ph = (j + 0.5f) / nP * kTwoPi;
            Vec3 wi(std::sin(th) * std::cos(ph), std::sin(th) * std::sin(ph), std::cos(th));
            Vec3 h = normalize(wo + wi);
            sum += sheenEval(wo.z, wi.z, h.z, a) * std::sin(th) * (kPi * 0.5f / nT) * (kTwoPi / nP);
          }
        }
        t[ia * 32 + ic] = static_cast<float>(std::min(sum, 1.0));
      }
    }
    return t;
  }();
  float fa = clampf((alpha - 0.07f) / (1.0f - 0.07f), 0.0f, 1.0f) * 31.0f;
  float fc = clampf(cosO * 32.0f - 0.5f, 0.0f, 31.0f);
  int a0 = std::min(static_cast<int>(fa), 30), c0 = std::min(static_cast<int>(fc), 30);
  float ta = fa - a0, tc = fc - c0;
  auto at = [&](int a, int c) { return table[a * 32 + c]; };
  float v0 = at(a0, c0) + (at(a0, c0 + 1) - at(a0, c0)) * tc;
  float v1 = at(a0 + 1, c0) + (at(a0 + 1, c0 + 1) - at(a0 + 1, c0)) * tc;
  return v0 + (v1 - v0) * ta;
}

float glossToAlpha(float gloss, bool isRoughness) {
  float r = isRoughness ? clampf(gloss, 0.0f, 1.0f) : 1.0f - clampf(gloss, 0.0f, 1.0f);
  return std::max(r * r, kMinAlpha);
}

// ---------------------------------------------------------------------------------------------
// Fresnel

namespace {

using cfloat = std::complex<float>;

// Amplitude reflection coefficients (s and p) from a medium of real index n1 into one of complex
// index n2, for the incidence cosine c1.
void amplitudes(float n1, cfloat n2, float c1, cfloat& rs, cfloat& rp) {
  float s1sq = std::max(0.0f, 1.0f - c1 * c1);
  cfloat c2 = std::sqrt(cfloat(1.0f) - cfloat(s1sq * n1 * n1) / (n2 * n2));
  rs = (n1 * c1 - n2 * c2) / (n1 * c1 + n2 * c2);
  rp = (n2 * c1 - n1 * c2) / (n2 * c1 + n1 * c2);
}

// Visible wavelengths for thin-film integration and their weights to linear RGB: the CIE 1931
// fit of Wyman et al. 2013 under a 6504 K white, normalized so a flat reflectance of 1 is white.
constexpr int kFilmSamples = 32;
struct FilmSpectrum {
  float lambda[kFilmSamples];
  Vec3 weight[kFilmSamples];
};

const FilmSpectrum& filmSpectrum() {
  static const FilmSpectrum fs = [] {
    auto lobe = [](float x, float mu, float s1, float s2) {
      float t = (x - mu) / (x < mu ? s1 : s2);
      return std::exp(-0.5f * t * t);
    };
    FilmSpectrum f;
    Vec3 sum(0.0f);
    for (int i = 0; i < kFilmSamples; ++i) {
      float l = 390.0f + 320.0f * (i + 0.5f) / kFilmSamples;
      f.lambda[i] = l;
      Vec3 xyz(1.056f * lobe(l, 599.8f, 37.9f, 31.0f) + 0.362f * lobe(l, 442.0f, 16.0f, 26.7f) -
                   0.065f * lobe(l, 501.1f, 20.4f, 26.2f),
               0.821f * lobe(l, 568.8f, 46.9f, 40.5f) + 0.286f * lobe(l, 530.9f, 16.3f, 31.1f),
               1.217f * lobe(l, 437.0f, 11.8f, 36.0f) + 0.681f * lobe(l, 459.0f, 26.0f, 13.8f));
      double lm = l * 1e-9;
      double planck = 1e-30 / (lm * lm * lm * lm * lm * (std::exp(1.4388e-2 / (lm * 6504.0)) - 1.0));
      xyz *= static_cast<float>(planck);
      f.weight[i] = Vec3(3.2404542f * xyz.x - 1.5371385f * xyz.y - 0.4985314f * xyz.z,
                         -0.9692660f * xyz.x + 1.8760108f * xyz.y + 0.0415560f * xyz.z,
                         0.0556434f * xyz.x - 0.2040259f * xyz.y + 1.0572252f * xyz.z);
      sum += f.weight[i];
    }
    for (auto& w : f.weight) w = Vec3(w.x / sum.x, w.y / sum.y, w.z / sum.z);
    return f;
  }();
  return fs;
}

// RGB channel value at a wavelength (blue at 465 nm, green at 550 nm, red at 630 nm).
float channelAt(const Vec3& v, float lambda) {
  if (lambda <= 465.0f) return v.z;
  if (lambda <= 550.0f) return v.z + (v.y - v.z) * (lambda - 465.0f) / 85.0f;
  if (lambda <= 630.0f) return v.y + (v.x - v.y) * (lambda - 550.0f) / 80.0f;
  return v.x;
}

}  // namespace

Vec3 fresnelConductor(float cosI, const Vec3& n, const Vec3& k) {
  float c = clampf(cosI, 0.0f, 1.0f);
  Vec3 out;
  for (int ch = 0; ch < 3; ++ch) {
    cfloat rs, rp;
    amplitudes(1.0f, cfloat(n[ch], k[ch]), c, rs, rp);
    out[ch] = 0.5f * (std::norm(rs) + std::norm(rp));
  }
  return out;
}

float fresnelThinSlab(float cosI, float eta) {
  cfloat rs, rp;
  amplitudes(1.0f, cfloat(std::max(eta, 1.0f), 0.0f), clampf(cosI, 0.0f, 1.0f), rs, rp);
  float Rs = std::norm(rs), Rp = std::norm(rp);
  // Per polarization R + T^2 R / (1 - R^2) = 2R / (1 + R) for a lossless sheet.
  return 0.5f * (2.0f * Rs / (1.0f + Rs) + 2.0f * Rp / (1.0f + Rp));
}

Vec3 thinFilmReflectance(float cosI, float filmIor, float thicknessNm, const Vec3& baseN, const Vec3& baseK) {
  const FilmSpectrum& fs = filmSpectrum();
  float c1 = clampf(cosI, 0.0f, 1.0f);
  float n2 = std::max(filmIor, 1.0f);
  float s1sq = 1.0f - c1 * c1;
  float c2 = std::sqrt(std::max(0.0f, 1.0f - s1sq / (n2 * n2)));
  cfloat r12s, r12p;
  amplitudes(1.0f, cfloat(n2, 0.0f), c1, r12s, r12p);
  Vec3 out(0.0f);
  for (int i = 0; i < kFilmSamples; ++i) {
    float lambda = fs.lambda[i];
    cfloat n3(channelAt(baseN, lambda), channelAt(baseK, lambda));
    // Film -> base interface, seen from inside the film.
    cfloat c3 = std::sqrt(cfloat(1.0f) - cfloat(s1sq) / (n3 * n3));
    cfloat r23s = (n2 * c2 - n3 * c3) / (n2 * c2 + n3 * c3);
    cfloat r23p = (n3 * c2 - n2 * c3) / (n3 * c2 + n2 * c3);
    // Airy summation of the waves bouncing in the film (phase of one round trip).
    cfloat e = std::polar(1.0f, 4.0f * kPi * n2 * thicknessNm * c2 / lambda);
    cfloat rs = (r12s + r23s * e) / (1.0f + r12s * r23s * e);
    cfloat rp = (r12p + r23p * e) / (1.0f + r12p * r23p * e);
    out += fs.weight[i] * (0.5f * (std::norm(rs) + std::norm(rp)));
  }
  return clampColor(out, 0.0f, 1.0f);
}

Vec3 Lobe::fresnelAt(float c) const {
  const bool film = filmThickness > 0.0f;
  auto dielectric = [&]() -> Vec3 {
    if (fresnelEta <= 0.0f) return Vec3(1.0f);
    if (film && fresnelEta >= 1.0f) return thinFilmReflectance(c, filmIor, filmThickness, Vec3(fresnelEta), Vec3(0.0f));
    return Vec3(fresnelDielectric(c, fresnelEta));
  };
  auto conductor = [&](const Vec3& n, const Vec3& k) {
    return film ? thinFilmReflectance(c, filmIor, filmThickness, n, k) : fresnelConductor(c, n, k);
  };
  switch (fresnel) {
    case FresnelMode::None: return tint;
    case FresnelMode::Dielectric: return tint * dielectric();
    case FresnelMode::Schlick: return schlick(tint, c);
    case FresnelMode::Metalness: {
      Vec3 diel = tint * dielectric();
      if (metal <= 0.0f) return diel;
      Vec3 m;
      if (metalNk) {
        m = tint * conductor(condN, condK);
      } else if (film) {
        // Artist F0 under a film: the conductor with n = 1 and the k giving that F0.
        Vec3 f0 = clampColor(metalF0, 0.0f, 0.999f), k;
        for (int ch = 0; ch < 3; ++ch) k[ch] = 2.0f * std::sqrt(f0[ch] / (1.0f - f0[ch]));
        m = conductor(Vec3(1.0f), k);
      } else {
        m = schlick(metalF0, c);
      }
      return lerp(diel, m, metal);
    }
    case FresnelMode::Conductor: return tint * conductor(condN, condK);
    case FresnelMode::ThinSlab:
      return tint * (fresnelEta > 0.0f ? fresnelThinSlab(c, std::max(fresnelEta, 1.0f / fresnelEta)) : 1.0f);
  }
  return tint;
}

// ---------------------------------------------------------------------------------------------
// Microfacet energy: single-scattering albedo of GGX, integrated with visible-normal sampling
// once per process.

namespace {

constexpr int kTabMu = 32, kTabRough = 32, kTabEta = 16;
constexpr float kTabEtaMax = 3.0f;

float tabMu(int i) { return std::max(static_cast<float>(i) / (kTabMu - 1), 0.02f); }
float tabAlpha(int j) {
  float r = static_cast<float>(j) / (kTabRough - 1);
  return std::max(r * r, 0.0015f);
}
float tabEta(int k) { return 1.0f + (kTabEtaMax - 1.0f) * static_cast<float>(k) / (kTabEta - 1); }

struct EnergyTables {
  std::vector<float> e0;     // [rough][mu], F = 1
  std::vector<float> e0avg;  // [rough]
  std::vector<float> reflIn, transIn, reflOut, transOut;  // [eta][rough][mu]
};

template <typename Fn>
void parallelRun(int n, const Fn& fn) {
  int threads = static_cast<int>(std::min<unsigned>(std::max(1u, std::thread::hardware_concurrency()), 32u));
  threads = std::min(threads, n);
  std::atomic<int> next{0};
  auto work = [&] {
    for (int i = next++; i < n; i = next++) fn(i);
  };
  std::vector<std::thread> pool;
  for (int t = 1; t < threads; ++t) pool.emplace_back(work);
  work();
  for (auto& t : pool) t.join();
}

// Means of F G2/G1 (reflection) and (1 - F) G2/G1 (transmission) over the visible normals: the
// directional albedos of the GGX lobes as Bsdf evaluates them. eta 0 = perfect mirror (F = 1).
void integrate(float mu, float alpha, float eta, double& refl, double& trans) {
  Vec3 wo(std::sqrt(std::max(0.0f, 1.0f - mu * mu)), 0.0f, mu);
  const int n = 24;
  float lo = ggxLambda(wo, alpha, alpha), g1 = 1.0f / (1.0f + lo);
  double r = 0.0, t = 0.0;
  for (int a = 0; a < n; ++a)
    for (int b = 0; b < n; ++b) {
      Vec3 h = sampleGgxVndf(wo, alpha, alpha, (a + 0.5f) / n, (b + 0.5f) / n);
      float F = eta > 0.0f ? fresnelDielectric(dot(wo, h), eta) : 1.0f;
      Vec3 wi = reflectAbout(wo, h);
      if (wi.z > 0.0f) r += F / (1.0f + lo + ggxLambda(wi, alpha, alpha)) / g1;
      Vec3 wt;
      if (eta > 0.0f && F < 1.0f && refractDir(wo, h, eta, wt) && wt.z < 0.0f)
        t += (1.0f - F) / (1.0f + lo + ggxLambda(wt, alpha, alpha)) / g1;
    }
  refl = r / (n * n);
  trans = t / (n * n);
}

const EnergyTables& energyTables() {
  static const EnergyTables tables = [] {
    EnergyTables T;
    size_t n2 = static_cast<size_t>(kTabMu) * kTabRough, n3 = n2 * kTabEta;
    T.e0.resize(n2);
    T.e0avg.resize(kTabRough);
    T.reflIn.resize(n3);
    T.transIn.resize(n3);
    T.reflOut.resize(n3);
    T.transOut.resize(n3);
    parallelRun(kTabRough * (2 * kTabEta + 1), [&](int task) {
      int j = task % kTabRough, which = task / kTabRough;  // 0: F = 1, then entering / leaving per eta
      float alpha = tabAlpha(j);
      for (int i = 0; i < kTabMu; ++i) {
        double r, t;
        if (which == 0) {
          integrate(tabMu(i), alpha, 0.0f, r, t);
          T.e0[static_cast<size_t>(j) * kTabMu + i] = static_cast<float>(std::min(r, 1.0));
          continue;
        }
        int k = (which - 1) % kTabEta;
        bool out = which > kTabEta;
        integrate(tabMu(i), alpha, out ? 1.0f / tabEta(k) : tabEta(k), r, t);
        size_t idx = (static_cast<size_t>(k) * kTabRough + j) * kTabMu + i;
        (out ? T.reflOut : T.reflIn)[idx] = static_cast<float>(r);
        (out ? T.transOut : T.transIn)[idx] = static_cast<float>(t);
      }
    });
    for (int j = 0; j < kTabRough; ++j) {
      double avg = 0.0;  // trapezoid rule for 2 mu E(mu) over [0, 1]
      for (int i = 0; i < kTabMu; ++i) {
        float mu = static_cast<float>(i) / (kTabMu - 1);
        double w = (i == 0 || i == kTabMu - 1) ? 0.5 : 1.0;
        avg += w * 2.0 * mu * T.e0[static_cast<size_t>(j) * kTabMu + i] / (kTabMu - 1);
      }
      T.e0avg[j] = static_cast<float>(avg);
    }
    return T;
  }();
  return tables;
}

float lookup2(const float* tab, float mu, float alpha) {
  float fi = clampf(mu, 0.0f, 1.0f) * (kTabMu - 1), fj = std::sqrt(clampf(alpha, 0.0f, 1.0f)) * (kTabRough - 1);
  int i = std::min(static_cast<int>(fi), kTabMu - 2), j = std::min(static_cast<int>(fj), kTabRough - 2);
  float ti = fi - static_cast<float>(i), tj = fj - static_cast<float>(j);
  auto at = [&](int jj, int ii) { return tab[static_cast<size_t>(jj) * kTabMu + ii]; };
  return (at(j, i) * (1 - ti) + at(j, i + 1) * ti) * (1 - tj) + (at(j + 1, i) * (1 - ti) + at(j + 1, i + 1) * ti) * tj;
}

}  // namespace

float ggxAlbedo(float cosO, float alpha) { return lookup2(energyTables().e0.data(), cosO, alpha); }

float ggxAlbedoAverage(float alpha) {
  const EnergyTables& T = energyTables();
  float f = std::sqrt(clampf(alpha, 0.0f, 1.0f)) * (kTabRough - 1);
  int j = std::min(static_cast<int>(f), kTabRough - 2);
  float t = f - static_cast<float>(j);
  return T.e0avg[j] * (1 - t) + T.e0avg[j + 1] * t;
}

void ggxDielectricAlbedo(float cosO, float alpha, float eta, float& reflect, float& transmit) {
  const EnergyTables& T = energyTables();
  bool out = eta < 1.0f;
  float e = clampf(out ? 1.0f / std::max(eta, 1e-3f) : eta, 1.0f, kTabEtaMax);
  float f = (e - 1.0f) / (kTabEtaMax - 1.0f) * (kTabEta - 1);
  int k = std::min(static_cast<int>(f), kTabEta - 2);
  float tk = f - static_cast<float>(k);
  size_t stride = static_cast<size_t>(kTabRough) * kTabMu;
  const float* R = (out ? T.reflOut : T.reflIn).data();
  const float* Tr = (out ? T.transOut : T.transIn).data();
  reflect = lookup2(R + k * stride, cosO, alpha) * (1 - tk) + lookup2(R + (k + 1) * stride, cosO, alpha) * tk;
  transmit = lookup2(Tr + k * stride, cosO, alpha) * (1 - tk) + lookup2(Tr + (k + 1) * stride, cosO, alpha) * tk;
}

const MetalPreset* metalPresets(int& count) {
  // Complex IOR per linear sRGB channel, reduced from measured spectral n, k (refractiveindex.info
  // database, CC0): the spectral Fresnel reflectance at 50 angles was converted to RGB (CIE 1931,
  // 6504 K white) and n, k fitted per channel to that angular curve. Data: Johnson & Christy
  // 1972/1974 (Au, Ag, Cu, Cr, Fe, Ti, Ni, Co), Rakic 1995 (Al), Querry 1987 (brass, Cu70Zn30).
  static const MetalPreset presets[] = {
      {"Gold", {0.1305f, 0.4929f, 1.4731f}, {10.0844f, 2.2717f, 1.7803f}},
      {"Silver", {0.0552f, 0.0550f, 0.0518f}, {4.3645f, 3.5607f, 2.9104f}},
      {"Copper", {0.2473f, 1.0365f, 1.2962f}, {3.3043f, 2.6281f, 2.3546f}},
      {"Aluminium", {1.4680f, 0.9894f, 0.6015f}, {7.5846f, 6.5561f, 5.3339f}},
      {"Chromium", {3.2305f, 3.1254f, 2.2756f}, {3.2860f, 3.3379f, 3.1088f}},
      {"Iron", {2.9169f, 2.9240f, 2.5380f}, {3.0697f, 2.9403f, 2.7453f}},
      {"Titanium", {2.7342f, 2.5126f, 2.2359f}, {3.8215f, 3.4038f, 3.0121f}},
      {"Nickel", {2.0250f, 1.9046f, 1.7247f}, {4.1631f, 3.5805f, 2.8839f}},
      {"Cobalt", {2.2404f, 2.0422f, 1.7061f}, {4.1998f, 3.8050f, 3.2270f}},
      {"Brass", {0.4555f, 0.5174f, 1.1386f}, {4.0153f, 2.6866f, 1.8062f}},
  };
  count = static_cast<int>(sizeof(presets) / sizeof(presets[0]));
  return presets;
}

static_assert(std::is_trivially_copyable<Lobe>::value, "Lobe is copied as raw bytes");

Lobe* Bsdf::add(const Lobe& l) {
  if (count_ >= kMaxLobes) return nullptr;
  if (veraa::isBlack(l.weight) && l.kind != LobeKind::GgxRefract) return nullptr;
  Lobe* dst = new (&storage_[count_ * sizeof(Lobe)]) Lobe(l);
  ++count_;
  return dst;
}

void Bsdf::scale(const Vec3& s) {
  Lobe* L = lobes();
  for (int i = 0; i < count_; ++i) L[i].weight *= s;
}

void Bsdf::append(const Bsdf& other, const Vec3& s, bool flipped) {
  if (veraa::isBlack(s)) return;
  for (int i = 0; i < other.count_ && count_ < kMaxLobes; ++i) {
    Lobe l = other.lobes()[i];
    l.weight *= s;
    if (flipped) l.flipped = !l.flipped;
    if (veraa::isBlack(l.weight)) continue;
    new (&storage_[count_ * sizeof(Lobe)]) Lobe(l);
    ++count_;
  }
}

bool Bsdf::hasSubsurface() const {
  for (int i = 0; i < count_; ++i)
    if (lobes()[i].kind == LobeKind::Subsurface) return true;
  return false;
}

namespace {

// Hemispherical average of a lobe's Fresnel term, 2 * integral of F(mu) mu (3-point Gauss-Legendre).
Vec3 fresnelAverage(const Lobe& l) {
  const float x[3] = {0.1127017f, 0.5f, 0.8872983f}, w[3] = {5.0f / 18.0f, 8.0f / 18.0f, 5.0f / 18.0f};
  Vec3 f(0.0f);
  for (int i = 0; i < 3; ++i) f += l.fresnelAt(x[i]) * (2.0f * x[i] * w[i]);
  return f;
}

bool isPlainDielectric(const Lobe& l) {
  return (l.fresnel == FresnelMode::Dielectric || (l.fresnel == FresnelMode::Metalness && l.metal <= 0.0f)) &&
         l.fresnelEta > 0.0f && l.filmThickness <= 0.0f;
}

}  // namespace

Vec3 specularAlbedo(const Lobe& l, float cosO) {
  if (l.kind != LobeKind::GgxReflect || !isPlainDielectric(l)) return l.fresnelAt(cosO);
  float alpha = std::sqrt(l.ax * l.ay), R, T;
  ggxDielectricAlbedo(cosO, alpha, l.fresnelEta, R, T);
  if (!l.msCompensate) return l.tint * R;
  float E = std::max(ggxAlbedo(cosO, alpha), 0.05f);
  return l.tint * R * (1.0f + luminance(fresnelAverage(l)) / std::max(luminance(l.tint), 1e-4f) * (1.0f - E) / E);
}

// Energy that single-scattering microfacet lobes lose to shadowing (light that would leave
// after bouncing between microfacets) is put back. Rough dielectric interfaces with
// transmission are scaled so reflection + transmission conserve energy; other rough reflections
// get the multiply scattered energy F_avg (1 - E) / E (Turquin 2019, exact for F = 1).
void Bsdf::compensateEnergy() {
  Lobe* L = lobes();
  bool done[kMaxLobes] = {};
  for (int t = 0; t < count_; ++t) {
    Lobe& lt = L[t];
    if ((lt.kind != LobeKind::GgxRefract && lt.kind != LobeKind::SpecularRefract) || !lt.msCompensate ||
        lt.fresnelEta <= 0.0f)
      continue;
    int partner = -1;
    for (int r = 0; r < count_ && partner < 0; ++r)
      if ((L[r].kind == LobeKind::GgxReflect || L[r].kind == LobeKind::SpecularReflect) && isPlainDielectric(L[r]) &&
          std::fabs(L[r].fresnelEta - lt.fresnelEta) < 1e-4f)
        partner = r;
    if (partner < 0) continue;
    Lobe& lr = L[partner];
    float eta = lt.fresnelEta, deltaE = 0.0f, roughE = 0.0f, R, T;
    if (lr.kind == LobeKind::SpecularReflect) {
      deltaE += fresnelDielectric(lr.woL.z, eta);
    } else {
      ggxDielectricAlbedo(lr.woL.z, std::sqrt(lr.ax * lr.ay), eta, R, T);
      roughE += R;
    }
    if (lt.kind == LobeKind::SpecularRefract) {
      deltaE += 1.0f - fresnelDielectric(lt.woL.z, eta);
    } else {
      ggxDielectricAlbedo(lt.woL.z, std::sqrt(lt.ax * lt.ay), eta, R, T);
      roughE += T;
    }
    done[partner] = done[t] = true;
    if (roughE <= 1e-4f) continue;
    float s = clampf((1.0f - deltaE) / roughE, 1.0f, 4.0f);
    if (lr.kind == LobeKind::GgxReflect) lr.weight *= s;
    if (lt.kind == LobeKind::GgxRefract) lt.weight *= s;
  }
  for (int i = 0; i < count_; ++i) {
    Lobe& l = L[i];
    if (l.kind != LobeKind::GgxReflect || !l.msCompensate || done[i]) continue;
    float E = std::max(ggxAlbedo(l.woL.z, std::sqrt(l.ax * l.ay)), 0.05f);
    l.weight *= Vec3(1.0f) + fresnelAverage(l) * ((1.0f - E) / E);
  }
}

void Bsdf::finalize(const Vec3& wo) {
  float est[kMaxLobes];
  float sum = 0.0f, diffuseLike = 0.0f;
  albedo_ = Vec3(0.0f);
  nonDelta_ = false;
  Lobe* L = lobes();
  for (int i = 0; i < count_; ++i) L[i].woL = clampUpper(L[i].frame.toLocal(wo));
  compensateEnergy();
  for (int i = 0; i < count_; ++i) {
    Lobe& l = L[i];
    Vec3 woL = l.woL;
    Vec3 a = l.weight;
    switch (l.kind) {
      case LobeKind::Sheen: a *= sheenAlbedo(woL.z, l.ax); break;
      case LobeKind::GgxReflect:
      case LobeKind::SpecularReflect: a *= l.fresnelAt(woL.z); break;
      case LobeKind::GgxRefract:
        if (l.fresnelEta > 0.0f && l.etaRgb.x <= 0.0f) {
          // Rough interfaces transmit even beyond the critical angle of the macro surface.
          float R, T;
          ggxDielectricAlbedo(woL.z, std::sqrt(l.ax * l.ay), l.fresnelEta, R, T);
          a *= std::max(T, 0.0f);
        } else {
          a *= vmax(Vec3(1.0f) - l.fresnelAt(woL.z), Vec3(0.0f));
        }
        break;
      default: break;
    }
    est[i] = std::max(luminance(a), 0.0f);
    if (l.kind == LobeKind::Lambert || l.kind == LobeKind::OrenNayar || l.kind == LobeKind::DiffuseTransmission)
      diffuseLike += est[i];
    if (l.kind != LobeKind::DiffuseTransmission && l.kind != LobeKind::Subsurface) albedo_ += a;
    else albedo_ += a;
    if (!isDelta(l.kind) && est[i] > 0.0f) nonDelta_ = true;
  }
  // Sample reflections a bit more often than their energy suggests; they carry sharp detail.
  for (int i = 0; i < count_; ++i) {
    LobeKind k = L[i].kind;
    if ((k == LobeKind::GgxReflect || k == LobeKind::SpecularReflect) && est[i] > 0.0f)
      est[i] = std::max(est[i], 0.15f * diffuseLike);
    sum += est[i];
  }
  totalSelect_ = sum;
  for (int i = 0; i < count_; ++i) L[i].select = sum > 0.0f ? est[i] / sum : 0.0f;
  albedo_ = clampColor(albedo_, 0.0f, 1.0f);
}

Vec3 Bsdf::evalLobe(const Lobe& l, const Vec3& woL, const Vec3& wiIn, float& pdf) const {
  pdf = 0.0f;
  Vec3 wiL = wiIn;
  if (l.flipped) wiL.z = -wiL.z;
  float cO = woL.z, cI = wiL.z;
  switch (l.kind) {
    case LobeKind::Lambert:
      if (cI <= 0.0f) return Vec3(0.0f);
      pdf = cI * kInvPi;
      return l.weight * (cI * kInvPi);
    case LobeKind::OrenNayar: {
      if (cI <= 0.0f) return Vec3(0.0f);
      float sinI = std::sqrt(std::max(0.0f, 1.0f - cI * cI));
      float sinO = std::sqrt(std::max(0.0f, 1.0f - cO * cO));
      float maxCos = 0.0f;
      if (sinI > 1e-4f && sinO > 1e-4f) maxCos = std::max(0.0f, (wiL.x * woL.x + wiL.y * woL.y) / (sinI * sinO));
      float sinA, tanB;
      if (cI > cO) {
        sinA = sinO;
        tanB = sinI / cI;
      } else {
        sinA = sinI;
        tanB = sinO / std::max(cO, 1e-4f);
      }
      pdf = cI * kInvPi;
      return l.weight * (cI * kInvPi * (l.orenA + l.orenB * maxCos * sinA * tanB));
    }
    case LobeKind::DiffuseTransmission:
      if (cI >= 0.0f) return Vec3(0.0f);
      pdf = -cI * kInvPi;
      return l.weight * (-cI * kInvPi);
    case LobeKind::Sheen: {
      if (cI <= 0.0f) return Vec3(0.0f);
      Vec3 h = normalize(woL + wiL);
      pdf = cI * kInvPi;
      return l.weight * sheenEval(cO, cI, h.z, std::max(l.ax, 0.02f));
    }
    case LobeKind::GgxReflect: {
      if (cI <= 0.0f) return Vec3(0.0f);
      Vec3 h = normalize(woL + wiL);
      float D = ggxD(h, l.ax, l.ay);
      if (D <= 0.0f) return Vec3(0.0f);
      Vec3 F = l.fresnelAt(dot(woL, h));
      pdf = ggxG1(woL, l.ax, l.ay) * D / (4.0f * cO);
      return l.weight * F * (D * ggxG2(woL, wiL, l.ax, l.ay) / (4.0f * cO));
    }
    case LobeKind::GgxRefract: {
      if (cI >= 0.0f) return Vec3(0.0f);
      float eta = l.eta;
      Vec3 h = normalize(woL + wiL * eta);
      if (h.z < 0.0f) h = -h;
      float dO = dot(woL, h), dI = dot(wiL, h);
      if (dO <= 0.0f || dI >= 0.0f) return Vec3(0.0f);
      float denom = dO + eta * dI;
      if (std::fabs(denom) < 1e-6f) return Vec3(0.0f);
      float D = ggxD(h, l.ax, l.ay);
      float G = ggxG2(woL, Vec3(wiL.x, wiL.y, -wiL.z), l.ax, l.ay);
      Vec3 F = l.fresnelAt(dO);
      float common = D * eta * eta * std::fabs(dI) / (denom * denom);
      pdf = ggxG1(woL, l.ax, l.ay) * dO / cO * common;
      return l.weight * vmax(Vec3(1.0f) - F, Vec3(0.0f)) * (common * G * dO / cO);
    }
    default: return Vec3(0.0f);
  }
}

BsdfEval Bsdf::eval(const Vec3& wo, const Vec3& wi, int geomSide) const {
  BsdfEval e;
  for (auto& f : e.f) f = Vec3(0.0f);
  const Lobe* L = lobes();
  for (int i = 0; i < count_; ++i) {
    const Lobe& l = L[i];
    if (isDelta(l.kind) || l.select <= 0.0f) continue;
    const Vec3& woL = l.woL;
    Vec3 wiL = l.frame.toLocal(wi);
    if (geomSide != 0 && (wiL.z > 0.0f) != (geomSide > 0)) continue;
    float pdf = 0.0f;
    Vec3 f = evalLobe(l, woL, wiL, pdf);
    e.f[static_cast<int>(l.category)] += f;
    e.pdf += pdf * l.select;
  }
  return e;
}

bool Bsdf::sample(const Vec3& wo, float uLobe, float u1, float u2, BsdfSample& s) const {
  s = BsdfSample();
  if (isBlack()) return false;
  int idx = -1;
  float cdf = 0.0f, uRest = 0.0f;
  const Lobe* L = lobes();
  for (int i = 0; i < count_; ++i) {
    float next = cdf + L[i].select;
    if (uLobe < next || i == count_ - 1) {
      if (L[i].select <= 0.0f) continue;
      idx = i;
      uRest = clampf((uLobe - cdf) / L[i].select, 0.0f, 0.99999f);
      break;
    }
    cdf = next;
  }
  if (idx < 0) return false;
  const Lobe& l = L[idx];
  s.lobe = idx;
  s.category = l.category;
  Vec3 woL = l.woL;
  Vec3 wiL;
  auto finishDelta = [&](const Vec3& w, const Vec3& weight, bool transmission) {
    Vec3 wl = w;
    if (l.flipped) {
      wl.z = -wl.z;
      transmission = !transmission;
    }
    s.wi = normalize(l.frame.toWorld(wl));
    s.weight = weight / l.select;
    s.pdf = l.select;
    s.delta = true;
    s.transmission = transmission;
    return isFiniteColor(s.weight);
  };
  // Dispersion: continue with one color channel picked uniformly.
  auto pickEta = [&](float& eta) {
    if (l.etaRgb.x <= 0.0f) return Vec3(1.0f);
    int ch = std::min(static_cast<int>(uRest * 3.0f), 2);
    s.channel = ch;
    eta = l.etaRgb[ch];
    Vec3 mask(0.0f);
    mask[ch] = 3.0f;
    return mask;
  };
  switch (l.kind) {
    case LobeKind::Lambert:
    case LobeKind::OrenNayar:
    case LobeKind::Sheen: wiL = sampleCosineHemisphere(u1, u2); break;
    case LobeKind::DiffuseTransmission:
      wiL = sampleCosineHemisphere(u1, u2);
      wiL.z = -wiL.z;
      break;
    case LobeKind::GgxReflect: {
      Vec3 h = sampleGgxVndf(woL, l.ax, l.ay, u1, u2);
      wiL = reflectAbout(woL, h);
      if (wiL.z <= 0.0f) return false;
      break;
    }
    case LobeKind::SpecularReflect: return finishDelta(reflectLocal(woL), l.weight * l.fresnelAt(woL.z), false);
    case LobeKind::GgxRefract: {
      float eta = l.eta;
      Vec3 mask = pickEta(eta);
      Vec3 h = sampleGgxVndf(woL, l.ax, l.ay, u1, u2);
      if (!refractDir(woL, h, eta, wiL) || wiL.z >= 0.0f) return false;
      if (s.channel >= 0) {
        // Single-lobe estimate with the channel's IOR.
        Lobe lc = l;
        lc.eta = eta;
        float pdf = 0.0f;
        Vec3 wl = wiL;
        if (l.flipped) wl.z = -wl.z;
        Vec3 f = evalLobe(lc, woL, wl, pdf);
        if (pdf <= 0.0f) return false;
        s.wi = normalize(l.frame.toWorld(l.flipped ? Vec3(wiL.x, wiL.y, -wiL.z) : wiL));
        s.weight = f * mask / (pdf * l.select);
        s.pdf = pdf * l.select;
        s.transmission = !l.flipped;
        s.delta = true;  // no MIS with light sampling for dispersive paths
        return isFiniteColor(s.weight);
      }
      break;
    }
    case LobeKind::SpecularRefract: {
      float eta = l.eta;
      Vec3 mask = pickEta(eta);
      Vec3 w;
      if (refractDir(woL, Vec3(0, 0, 1), eta, w)) return finishDelta(w, l.weight * mask, true);
      return finishDelta(reflectLocal(woL), l.weight * mask, false);  // total internal reflection
    }
    case LobeKind::ThinTransmit: return finishDelta(-woL, l.weight, true);
    case LobeKind::Subsurface: {
      Vec3 w = sampleCosineHemisphere(u1, u2);
      w.z = -w.z;
      bool ok = finishDelta(w, l.weight, true);
      s.subsurface = true;
      return ok;
    }
  }
  // Non-delta: combine every non-delta lobe (one-sample MIS over lobes).
  if (l.flipped) wiL.z = -wiL.z;
  s.wi = normalize(l.frame.toWorld(wiL));
  BsdfEval e = eval(wo, s.wi);
  if (e.pdf <= 0.0f) return false;
  s.pdf = e.pdf;
  s.weight = e.total() / e.pdf;
  s.transmission = isReflective(l.kind) == l.flipped;
  return isFiniteColor(s.weight);
}

}  // namespace render
}  // namespace veraa
