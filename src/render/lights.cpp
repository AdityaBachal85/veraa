#include "lights.h"

#include <algorithm>
#include <map>
#include <sstream>

#include "environment.h"
#include "renderobjects.h"
#include "scene.h"
#include "sky.h"

namespace veraa {
namespace render {

namespace {

float smoothstepf(float a, float b, float x) {
  if (a == b) return x >= b ? 1.0f : 0.0f;
  float t = clampf((x - a) / (b - a), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

// Distance along the ray to the sphere (nearest positive root), or -1.
float raySphere(const Vec3& o, const Vec3& d, const Vec3& c, float r) {
  Vec3 oc = o - c;
  float b = dot(oc, d);
  float cc = dot(oc, oc) - r * r;
  float disc = b * b - cc;
  if (disc < 0.0f) return -1.0f;
  float s = std::sqrt(disc);
  float t0 = -b - s, t1 = -b + s;
  if (t0 > 1e-4f) return t0;
  if (t1 > 1e-4f) return t1;
  return -1.0f;
}

float sphereConePdf(const Vec3& p, const Vec3& c, float r) {
  float d2 = length2(c - p);
  if (d2 <= r * r) return 0.0f;
  float sin2 = r * r / d2;
  float oneMinusCos = sin2 < 1e-4f ? 0.5f * sin2 : 1.0f - std::sqrt(1.0f - sin2);
  return 1.0f / (kTwoPi * oneMinusCos);
}

std::vector<std::string> splitNames(const std::string& list) {
  std::vector<std::string> out;
  std::string cur;
  std::istringstream in(list);
  while (std::getline(in, cur, ',')) {
    while (!cur.empty() && std::isspace(static_cast<unsigned char>(cur.front()))) cur.erase(cur.begin());
    while (!cur.empty() && std::isspace(static_cast<unsigned char>(cur.back()))) cur.pop_back();
    if (!cur.empty()) out.push_back(cur);
  }
  return out;
}

}  // namespace

bool LightLink::listed(uint32_t node) const { return std::binary_search(nodes.begin(), nodes.end(), node); }

Vec3 Light::areaRadiance(const Vec3& local, float cosL) const {
  Vec3 L = color;
  if (texture) L *= texture->color(Vec2(local.x * 0.5f + 0.5f, local.y * 0.5f + 0.5f), true);
  if (spreadK > 0.0f) L *= (spreadK + 2.0f) * 0.5f * std::pow(std::max(cosL, 0.0f), spreadK);
  return L;
}

bool Light::sample(const Vec3& p, float u1, float u2, LightSample& s) const {
  s = LightSample();
  switch (type) {
    case Type::Rect:
    case Type::Disc:
    case Type::Portal: {
      float a, b;
      if (type == Type::Disc) {
        Vec2 d = sampleConcentricDisk(u1, u2);
        a = d.x;
        b = d.y;
      } else {
        a = 2.0f * u1 - 1.0f;
        b = 2.0f * u2 - 1.0f;
      }
      Vec3 q = position + ex * a + ey * b;
      Vec3 d = q - p;
      float d2 = length2(d);
      if (d2 <= 1e-12f) return false;
      float dist = std::sqrt(d2);
      s.wi = d / dist;
      float cosL = -dot(s.wi, n);
      if (cosL <= 0.0f) {
        if (!doubleSided) return false;
        cosL = -cosL;
      }
      if (cosL < 1e-6f) return false;
      s.dist = dist;
      s.pdf = d2 / (cosL * area);
      if (type == Type::Portal) {
        s.radiance = env ? env->eval(s.wi) : Vec3(0.0f);
        s.dist = kInfinity;  // the environment is beyond the opening
      } else {
        s.radiance = areaRadiance(Vec3(a, b, 0.0f), cosL);
      }
      return !isBlack(s.radiance);
    }
    case Type::Sphere: {
      Vec3 toC = position - p;
      float dc2 = length2(toC);
      if (dc2 <= radius * radius * 1.0001f) return false;  // inside the light
      float dc = std::sqrt(dc2);
      Vec3 axis = toC / dc;
      float sin2 = radius * radius / dc2;
      float cosMax = std::sqrt(std::max(0.0f, 1.0f - sin2));
      Frame f(axis);
      s.wi = normalize(f.toWorld(sampleUniformCone(u1, u2, cosMax)));
      float t = raySphere(p, s.wi, position, radius);
      if (t < 0.0f) t = dc - radius;  // grazing numerical miss
      s.dist = t;
      s.pdf = sphereConePdf(p, position, radius);
      s.radiance = color;
      return s.pdf > 0.0f;
    }
    case Type::Omni:
    case Type::Spot:
    case Type::Ies: {
      Vec3 lp = position;
      if (radius > 0.0f) lp += sampleUniformSphere(u1, u2) * radius;
      Vec3 d = lp - p;
      float d2 = length2(d);
      if (d2 <= 1e-12f) return false;
      float dist = std::sqrt(d2);
      s.wi = d / dist;
      s.dist = dist;
      s.delta = true;
      // Intensity convention: the multiplier is the illuminance (lux) at 100 scene units (1 m),
      // i.e. the intensity in candela.
      s.radiance = decay ? color * (1e4f / d2) : color;
      if (type == Type::Spot) {
        float cosA = -dot(s.wi, dir);
        float f = smoothstepf(cosFall, cosHot, cosA);
        if (f <= 0.0f) return false;
        s.radiance *= f;
      } else if (type == Type::Ies && ies) {
        Vec3 v = -s.wi;  // from the light toward the point
        float lxv = dot(v, lx), lyv = dot(v, ly), lzv = dot(v, lz);
        float vert = degrees(std::acos(clampf(-lzv, -1.0f, 1.0f)));
        float horiz = degrees(std::atan2(lyv, lxv));
        s.radiance *= ies->intensity(vert, horiz) * iesScale;
      }
      return !isBlack(s.radiance);
    }
    case Type::Sun: {
      Frame f(dir);
      s.wi = cosSun < 1.0f ? normalize(f.toWorld(sampleUniformCone(u1, u2, cosSun))) : dir;
      s.dist = kInfinity;
      s.delta = true;
      s.radiance = color;
      return true;
    }
    case Type::Mesh: {
      if (triCdf.empty()) return false;
      float u = u1;
      size_t t = static_cast<size_t>(std::upper_bound(triCdf.begin(), triCdf.end(), u) - triCdf.begin());
      t = std::min(t, triCdf.size() - 1);
      float lo = t > 0 ? triCdf[t - 1] : 0.0f, hi = triCdf[t];
      float ur = hi > lo ? (u - lo) / (hi - lo) : 0.5f;
      float r1 = std::sqrt(clampf(ur, 0.0f, 1.0f)), r2 = u2;
      Vec2 bary(r1 * (1.0f - r2), r1 * r2);
      const Vec3 &a = tri[t * 3], &b = tri[t * 3 + 1], &c = tri[t * 3 + 2];
      Vec3 q = a * (1.0f - bary.x - bary.y) + b * bary.x + c * bary.y;
      Vec3 ng = cross(b - a, c - a);
      float l = length(ng);
      if (l <= 0.0f) return false;
      ng /= l;
      Vec3 d = q - p;
      float d2 = length2(d);
      if (d2 <= 1e-12f) return false;
      float dist = std::sqrt(d2);
      s.wi = d / dist;
      float cosL = -dot(s.wi, ng);
      bool front = cosL > 0.0f;
      if (!front && !doubleSided) return false;
      cosL = std::fabs(cosL);
      if (cosL < 1e-6f) return false;
      s.dist = dist;
      s.pdf = meshPdf(d2, cosL);
      s.radiance = meshRadiance ? meshRadiance(static_cast<int>(t), bary, front) : color;
      return !isBlack(s.radiance) && s.pdf > 0.0f;
    }
  }
  return false;
}

float Light::meshPdf(float dist2, float cosLight) const {
  if (area <= 0.0f || cosLight <= 1e-6f) return 0.0f;
  return dist2 / (cosLight * area);
}

bool Light::intersect(const Vec3& o, const Vec3& d, float tMax, float& t, Vec3& Le, float& pdf) const {
  if (type == Type::Rect || type == Type::Disc || type == Type::Portal) {
    float denom = dot(d, n);
    if (std::fabs(denom) < 1e-8f) return false;
    float tt = dot(position - o, n) / denom;
    if (tt <= 1e-4f || tt >= tMax) return false;
    Vec3 q = o + d * tt - position;
    float a = dot(q, ex) / length2(ex), b = dot(q, ey) / length2(ey);
    if (type == Type::Disc) {
      if (a * a + b * b > 1.0f) return false;
    } else if (std::fabs(a) > 1.0f || std::fabs(b) > 1.0f) {
      return false;
    }
    float cosL = -denom;  // > 0 when the ray hits the emitting side
    if (cosL <= 0.0f) {
      if (!doubleSided) return false;
      cosL = -cosL;
    }
    t = tt;
    Le = type == Type::Portal ? Vec3(0.0f) : areaRadiance(Vec3(a, b, 0.0f), cosL);
    pdf = tt * tt / (cosL * area);
    return true;
  }
  if (type == Type::Sphere) {
    float tt = raySphere(o, d, position, radius);
    if (tt <= 0.0f || tt >= tMax) return false;
    t = tt;
    Le = color;
    pdf = sphereConePdf(o, position, radius);
    return true;
  }
  return false;
}

float Light::power() const {
  float l = luminance(color);
  switch (type) {
    case Type::Rect:
    case Type::Disc: return l * area * kPi * (doubleSided ? 2.0f : 1.0f) * (texture ? 0.5f : 1.0f);
    case Type::Sphere: return l * 4.0f * kPi * radius * radius * kPi;
    case Type::Omni: return l * 4.0f * kPi * 1e4f;
    case Type::Spot: return l * kTwoPi * (1.0f - 0.5f * (cosHot + cosFall)) * 1e4f;
    case Type::Ies: return l * (ies ? ies->totalFlux() * iesScale : 1.0f) * 1e4f;
    case Type::Sun: return l * 1e6f;
    case Type::Mesh: return l * area * kPi * (doubleSided ? 2.0f : 1.0f);
    case Type::Portal: return (env ? 1.0f : 0.0f) * area * kPi * 2.0f;
  }
  return l;
}

Vec3 physicalSunIrradiance(float elevationRad, float turbidity, float ozone) {
  if (elevationRad < -0.02f) return Vec3(0.0f);
  Vec3 sd(std::cos(elevationRad), 0.0f, std::sin(std::max(elevationRad, 0.0f)));
  AtmosphereParams ap;
  ap.turbidity = turbidity;
  ap.ozone = ozone;
  Vec3 T = atmosphereTransmittance(normalize(sd), ap);
  float fade = elevationRad < 0.0f ? clampf(1.0f + elevationRad / 0.02f, 0.0f, 1.0f) : 1.0f;
  return T * (kSolarIlluminance * fade);
}

// ---------------------------------------------------------------------------------------------

LightBuildResult collectLights(const Scene& scene, TextureCache& cache, std::vector<std::string>* warnings) {
  LightBuildResult R;
  R.groupNames = {kEnvironmentGroup, kSelfIllumGroup};
  std::map<std::string, int> groupIndex{{kEnvironmentGroup, 0}, {kSelfIllumGroup, 1}};
  auto groupFor = [&](const std::string& name) {
    auto it = groupIndex.find(name);
    if (it != groupIndex.end()) return it->second;
    int idx = static_cast<int>(R.groupNames.size());
    if (idx >= 32) return 1;  // too many groups: merge into self-illumination (rare)
    R.groupNames.push_back(name);
    groupIndex[name] = idx;
    return idx;
  };
  auto linkFrom = [&](const ParamBlock& p) {
    LightLink link;
    if (!p.has("excludeList")) return link;
    link.include = p.getInt("excludeMode") == 1;
    link.affects = p.getInt("excludeAffects");
    for (const auto& nm : splitNames(p.getString("excludeList")))
      if (const Node* n = scene.findNodeByName(nm)) link.nodes.push_back(n->id);
    std::sort(link.nodes.begin(), link.nodes.end());
    if (!link.include && link.nodes.empty()) link = LightLink();
    return link;
  };
  auto applyCommon = [&](Light& l, const Node& node, const ParamBlock& p) {
    l.nodeId = node.id;
    l.name = node.name;
    l.castShadows = p.getBool("castShadows");
    if (p.has("affectDiffuse")) {
      l.affectDiffuse = p.getBool("affectDiffuse");
      l.affectSpecular = p.getBool("affectSpecular");
      l.affectReflections = p.getBool("affectReflections");
      l.diffuseMult = p.getFloat("diffuseContribution");
      l.specularMult = p.getFloat("specularContribution");
    }
    l.link = linkFrom(p);
    std::string g = p.has("lightGroup") ? p.getString("lightGroup") : std::string();
    l.group = groupFor(g.empty() ? node.name : g);
  };
  auto lightColor = [](const ParamBlock& p) {
    Vec3 c = p.has("colorMode") && p.getInt("colorMode") == 1 ? kelvinToRgb(p.getFloat("temperature")) : p.getColor("color");
    return c;
  };

  bool physicalSunFound = false;
  for (const auto& nodePtr : scene.nodes) {
    const Node& node = *nodePtr;
    const LightObject* lo = node.light();
    if (!lo) continue;
    const ParamBlock& p = lo->params;
    if (!p.getBool("enabled")) continue;
    Mat4 M = node.worldMatrix();
    std::string type = lo->type();
    Vec3 minusZ = normalize(M.transformDir(Vec3(0, 0, -1)));
    float maxScale = std::max(length(M.column(0)), std::max(length(M.column(1)), length(M.column(2))));
    Light l;
    l.position = M.translation();

    if (type == "VeraaLight") {
      auto shape = static_cast<VeraaLightShape>(p.getInt("lightType"));
      Vec3 color = lightColor(p) * p.getFloat("intensity");
      auto units = static_cast<LightUnits>(p.getInt("units"));
      bool invisible = p.getBool("invisible");
      if (shape == VeraaLightShape::Dome) {
        if (R.hasDome) {
          if (warnings) warnings->push_back("Only one dome light is used: " + node.name + " is ignored");
          continue;
        }
        R.hasDome = true;
        R.domeTexture = p.getString("domeTexture");
        R.domeColor = color;
        R.domeRotation = p.getFloat("domeRotation");
        R.domeSpherical = p.getBool("domeSpherical");
        R.domeInvisible = invisible;
        R.domeNode = node.id;
        std::string g = p.getString("lightGroup");
        R.domeGroup = groupFor(g.empty() ? node.name : g);
        continue;
      }
      l.invisible = invisible;
      l.doubleSided = p.getBool("doubleSided");
      applyCommon(l, node, p);
      float dirSpread = p.getFloat("directional");
      if (dirSpread > 0.0f) {
        float k = 1.0f / ((1.0f - std::min(dirSpread, 0.995f)) * (1.0f - std::min(dirSpread, 0.995f))) - 1.0f;
        l.spreadK = std::min(k, 4000.0f);
      }
      int sides = l.doubleSided ? 2 : 1;
      if (shape == VeraaLightShape::Plane || shape == VeraaLightShape::Disc) {
        bool disc = shape == VeraaLightShape::Disc;
        l.type = p.getBool("skylightPortal") ? Light::Type::Portal : (disc ? Light::Type::Disc : Light::Type::Rect);
        if (disc) {
          float r = p.getFloat("lightRadius");
          l.ex = M.transformDir(Vec3(r, 0, 0));
          l.ey = M.transformDir(Vec3(0, r, 0));
          l.area = kPi * length(l.ex) * length(l.ey);
        } else {
          l.ex = M.transformDir(Vec3(p.getFloat("width") * 0.5f, 0, 0));
          l.ey = M.transformDir(Vec3(0, p.getFloat("length") * 0.5f, 0));
          l.area = 4.0f * length(l.ex) * length(l.ey);
        }
        l.n = minusZ;
        if (l.area <= 0.0f) continue;
        l.color = color * luminanceFromUnits(units, 1.0f, l.area * 1e-4f, sides);
        const std::string& tex = p.getString("textureFile");
        if (!tex.empty() && l.type != Light::Type::Portal) l.texture = cache.get(scene.resolvePath(tex), warnings);
        if (l.type == Light::Type::Portal) l.doubleSided = false;
      } else if (shape == VeraaLightShape::Sphere) {
        l.type = Light::Type::Sphere;
        l.radius = p.getFloat("lightRadius") * maxScale;
        if (l.radius <= 0.0f) continue;
        float areaM2 = 4.0f * kPi * l.radius * l.radius * 1e-4f;
        l.color = color * luminanceFromUnits(units, 1.0f, areaM2, 1);
      } else {  // Mesh
        const Node* src = scene.findNodeByName(p.getString("meshNode"));
        if (!src || src->kind() != ObjectKind::Geometry) {
          if (warnings) warnings->push_back("Mesh light " + node.name + " has no valid mesh object");
          continue;
        }
        l.type = Light::Type::Mesh;
        l.color = color;  // converted to luminance once the area is known (renderer)
        R.meshRequests.push_back({static_cast<int>(R.lights.size()), src->id});
        R.hiddenByMeshLights.push_back(src->id);
      }
      R.lights.push_back(std::move(l));
      continue;
    }

    if (type == "VeraaIES") {
      auto prof = std::make_shared<IesProfile>();
      std::string err;
      std::string file = p.getString("iesFile");
      if (file.empty() || !prof->load(scene.resolvePath(file), &err)) {
        if (warnings) warnings->push_back(file.empty() ? "IES light " + node.name + " has no IES file" : err);
        continue;
      }
      l.type = Light::Type::Ies;
      l.ies = prof;
      l.lx = normalize(M.transformDir(Vec3(1, 0, 0)));
      l.ly = normalize(M.transformDir(Vec3(0, 1, 0)));
      l.lz = normalize(M.transformDir(Vec3(0, 0, 1)));
      l.radius = p.getFloat("shapeRadius");
      l.decay = true;
      float scale = 1.0f;
      switch (p.getInt("powerMode")) {
        case 1: scale = p.getFloat("power") / std::max(prof->totalFlux(), 1e-6f); break;
        case 2: scale = p.getFloat("power") / std::max(prof->maxCandela, 1e-6f); break;
        default: break;
      }
      l.iesScale = scale;
      l.color = lightColor(p) * p.getFloat("intensity");
      applyCommon(l, node, p);
      R.lights.push_back(std::move(l));
      continue;
    }

    if (type == "VeraaSun" || type == "SunPositioner") {
      const auto* so = static_cast<const VeraaSunObject*>(lo);
      Vec3 dirToSun = so->sunDirection(-minusZ);
      float elev = std::asin(clampf(dirToSun.z, -1.0f, 1.0f));
      float turb = p.getFloat("turbidity"), ozone = p.getFloat("ozone");
      Vec3 E = physicalSunIrradiance(elev, turb, ozone) * p.getColor("filterColor") * p.getFloat("intensity");
      l.type = Light::Type::Sun;
      l.dir = dirToSun;
      l.cosSun = std::cos(radians(0.265f * p.getFloat("sizeMultiplier")));
      l.color = E;
      l.invisible = p.getBool("invisible");
      applyCommon(l, node, p);
      if (!physicalSunFound) {
        physicalSunFound = true;
        R.hasSun = true;
        R.physicalSun = true;
        R.sunDir = dirToSun;
        R.sunIrradiance = luminance(E);
        R.sunTurbidity = turb;
        R.sunOzone = ozone;
        R.sunIntensity = p.getFloat("intensity");
        R.sunSize = p.getFloat("sizeMultiplier");
        R.sunSkyModel = p.getInt("skyModel");
        R.sunGroundAlbedo = p.getColor("groundAlbedo");
        R.sunFilter = p.getColor("filterColor");
        R.sunInvisible = p.getBool("invisible");
        R.sunCos = l.cosSun;
        R.sunColor = E;
      }
      if (maxComponent(E) > 0.0f) R.lights.push_back(std::move(l));
      continue;
    }

    // Standard lights.
    float intensity = p.getFloat("intensity");
    Vec3 color = p.getColor("color") * intensity;
    if (maxComponent(color) <= 0.0f) continue;
    l.color = color;
    applyCommon(l, node, p);
    if (type == "RectLight") {
      l.type = Light::Type::Rect;
      l.ex = M.transformDir(Vec3(p.getFloat("width") * 0.5f, 0, 0));
      l.ey = M.transformDir(Vec3(0, p.getFloat("length") * 0.5f, 0));
      l.n = minusZ;
      l.area = 4.0f * length(l.ex) * length(l.ey);
      l.doubleSided = p.getBool("doubleSided");
      l.invisible = p.getBool("invisible");
      if (l.area <= 0.0f) continue;
    } else if (type == "SphereLight") {
      l.type = Light::Type::Sphere;
      l.radius = p.getFloat("radius") * maxScale;
      l.invisible = p.getBool("invisible");
      if (l.radius <= 0.0f) continue;
    } else if (type == "OmniLight" || type == "SpotLight") {
      l.type = type == "OmniLight" ? Light::Type::Omni : Light::Type::Spot;
      l.radius = p.getFloat("radius");
      l.decay = p.getInt("decay") == 1;
      if (l.type == Light::Type::Spot) {
        l.dir = minusZ;
        float hot = p.getFloat("hotspot"), fall = std::max(p.getFloat("falloff"), hot + 0.01f);
        l.cosHot = std::cos(radians(hot * 0.5f));
        l.cosFall = std::cos(radians(fall * 0.5f));
      }
    } else if (type == "SunLight") {
      l.type = Light::Type::Sun;
      l.dir = -minusZ;
      l.cosSun = std::cos(radians(0.265f * p.getFloat("sizeMultiplier")));
      if (!R.hasSun) {
        R.hasSun = true;
        R.sunDir = l.dir;
        R.sunIrradiance = luminance(color);
      }
    } else {
      continue;
    }
    R.lights.push_back(std::move(l));
  }
  return R;
}

}  // namespace render
}  // namespace veraa
