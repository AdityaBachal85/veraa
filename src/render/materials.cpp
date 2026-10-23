#include "materials.h"

#include <algorithm>

namespace veraa {
namespace render {

namespace {

constexpr float kDeltaGloss = 0.999f;
constexpr float kBumpScale = 0.5f;  // bump height (scene units) at bump amount 1

// Shading normal on the viewer's side, protected against back-facing bump results.
Vec3 viewerNormal(const ShadeContext& sc, const Vec3& n) {
  Vec3 ngf = dot(sc.ng, sc.wo) >= 0.0f ? sc.ng : -sc.ng;
  Vec3 nf = dot(n, ngf) >= 0.0f ? n : -n;
  if (dot(nf, sc.wo) <= 1e-4f) {
    // Tilt toward the viewer just enough to see the surface.
    Vec3 t = nf + sc.wo * (1e-3f - dot(nf, sc.wo) + 1e-3f);
    nf = length2(t) > 1e-20f ? normalize(t) : ngf;
  }
  return nf;
}

Frame makeFrame(const ShadeContext& sc, const Vec3& nf, float rotationDeg = 0.0f) {
  Vec3 t = sc.dpdu - nf * dot(nf, sc.dpdu);
  Vec3 b;
  if (length2(t) < 1e-20f) {
    makeBasis(nf, t, b);
  } else {
    t = normalize(t);
    b = cross(nf, t);
  }
  if (rotationDeg != 0.0f) {
    float a = radians(rotationDeg), c = std::cos(a), s = std::sin(a);
    Vec3 t2 = t * c + b * s;
    b = cross(nf, t2);
    t = t2;
  }
  return Frame(t, b, nf);
}

Vec3 one() { return Vec3(1.0f); }

}  // namespace

Vec3 subsurfaceSingleScatterAlbedo(const Vec3& A) {
  auto inv = [](float a) {
    a = clampf(a, 0.0f, 0.999f);
    float s = 4.09712f + 4.20863f * a - std::sqrt(9.59217f + 41.6808f * a + 17.7126f * a * a);
    return clampf(1.0f - s * s, 0.0f, 0.9999f);
  };
  return {inv(A.x), inv(A.y), inv(A.z)};
}

void applyBump(ShadeContext& sc, const MapNode* bump, float amount) {
  if (!bump || amount == 0.0f) return;
  if (bump->isNormalMap()) {
    sc.ns = bump->perturbNormal(sc);
    return;
  }
  Vec3 n = sc.ns;
  Vec3 dpdu = sc.dpdu, dpdv = sc.dpdv;
  if (length2(dpdu) < 1e-20f || length2(dpdv) < 1e-20f) {
    // No UVs: build a frame with 1 unit per UV unit so 3D maps still bump.
    makeBasis(n, dpdu, dpdv);
  }
  const float du = 1.0f / 2048.0f, dv = 1.0f / 2048.0f;
  ShadeContext su = sc, sv = sc;
  su.uv.x += du;
  su.p += dpdu * du;
  su.pObj += dpdu * du;
  sv.uv.y += dv;
  sv.p += dpdv * dv;
  sv.pObj += dpdv * dv;
  float h0 = bump->value(sc), hu = bump->value(su), hv = bump->value(sv);
  float scale = amount * kBumpScale;
  Vec3 pu = dpdu + n * ((hu - h0) / du * scale);
  Vec3 pv = dpdv + n * ((hv - h0) / dv * scale);
  Vec3 nb = cross(pu, pv);
  if (length2(nb) < 1e-30f || !std::isfinite(nb.x)) return;
  nb = normalize(nb);
  if (dot(nb, n) < 0.0f) nb = -nb;
  sc.ns = nb;
}

namespace {

// ---------------------------------------------------------------------------------------------
// Plain color (no material assigned)

class ColorMaterial : public ShadeMaterial {
 public:
  explicit ColorMaterial(const Vec3& c) : c_(clampColor(c, 0.0f, 1.0f)) {}
  void shade(ShadeContext& sc, SurfaceShade& out) const override {
    Vec3 nf = viewerNormal(sc, sc.ns);
    Lobe l;
    l.kind = LobeKind::Lambert;
    l.frame = Frame(nf);
    l.weight = c_;
    out.bsdf.add(l);
    out.shadingNormal = nf;
  }

