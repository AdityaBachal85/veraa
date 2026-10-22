#include "bsdf.h"

#include <vector>

namespace veraa {
namespace render {

namespace {

constexpr float kMinAlpha = 0.0015f;

float ggxG1(const Vec3& w, float ax, float ay) { return 1.0f / (1.0f + ggxLambda(w, ax, ay)); }
float ggxG2(const Vec3& wo, const Vec3& wi, float ax, float ay) {
  return 1.0f / (1.0f + ggxLambda(wo, ax, ay) + ggxLambda(wi, ax, ay));
}

// "Charlie" sheen distribution (Estevez & Kulla 2017) with the Ashikhmin visibility term.
float charlieD(float cosH, float alpha) {
  float inv = 1.0f / alpha;
  float sin2 = std::max(0.0f, 1.0f - cosH * cosH);
  return (2.0f + inv) * std::pow(sin2, inv * 0.5f) / kTwoPi;
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
            float D = charlieD(h.z, a);
            float V = 1.0f / (4.0f * (wi.z + wo.z - wi.z * wo.z));
            sum += D * V * wi.z * std::sin(th) * (kPi * 0.5f / nT) * (kTwoPi / nP);
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

Vec3 Lobe::fresnelAt(float c) const {
  switch (fresnel) {
    case FresnelMode::None: return tint;
    case FresnelMode::Dielectric: return tint * (fresnelEta > 0.0f ? fresnelDielectric(c, fresnelEta) : 1.0f);
    case FresnelMode::Schlick: return schlick(tint, c);
    case FresnelMode::Metalness: {
      Vec3 diel = tint * (fresnelEta > 0.0f ? fresnelDielectric(c, fresnelEta) : 1.0f);
      return metal > 0.0f ? lerp(diel, schlick(metalF0, c), metal) : diel;
    }
  }
  return tint;
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

void Bsdf::finalize(const Vec3& wo) {
  float est[kMaxLobes];
  float sum = 0.0f, diffuseLike = 0.0f;
  albedo_ = Vec3(0.0f);
  nonDelta_ = false;
  Lobe* L = lobes();
  for (int i = 0; i < count_; ++i) {
    Lobe& l = L[i];
    Vec3 woL = clampUpper(l.frame.toLocal(wo));
    l.woL = woL;
    Vec3 a = l.weight;
    switch (l.kind) {
      case LobeKind::Sheen: a *= sheenAlbedo(woL.z, l.ax); break;
      case LobeKind::GgxReflect:
      case LobeKind::SpecularReflect: a *= l.fresnelAt(woL.z); break;
      case LobeKind::GgxRefract: a *= vmax(Vec3(1.0f) - l.fresnelAt(woL.z), Vec3(0.0f)); break;
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
      float D = charlieD(h.z, std::max(l.ax, 0.02f));
      float V = 1.0f / (4.0f * (cI + cO - cI * cO));
      pdf = cI * kInvPi;
      return l.weight * (D * V * cI);
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
