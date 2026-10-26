// Path tracer: next-event estimation with multiple importance sampling, render element and
// Light Mix bookkeeping, homogeneous/height fog, random-walk subsurface scattering, matte
// surfaces and light linking.
#include <algorithm>

#include "render_impl.h"

namespace veraa {

using namespace render;

namespace {

constexpr int kMaxPassThrough = 64;   // cut-out (opacity) crossings per path
constexpr int kMaxShadowLayers = 24;  // transparent surfaces crossed by one shadow ray
constexpr int kMaxWalkSteps = 256;
constexpr int kMaxGuideVertices = 64;  // path vertices whose radiance trains the guide
constexpr float kMinGuideSelect = 0.1f;   // learned guide selection below which a trained region stops guiding
constexpr float kGlossyFilterScale = 0.4f;  // minimum alpha after a diffuse bounce at glossy filter 1

// Roughness of a lobe for the glossy filter (1 = diffuse), 0 for delta lobes.
float lobeRoughness(const Lobe& l) {
  switch (l.kind) {
    case LobeKind::Lambert:
    case LobeKind::OrenNayar:
    case LobeKind::DiffuseTransmission:
    case LobeKind::Sheen: return 1.0f;
    case LobeKind::GgxReflect:
    case LobeKind::GgxRefract: return std::sqrt(l.ax * l.ay);
    default: return 0.0f;
  }
}

// How much a lobe benefits from guided directions: fully for diffuse-like lobes, fading out for
// glossy lobes narrower than the learned distributions resolve, not at all for delta lobes.
float lobeGuidability(const Lobe& l) {
  float r = lobeRoughness(l);
  return r >= 1.0f ? 1.0f : clampf((r - 0.2f) / 0.3f, 0.0f, 1.0f);
}

// Path guiding at one vertex: directions come from the learned distribution with probability
// prob and from the BSDF otherwise (one-sample MIS with the balance heuristic).
struct GuideContext {
  const PathGuide::Region* region = nullptr;
  float prob = 0.0f;
  // Density of the combined strategy for a direction the BSDF samples with density bsdfPdf.
  float mix(float bsdfPdf, const Vec3& wi) const {
    return prob > 0.0f ? (1.0f - prob) * bsdfPdf + prob * PathGuide::pdf(region, wi) : bsdfPdf;
  }
};

inline Vec3 vexp(const Vec3& v) { return {std::exp(v.x), std::exp(v.y), std::exp(v.z)}; }

float hgPhase(float cosT, float g) {
  float g2 = g * g;
  float d = 1.0f + g2 - 2.0f * g * cosT;
  return (1.0f - g2) / (4.0f * kPi * d * std::sqrt(std::max(d, 1e-8f)));
}

// Samples a direction around w with the Henyey-Greenstein phase function.
Vec3 sampleHg(const Vec3& w, float g, float u1, float u2) {
  float cosT;
  if (std::fabs(g) < 1e-3f) {
    cosT = 1.0f - 2.0f * u1;
  } else {
    float s = (1.0f - g * g) / (1.0f - g + 2.0f * g * u1);
    cosT = (1.0f + g * g - s * s) / (2.0f * g);
  }
  cosT = clampf(cosT, -1.0f, 1.0f);
  float sinT = std::sqrt(std::max(0.0f, 1.0f - cosT * cosT));
  float phi = kTwoPi * u2;
  Frame f(w);
  return normalize(f.toWorld(Vec3(std::cos(phi) * sinT, std::sin(phi) * sinT, cosT)));
}

int directElement(int cat, bool delta) {
  switch (cat) {
    case 0: return kElemLighting;
    case 1: return delta ? kElemReflection : kElemSpecular;
    case 2: return kElemRefraction;
    case 3: return kElemSSS;
    case -2: return kElemAtmosphere;
    default: return kElemLighting;
  }
}

int indirectElement(int cat) {
  switch (cat) {
    case 0: return kElemGI;
    case 1: return kElemReflection;
    case 2: return kElemRefraction;
    case 3: return kElemSSS;
    case -2: return kElemAtmosphere;
    default: return kElemGI;
  }
}

Vec2 sampleAperture(float u1, float u2, int blades, float rotation) {
  if (blades < 3) return sampleConcentricDisk(u1, u2);
  float sector = u1 * blades;
  int i = std::min(static_cast<int>(sector), blades - 1);
  float u = sector - i;
  float a0 = rotation + kTwoPi * i / blades, a1 = rotation + kTwoPi * (i + 1) / blades;
  Vec2 p0(std::cos(a0), std::sin(a0)), p1(std::cos(a1), std::sin(a1));
  float su = std::sqrt(u);
  float b1 = su * (1.0f - u2), b2 = su * u2;
  return p0 * b1 + p1 * b2;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Rays and surfaces

bool Renderer::Impl::intersect(const Vec3& o, const Vec3& d, float tNear, float tFar, unsigned mask, float time,
                               RTCRayHit& rh, int& planeHit) const {
  planeHit = -1;
  Vec3 org = o;
  float base = 0.0f;
  for (int attempt = 0; attempt < 64; ++attempt) {
    rh.ray.org_x = org.x;
    rh.ray.org_y = org.y;
    rh.ray.org_z = org.z;
    rh.ray.tnear = std::max(tNear - base, 0.0f);
    rh.ray.dir_x = d.x;
    rh.ray.dir_y = d.y;
    rh.ray.dir_z = d.z;
    rh.ray.time = time;
    rh.ray.tfar = tFar - base;
    rh.ray.mask = mask;
    rh.ray.id = 0;
    rh.ray.flags = 0;
    rh.hit.geomID = RTC_INVALID_GEOMETRY_ID;
    rh.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;
    rtcIntersect1(rtc, &rh, nullptr);
    if (rh.hit.geomID == RTC_INVALID_GEOMETRY_ID || masksSupported) break;
    // Ray masks unavailable: skip geometry the mask excludes by hand.
    unsigned top = rh.hit.instID[0] != RTC_INVALID_GEOMETRY_ID ? rh.hit.instID[0] : rh.hit.geomID;
    if (records[top].mask & mask) break;
    base += rh.ray.tfar + 1e-4f;
    org = o + d * base;
    if (base >= tFar) {
      rh.hit.geomID = RTC_INVALID_GEOMETRY_ID;
      break;
    }
  }
  bool hit = rh.hit.geomID != RTC_INVALID_GEOMETRY_ID;
  if (hit) rh.ray.tfar += base;
  float best = hit ? rh.ray.tfar : tFar;
  for (size_t i = 0; i < planes.size(); ++i) {
    const InfinitePlane& pl = planes[i];
    if (!(pl.mask & mask)) continue;
    float denom = dot(d, pl.n);
    if (std::fabs(denom) < 1e-9f) continue;
    float t = dot(pl.p - o, pl.n) / denom;
    if (t > std::max(tNear, 1e-4f) && t < best) {
      best = t;
      planeHit = static_cast<int>(i);
    }
  }
  if (planeHit >= 0) {
    rh.ray.tfar = best;
    return true;
  }
  return hit;
}

void Renderer::Impl::surface(const RTCRayHit& rh, int planeHit, const Vec3& o, const Vec3& d, float time,
                             SurfaceHit& s) const {
  s = SurfaceHit();
  s.t = rh.ray.tfar;
  if (planeHit >= 0) {
    const InfinitePlane& pl = planes[planeHit];
    s.p = o + d * s.t;
    s.ng = s.ns = pl.n;
    Vec3 rel = s.p - pl.p;
    s.uv = Vec2(dot(rel, pl.tu) / pl.uvScale, dot(rel, pl.tv) / pl.uvScale);
    s.dpdu = pl.tu * pl.uvScale;
    s.dpdv = pl.tv * pl.uvScale;
    s.pObj = Vec3(dot(rel, pl.tu), dot(rel, pl.tv), dot(rel, pl.n));
    s.nObj = Vec3(0, 0, 1);
    s.objZ = pl.n;
    s.material = pl.material;
    s.geom = -2 - planeHit;
    s.nodeId = pl.nodeId;
    s.sceneMaterialId = pl.sceneMaterialId;
    s.gBufferId = pl.gBufferId;
    s.wireColor = pl.wireColor;
    s.receiveShadows = pl.receiveShadows;
    return;
  }
  unsigned top = rh.hit.instID[0] != RTC_INVALID_GEOMETRY_ID ? rh.hit.instID[0] : rh.hit.geomID;
  const GeomRecord& rec = records[top];
  s.geom = static_cast<int>(top);
  s.prim = rh.hit.primID;
  s.bary = Vec2(rh.hit.u, rh.hit.v);
  s.material = rec.material;
  s.nodeId = rec.nodeId;
  s.instanceId = rec.instanceId;
  s.sceneMaterialId = rec.sceneMaterialId;
  s.gBufferId = rec.gBufferId;
  s.wireColor = rec.wireColor;
  s.receiveShadows = rec.receiveShadows;
  s.lightIndex = rec.lightIndex;
  s.objZ = rec.objZ;
  if (rec.kind == GeomRecord::Kind::Curves) {
    const CurveData& cd = curves[rec.curves];
    s.p = o + d * s.t;
    Vec3 ng(rh.hit.Ng_x, rh.hit.Ng_y, rh.hit.Ng_z);
    s.ng = s.ns = length2(ng) > 0.0f ? normalize(ng) : -d;
    int strand = cd.strandOfSegment[s.prim];
    s.uv = cd.rootUv[strand];
    s.pObj = s.p;
    s.nObj = s.ns;
    s.elementId = strand;
    return;
  }
  const MeshData& m = meshes[rec.mesh];
  unsigned prim = s.prim;
  uint32_t i0 = m.idx[prim * 3], i1 = m.idx[prim * 3 + 1], i2 = m.idx[prim * 3 + 2];
  float bu = s.bary.x, bv = s.bary.y, b0 = 1.0f - bu - bv;
  const Vec3 &p0 = m.P[i0], &p1 = m.P[i1], &p2 = m.P[i2];
  Vec3 P = p0 * b0 + p1 * bu + p2 * bv;
  Vec3 ngL = cross(p1 - p0, p2 - p0);
  Vec3 nsL = m.N[i0] * b0 + m.N[i1] * bu + m.N[i2] * bv;
  s.uv = m.UV[i0] * b0 + m.UV[i1] * bu + m.UV[i2] * bv;
  // UV derivatives (object or world space, transformed below with the positions).
  Vec2 t0 = m.UV[i0], t1 = m.UV[i1], t2 = m.UV[i2];
  Vec2 d1 = t1 - t0, d2 = t2 - t0;
  Vec3 e1 = p1 - p0, e2 = p2 - p0;
  float det = d1.x * d2.y - d1.y * d2.x;
  Vec3 dpdu, dpdv;
  if (std::fabs(det) > 1e-12f) {
    float inv = 1.0f / det;
    dpdu = (e1 * d2.y - e2 * d1.y) * inv;
    dpdv = (e2 * d1.x - e1 * d2.x) * inv;
  }
  s.faceMatId = prim < m.matIds.size() ? m.matIds[prim] : 0;
  s.elementId = prim < m.elements.size() ? m.elements[prim] : 0;
  if (rec.kind == GeomRecord::Kind::Instance) {
    Mat4 M = rec.M0;
    if (rec.motion) {
      for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) M.m[r][c] = rec.M0.m[r][c] + (rec.M1.m[r][c] - rec.M0.m[r][c]) * time;
    }
    Mat4 Mi = M.inverse();
    s.pObj = P;
    s.nObj = length2(nsL) > 0.0f ? normalize(nsL) : Vec3(0, 0, 1);
    s.p = M.transformPoint(P);
    Vec3 ng = Mi.transformNormalWithInverse(ngL);
    if (M.determinant3x3() < 0.0f) ng = -ng;
    s.ng = length2(ng) > 0.0f ? normalize(ng) : Vec3(0, 0, 1);
    Vec3 ns = Mi.transformNormalWithInverse(nsL);
    s.ns = length2(ns) > 1e-20f ? normalize(ns) : s.ng;
    s.dpdu = M.transformDir(dpdu);
    s.dpdv = M.transformDir(dpdv);
    s.objZ = normalize(M.transformDir(Vec3(0, 0, 1)));
  } else {
    s.p = P;
    s.ng = length2(ngL) > 0.0f ? normalize(ngL) : Vec3(0, 0, 1);
    s.ns = length2(nsL) > 1e-20f ? normalize(nsL) : s.ng;
    s.dpdu = dpdu;
    s.dpdv = dpdv;
    s.pObj = rec.worldToObject.transformPoint(P);
    s.nObj = normalize(rec.worldToObject.transformDir(s.ns));
  }
  if (dot(s.ns, s.ng) < 0.0f) s.ns = s.ng;  // inconsistent normals: trust the geometry
  // Prefer the precise hit point along the ray for large triangles.
  if (rec.kind == GeomRecord::Kind::Mesh) {
    Vec3 pr = o + d * s.t;
    if (length2(pr - s.p) < 1e-2f * (1.0f + length2(s.p) * 1e-8f)) s.p = pr;
  }
}

ShadeContext Renderer::Impl::makeContext(const SurfaceHit& s, const Vec3& wo, RayKind kind, int depth, Rng& rng,
                                         float time, float footprint) const {
  ShadeContext sc;
  sc.p = s.p;
  sc.pObj = s.pObj;
  sc.ng = s.ng;
  sc.ns = s.ns;
  sc.nObj = s.nObj;
  sc.wo = wo;
  sc.uv = s.uv;
  sc.dpdu = s.dpdu;
  sc.dpdv = s.dpdv;
  sc.objZ = s.objZ;
  float scale = std::max(std::sqrt(std::max(length2(s.dpdu), length2(s.dpdv))), 1e-6f);
  sc.footprint = footprint / scale;
  sc.cameraDistance = length(s.p - cam.pos);
  sc.lightDir = keyLightDir;
  sc.nodeId = s.nodeId;
  sc.instanceId = s.instanceId;
  sc.faceMatId = s.faceMatId;
  sc.elementId = s.elementId;
  sc.geom = s.geom;
  sc.prim = s.prim;
  sc.bary = s.bary;
  sc.outside = dot(s.ng, wo) >= 0.0f;
  sc.rayKind = kind;
  sc.depth = depth;
  sc.tracer = this;
  sc.rng = &rng;
  sc.time = time;
  return sc;
}

// ---------------------------------------------------------------------------------------------
// Shadows and fog

// Clips the ray segment [0, t] to the fog volume. Returns false when it misses.
static bool clipToBox(const Aabb& box, const Vec3& o, const Vec3& d, float t, float& t0, float& t1) {
  t0 = 0.0f;
  t1 = t;
  for (int a = 0; a < 3; ++a) {
    float inv = 1.0f / d[a];
    float n = (box.lo[a] - o[a]) * inv, f = (box.hi[a] - o[a]) * inv;
    if (n > f) std::swap(n, f);
    if (!std::isfinite(n) || !std::isfinite(f)) {
      if (o[a] < box.lo[a] || o[a] > box.hi[a]) return false;
      continue;
    }
    t0 = std::max(t0, n);
    t1 = std::min(t1, f);
    if (t0 > t1) return false;
  }
  return true;
}

// Optical depth of the fog between distances a and b along the ray (a < b, both finite).
float Renderer::Impl::fogTransmittanceTau(const Vec3& o, const Vec3& d, float t) const {
  if (!fogOn) return 0.0f;
  float a, b;
  if (!clipToBox(fogBox, o, d, t, a, b)) return 0.0f;
  if (fogType == 0) return fogDensity * (b - a);
  // Exponential height fog: density(z) = exp(-(z - base) * falloff / height).
  float k = fogFalloff / fogHeight;
  float dz = d.z;
  float za = o.z + dz * a - fogBase;
  float ea = std::exp(-za * k);
  float len = b - a;
  if (std::fabs(dz) < 1e-5f) return fogDensity * ea * len;
  return fogDensity * ea * (1.0f - std::exp(-dz * k * len)) / (dz * k);
}

bool Renderer::Impl::sampleFog(const Vec3& o, const Vec3& d, float tMax, float u, float& t) const {
  float a, b;
  if (!clipToBox(fogBox, o, d, tMax, a, b)) return false;
  float target = -std::log(std::max(1.0f - u, 1e-12f));
  if (fogType == 0) {
    t = a + target / fogDensity;
    return t < b;
  }
  float k = fogFalloff / fogHeight;
  float dz = d.z;
  float za = o.z + dz * a - fogBase;
  float ea = std::exp(-za * k);
  if (std::fabs(dz) < 1e-5f) {
    t = a + target / (fogDensity * ea);
    return t < b;
  }
  // Solve tau(a, t) = target.
  float x = 1.0f - target * dz * k / (fogDensity * ea);
  if (x <= 0.0f) return false;  // leaves before accumulating enough density
  t = a - std::log(x) / (dz * k);
  return t > a && t < b;
}

Vec3 Renderer::Impl::transmittance(Vec3 o, const Vec3& d, float tMax, float time, const Light* light,
                                   Vec3* viaOpenings) const {
  Vec3 T(1.0f);
  if (viaOpenings) *viaOpenings = Vec3(0.0f);
  if (fogOn) {
    float tau = fogTransmittanceTau(o, d, tMax);
    if (!std::isfinite(tau)) return Vec3(0.0f);
    T *= std::exp(-tau);
  }
  Vec3 open = T;  // light that crossed no smooth refraction
  auto done = [&](const Vec3& t) {
    if (viaOpenings) *viaOpenings = isBlack(t) ? Vec3(0.0f) : vmin(open, t);
    return t;
  };
  bool linked = light && light->link.active() && light->link.affects != 1;
  if (!linked) {
    // Opaque blockers first: a cheap any-hit query that settles most shadow rays.
    RTCRay ray;
    ray.org_x = o.x;
    ray.org_y = o.y;
    ray.org_z = o.z;
    ray.tnear = 0.0f;
    ray.dir_x = d.x;
    ray.dir_y = d.y;
    ray.dir_z = d.z;
    ray.time = time;
    ray.tfar = tMax;
    ray.mask = kMaskShadowOpaque;
    ray.id = 0;
    ray.flags = 0;
    if (masksSupported) {
      rtcOccluded1(rtc, &ray, nullptr);
      if (ray.tfar < 0.0f) return Vec3(0.0f);  // Embree sets tfar = -inf on a hit
      for (const auto& pl : planes) {
        if (!(pl.mask & kMaskShadowOpaque)) continue;
        float denom = dot(d, pl.n);
        if (std::fabs(denom) < 1e-9f) continue;
        float t = dot(pl.p - o, pl.n) / denom;
        if (t > 1e-4f && t < tMax) return Vec3(0.0f);
      }
      if (!anyShadowTransparent) return done(T);
    }
  }
  // Walk through the casters nearest first (transparent ones, or all of them with light linking
  // or without ray mask support).
  unsigned mask = linked || !masksSupported ? (kMaskShadowOpaque | kMaskShadowTransp) : kMaskShadowTransp;
  RTCRayHit rh;
  Rng rng(0x51ED, 7);
  for (int layer = 0; layer < kMaxShadowLayers; ++layer) {
    int planeHit = -1;
    if (!intersect(o, d, 0.0f, tMax, mask, time, rh, planeHit)) return done(T);
    SurfaceHit s;
    surface(rh, planeHit, o, d, time, s);
    unsigned topMask = planeHit >= 0 ? planes[planeHit].mask : records[s.geom].mask;
    bool skip = linked && !light->link.castsShadowFrom(s.nodeId);
    if (!skip) {
      if (topMask & kMaskShadowOpaque) return Vec3(0.0f);
      ShadeContext sc = makeContext(s, -d, RayKind::Shadow, 1, rng, time, 0.0f);
      const ShadeMaterial* m = materials[s.material]->resolve(sc);
      Vec3 pass = m->shadowTransmission(sc, std::fabs(dot(s.ng, d)));
      T *= pass;
      if (viaOpenings) open *= m->smoothShadowRefraction(sc) ? vmin(Vec3(1.0f - m->opacity(sc)), pass) : pass;
      if (maxComponent(T) <= 1e-4f) return Vec3(0.0f);
    }
    o = offsetRayOrigin(s.p, dot(d, s.ng) > 0.0f ? s.ng : -s.ng);
    tMax -= rh.ray.tfar;
    if (tMax <= 0.0f) return done(T);
  }
  return done(T);
}

// ---------------------------------------------------------------------------------------------
// Light selection

int Renderer::Impl::pickLight(float u, float& prob) const {
  if (lights.empty()) return -1;
  size_t i = static_cast<size_t>(std::upper_bound(lightCdf.begin(), lightCdf.end(), u) - lightCdf.begin());
  i = std::min(i, lights.size() - 1);
  prob = lightProb[i];
  return prob > 0.0f ? static_cast<int>(i) : -1;
}

float Renderer::Impl::lightSelectProb(int index, const Vec3& p, const Vec3& n, bool twoSided) const {
  if (index < 0 || index >= static_cast<int>(lights.size())) return 0.0f;
  if (lights[index].type == Light::Type::Sun) return 1.0f;
  switch (lightSampling) {
    case 1: return 1.0f;
    case 2: return lightProb[index];
    default: return lightTree.pmf(p, n, twoSided, index);
  }
}

// ---------------------------------------------------------------------------------------------
// Camera

void Renderer::Impl::cameraRay(float px, float py, Rng& rng, Vec3& o, Vec3& d, float& weight, float& time,
                               float& tNear, float& tFar) const {
  weight = 1.0f;
  time = cam.motionBlur ? rng.next() : 0.0f;
  Vec3 pos = cam.pos, f = cam.f, r = cam.r, u = cam.u;
  if (cam.moves) {
    pos = lerp(cam.pos, cam.pos1, time);
    f = normalize(lerp(cam.f, cam.f1, time));
    u = normalize(lerp(cam.u, cam.u1, time));
    r = normalize(cross(f, u));
    u = cross(r, f);
  }
  float sx = 2.0f * px / w - 1.0f;
  float sy = 1.0f - 2.0f * py / h;
  float x = sx * cam.aspect + cam.shiftX, y = sy + cam.shiftY;
  if (cam.distortion != 0.0f) {
    float r2 = x * x + y * y;
    float k = 1.0f + cam.distortion * r2 * 0.25f;
    x *= k;
    y *= k;
  }
  tNear = 0.0f;
  tFar = kInfinity;
  if (cam.ortho) {
    o = pos + r * (x * cam.halfH) + u * (y * cam.halfH);
    d = f;
    if (cam.nearClip > 0.0f) tNear = cam.nearClip;
    if (cam.farClip > 0.0f) tFar = cam.farClip;
    return;
  }
  d = normalize(f + r * (x * cam.tanHalf) + u * (y * cam.tanHalf));
  o = pos;
  float cosA = dot(d, f);
  if (cam.vignetting > 0.0f) weight = lerpf(1.0f, cosA * cosA * cosA * cosA, clampf(cam.vignetting, 0.0f, 1.0f));
  if (cam.aperture > 0.0f) {
    Vec3 focusPoint = o + d * (cam.focus / std::max(cosA, 1e-4f));
    Vec2 lens = sampleAperture(rng.next(), rng.next(), cam.blades, cam.bladeRot) * cam.aperture;
    lens.x *= 1.0f - cam.anisotropy;
    lens.y *= 1.0f + cam.anisotropy;
    o = pos + r * lens.x + u * lens.y;
    d = normalize(focusPoint - o);
    cosA = dot(d, f);
  }
  if (cam.nearClip > 0.0f) tNear = cam.nearClip / std::max(cosA, 1e-4f);
  if (cam.farClip > 0.0f) tFar = cam.farClip / std::max(cosA, 1e-4f);
}

// ---------------------------------------------------------------------------------------------
// Subsurface random walk

bool Renderer::Impl::randomWalk(const SurfaceHit& entry, const SubsurfaceParams& sss, const Vec3& dirIn, float time,
                                Rng& rng, SurfaceHit& exitHit, Vec3& exitDir, Vec3& weight) const {
  Vec3 sigmaT(1.0f / std::max(sss.radius.x, 1e-5f), 1.0f / std::max(sss.radius.y, 1e-5f),
              1.0f / std::max(sss.radius.z, 1e-5f));
  Vec3 alpha = subsurfaceSingleScatterAlbedo(sss.albedo);
  Vec3 sigmaS = sigmaT * alpha;
  Vec3 throughput(1.0f);
  Vec3 ngIn = dot(entry.ng, dirIn) < 0.0f ? entry.ng : -entry.ng;  // outward normal at the entry
  Vec3 p = offsetRayOrigin(entry.p, -ngIn);
  Vec3 dir = dirIn;
  RTCRayHit rh;
  for (int step = 0; step < kMaxWalkSteps; ++step) {
    float sum = throughput.x + throughput.y + throughput.z;
    if (sum <= 0.0f) return false;
    Vec3 pw = throughput / sum;  // channel selection probabilities
    float uc = rng.next();
    int ch = uc < pw.x ? 0 : (uc < pw.x + pw.y ? 1 : 2);
    float t = -std::log(std::max(1.0f - rng.next(), 1e-12f)) / sigmaT[ch];
    int planeHit = -1;
    bool hit = intersect(p, dir, 0.0f, t, kMaskGI, time, rh, planeHit);
    if (hit) {
      float th = rh.ray.tfar;
      Vec3 T = vexp(-sigmaT * th);
      float pdf = pw.x * T.x + pw.y * T.y + pw.z * T.z;
      if (pdf <= 0.0f) return false;
      throughput *= T / pdf;
      surface(rh, planeHit, p, dir, time, exitHit);
      exitDir = dir;
      weight = throughput;
      return isFiniteColor(weight);
    }
    Vec3 T = vexp(-sigmaT * t);
    float pdf = pw.x * sigmaT.x * T.x + pw.y * sigmaT.y * T.y + pw.z * sigmaT.z * T.z;
    if (pdf <= 0.0f) return false;
    throughput *= sigmaS * T / pdf;
    p = p + dir * t;
    dir = sampleHg(dir, sss.anisotropy, rng.next(), rng.next());
    if (step > 8) {
      float survive = std::min(maxComponent(throughput), 0.98f);
      if (rng.next() >= survive) return false;
      throughput /= survive;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------------------------
// Path tracing

void Renderer::Impl::trace(Vec3 o, Vec3 d, float tNear, float tFar, float time, Rng& rng, PathOutput& out) const {
  Vec3 beta(1.0f), sigma(0.0f);
  bool specular = true;     // last scattering was a delta lobe (or this is the camera ray)
  bool firstDelta = false;
  int firstCat = -1;        // lobe category leaving the first vertex (-2 = fog)
  int prevCat = -1;
  bool afterRough = false;  // a diffuse or glossy bounce happened (caustics option)
  float pathRough = 0.0f;   // roughest lobe scattered from so far (glossy filter)
  float prevPdf = 0.0f;
  int depth = 0, passThrough = 0;
  bool aovDone = false;
  RayKind kind = RayKind::Camera;
  unsigned mask = kMaskCamera;
  uint32_t prevNode = 0;
  // Last scattering vertex: light selection probabilities for MIS depend on it. `traveled` is
  // the distance from it to the current ray origin (cut-outs and thin glass are crossed without
  // scattering).
  Vec3 prevP = o, prevN(0.0f);
  bool prevTwoSided = false;
  float traveled = 0.0f;
  float spread = 2.0f * cam.tanHalf / std::max(h, 1), width = 0.0f;
  float giScale = 1.0f;

  // Path guiding: vertices whose incident radiance is recorded when the path ends.
  struct GuideVertex {
    Vec3 p, wi, throughput, radiance;
    float pdf;
    Vec3 f;  // BSDF times cosine in wi
    const PathGuide::Region* region;  // mixture this vertex sampled (null without guiding)
    float guidePdf, bsdfPdf, mass;
  };
  const bool training = guide && guideTraining.load(std::memory_order_relaxed);
  thread_local std::vector<GuideVertex> guideStore;
  if (training && guideStore.size() < static_cast<size_t>(kMaxGuideVertices)) guideStore.resize(kMaxGuideVertices);
  GuideVertex* guideVerts = guideStore.data();
  int guideCount = 0;

  auto add = [&](Vec3 c, int receivingVertex, int elemIfDirect, int group) {
    if (!isFiniteColor(c)) return;
    if (receivingVertex >= 1) {
      c = clampLuminance(c, clampIndirectScene);
      c *= giScale;
    } else if (clampDirectScene > 0.0f) {
      c = clampLuminance(c, clampDirectScene);
    }
    out.L += c;
    if (anyBeautyElement) {
      int e = receivingVertex <= 0 ? elemIfDirect : indirectElement(firstCat);
      out.elem[e] += c;
    }
    if (lightMixOn && group >= 0 && group < kMaxLightGroups) out.group[group] += c;
    for (int k = 0; k < guideCount; ++k) guideVerts[k].radiance += c;
  };

  // Next-event estimation from a point with geometric normal ngf (n: the receiver normal for light
  // selection, zero in volumes). With bsdf == null, the HG phase function of the fog is used.
  auto directLight = [&](const Vec3& p, const Vec3& ngf, const Vec3& n, bool twoSided, const Vec3& wo,
                         const Bsdf* bsdf, uint32_t nodeId, bool receiveShadows, int vertex, bool inMedium,
                         const GuideContext& gc) {
    auto evalAt = [&](const Vec3& wi, BsdfEval& e) {
      if (bsdf) {
        int side = dot(wi, ngf) > 0.0f ? 1 : -1;
        e = bsdf->eval(wo, wi, side);
      } else {
        for (auto& f : e.f) f = Vec3(0.0f);
        float ph = hgPhase(dot(-wo, wi), fogG);
        e.f[0] = Vec3(ph);
        e.pdf = ph;
      }
    };
    auto contribute = [&](const BsdfEval& e, const Vec3& radiance, const Light* light, int group, const Vec3& T) {
      for (int k = 0; k < kLobeCategories; ++k) {
        Vec3 f = e.f[k];
        if (isBlack(f)) continue;
        if (light) {
          bool diffuseLike = k == 0 || k == 2 || k == 3;
          float m = diffuseLike ? (light->affectDiffuse ? light->diffuseMult : 0.0f)
                                : (light->affectSpecular ? light->specularMult : 0.0f);
          if (m <= 0.0f) continue;
          f *= m;
        }
        Vec3 c = beta * f * radiance * T;
        int elem = inMedium && vertex == 0 ? kElemAtmosphere : directElement(k, false);
        add(c, vertex, elem, group);
      }
    };
    auto shadowOrigin = [&](const Vec3& wi) {
      if (inMedium) return p;
      return offsetRayOrigin(p, dot(wi, ngf) > 0.0f ? ngf : -ngf);
    };
    // Shadowed, MIS-weighted transmittance. While refractive caustics are off, light that came
    // through smooth glass can only be found by shadow rays, so that part keeps its full weight.
    auto visibility = [&](const Vec3& wi, float dist, const Light* light, float misWeight, float fullWeight) {
      if (receiveShadows && (!light || light->castShadows)) {
        float tMax = std::isfinite(dist) ? dist * (1.0f - 1e-4f) : kInfinity;
        if (caustics || misWeight == fullWeight) return transmittance(shadowOrigin(wi), wi, tMax, time, light) * misWeight;
        Vec3 open;
        Vec3 T = transmittance(shadowOrigin(wi), wi, tMax, time, light, &open);
        return open * misWeight + (T - open) * fullWeight;
      }
      if (!fogOn) return Vec3(misWeight);
      float tau = fogTransmittanceTau(p, wi, std::isfinite(dist) ? dist : kInfinity);
      return Vec3(std::isfinite(tau) ? std::exp(-tau) * misWeight : 0.0f);
    };
    auto sampleLight = [&](int li, float sel) {
      const Light& light = lights[li];
      float u1 = rng.next(), u2 = rng.next();
      if (!light.link.illuminates(nodeId)) return;
      LightSample ls;
      if (!light.sample(p, u1, u2, ls)) return;
      BsdfEval e;
      evalAt(ls.wi, e);
      if (isBlack(e.total())) return;
      float misWeight, fullWeight;
      if (ls.delta) {
        misWeight = fullWeight = 1.0f / sel;
      } else {
        float lpdf = ls.pdf * sel;
        fullWeight = 1.0f / lpdf;
        misWeight = powerHeuristic(lpdf, bsdf ? gc.mix(e.pdf, ls.wi) : e.pdf) * fullWeight;
      }
      Vec3 T = visibility(ls.wi, ls.dist, &light, misWeight, fullWeight);
      if (isBlack(T)) return;
      contribute(e, ls.radiance, &light, light.type == Light::Type::Portal ? envGroup : light.group, T);
    };
    for (int li : distantLights) sampleLight(li, 1.0f);
    if (!localLights.empty()) {
      if (lightSampling == 1) {
        for (int li : localLights) sampleLight(li, 1.0f);
      } else if (lightSampling == 2) {
        float sel = 1.0f;
        int li = pickLight(rng.next(), sel);
        if (li >= 0) sampleLight(li, sel);
      } else {
        LightPick picks[LightTree::kMaxPicks];
        int np = lightTree.pick(p, n, twoSided, rng.next(), picks);
        for (int k = 0; k < np; ++k) sampleLight(picks[k].light, picks[k].prob);
      }
    }
    if (!hasPortals && !env.isBlack()) {
      Vec3 wi, Le;
      float epdf = 0.0f;
      if (env.sample(rng.next(), rng.next(), wi, Le, epdf) && epdf > 0.0f) {
        BsdfEval e;
        evalAt(wi, e);
        if (!isBlack(e.total())) {
          float fullWeight = 1.0f / epdf;
          float misWeight = powerHeuristic(epdf, bsdf ? gc.mix(e.pdf, wi) : e.pdf) * fullWeight;
          Vec3 T = visibility(wi, kInfinity, nullptr, misWeight, fullWeight);
          if (!isBlack(T)) contribute(e, Le, nullptr, envGroup, T);
        }
      }
    }
  };

  // MIS pdf of the environment through skylight portals.
  auto portalPdf = [&](const Vec3& org, const Vec3& dir) {
    float pdf = 0.0f;
    for (int pi : portals) {
      float t, lp;
      Vec3 le;
      if (lights[pi].intersect(org, dir, kInfinity, t, le, lp)) pdf += lp * lightSelectProb(pi, prevP, prevN, prevTwoSided);
    }
    return pdf;
  };

  RTCRayHit rh;
  SurfaceHit s;
  for (;;) {
    int planeHit = -1;
    bool hit = intersect(o, d, tNear, tFar, mask, time, rh, planeHit);
    float tHit = hit ? rh.ray.tfar : kInfinity;
    width += spread * (std::isfinite(tHit) ? tHit : 0.0f);

    // Analytic area lights in front of the surface.
    int lightIdx = -1;
    Vec3 Le;
    float lightPdf = 0.0f;
    for (int li : areaLights) {
      const Light& l = lights[li];
      if (l.type == Light::Type::Portal) continue;
      if (kind == RayKind::Camera && l.invisible) continue;
      float t, pdf;
      Vec3 le;
      if (l.intersect(o, d, tHit, t, le, pdf)) {
        tHit = t;
        Le = le;
        lightPdf = pdf;
        lightIdx = li;
      }
    }

    // Participating media: global fog, then the interior of refractive objects.
    if (fogOn && (hit || lightIdx >= 0 || fogAffectBackground || kind != RayKind::Camera)) {
      float tf;
      if (sampleFog(o, d, tHit, rng.next(), tf)) {
        Vec3 p = o + d * tf;
        if (!isBlack(sigma)) beta *= vexp(-sigma * tf);
        beta *= fogAlbedo;
        if (!isBlack(fogEmission)) add(beta * fogEmission, depth, kElemAtmosphere, 1);
        if (depth == 0 && firstCat == -1) firstCat = -2;
        if (depth >= maxDepth) break;
        directLight(p, Vec3(0, 0, 1), Vec3(0.0f), false, -d, nullptr, 0, true, depth, true, GuideContext());
        Vec3 nd = sampleHg(d, fogG, rng.next(), rng.next());
        prevPdf = hgPhase(dot(d, nd), fogG);
        specular = false;
        afterRough = true;
        pathRough = 1.0f;
        prevCat = 0;
        prevNode = 0;
        prevP = p;
        prevN = Vec3(0.0f);
        prevTwoSided = false;
        traveled = 0.0f;
        o = p;
        d = nd;
        tNear = 0.0f;
        tFar = kInfinity;
        kind = RayKind::Diffuse;
        mask = kMaskGI;
        spread = std::max(spread, 0.3f);
        ++depth;
        if (depth >= 3) {
          float survive = std::min(maxComponent(beta), 0.95f);
          if (rng.next() >= survive) break;
          beta /= survive;
        }
        continue;
      }
    }
    if (!isBlack(sigma) && std::isfinite(tHit)) beta *= vexp(-sigma * tHit);

    bool causticBlocked = !caustics && afterRough && specular && depth >= 2;

    if (lightIdx >= 0) {
      const Light& l = lights[lightIdx];
      bool allowed = l.link.illuminates(prevNode) && !causticBlocked &&
                     !((kind == RayKind::Reflect || kind == RayKind::Refract) && !l.affectReflections);
      if (allowed) {
        if (traveled > 0.0f && tHit > 0.0f) lightPdf *= (tHit + traveled) * (tHit + traveled) / (tHit * tHit);
        float wgt = specular ? 1.0f
                             : powerHeuristic(prevPdf, lightPdf * lightSelectProb(lightIdx, prevP, prevN, prevTwoSided));
        float m = prevCat == 1 ? (l.affectSpecular ? l.specularMult : 0.0f) : (prevCat >= 0 ? (l.affectDiffuse ? l.diffuseMult : 0.0f) : 1.0f);
        add(beta * Le * (wgt * m), depth - 1, depth == 0 ? kElemSelfIllum : directElement(firstCat, firstDelta), l.group);
      }
      if (!aovDone) {
        out.albedo = clampColor(Le, 0.0f, 1.0f);
        out.normal = -d;
        out.alpha = 1.0f;
        aovDone = true;
      }
      break;
    }

    if (!hit) {
      Vec3 envL;
      bool cameraRay = kind == RayKind::Camera;
      if (cameraRay && separateBackground) {
        envL = envBack.visibleToCamera ? envBack.eval(d) : envBack.background;
      } else if (cameraRay && !env.visibleToCamera) {
        envL = env.background;
      } else if (!causticBlocked) {
        envL = env.eval(d);
        if (!specular) {
          float lp = hasPortals ? portalPdf(o, d) : env.pdf(d);
          envL *= powerHeuristic(prevPdf, lp);
        }
        if (specular) envL += env.sunDisk(d);
      }
      add(beta * envL, depth - 1, depth == 0 ? kElemBackground : directElement(firstCat, firstDelta), envGroup);
      if (!aovDone) {
        out.albedo = clampColor(envL, 0.0f, 1.0f);
        out.alpha = 0.0f;
        aovDone = true;
      }
      break;
    }

    surface(rh, planeHit, o, d, time, s);
    float footprint = width;
    ShadeContext sc = makeContext(s, -d, kind, depth, rng, time, footprint);
    const ShadeMaterial* mat = materials[s.material]->resolve(sc);

    // Stochastic cut-out (opacity maps for leaves, fences, ...).
    if (mat->hasOpacity() && rng.next() >= mat->opacity(sc)) {
      if (++passThrough > kMaxPassThrough) break;
      traveled += s.t;
      o = offsetRayOrigin(s.p, dot(d, s.ng) > 0.0f ? s.ng : -s.ng);
      tNear = 0.0f;
      if (std::isfinite(tFar)) tFar -= s.t;
      continue;
    }

    if (!aovDone && kind == RayKind::Camera) {
      out.hit = true;
      out.depth = s.t;
      out.nodeId = s.nodeId;
      out.sceneMaterialId = s.sceneMaterialId;
      out.gBufferId = s.gBufferId;
      out.wireColor = s.wireColor;
      out.alpha = 1.0f;
    }

    // Emitting geometry of mesh lights.
    if (s.lightIndex >= 0) {
      int tri = static_cast<int>(s.prim);
      if (!lights[s.lightIndex].primLight.empty() && s.prim < lights[s.lightIndex].primLight.size()) {
        const Light& whole = lights[s.lightIndex];  // split into chunks: find this triangle's
        tri = whole.primLocal[s.prim];
        s.lightIndex = whole.primLight[s.prim];
      }
      const Light& l = lights[s.lightIndex];
      bool front = dot(s.ng, d) < 0.0f;
      Vec3 le = l.meshRadiance ? l.meshRadiance(tri, s.bary, front) : l.color;
      bool allowed = l.link.illuminates(prevNode) && !causticBlocked &&
                     !((kind == RayKind::Reflect || kind == RayKind::Refract) && !l.affectReflections);
      if (!isBlack(le) && allowed) {
        float cosL = std::fabs(dot(s.ng, d));
        float dist = s.t + traveled;
        float wgt = specular ? 1.0f
                             : powerHeuristic(prevPdf, l.meshPdf(dist * dist, cosL) *
                                                           lightSelectProb(s.lightIndex, prevP, prevN, prevTwoSided));
        add(beta * le * wgt, depth - 1, depth == 0 ? kElemSelfIllum : directElement(firstCat, firstDelta), l.group);
      }
      if (!aovDone) {
        out.albedo = clampColor(le, 0.0f, 1.0f);
        out.normal = -d;
        aovDone = true;
      }
      break;
    }

    SurfaceShade sh;
    mat->shade(sc, sh);
    Vec3 ngf = dot(s.ng, d) < 0.0f ? s.ng : -s.ng;

    // Matte / shadow catcher seen by the camera.
    if (kind == RayKind::Camera && sh.matte && sh.matte->matte) {
      const MatteParams& mp = *sh.matte;
      Vec3 B = separateBackground ? (envBack.visibleToCamera ? envBack.eval(d) : envBack.background)
                                  : (env.visibleToCamera ? env.eval(d) : env.background);
      float shadow = 0.0f;
      if (mp.shadows) {
        // Shadow amount: lit irradiance relative to the unshadowed irradiance.
        float lit = 0.0f, full = 0.0f;
        Vec3 p = offsetRayOrigin(s.p, ngf);
        for (const auto& light : lights) {
          LightSample ls;
          if (!light.sample(s.p, rng.next(), rng.next(), ls)) continue;
          float c = dot(ls.wi, ngf);
          if (c <= 0.0f) continue;
          float e = luminance(ls.radiance) * c / (ls.delta ? 1.0f : std::max(ls.pdf, 1e-8f));
          full += e;
          float tMax = std::isfinite(ls.dist) ? ls.dist * (1.0f - 1e-4f) : kInfinity;
          lit += e * luminance(transmittance(p, ls.wi, tMax, time, &light));
        }
        if (!hasPortals && !env.isBlack()) {
          Vec3 wi, le;
          float epdf;
          if (env.sample(rng.next(), rng.next(), wi, le, epdf) && epdf > 0.0f && dot(wi, ngf) > 0.0f) {
            float e = luminance(le) * dot(wi, ngf) / epdf;
            full += e;
            lit += e * luminance(transmittance(p, wi, kInfinity, time, nullptr));
          }
        }
        shadow = full > 0.0f ? clampf(1.0f - lit / full, 0.0f, 1.0f) : 0.0f;
      }
      float sAmt = clampf(shadow * mp.shadowBrightness, 0.0f, 1.0f);
      Vec3 c = lerp(B, mp.shadowColor, sAmt);
      add(beta * c, -1, kElemBackground, envGroup);
      out.alpha = mp.alphaContribution < 0.0f ? (mp.shadowsAffectAlpha ? sAmt : 0.0f)
                                               : clampf(mp.alphaContribution, 0.0f, 1.0f);
      out.albedo = clampColor(c, 0.0f, 1.0f);
      out.normal = ngf;
      break;
    }

    if (kind == RayKind::Diffuse) beta *= sh.generateGI;

    if (!aovDone) {
      out.albedo = isBlack(sh.bsdf.albedo()) && !isBlack(sh.emission) ? clampColor(sh.emission, 0.0f, 1.0f) : Vec3(0.0f);
      out.normal = sh.shadingNormal;
    }
    if (!isBlack(sh.emission) && (depth == 0 || sh.emissionAffectsGI) && !causticBlocked)
      add(beta * sh.emission, depth - 1, depth == 0 ? kElemSelfIllum : directElement(firstCat, firstDelta), 1);

    // Glossy filter: after rough bounces, glossy lobes are blurred so that small bright lights
    // seen in sharp reflections of indirectly lit surfaces do not turn into fireflies.
    if (filterGlossy > 0.0f && pathRough > 0.0f) {
      float minAlpha = std::min(filterGlossy * kGlossyFilterScale * pathRough, 1.0f);
      for (int i = 0; i < sh.bsdf.count(); ++i) {
        Lobe& l = sh.bsdf.lobe(i);
        if (l.kind != LobeKind::GgxReflect && l.kind != LobeKind::GgxRefract) continue;
        l.ax = std::max(l.ax, minAlpha);
        l.ay = std::max(l.ay, minAlpha);
      }
    }

    sh.bsdf.finalize(sc.wo);
    if (!aovDone) {
      if (!sh.bsdf.empty()) out.albedo = sh.bsdf.albedo();
      aovDone = true;
    }
    if (depth >= maxDepth || sh.bsdf.isBlack()) {
      if (sh.hasExitColor && kind == RayKind::Refract && depth >= maxDepth)
        add(beta * sh.exitColor, depth, kElemRefraction, envGroup);
      break;
    }

    // What the lobes ask for: guiding, light from behind (transmission), roughness.
    float guidable = 0.0f, vertexRough = 0.0f;
    bool twoSided = false;
    for (int i = 0; i < sh.bsdf.count(); ++i) {
      const Lobe& l = sh.bsdf.lobe(i);
      if (l.select <= 0.0f) continue;
      guidable += l.select * lobeGuidability(l);
      vertexRough = std::max(vertexRough, lobeRoughness(l));
      if (l.kind == LobeKind::DiffuseTransmission || l.kind == LobeKind::GgxRefract) twoSided = true;
    }
    GuideContext gc;
    if (guide && guidable > 0.0f && guide->ready()) {
      gc.region = guide->region(s.p);
      // After training, regions that learned guiding does not pay off skip it (and its lookups).
      bool worth = training || gc.region->selectProb >= kMinGuideSelect;
      if (worth && PathGuide::canSample(gc.region)) gc.prob = gc.region->selectProb * std::min(guidable, 1.0f);
    }

    if (sh.bsdf.hasNonDelta() && (!lights.empty() || !env.isBlack()))
      directLight(s.p, ngf, ngf, twoSided, sc.wo, &sh.bsdf, s.nodeId, s.receiveShadows, depth, false, gc);

    BsdfSample bs;
    bool guided = false;
    float guidePdf = 0.0f, bsdfPdf = 0.0f;  // the two strategies' densities of the sampled direction
    float uL = rng.next(), u1 = rng.next(), u2 = rng.next();
    if (gc.prob > 0.0f && uL < gc.prob) {
      // Guided direction, weighted by the BSDF against the combined density.
      guided = true;
      float pg;
      Vec3 wi = PathGuide::sample(gc.region, rng, pg);
      float sideG = dot(wi, ngf);
      BsdfEval e = sh.bsdf.eval(sc.wo, wi, sideG > 0.0f ? 1 : -1);
      guidePdf = pg;
      bsdfPdf = sh.bsdf.eval(sc.wo, wi).pdf;
      float pMix = gc.prob * pg + (1.0f - gc.prob) * bsdfPdf;
      Vec3 f = e.f[0] * sh.receiveGI + e.f[1] + e.f[2] + e.f[3];
      if (!(pMix > 0.0f) || isBlack(f) || sideG == 0.0f) break;
      bs.wi = wi;
      bs.weight = f / pMix;
      bs.pdf = pMix;
      bs.transmission = sideG < 0.0f;
      int cat = 0;
      for (int k = 1; k < kLobeCategories; ++k)
        if (luminance(e.f[k]) > luminance(e.f[cat])) cat = k;
      bs.category = static_cast<LobeCategory>(cat);
    } else {
      if (gc.prob > 0.0f) uL = std::min((uL - gc.prob) / (1.0f - gc.prob), 0.99999994f);
      if (!sh.bsdf.sample(sc.wo, uL, u1, u2, bs)) break;
      if (gc.prob > 0.0f) {
        if (bs.delta) {
          bs.weight /= 1.0f - gc.prob;
        } else {
          bsdfPdf = bs.pdf;
          guidePdf = PathGuide::pdf(gc.region, bs.wi);
          float pMix = (1.0f - gc.prob) * bsdfPdf + gc.prob * guidePdf;
          bs.weight *= bs.pdf / pMix;
          bs.pdf = pMix;
        }
      }
    }
    float side = dot(bs.wi, ngf);
    if ((side > 0.0f) == bs.transmission) break;  // shading normal sent the ray through the geometry
    beta *= bs.weight;
    if (bs.category == LobeCategory::Diffuse && !guided) beta *= sh.receiveGI;
    if (!isFiniteColor(beta) || isBlack(beta)) break;
    if (depth == 0) {
      firstCat = static_cast<int>(bs.category);
      firstDelta = bs.delta;
    }
    // Thin glass after a rough bounce: the ray continues straight on, along the same light path a
    // shadow ray through the glass takes, so it keeps the scattering vertex for MIS (and guiding
    // learns the light behind windows).
    if (!specular && bs.delta && bs.lobe >= 0 && sh.bsdf.lobe(bs.lobe).kind == LobeKind::ThinTransmit) {
      if (++passThrough > kMaxPassThrough) break;
      traveled += s.t;
      o = offsetRayOrigin(s.p, side > 0.0f ? ngf : -ngf);
      tNear = 0.0f;
      tFar = kInfinity;
      continue;
    }
    prevCat = static_cast<int>(bs.category);

    // Subsurface: random walk to an exit point, then continue as a diffuse surface there.
    if (bs.subsurface && sh.hasSubsurface) {
      SurfaceHit ex;
      Vec3 wWalk, exitDir;
      if (!randomWalk(s, sh.sss, bs.wi, time, rng, ex, exitDir, wWalk)) break;
      beta *= wWalk;
      if (!isFiniteColor(beta) || isBlack(beta)) break;
      Vec3 outN = dot(ex.ng, exitDir) > 0.0f ? ex.ng : -ex.ng;  // pointing out of the object
      Vec3 ns = dot(ex.ns, outN) > 0.0f ? ex.ns : -ex.ns;
      Bsdf exitBsdf;
      Lobe l;
      l.kind = LobeKind::Lambert;
      l.category = LobeCategory::Subsurface;
      l.frame = Frame(ns);
      l.weight = Vec3(1.0f);
      exitBsdf.add(l);
      exitBsdf.finalize(outN);
      if (depth + 1 >= maxDepth) break;
      directLight(ex.p, outN, outN, false, outN, &exitBsdf, ex.nodeId, ex.receiveShadows, depth, false, GuideContext());
      Vec3 wiL = sampleCosineHemisphere(rng.next(), rng.next());
      Vec3 wi = normalize(l.frame.toWorld(wiL));
      if (dot(wi, outN) <= 0.0f) break;
      prevPdf = wiL.z * kInvPi;
      specular = false;
      afterRough = true;
      pathRough = 1.0f;
      prevNode = ex.nodeId;
      prevP = ex.p;
      prevN = outN;
      prevTwoSided = false;
      traveled = 0.0f;
      o = offsetRayOrigin(ex.p, outN);
      d = wi;
      tNear = 0.0f;
      tFar = kInfinity;
      kind = RayKind::Diffuse;
      mask = kMaskGI;
      spread = std::max(spread, 0.3f);
      ++depth;
      continue;
    }

    if (!bs.delta) {
      afterRough = true;
      pathRough = std::max(pathRough, guided || bs.lobe < 0 ? vertexRough : lobeRoughness(sh.bsdf.lobe(bs.lobe)));
      if (training && guideCount < kMaxGuideVertices)
        guideVerts[guideCount++] = {s.p,        bs.wi,    beta,     Vec3(0.0f), bs.pdf, bs.weight * bs.pdf,
                                    gc.prob > 0.0f ? gc.region : nullptr, guidePdf, bsdfPdf,
                                    gc.prob > 0.0f ? gc.prob / gc.region->selectProb : 0.0f};
    }
    specular = bs.delta;
    prevPdf = bs.pdf;
    prevNode = s.nodeId;
    prevP = s.p;
    prevN = ngf;
    prevTwoSided = twoSided;
    traveled = 0.0f;
    if (bs.transmission && !sh.thin) {
      bool entering = sc.outside;
      sigma = entering ? sh.absorption : Vec3(0.0f);
    }
    kind = bs.transmission ? RayKind::Refract
                           : (bs.category == LobeCategory::Diffuse ? RayKind::Diffuse : RayKind::Reflect);
    if (bs.category == LobeCategory::Specular && !bs.delta) {
      // Glossy reflections: blurrier footprint, GI visibility for very rough lobes.
      spread += 0.05f;
    }
    if (kind == RayKind::Diffuse) spread = std::max(spread, 0.3f);
    mask = kind == RayKind::Diffuse ? kMaskGI : kMaskSecondary;
    o = offsetRayOrigin(s.p, side > 0.0f ? ngf : -ngf);
    d = bs.wi;
    tNear = 0.0f;
    tFar = kInfinity;
    ++depth;
    if (depth == 1 && giMultiplier != 1.0f && kind == RayKind::Diffuse) giScale = giMultiplier;

    if (depth >= 3) {
      float survive = std::min(maxComponent(beta), 0.95f);
      if (rng.next() >= survive) break;
      beta /= survive;
    }
  }

  // Train the guide: the radiance that arrived at each vertex from its sampled direction.
  for (int k = 0; k < guideCount; ++k) {
    const GuideVertex& v = guideVerts[k];
    Vec3 Li;
    for (int c = 0; c < 3; ++c) Li[c] = v.throughput[c] > 1e-20f ? v.radiance[c] / v.throughput[c] : 0.0f;
    guide->record(v.p, v.wi, luminance(Li) / v.pdf, rng);
    if (v.region) PathGuide::addSelectionGradient(v.region, luminance(v.f * Li), v.pdf, v.guidePdf, v.bsdfPdf, v.mass);
  }
}

// ---------------------------------------------------------------------------------------------
// Map services

float Renderer::Impl::traceDistance(const Vec3& o, const Vec3& d, float tMax, uint32_t* nodeId) const {
  RTCRayHit rh;
  int planeHit = -1;
  if (!intersect(o, d, 0.0f, tMax, kMaskGI, 0.0f, rh, planeHit)) {
    if (nodeId) *nodeId = 0;
    return tMax;
  }
  if (nodeId) {
    if (planeHit >= 0) {
      *nodeId = planes[planeHit].nodeId;
    } else {
      unsigned top = rh.hit.instID[0] != RTC_INVALID_GEOMETRY_ID ? rh.hit.instID[0] : rh.hit.geomID;
      *nodeId = records[top].nodeId;
      if (*nodeId == 0) *nodeId = 0xFFFFFFFFu;
    }
  }
  return rh.ray.tfar;
}

bool Renderer::Impl::triangle(int geom, unsigned prim, Vec3 corners[3], unsigned& edgeMask) const {
  if (geom < 0 || geom >= static_cast<int>(records.size())) return false;
  const GeomRecord& rec = records[geom];
  if (rec.kind == GeomRecord::Kind::Curves || rec.mesh < 0) return false;
  const MeshData& m = meshes[rec.mesh];
  if (prim * 3 + 2 >= m.idx.size()) return false;
  for (int k = 0; k < 3; ++k) {
    Vec3 p = m.P[m.idx[prim * 3 + k]];
    corners[k] = rec.kind == GeomRecord::Kind::Instance ? rec.M0.transformPoint(p) : p;
  }
  edgeMask = prim < m.edgeMask.size() ? m.edgeMask[prim] : 7u;
  return true;
}

float Renderer::Impl::distanceTo(int field, const Vec3& p, float maxDist) const {
  if (field < 0 || field >= static_cast<int>(distanceFields.size())) return maxDist;
  return distanceFields[field].query(p, maxDist);
}

}  // namespace veraa