 private:
  Vec3 c_;
};

// ---------------------------------------------------------------------------------------------
// VeraaMtl

class VeraaMtl : public ShadeMaterial {
 public:
  VeraaMtl(const Material& m, MaterialCompileContext& c) {
    const ParamBlock& p = m.params;
    MapCompileContext& mc = c.maps;
    Material up = m;
    up.upgradeLegacyMaps();
    const MapSlots& maps = up.maps;
    diffuse_ = colorInput(p, maps, "diffuse", mc);
    roughness_ = floatInput(p, maps, "roughness", mc);
    reflect_ = colorInput(p, maps, "reflect", mc);
    reflGloss_ = floatInput(p, maps, "reflectGlossiness", mc);
    useRoughness_ = p.getBool("useRoughness");
    fresnel_ = p.getBool("fresnel");
    lockIor_ = p.getBool("lockIor");
    fresnelIor_ = floatInput(p, maps, "fresnelIor", mc);
    metal_ = floatInput(p, maps, "metalness", mc);
    aniso_ = floatInput(p, maps, "anisotropy", mc);
    anisoRot_ = floatInput(p, maps, "anisoRotation", mc);
    refract_ = colorInput(p, maps, "refract", mc);
    refrGloss_ = floatInput(p, maps, "refractGlossiness", mc);
    ior_ = floatInput(p, maps, "ior", mc);
    abbe_ = p.getFloat("abbe");
    fog_ = colorInput(p, maps, "fogColor", mc);
    fogMult_ = p.getFloat("fogMultiplier");
    thin_ = p.getBool("thinWalled");
    affectShadows_ = p.getBool("affectShadows");
    exitOn_ = p.getBool("exitColorOn");
    exitColor_ = p.getColor("exitColor");
    translucency_ = p.getInt("translucency");
    transl_ = colorInput(p, maps, "translucencyColor", mc);
    coatAmount_ = floatInput(p, maps, "coatAmount", mc);
    coatColor_ = colorInput(p, maps, "coatColor", mc);
    coatGloss_ = floatInput(p, maps, "coatGlossiness", mc);
    coatIor_ = std::max(p.getFloat("coatIor"), 1.0001f);
    float cbAmt = 1.0f;
    coatBump_ = slotMap(maps, "coatBump", mc, &cbAmt);
    coatBumpAmount_ = cbAmt * p.getFloat("bumpAmount");
    sheen_ = colorInput(p, maps, "sheenColor", mc);
    sheenGloss_ = floatInput(p, maps, "sheenGlossiness", mc);
    selfIllum_ = colorInput(p, maps, "selfIllum", mc);
    selfMult_ = p.getFloat("selfIllumMultiplier");
    selfGI_ = p.getBool("selfIllumGI");
    opacity_ = floatInput(p, maps, "opacity", mc);
    clip_ = p.getInt("opacityMode") == 1;
    float bAmt = 1.0f;
    bump_ = slotMap(maps, "bump", mc, &bAmt);
    bumpAmount_ = bAmt * p.getFloat("bumpAmount");
    float dAmt = 1.0f;
    disp_ = slotMap(maps, "displacement", mc, &dAmt);
    dispAmount_ = p.getFloat("displacementAmount") * dAmt;
    dispShift_ = p.getFloat("displacementShift");

    hasRefraction_ = refract_.mapped() || maxComponent(refract_.value) > 0.0f;
    hasOpacity_ = opacity_.mapped() || opacity_.value < 1.0f;
    emits_ = selfIllum_.mapped() || maxComponent(selfIllum_.value) > 0.0f;
    // Fog color is the transmittance after 10 scene units (10 cm) at multiplier 1.
    hasFog_ = fog_.mapped() || maxComponent(Vec3(1.0f) - fog_.value) > 0.0f;
  }

  float opacity(const ShadeContext& sc) const override {
    if (!hasOpacity_) return 1.0f;
    float a = opacity_.map ? (opacity_.amount >= 1.0f ? opacity_.map->opacity(sc)
                                                      : opacity_.value + (opacity_.map->opacity(sc) - opacity_.value) * opacity_.amount)
                           : opacity_.value;
    a = clampf(a, 0.0f, 1.0f);
    return clip_ ? (a >= 0.5f ? 1.0f : 0.0f) : a;
  }
  bool hasOpacity() const override { return hasOpacity_; }
  bool shadowTransparent() const override { return hasOpacity_ || (hasRefraction_ && affectShadows_); }
  bool emits() const override { return emits_; }
  bool isRefractiveSolid() const override { return hasRefraction_ && !thin_; }

  Vec3 shadowTransmission(const ShadeContext& sc, float cosAbs) const override {
    float a = opacity(sc);
    Vec3 pass(1.0f - a);
    if (hasRefraction_ && affectShadows_) {
      Vec3 refl = clampColor(reflect_.eval(sc), 0.0f, 1.0f);
      float fior = lockIor_ ? ior_.eval(sc) : fresnelIor_.eval(sc);
      Vec3 F = fresnel_ ? refl * fresnelDielectric(cosAbs, std::max(fior, 1.0001f)) : refl;
      float metal = clampf(metal_.eval(sc), 0.0f, 1.0f);
      pass += (one() - F) * clampColor(refract_.eval(sc), 0.0f, 1.0f) * ((1.0f - metal) * a);
    }
    return pass;
  }

  const MapNode* displacement(float& amount, float& shift) const override {
    amount = dispAmount_;
    shift = dispShift_;
    return disp_.get();
  }

  void shade(ShadeContext& sc, SurfaceShade& out) const override {
    applyBump(sc, bump_.get(), bumpAmount_);
    Vec3 nf = viewerNormal(sc, sc.ns);
    out.shadingNormal = nf;
    float cosO = std::max(dot(nf, sc.wo), 1e-4f);

    Vec3 diffuse = clampColor(diffuse_.eval(sc), 0.0f, 1.0f);
    Vec3 refl = clampColor(reflect_.eval(sc), 0.0f, 1.0f);
    float metal = clampf(metal_.eval(sc), 0.0f, 1.0f);
    float ior = std::max(ior_.eval(sc), 1.0001f);
    float fior = std::max(lockIor_ ? ior : fresnelIor_.eval(sc), 1.0001f);
    Vec3 refr = hasRefraction_ ? clampColor(refract_.eval(sc), 0.0f, 1.0f) * (1.0f - metal) : Vec3(0.0f);
    bool refracts = !isBlack(refr);
    // Only solid refractive objects have an inside; everything else looks the same from both sides.
    bool entering = sc.outside || !refracts || thin_;

    Lobe spec;
    spec.category = LobeCategory::Specular;
    spec.fresnel = FresnelMode::Metalness;
    spec.tint = refl;
    spec.metalF0 = diffuse * refl;
    spec.metal = metal;
    spec.fresnelEta = fresnel_ ? (entering ? fior : 1.0f / fior) : 0.0f;
    spec.eta = spec.fresnelEta;
    Vec3 s0 = spec.fresnelAt(cosO);
    Vec3 rest = vmax(one() - s0, Vec3(0.0f));
    Vec3 wt = rest * refr;
    Vec3 wdNoColor = rest * (one() - refr) * (1.0f - metal);
    if (!entering && refracts && !thin_) wdNoColor = Vec3(0.0f);
    Vec3 wd = wdNoColor * diffuse;

    // ---- Layer weights from coat and sheen (applied to everything below) ----
    Vec3 below(1.0f);
    float coat = clampf(coatAmount_.eval(sc), 0.0f, 1.0f);
    float coatF = 0.0f;
    if (coat > 0.0f && entering) {
      coatF = fresnelDielectric(cosO, coatIor_);
      below = lerp(one(), clampColor(coatColor_.eval(sc), 0.0f, 1.0f), coat) * (1.0f - coat * coatF);
    }
    Vec3 sheen = sheen_.mapped() || maxComponent(sheen_.value) > 0.0f ? clampColor(sheen_.eval(sc), 0.0f, 1.0f) : Vec3(0.0f);
    float sheenAlpha = 0.3f;
    if (!isBlack(sheen)) {
      float r = 1.0f - clampf(sheenGloss_.eval(sc), 0.0f, 1.0f);
      sheenAlpha = clampf(r, 0.07f, 1.0f);
      below *= 1.0f - maxComponent(sheen) * sheenAlbedo(cosO, sheenAlpha);
    }

    // ---- Diffuse / translucency ----
    float rough = clampf(roughness_.eval(sc), 0.0f, 1.0f);
    if (!isBlack(wd)) {
      if (translucency_ == 2) {
        Lobe s;
        s.kind = LobeKind::Subsurface;
        s.category = LobeCategory::Subsurface;
        s.frame = Frame(nf);
        s.weight = wdNoColor * below;
        out.bsdf.add(s);
        out.hasSubsurface = true;
        out.sss.albedo = diffuse;
        out.sss.radius = vmax(clampColor(transl_.eval(sc), 0.0f, 1e6f), Vec3(1e-3f)) * (1.0f / std::max(fogMult_, 1e-3f));
        out.sss.ior = ior;
      } else {
        Vec3 t = translucency_ == 1 ? clampColor(transl_.eval(sc), 0.0f, 1.0f) * 0.5f : Vec3(0.0f);
        Lobe d;
        d.kind = rough > 0.0f ? LobeKind::OrenNayar : LobeKind::Lambert;
        d.frame = Frame(nf);
        d.weight = wd * (one() - t) * below;
        if (rough > 0.0f) {
          float sigma = rough * 0.5f, s2 = sigma * sigma;
          d.orenA = 1.0f - 0.5f * s2 / (s2 + 0.33f);
          d.orenB = 0.45f * s2 / (s2 + 0.09f);
        }
        out.bsdf.add(d);
        if (!isBlack(t)) {
          Lobe tr;
          tr.kind = LobeKind::DiffuseTransmission;
          tr.category = LobeCategory::Transmission;
          tr.frame = Frame(nf);
          tr.weight = wd * t * below;
          out.bsdf.add(tr);
        }
      }
    }

    // ---- Reflection ----
    float gloss = reflGloss_.eval(sc);
    bool deltaS = useRoughness_ ? gloss <= 1.0f - kDeltaGloss : gloss >= kDeltaGloss;
    float aniso = clampf(aniso_.eval(sc), -0.99f, 0.99f);
    if (maxComponent(s0) > 0.0f || metal > 0.0f) {
      spec.frame = aniso != 0.0f ? makeFrame(sc, nf, anisoRot_.eval(sc)) : Frame(nf);
      spec.weight = below;
      if (deltaS) {
        spec.kind = LobeKind::SpecularReflect;
      } else {
        spec.kind = LobeKind::GgxReflect;
        float a = glossToAlpha(gloss, useRoughness_);
        float aspect = std::sqrt(1.0f - 0.9f * std::fabs(aniso));
        spec.ax = aniso >= 0.0f ? a / aspect : a * aspect;
        spec.ay = aniso >= 0.0f ? a * aspect : a / aspect;
        spec.ax = std::max(spec.ax, 0.0015f);
        spec.ay = std::max(spec.ay, 0.0015f);
      }
      out.bsdf.add(spec);
    }

    // ---- Refraction ----
    if (refracts) {
      Lobe t;
      t.category = LobeCategory::Transmission;
      t.frame = Frame(nf);
      if (thin_) {
        t.kind = LobeKind::ThinTransmit;
        t.weight = wt * below;
      } else {
        float etaT = entering ? ior : 1.0f / ior;
        t.eta = etaT;
        t.fresnel = FresnelMode::Metalness;
        t.tint = refl;
        t.metalF0 = spec.metalF0;
        t.metal = 0.0f;
        t.fresnelEta = spec.fresnelEta;
        if (abbe_ > 0.0f) {
          // Cauchy fit through the d line with the given Abbe number.
          float dn = (ior - 1.0f) / abbe_;
          float B = dn / (1.0f / (486.1f * 486.1f) - 1.0f / (656.3f * 656.3f));
          float A = ior - B / (587.6f * 587.6f);
          Vec3 n(A + B / (610.0f * 610.0f), A + B / (550.0f * 550.0f), A + B / (465.0f * 465.0f));
          t.etaRgb = entering ? n : Vec3(1.0f / n.x, 1.0f / n.y, 1.0f / n.z);
        }
        float rg = refrGloss_.eval(sc);
        bool deltaT = useRoughness_ ? rg <= 1.0f - kDeltaGloss : rg >= kDeltaGloss;
        if (deltaT) {
          t.kind = LobeKind::SpecularRefract;
          t.weight = wt * below;
        } else {
          t.kind = LobeKind::GgxRefract;
          t.ax = t.ay = glossToAlpha(rg, useRoughness_);
          t.weight = refr * below;
        }
      }
      out.bsdf.add(t);
      out.thin = thin_;
      if (!thin_ && hasFog_) {
        Vec3 fog = clampColor(fog_.eval(sc), 1e-4f, 1.0f);
        out.absorption = Vec3(-std::log(fog.x), -std::log(fog.y), -std::log(fog.z)) * (fogMult_ / 10.0f);
      }
      out.hasExitColor = exitOn_;
      out.exitColor = exitColor_;
    }

    // ---- Sheen ----
    if (!isBlack(sheen)) {
      Lobe sh;
      sh.kind = LobeKind::Sheen;
      sh.category = LobeCategory::Specular;
      sh.frame = Frame(nf);
      sh.ax = sh.ay = sheenAlpha;
      sh.weight = sheen * (coat > 0.0f ? (1.0f - coat * coatF) : 1.0f);
      out.bsdf.add(sh);
    }

    // ---- Coat ----
    if (coat > 0.0f && entering) {
      Vec3 cn = nf;
      if (coatBump_) {
        ShadeContext cs = sc;
        cs.ns = sc.ns;
        applyBump(cs, coatBump_.get(), coatBumpAmount_);
        cn = viewerNormal(sc, cs.ns);
      }
      Lobe c;
      c.category = LobeCategory::Specular;
      c.frame = Frame(cn);
      c.fresnel = FresnelMode::Dielectric;
      c.tint = one();
      c.fresnelEta = c.eta = coatIor_;
      c.weight = Vec3(coat);
      float cg = coatGloss_.eval(sc);
      if (cg >= kDeltaGloss) {
        c.kind = LobeKind::SpecularReflect;
      } else {
        c.kind = LobeKind::GgxReflect;
        c.ax = c.ay = glossToAlpha(cg);
      }
      out.bsdf.add(c);
    }

    if (emits_ && sc.outside) {
      out.emission = selfIllum_.eval(sc) * selfMult_;
      out.emissionAffectsGI = selfGI_;
    }
  }

 private:
  ColorInput diffuse_, reflect_, refract_, fog_, transl_, coatColor_, sheen_, selfIllum_;
  FloatInput roughness_, reflGloss_, fresnelIor_, metal_, aniso_, anisoRot_, refrGloss_, ior_, coatAmount_, coatGloss_,
      sheenGloss_, opacity_;
  MapPtr bump_, coatBump_, disp_;
  float bumpAmount_ = 1, coatBumpAmount_ = 1, dispAmount_ = 0, dispShift_ = 0;
  float abbe_ = 0, fogMult_ = 1, selfMult_ = 1, coatIor_ = 1.6f;
  bool useRoughness_ = false, fresnel_ = true, lockIor_ = false, thin_ = false, affectShadows_ = true;
  bool exitOn_ = false, selfGI_ = true, clip_ = false;
  int translucency_ = 0;
  Vec3 exitColor_;
  bool hasRefraction_ = false, hasOpacity_ = false, emits_ = false, hasFog_ = false;
};

// ---------------------------------------------------------------------------------------------
// VeraaLightMtl

class LightMtl : public ShadeMaterial {
 public:
  LightMtl(const Material& m, MaterialCompileContext& c) {
    const ParamBlock& p = m.params;
    color_ = colorInput(p, m.maps, "color", c.maps);
    mult_ = p.getFloat("multiplier");
    if (p.getBool("compensateExposure")) mult_ *= c.exposureCompensation;
    opacity_ = floatInput(p, m.maps, "opacity", c.maps);
    twoSided_ = p.getBool("twoSided");
    direct_ = p.getBool("directIllumination");
    hasOpacity_ = opacity_.mapped() || opacity_.value < 1.0f;
  }
  float opacity(const ShadeContext& sc) const override {
    if (!hasOpacity_) return 1.0f;
    float a = opacity_.map ? opacity_.map->opacity(sc) : opacity_.value;
    return clampf(a, 0.0f, 1.0f);
  }
  bool hasOpacity() const override { return hasOpacity_; }
  bool emits() const override { return true; }
  bool isMeshLight() const override { return direct_; }
  void shade(ShadeContext& sc, SurfaceShade& out) const override {
    out.shadingNormal = viewerNormal(sc, sc.ns);
    if (sc.outside || twoSided_) out.emission = color_.eval(sc) * mult_;
  }
  // Radiance emitted at a point (mesh light sampling).
  Vec3 radiance(const ShadeContext& sc) const { return (sc.outside || twoSided_) ? color_.eval(sc) * mult_ : Vec3(0.0f); }

 private:
  ColorInput color_;
  FloatInput opacity_;
  float mult_ = 1.0f;
  bool twoSided_ = false, direct_ = false, hasOpacity_ = false;
};

// ---------------------------------------------------------------------------------------------
// Compound materials

std::unique_ptr<ShadeMaterial> compileSub(const SubMaterialSlot& s, MaterialCompileContext& c) {
  return s.active() ? compileMaterial(*s.material, c) : nullptr;
}

class BlendMtl : public ShadeMaterial {
 public:
  BlendMtl(const Material& m, MaterialCompileContext& c) {
    if (!m.subs.empty()) base_ = compileSub(m.subs[0], c);
    if (!base_) base_ = compileColorMaterial(Vec3(0.5f));
    additive_ = m.params.getBool("additive");
    for (size_t i = 1; i < m.subs.size(); ++i) {
      auto sub = compileSub(m.subs[i], c);
      if (!sub) continue;
      coats_.push_back({std::move(sub), colorInput(m.params, m.maps, "blend" + std::to_string(i), c.maps)});
    }
  }
  float opacity(const ShadeContext& sc) const override {
    float a = base_->resolve(sc)->opacity(sc);
    for (const auto& co : coats_) {
      float b = clampf(luminance(co.blend.eval(sc)), 0.0f, 1.0f);
      a = a + (co.mtl->resolve(sc)->opacity(sc) - a) * b;
    }
    return a;
  }
  bool hasOpacity() const override {
    bool any = base_->hasOpacity();
    for (const auto& co : coats_) any = any || co.mtl->hasOpacity();
    return any;
  }
  bool shadowTransparent() const override {
    bool any = base_->shadowTransparent();
    for (const auto& co : coats_) any = any || co.mtl->shadowTransparent();
    return any;
  }
  Vec3 shadowTransmission(const ShadeContext& sc, float cosAbs) const override {
    Vec3 t = base_->resolve(sc)->shadowTransmission(sc, cosAbs);
    for (const auto& co : coats_) {
      Vec3 b = clampColor(co.blend.eval(sc), 0.0f, 1.0f);
      Vec3 ct = co.mtl->resolve(sc)->shadowTransmission(sc, cosAbs);
      t = additive_ ? t : t * (one() - b) + ct * b;
    }
    return t;
  }
  bool emits() const override {
    bool any = base_->emits();
    for (const auto& co : coats_) any = any || co.mtl->emits();
    return any;
  }
  void shade(ShadeContext& sc, SurfaceShade& out) const override {
    ShadeContext s0 = sc;
    base_->resolve(sc)->shade(s0, out);
    for (const auto& co : coats_) {
      Vec3 b = clampColor(co.blend.eval(sc), 0.0f, 1.0f);
      if (isBlack(b)) continue;
      SurfaceShade cs;
      ShadeContext s1 = sc;
      co.mtl->resolve(sc)->shade(s1, cs);
      if (!additive_) {
        out.bsdf.scale(one() - b);
        out.emission *= one() - b;
      }
      out.bsdf.append(cs.bsdf, b);
      out.emission += cs.emission * b;
      if (cs.hasSubsurface && !out.hasSubsurface) {
        out.hasSubsurface = true;
        out.sss = cs.sss;
      }
      if (luminance(b) > 0.5f) {
        if (!isBlack(cs.absorption)) out.absorption = cs.absorption;
        out.thin = cs.thin;
      }
    }
    sc.ns = s0.ns;
  }

 private:
  struct Coat {
    std::unique_ptr<ShadeMaterial> mtl;
    ColorInput blend;
  };
  std::unique_ptr<ShadeMaterial> base_;
  std::vector<Coat> coats_;
  bool additive_ = false;
};

class MultiSubMtl : public ShadeMaterial {
 public:
  MultiSubMtl(const Material& m, MaterialCompileContext& c) {
    for (const auto& s : m.subs) {
      Entry e;
      e.id = s.matId;
      e.mtl = compileSub(s, c);
      entries_.push_back(std::move(e));
    }
    fallback_ = compileColorMaterial(Vec3(0.5f));
  }
  const ShadeMaterial* resolve(const ShadeContext& sc) const override {
    if (entries_.empty()) return fallback_.get();
    int id = sc.faceMatId + 1;
    const ShadeMaterial* r = nullptr;
    bool found = false;
    for (const auto& e : entries_)
      if (e.id == id) {
        r = e.mtl.get();
        found = true;
        break;
      }
    if (!found) {
      int n = static_cast<int>(entries_.size());
      r = entries_[((sc.faceMatId % n) + n) % n].mtl.get();
    }
    return r ? r->resolve(sc) : fallback_.get();
  }
  bool hasOpacity() const override {
    for (const auto& e : entries_)
      if (e.mtl && e.mtl->hasOpacity()) return true;
    return false;
  }
  bool shadowTransparent() const override {
    for (const auto& e : entries_)
      if (e.mtl && e.mtl->shadowTransparent()) return true;
    return false;
  }
  bool emits() const override {
    for (const auto& e : entries_)
      if (e.mtl && e.mtl->emits()) return true;
    return false;
  }
  float opacity(const ShadeContext& sc) const override { return resolve(sc)->opacity(sc); }
  Vec3 shadowTransmission(const ShadeContext& sc, float cosAbs) const override {
    return resolve(sc)->shadowTransmission(sc, cosAbs);
  }
  void shade(ShadeContext& sc, SurfaceShade& out) const override { resolve(sc)->shade(sc, out); }

 private:
  struct Entry {
    int id = 1;
    std::unique_ptr<ShadeMaterial> mtl;
  };
  std::vector<Entry> entries_;
  std::unique_ptr<ShadeMaterial> fallback_;
};

class TwoSidedMtl : public ShadeMaterial {
 public:
  TwoSidedMtl(const Material& m, MaterialCompileContext& c) {
    if (m.subs.size() > 0) front_ = compileSub(m.subs[0], c);
    if (m.subs.size() > 1) back_ = compileSub(m.subs[1], c);
    if (!front_) front_ = compileColorMaterial(Vec3(0.5f));
    transl_ = colorInput(m.params, m.maps, "translucency", c.maps);
  }
  float opacity(const ShadeContext& sc) const override { return front_->resolve(sc)->opacity(sc); }
  bool hasOpacity() const override { return front_->hasOpacity(); }
  bool emits() const override { return front_->emits(); }
  // Light through the sheet is traced as diffuse transmission, so shadows only see cut-outs.
  bool shadowTransparent() const override { return front_->hasOpacity(); }
  void shade(ShadeContext& sc, SurfaceShade& out) const override {
    Vec3 t = clampColor(transl_.eval(sc), 0.0f, 1.0f);
    ShadeContext sf = sc;
    front_->resolve(sc)->shade(sf, out);
    out.bsdf.scale(one() - t);
    out.emission *= one() - t;
    if (!isBlack(t)) {
      SurfaceShade bs;
      ShadeContext sb = sc;
      const ShadeMaterial* back = back_ ? back_.get() : front_.get();
      back->resolve(sc)->shade(sb, bs);
      out.bsdf.append(bs.bsdf, t, true);
    }
    // Both sides are surfaces of a thin sheet: no inside.
    out.absorption = Vec3(0.0f);
    out.thin = true;
    sc.ns = sf.ns;
  }

 private:
  std::unique_ptr<ShadeMaterial> front_, back_;
  ColorInput transl_;
};

class OverrideMtl : public ShadeMaterial {
 public:
  OverrideMtl(const Material& m, MaterialCompileContext& c) {
    for (size_t i = 0; i < 5; ++i) subs_[i] = i < m.subs.size() ? compileSub(m.subs[i], c) : nullptr;
    if (!subs_[0]) subs_[0] = compileColorMaterial(Vec3(0.5f));
  }
  const ShadeMaterial* pick(RayKind k) const {
    int i = 0;
    switch (k) {
      case RayKind::Diffuse: i = 1; break;
      case RayKind::Reflect: i = 2; break;
      case RayKind::Refract: i = 3; break;
      case RayKind::Shadow: i = 4; break;
      default: i = 0; break;
    }
    return subs_[i] ? subs_[i].get() : subs_[0].get();
  }
  const ShadeMaterial* resolve(const ShadeContext& sc) const override { return pick(sc.rayKind)->resolve(sc); }
  float opacity(const ShadeContext& sc) const override { return resolve(sc)->opacity(sc); }
  bool hasOpacity() const override {
    for (const auto& s : subs_)
      if (s && s->hasOpacity()) return true;
    return false;
  }
  bool shadowTransparent() const override {
    const ShadeMaterial* s = subs_[4] ? subs_[4].get() : subs_[0].get();
    return s->shadowTransparent() || hasOpacity();
  }
  Vec3 shadowTransmission(const ShadeContext& sc, float cosAbs) const override {
    ShadeContext s = sc;
    s.rayKind = RayKind::Shadow;
    return pick(RayKind::Shadow)->resolve(s)->shadowTransmission(s, cosAbs);
  }
  bool emits() const override { return subs_[0]->emits(); }
  void shade(ShadeContext& sc, SurfaceShade& out) const override { resolve(sc)->shade(sc, out); }

 private:
  std::unique_ptr<ShadeMaterial> subs_[5];
};

class WrapperMtl : public ShadeMaterial {
 public:
  WrapperMtl(const Material& m, MaterialCompileContext& c) {
    if (!m.subs.empty()) base_ = compileSub(m.subs[0], c);
    if (!base_) base_ = compileColorMaterial(Vec3(0.5f));
    const ParamBlock& p = m.params;
    generateGI_ = p.getFloat("generateGI");
    receiveGI_ = p.getFloat("receiveGI");
    matte_.matte = p.getBool("matteSurface");
    matte_.alphaContribution = p.getFloat("alphaContribution");
    matte_.shadows = p.getBool("shadows");
    matte_.shadowsAffectAlpha = p.getBool("shadowsAffectAlpha");
    matte_.shadowColor = p.getColor("shadowColor");
    matte_.shadowBrightness = p.getFloat("shadowBrightness");
    matte_.reflectionAmount = p.getFloat("reflectionAmount");
  }
  float opacity(const ShadeContext& sc) const override { return base_->resolve(sc)->opacity(sc); }
  bool hasOpacity() const override { return base_->hasOpacity(); }
  bool shadowTransparent() const override { return base_->shadowTransparent(); }
  Vec3 shadowTransmission(const ShadeContext& sc, float cosAbs) const override {
    return base_->resolve(sc)->shadowTransmission(sc, cosAbs);
  }
  bool emits() const override { return base_->emits(); }
  void shade(ShadeContext& sc, SurfaceShade& out) const override {
    base_->resolve(sc)->shade(sc, out);
    out.generateGI = generateGI_;
    out.receiveGI = receiveGI_;
    out.matte = &matte_;
  }

 private:
  std::unique_ptr<ShadeMaterial> base_;
  float generateGI_ = 1, receiveGI_ = 1;
  MatteParams matte_;
};

// ---------------------------------------------------------------------------------------------
// VeraaCarPaintMtl

class CarPaintMtl : public ShadeMaterial {
 public:
  CarPaintMtl(const Material& m, MaterialCompileContext& c) {
    const ParamBlock& p = m.params;
    MapCompileContext& mc = c.maps;
    base_ = colorInput(p, m.maps, "baseColor", mc);
    baseRefl_ = floatInput(p, m.maps, "baseReflection", mc);
    baseGloss_ = floatInput(p, m.maps, "baseGlossiness", mc);
    flake_ = colorInput(p, m.maps, "flakeColor", mc);
    flakeGloss_ = p.getFloat("flakeGlossiness");
    flakeOrient_ = p.getFloat("flakeOrientation");
    flakeDensity_ = p.getFloat("flakeDensity");
    flakeSize_ = std::max(p.getFloat("flakeSize"), 1e-4f);
    seed_ = static_cast<uint32_t>(p.getInt("flakeSeed"));
    coatColor_ = colorInput(p, m.maps, "coatColor", mc);
    coatStrength_ = floatInput(p, m.maps, "coatStrength", mc);
    coatGloss_ = floatInput(p, m.maps, "coatGlossiness", mc);
    float bAmt = 1.0f;
    bump_ = slotMap(m.maps, "bump", mc, &bAmt);
    bumpAmount_ = bAmt * p.getFloat("bumpAmount");
  }
  void shade(ShadeContext& sc, SurfaceShade& out) const override {
    Vec3 ngeo = sc.ns;  // the clear coat stays smooth
    applyBump(sc, bump_.get(), bumpAmount_);
    Vec3 nf = viewerNormal(sc, sc.ns);
    Vec3 nc = viewerNormal(sc, ngeo);
    out.shadingNormal = nc;
    float cosO = std::max(dot(nc, sc.wo), 1e-4f);
    // Clear coat on top.
    float cs = clampf(coatStrength_.eval(sc), 0.0f, 1.0f);
    float Fc = cs + (1.0f - cs) * std::pow(1.0f - cosO, 5.0f);
    Vec3 coatTint = clampColor(coatColor_.eval(sc), 0.0f, 1.0f);
    Vec3 below = coatTint * (1.0f - Fc);
    {
      Lobe c;
      c.category = LobeCategory::Specular;
      c.frame = Frame(nc);
      c.fresnel = FresnelMode::Schlick;
      c.tint = Vec3(cs);
      float g = coatGloss_.eval(sc);
      if (g >= kDeltaGloss) {
        c.kind = LobeKind::SpecularReflect;
      } else {
        c.kind = LobeKind::GgxReflect;
        c.ax = c.ay = glossToAlpha(g);
      }
      out.bsdf.add(c);
    }
    // Flakes: a 3D cell pattern in object space; each cell may hold one tilted mirror flake.
    Vec3 q = sc.pObj / flakeSize_;
    int cx = static_cast<int>(std::floor(q.x)), cy = static_cast<int>(std::floor(q.y)), cz = static_cast<int>(std::floor(q.z));
    uint32_t h = hashU32(static_cast<uint32_t>(cx) * 73856093u ^ static_cast<uint32_t>(cy) * 19349663u ^
                         static_cast<uint32_t>(cz) * 83492791u ^ (seed_ * 0x9E3779B9u));
    float coverage = clampf(flakeDensity_ * 0.5f, 0.0f, 1.0f);
    bool hasFlake = hashFloat(h) < coverage;
    if (hasFlake) {
      Vec3 rnd(hashFloat(h + 1) * 2.0f - 1.0f, hashFloat(h + 2) * 2.0f - 1.0f, hashFloat(h + 3) * 2.0f - 1.0f);
      Vec3 fn = normalize(nf + rnd * flakeOrient_);
      if (dot(fn, sc.wo) <= 0.05f) fn = nf;
      Lobe f;
      f.kind = LobeKind::GgxReflect;
      f.category = LobeCategory::Specular;
      f.frame = Frame(fn);
      f.fresnel = FresnelMode::None;
      f.tint = clampColor(flake_.eval(sc), 0.0f, 1.0f);
      f.ax = f.ay = glossToAlpha(flakeGloss_);
      f.weight = below * 0.7f;
      out.bsdf.add(f);
      below *= 0.5f;
    }
    // Base: diffuse with a glossy dielectric reflection.
    Vec3 base = clampColor(base_.eval(sc), 0.0f, 1.0f);
    float br = clampf(baseRefl_.eval(sc), 0.0f, 1.0f);
    Lobe s;
    s.kind = LobeKind::GgxReflect;
    s.category = LobeCategory::Specular;
    s.frame = Frame(nf);
    s.fresnel = FresnelMode::Dielectric;
    s.tint = Vec3(br);
    s.fresnelEta = s.eta = 1.5f;
    s.ax = s.ay = glossToAlpha(baseGloss_.eval(sc));
    s.weight = below;
    Vec3 s0 = s.fresnelAt(std::max(dot(nf, sc.wo), 1e-4f));
    out.bsdf.add(s);
    Lobe d;
    d.kind = LobeKind::Lambert;
    d.frame = Frame(nf);
    d.weight = base * (one() - s0) * below;
    out.bsdf.add(d);
  }

 private:
  ColorInput base_, flake_, coatColor_;
  FloatInput baseRefl_, baseGloss_, coatStrength_, coatGloss_;
  float flakeGloss_ = 0.8f, flakeOrient_ = 0.3f, flakeDensity_ = 0.5f, flakeSize_ = 0.05f;
  uint32_t seed_ = 1;
  MapPtr bump_;
  float bumpAmount_ = 1.0f;
};

// ---------------------------------------------------------------------------------------------
// VeraaSSSMtl

class SssMtl : public ShadeMaterial {
 public:
  SssMtl(const Material& m, MaterialCompileContext& c) {
    const ParamBlock& p = m.params;
    MapCompileContext& mc = c.maps;
    sssColor_ = colorInput(p, m.maps, "sssColor", mc);
    radiusColor_ = colorInput(p, m.maps, "radiusColor", mc);
    radius_ = p.getFloat("scatterRadius");
    phase_ = p.getFloat("phase");
    ior_ = std::max(p.getFloat("ior"), 1.0001f);
    diffColor_ = colorInput(p, m.maps, "diffuseColor", mc);
    diffAmount_ = floatInput(p, m.maps, "diffuseAmount", mc);
    specColor_ = colorInput(p, m.maps, "specularColor", mc);
    specAmount_ = p.getFloat("specularAmount");
    specGloss_ = floatInput(p, m.maps, "specularGlossiness", mc);
    float bAmt = 1.0f;
    bump_ = slotMap(m.maps, "bump", mc, &bAmt);
    bumpAmount_ = bAmt * p.getFloat("bumpAmount");
  }
  void shade(ShadeContext& sc, SurfaceShade& out) const override {
    applyBump(sc, bump_.get(), bumpAmount_);
    Vec3 nf = viewerNormal(sc, sc.ns);
    out.shadingNormal = nf;
    float cosO = std::max(dot(nf, sc.wo), 1e-4f);
    Lobe s;
    s.category = LobeCategory::Specular;
    s.frame = Frame(nf);
    s.fresnel = FresnelMode::Dielectric;
    s.tint = clampColor(specColor_.eval(sc), 0.0f, 1.0f) * specAmount_;
    s.fresnelEta = s.eta = sc.outside ? ior_ : 1.0f / ior_;
    float g = specGloss_.eval(sc);
    if (g >= kDeltaGloss) {
      s.kind = LobeKind::SpecularReflect;
    } else {
      s.kind = LobeKind::GgxReflect;
      s.ax = s.ay = glossToAlpha(g);
    }
    Vec3 s0 = s.fresnelAt(cosO);
    if (maxComponent(s.tint) > 0.0f) out.bsdf.add(s);
    Vec3 rest = vmax(one() - s0, Vec3(0.0f));
    float da = clampf(diffAmount_.eval(sc), 0.0f, 1.0f);
    if (da > 0.0f) {
      Lobe d;
      d.kind = LobeKind::Lambert;
      d.frame = Frame(nf);
      d.weight = rest * clampColor(diffColor_.eval(sc), 0.0f, 1.0f) * da;
      out.bsdf.add(d);
    }
    if (da < 1.0f && sc.outside) {
      Lobe ss;
      ss.kind = LobeKind::Subsurface;
      ss.category = LobeCategory::Subsurface;
      ss.frame = Frame(nf);
      ss.weight = rest * (1.0f - da);
      out.bsdf.add(ss);
      out.hasSubsurface = true;
      out.sss.albedo = clampColor(sssColor_.eval(sc), 0.0f, 1.0f);
      out.sss.radius = vmax(clampColor(radiusColor_.eval(sc), 0.0f, 1e6f) * radius_, Vec3(1e-4f));
      out.sss.anisotropy = phase_;
      out.sss.ior = ior_;
    } else if (da < 1.0f) {
      // Seen from inside (e.g. a thin shell): fall back to a diffuse look.
      Lobe d;
      d.kind = LobeKind::Lambert;
      d.frame = Frame(nf);
      d.weight = rest * clampColor(sssColor_.eval(sc), 0.0f, 1.0f) * (1.0f - da);
      out.bsdf.add(d);
    }
  }

 private:
  ColorInput sssColor_, radiusColor_, diffColor_, specColor_;
  FloatInput diffAmount_, specGloss_;
  float radius_ = 1, phase_ = 0, ior_ = 1.4f, specAmount_ = 1;
  MapPtr bump_;
  float bumpAmount_ = 1.0f;
};

}  // namespace

std::unique_ptr<ShadeMaterial> compileColorMaterial(const Vec3& diffuse) { return std::make_unique<ColorMaterial>(diffuse); }

std::unique_ptr<ShadeMaterial> compileMaterial(const Material& m, MaterialCompileContext& c) {
  const std::string& t = m.materialType;
  if (t == "VeraaBlendMtl") return std::make_unique<BlendMtl>(m, c);
  if (t == "MultiSubObject") return std::make_unique<MultiSubMtl>(m, c);
  if (t == "VeraaLightMtl") return std::make_unique<LightMtl>(m, c);
  if (t == "VeraaTwoSidedMtl") return std::make_unique<TwoSidedMtl>(m, c);
  if (t == "VeraaCarPaintMtl") return std::make_unique<CarPaintMtl>(m, c);
  if (t == "VeraaSSSMtl") return std::make_unique<SssMtl>(m, c);
  if (t == "VeraaOverrideMtl") return std::make_unique<OverrideMtl>(m, c);
  if (t == "VeraaWrapperMtl") return std::make_unique<WrapperMtl>(m, c);
  return std::make_unique<VeraaMtl>(m, c);
}

}  // namespace render
}  // namespace veraa
