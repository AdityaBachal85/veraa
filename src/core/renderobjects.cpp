#include "renderobjects.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>

#include "photometry.h"
#include "registry.h"

namespace veraa {

namespace {

void addGeneral(ParamBlock& p, float intensity, const Vec3& color = Vec3(1.0f)) {
  p.beginGroup("General");
  p.addBool("enabled", "On", true);
  p.addBool("castShadows", "Cast shadows", true);
  p.beginGroup("Intensity");
  p.addColor("color", "Color", color);
  p.addEnum("colorMode", "Color mode", {"Color", "Temperature"}, 0);
  p.addFloat("temperature", "Temperature (K)", 6500.0, 1000.0, 40000.0, 100.0, 0);
  p.addFloat("intensity", "Multiplier", intensity, 0.0, 1e9, 0.1, 3);
}

void addAffectAndLinking(ParamBlock& p) {
  p.beginGroup("Affect");
  p.addBool("affectDiffuse", "Affect diffuse", true);
  p.addFloat("diffuseContribution", "Diffuse contribution", 1.0, 0.0, 100.0, 0.05);
  p.addBool("affectSpecular", "Affect specular", true);
  p.addFloat("specularContribution", "Specular contribution", 1.0, 0.0, 100.0, 0.05);
  p.addBool("affectReflections", "Affect reflections", true);
  p.beginGroup("Include / exclude");
  p.addEnum("excludeMode", "Mode", {"Exclude", "Include"}, 0);
  p.addString("excludeList", "Objects (names, comma separated)", "");
  p.addEnum("excludeAffects", "Affects", {"Illumination and shadows", "Illumination", "Shadows"}, 0);
  p.beginGroup("Light Mix");
  p.addString("lightGroup", "Light group (empty = own group)", "");
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// VeraaLight

VeraaLightObject::VeraaLightObject() {
  ParamBlock& p = params;
  p.beginGroup("General");
  p.addEnum("lightType", "Type", {"Plane", "Disc", "Sphere", "Mesh", "Dome"}, 0);
  addGeneral(p, 5.0f);
  p.beginGroup("Intensity");
  p.addEnum("units", "Units",
            {"Default (image)", "Luminous power (lm)", "Luminance (cd/m²)", "Radiant power (W)", "Radiance (W/m²/sr)"},
            0);
  p.addFile("textureFile", "Texture (plane, disc, dome)", "Images (*.png *.jpg *.jpeg *.bmp *.tga *.hdr *.exr)");
  p.beginGroup("Size");
  p.addFloat("width", "Width (X)", 100.0, 0.01, 1e7, 1.0);
  p.addFloat("length", "Length (Y)", 100.0, 0.01, 1e7, 1.0);
  p.addFloat("lightRadius", "Radius (disc, sphere)", 20.0, 0.01, 1e7, 0.5);
  p.addString("meshNode", "Mesh object (name)", "");
  p.beginGroup("Dome");
  p.addFile("domeTexture", "Dome HDRI", "HDR images (*.hdr *.exr);;Images (*.png *.jpg *.jpeg)");
  p.addFloat("domeRotation", "Rotation", 0.0, -360.0, 360.0, 1.0, 1);
  p.addBool("domeSpherical", "Full sphere (off = hemisphere)", true);
  p.beginGroup("Options");
  p.addBool("doubleSided", "Double-sided", false);
  p.addBool("invisible", "Invisible", false);
  p.addBool("noDecay", "No decay", false);
  p.addBool("skylightPortal", "Skylight portal", false);
  p.addFloat("directional", "Directional (spread)", 0.0, 0.0, 1.0, 0.05).tooltip =
      "0 = emits in all directions; 1 = a narrow beam along the light's -Z axis";
  addAffectAndLinking(p);
}

// ---------------------------------------------------------------------------------------------
// VeraaIES

VeraaIesObject::VeraaIesObject() {
  ParamBlock& p = params;
  addGeneral(p, 1.0f);
  p.beginGroup("Photometric web");
  p.addFile("iesFile", "IES file", "IES photometric files (*.ies)");
  p.addEnum("powerMode", "Intensity from", {"File", "Luminous power (lm)", "Peak intensity (cd)"}, 0);
  p.addFloat("power", "Power value", 1500.0, 0.0, 1e9, 10.0, 1);
  p.addFloat("shapeRadius", "Shape radius (soft shadows)", 0.0, 0.0, 1e5, 0.5);
  addAffectAndLinking(p);
}

// ---------------------------------------------------------------------------------------------
// VeraaSun / Sun Positioner

VeraaSunObject::VeraaSunObject(bool positioner) : positioner_(positioner) {
  ParamBlock& p = params;
  p.beginGroup("General");
  p.addBool("enabled", "On", true);
  p.addBool("castShadows", "Cast shadows", true);
  p.addBool("invisible", "Invisible", false);
  p.beginGroup("Sun");
  p.addFloat("intensity", "Intensity multiplier", 1.0, 0.0, 1e6, 0.01, 3);
  p.addFloat("sizeMultiplier", "Size multiplier", 1.0, 0.0, 100.0, 0.1);
  p.addColor("filterColor", "Filter color", Vec3(1.0f));
  p.addFloat("turbidity", "Turbidity", 2.5, 1.7, 20.0, 0.1);
  p.addFloat("ozone", "Ozone", 0.35, 0.0, 1.0, 0.05);
  // Values are stored in files: 1 was the single scattering model, now the full physical atmosphere.
  p.addEnum("skyModel", "Sky model", {"Preetham et al.", "Physical atmosphere", "Hosek-Wilkie 2012"}, 1).tooltip =
      "Physical atmosphere: spectral single and multiple scattering with ozone, valid through sunset and "
      "twilight. Hosek-Wilkie: the analytic clear-sky fit (sun above the horizon). Preetham: the classic "
      "analytic model";
  p.addColor("groundAlbedo", "Ground albedo", Vec3(0.2f));
  p.addFloat("skyIntensity", "Sky intensity multiplier", 1.0, 0.0, 1e6, 0.05, 3).tooltip =
      "Scales the sky only (the sun keeps its intensity)";
  p.addFloat("horizonBlur", "Horizon blur (deg)", 1.5, 0.0, 30.0, 0.1, 2).tooltip =
      "Width of the soft transition from the horizon haze to the ground below the horizon";
  p.beginGroup("Location and time");
  p.addEnum("positionMode", "Position", {"Manual (node orientation)", "Date, time and location"}, positioner ? 1 : 0);
  p.addFloat("latitude", "Latitude", 48.86, -90.0, 90.0, 0.01, 3);
  p.addFloat("longitude", "Longitude (east +)", 2.35, -180.0, 180.0, 0.01, 3);
  p.addFloat("timezone", "Time zone (UTC +h)", 1.0, -12.0, 14.0, 0.5, 1);
  p.addBool("daylightSaving", "Daylight saving time", true);
  p.addInt("year", "Year", 2024, 1900, 2200);
  p.addInt("month", "Month", 6, 1, 12);
  p.addInt("day", "Day", 21, 1, 31);
  p.addFloat("hour", "Time (hours)", 15.0, 0.0, 24.0, 0.25, 2);
  p.addFloat("northDirection", "North direction", 0.0, -360.0, 360.0, 1.0, 1);
  p.addFloat("compassRadius", "Compass radius", 150.0, 1.0, 1e6, 10.0);
  addAffectAndLinking(p);
}

void VeraaSunObject::solarAngles(float& azimuthDeg, float& elevationDeg) const {
  SolarInput in;
  in.year = params.getInt("year");
  in.month = params.getInt("month");
  in.day = params.getInt("day");
  in.hour = params.getFloat("hour");
  in.timezone = params.getFloat("timezone");
  in.daylightSaving = params.getBool("daylightSaving");
  in.latitude = params.getFloat("latitude");
  in.longitude = params.getFloat("longitude");
  SolarPosition sp = solarPosition(in);
  azimuthDeg = sp.azimuthDeg;
  elevationDeg = sp.elevationDeg;
}

Vec3 VeraaSunObject::sunDirection(const Vec3& nodeDir) const {
  if (params.getInt("positionMode") != 1) return normalize(nodeDir);
  float az, el;
  solarAngles(az, el);
  return solarDirection(az, el, params.getFloat("northDirection"));
}

// ---------------------------------------------------------------------------------------------
// VeraaPhysicalCamera

VeraaPhysicalCameraObject::VeraaPhysicalCameraObject() {
  params = ParamBlock();
  ParamBlock& p = params;
  p.beginGroup("Basic");
  p.addEnum("cameraType", "Type", {"Still camera", "Cinematic camera"}, 0);
  p.beginGroup("Sensor and lens");
  p.addFloat("filmGate", "Film gate (mm)", 36.0, 1.0, 1000.0, 0.5, 2);
  p.addFloat("focalLength", "Focal length (mm)", 40.0, 1.0, 10000.0, 1.0, 1);
  p.addFloat("zoomFactor", "Zoom factor", 1.0, 0.01, 100.0, 0.05, 3);
  p.addBool("fovOverride", "Specify field of view", false);
  p.addFloat("fov", "Field of view (horizontal)", 48.5, 1.0, 175.0, 1.0, 1);
  p.beginGroup("Aperture and focus");
  p.addFloat("fNumber", "F-number", 8.0, 0.5, 128.0, 0.1, 2);
  p.addBool("dof", "Depth of field", false);
  p.addFloat("focusDistance", "Focus distance", 300.0, 0.01, 1e8, 1.0);
  p.addInt("blades", "Aperture blades (0 = round)", 0, 0, 16);
  p.addFloat("bladeRotation", "Blade rotation", 0.0, -360.0, 360.0, 1.0, 1);
  p.addFloat("bokehAnisotropy", "Bokeh anisotropy", 0.0, -0.99, 0.99, 0.05);
  p.beginGroup("Exposure");
  p.addBool("exposure", "Exposure", true);
  p.addFloat("shutterSpeed", "Shutter speed (1/s)", 200.0, 0.0001, 1e6, 1.0, 1);
  p.addFloat("shutterAngle", "Shutter angle (deg, cinematic)", 180.0, 1.0, 360.0, 1.0, 1);
  p.addFloat("iso", "Film speed (ISO)", 100.0, 1.0, 1e6, 10.0, 0);
  p.addEnum("whiteBalanceMode", "White balance", {"Neutral", "Daylight (5500 K)", "Temperature", "Custom color"}, 0);
  p.addFloat("whiteBalanceK", "White balance temperature (K)", 6500.0, 1000.0, 40000.0, 100.0, 0);
  p.addColor("whiteBalanceColor", "White balance color", Vec3(1.0f));
  p.addBool("vignettingOn", "Vignetting", true);
  p.addFloat("vignetting", "Vignetting amount", 1.0, 0.0, 3.0, 0.05);
  p.beginGroup("Tilt and shift");
  p.addFloat("verticalShift", "Vertical shift", 0.0, -2.0, 2.0, 0.01, 3);
  p.addFloat("horizontalShift", "Horizontal shift", 0.0, -2.0, 2.0, 0.01, 3);
  p.addBool("autoVerticalTilt", "Auto vertical tilt (straight verticals)", false);
  p.beginGroup("Distortion");
  p.addFloat("distortion", "Distortion (+ barrel, - pincushion)", 0.0, -1.0, 1.0, 0.01, 3);
  p.beginGroup("Motion blur");
  p.addBool("motionBlur", "Motion blur", false);
  p.beginGroup("Clipping");
  p.addBool("clipping", "Clipping", false);
  p.addFloat("nearClip", "Near clip", 0.0, 0.0, 1e8, 1.0);
  p.addFloat("farClip", "Far clip", 100000.0, 0.0, 1e9, 10.0);
}

float VeraaPhysicalCameraObject::horizontalFovDeg() const {
  if (params.getBool("fovOverride")) return params.getFloat("fov");
  float f = params.getFloat("focalLength") * params.getFloat("zoomFactor");
  return degrees(2.0f * std::atan(params.getFloat("filmGate") * 0.5f / std::max(f, 1e-3f)));
}

float VeraaPhysicalCameraObject::verticalFovDeg(float aspect) const {
  float h = radians(horizontalFovDeg());
  return degrees(2.0f * std::atan(std::tan(h * 0.5f) / std::max(aspect, 1e-6f)));
}

float VeraaPhysicalCameraObject::apertureRadius() const {
  float f = params.getFloat("focalLength") * params.getFloat("zoomFactor");  // mm
  return f / std::max(params.getFloat("fNumber"), 0.1f) * 0.5f * 0.1f;          // cm
}

float VeraaPhysicalCameraObject::shutterSeconds(float fps) const {
  if (params.getInt("cameraType") == 1) return params.getFloat("shutterAngle") / 360.0f / std::max(fps, 1.0f);
  return 1.0f / std::max(params.getFloat("shutterSpeed"), 1e-6f);
}

float VeraaPhysicalCameraObject::exposureMultiplier(float fps) const {
  if (!params.getBool("exposure")) return 1.0f;
  return exposureScale(ev100(params.getFloat("fNumber"), shutterSeconds(fps), params.getFloat("iso")));
}

Vec3 VeraaPhysicalCameraObject::whiteBalance() const {
  Vec3 white(1.0f);
  switch (params.getInt("whiteBalanceMode")) {
    case 1: white = kelvinToRgb(5500.0f); break;
    case 2: white = kelvinToRgb(params.getFloat("whiteBalanceK")); break;
    case 3: white = params.getColor("whiteBalanceColor"); break;
    default: break;
  }
  float lum = luminance(white);
  if (lum <= 0.0f) return Vec3(1.0f);
  white /= lum;
  return Vec3(1.0f / std::max(white.x, 1e-3f), 1.0f / std::max(white.y, 1e-3f), 1.0f / std::max(white.z, 1e-3f));
}

// ---------------------------------------------------------------------------------------------
// Proxy files

namespace {

std::mutex& proxyMutex() {
  static std::mutex m;
  return m;
}

struct ProxyCacheEntry {
  std::filesystem::file_time_type stamp;
  std::shared_ptr<const TriMesh> mesh;
};

std::map<std::string, ProxyCacheEntry>& proxyCache() {
  static std::map<std::string, ProxyCacheEntry> cache;
  return cache;
}

template <class T>
void writeRaw(std::ofstream& f, const T* data, size_t count) {
  f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(sizeof(T) * count));
}

template <class T>
bool readRaw(std::ifstream& f, T* data, size_t count) {
  f.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(sizeof(T) * count));
  return static_cast<bool>(f);
}

void addBoxMesh(PolyMesh& m, const Aabb& b) {
  Vec3 lo = b.lo, hi = b.hi;
  int base = static_cast<int>(m.positions.size());
  for (int i = 0; i < 8; ++i)
    m.positions.push_back(Vec3((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z));
  const int faces[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
  for (const auto& f : faces) m.addFace({base + f[0], base + f[1], base + f[2], base + f[3]}, {}, 0u);
}

}  // namespace

bool writeProxyFile(const std::string& path, const TriMesh& mesh, std::string* error) {
  std::ofstream f(std::filesystem::u8path(path), std::ios::binary);
  if (!f) {
    if (error) *error = "Cannot write " + path;
    return false;
  }
  uint32_t nv = static_cast<uint32_t>(mesh.positions.size()), nt = static_cast<uint32_t>(mesh.indices.size() / 3);
  f.write("VXP1", 4);
  writeRaw(f, &nv, 1);
  writeRaw(f, &nt, 1);
  for (const auto& p : mesh.positions) writeRaw(f, &p.x, 3);
  for (uint32_t i = 0; i < nv; ++i) {
    Vec3 n = i < mesh.normals.size() ? mesh.normals[i] : Vec3(0, 0, 1);
    writeRaw(f, &n.x, 3);
  }
  for (uint32_t i = 0; i < nv; ++i) {
    Vec2 t = i < mesh.uvs.size() ? mesh.uvs[i] : Vec2();
    writeRaw(f, &t.x, 2);
  }
  writeRaw(f, mesh.indices.data(), mesh.indices.size());
  for (uint32_t t = 0; t < nt; ++t) {
    int32_t id = t < mesh.triMatIds.size() ? mesh.triMatIds[t] : 0;
    writeRaw(f, &id, 1);
  }
  if (!f) {
    if (error) *error = "Error while writing " + path;
    return false;
  }
  return true;
}

std::shared_ptr<const TriMesh> loadProxyFile(const std::string& path, std::string* error) {
  std::error_code ec;
  auto fsPath = std::filesystem::u8path(path);
  auto stamp = std::filesystem::last_write_time(fsPath, ec);
  if (ec) {
    if (error) *error = "Proxy file not found: " + path;
    return nullptr;
  }
  {
    std::lock_guard<std::mutex> lock(proxyMutex());
    auto it = proxyCache().find(path);
    if (it != proxyCache().end() && it->second.stamp == stamp) return it->second.mesh;
  }
  std::ifstream f(fsPath, std::ios::binary);
  char magic[4] = {};
  uint32_t nv = 0, nt = 0;
  if (!f || !readRaw(f, magic, 4) || std::memcmp(magic, "VXP1", 4) != 0 || !readRaw(f, &nv, 1) || !readRaw(f, &nt, 1) ||
      nv > (1u << 28) || nt > (1u << 28)) {
    if (error) *error = "Not a Veraa proxy file: " + path;
    return nullptr;
  }
  auto mesh = std::make_shared<TriMesh>();
  mesh->positions.resize(nv);
  mesh->normals.resize(nv);
  mesh->uvs.resize(nv);
  mesh->indices.resize(static_cast<size_t>(nt) * 3);
  mesh->triMatIds.resize(nt);
  bool ok = true;
  for (auto& p : mesh->positions) ok = ok && readRaw(f, &p.x, 3);
  for (auto& n : mesh->normals) ok = ok && readRaw(f, &n.x, 3);
  for (auto& t : mesh->uvs) ok = ok && readRaw(f, &t.x, 2);
  ok = ok && readRaw(f, mesh->indices.data(), mesh->indices.size());
  std::vector<int32_t> ids(nt);
  ok = ok && readRaw(f, ids.data(), ids.size());
  if (!ok) {
    if (error) *error = "Truncated proxy file: " + path;
    return nullptr;
  }
  for (uint32_t i = 0; i < nt; ++i) mesh->triMatIds[i] = ids[i];
  for (uint32_t& i : mesh->indices)
    if (i >= nv) {
      if (error) *error = "Corrupt proxy file: " + path;
      return nullptr;
    }
  for (const auto& p : mesh->positions) mesh->bounds.expand(p);
  std::lock_guard<std::mutex> lock(proxyMutex());
  proxyCache()[path] = {stamp, mesh};
  return mesh;
}

VeraaProxyObject::VeraaProxyObject() {
  params.beginGroup("Proxy");
  params.addFile("file", "Proxy file", "Veraa proxy (*.vxp)");
  params.addEnum("displayMode", "Display", {"Bounding box", "Preview (some faces)", "Full mesh"}, 1);
  params.addInt("previewFaces", "Preview faces", 5000, 10, 10000000);
}

PolyMesh VeraaProxyObject::build() const {
  PolyMesh out;
  auto mesh = loadProxyFile(params.getString("file"));
  if (!mesh || mesh->empty()) {
    // Placeholder so the node stays visible and selectable.
    { Aabb box; box.expand(Vec3(-10.0f)); box.expand(Vec3(10.0f)); addBoxMesh(out, box); }
    return out;
  }
  int mode = params.getInt("displayMode");
  if (mode == 0) {
    addBoxMesh(out, mesh->bounds);
    return out;
  }
  size_t nt = mesh->triangleCount();
  size_t step = 1;
  if (mode == 1) step = std::max<size_t>(1, nt / std::max(params.getInt("previewFaces"), 1));
  out.positions = mesh->positions;
  out.uvs = mesh->uvs;
  for (size_t t = 0; t < nt; t += step) {
    int a = static_cast<int>(mesh->indices[t * 3]), b = static_cast<int>(mesh->indices[t * 3 + 1]),
        c = static_cast<int>(mesh->indices[t * 3 + 2]);
    out.addFace({a, b, c}, {a, b, c}, 1u, t < mesh->triMatIds.size() ? mesh->triMatIds[t] : 0);
  }
  return out;
}

// ---------------------------------------------------------------------------------------------
// Fur

VeraaFurObject::VeraaFurObject() {
  ParamBlock& p = params;
  p.beginGroup("Source");
  p.addString("sourceNode", "Grow on object (name)", "");
  p.addEnum("distribution", "Distribution", {"Per area", "Per face"}, 0);
  p.addFloat("density", "Strands per cm² (per area)", 4.0, 0.0, 1e5, 0.5);
  p.addInt("perFace", "Strands per face (per face)", 20, 0, 100000);
  p.addInt("maxStrands", "Max strands", 300000, 1, 50000000);
  p.beginGroup("Geometry");
  p.addFloat("length", "Length", 4.0, 0.001, 1e5, 0.1);
  p.addFloat("lengthVariation", "Length variation", 0.3, 0.0, 1.0, 0.05);
  p.addFloat("thickness", "Thickness", 0.06, 0.0001, 100.0, 0.01, 3);
  p.addFloat("taper", "Taper", 0.8, 0.0, 1.0, 0.05);
  p.addInt("knots", "Knots", 6, 2, 64);
  p.addFloat("gravity", "Gravity", 0.4, -10.0, 10.0, 0.05);
  p.addFloat("directionVariation", "Direction variation", 0.25, 0.0, 3.0, 0.05);
  p.addFloat("curlRadius", "Curl radius", 0.0, 0.0, 100.0, 0.05);
  p.addFloat("curlTurns", "Curl turns", 2.0, 0.0, 50.0, 0.1);
  p.addInt("seed", "Seed", 1, 0, 1000000);
  p.beginGroup("Viewport");
  p.addInt("previewStrands", "Preview strands", 1500, 0, 100000);
}

namespace {
// Small deterministic generator (SplitMix64).
struct SplitMix {
  uint64_t s;
  explicit SplitMix(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ULL + 0x632BE59BD9B4E019ULL) {}
  uint64_t next() {
    uint64_t z = (s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }
  float uniform() { return static_cast<float>(next() >> 40) * (1.0f / 16777216.0f); }
};
}  // namespace

std::vector<FurStrand> generateFur(const ParamBlock& p, const TriMesh& mesh, size_t maxStrands) {
  std::vector<FurStrand> out;
  size_t nt = mesh.triangleCount();
  if (nt == 0) return out;
  std::vector<double> cdf(nt + 1, 0.0);
  for (size_t t = 0; t < nt; ++t) {
    const Vec3& a = mesh.positions[mesh.indices[t * 3]];
    const Vec3& b = mesh.positions[mesh.indices[t * 3 + 1]];
    const Vec3& c = mesh.positions[mesh.indices[t * 3 + 2]];
    cdf[t + 1] = cdf[t] + 0.5 * length(cross(b - a, c - a));
  }
  double area = cdf[nt];
  if (area <= 0.0) return out;
  double wanted = p.getInt("distribution") == 0 ? area * p.getFloat("density")
                                                 : static_cast<double>(nt) * p.getInt("perFace");
  size_t count = static_cast<size_t>(std::min<double>(wanted, p.getInt("maxStrands")));
  if (maxStrands > 0) count = std::min(count, maxStrands);
  out.reserve(count);
  SplitMix rng(static_cast<uint64_t>(p.getInt("seed")) + 17);
  int knots = std::max(p.getInt("knots"), 2);
  float len0 = p.getFloat("length"), lenVar = p.getFloat("lengthVariation"), thick = p.getFloat("thickness");
  float taper = p.getFloat("taper"), gravity = p.getFloat("gravity"), dirVar = p.getFloat("directionVariation");
  float curlR = p.getFloat("curlRadius"), curlTurns = p.getFloat("curlTurns");
  for (size_t i = 0; i < count; ++i) {
    double u = rng.uniform() * area;
    size_t t = static_cast<size_t>(std::upper_bound(cdf.begin(), cdf.end(), u) - cdf.begin());
    t = std::min(std::max<size_t>(t, 1), nt) - 1;
    uint32_t i0 = mesh.indices[t * 3], i1 = mesh.indices[t * 3 + 1], i2 = mesh.indices[t * 3 + 2];
    float r1 = std::sqrt(rng.uniform()), r2 = rng.uniform();
    float b0 = 1.0f - r1, b1 = r1 * (1.0f - r2), b2 = r1 * r2;
    Vec3 root = mesh.positions[i0] * b0 + mesh.positions[i1] * b1 + mesh.positions[i2] * b2;
    Vec3 n = mesh.normals.size() == mesh.positions.size()
                 ? mesh.normals[i0] * b0 + mesh.normals[i1] * b1 + mesh.normals[i2] * b2
                 : cross(mesh.positions[i1] - mesh.positions[i0], mesh.positions[i2] - mesh.positions[i0]);
    n = length2(n) > 0.0f ? normalize(n) : Vec3(0, 0, 1);
    Vec3 jitter(rng.uniform() * 2.0f - 1.0f, rng.uniform() * 2.0f - 1.0f, rng.uniform() * 2.0f - 1.0f);
    Vec3 dir = normalize(n + jitter * dirVar);
    float len = len0 * (1.0f - lenVar * rng.uniform());
    Vec3 tx, ty;
    makeBasis(dir, tx, ty);
    float phase = rng.uniform() * kTwoPi;
    FurStrand s;
    s.rootRadius = thick * 0.5f;
    s.tipRadius = s.rootRadius * (1.0f - taper);
    if (!mesh.uvs.empty()) s.rootUv = mesh.uvs[i0] * b0 + mesh.uvs[i1] * b1 + mesh.uvs[i2] * b2;
    s.points.reserve(knots);
    // Integrate the strand as segments of equal length bent by gravity (world -Z).
    Vec3 p0 = root, d = dir;
    float seg = len / (knots - 1);
    s.points.push_back(root);
    for (int k = 1; k < knots; ++k) {
      d = normalize(d + Vec3(0, 0, -gravity) * (1.0f / (knots - 1)) * 2.0f);
      p0 = p0 + d * seg;
      Vec3 q = p0;
      if (curlR > 0.0f) {
        float tk = static_cast<float>(k) / (knots - 1);
        float a = phase + kTwoPi * curlTurns * tk;
        q += (tx * std::cos(a) + ty * std::sin(a)) * (curlR * tk);
      }
      s.points.push_back(q);
    }
    out.push_back(std::move(s));
  }
  return out;
}

// ---------------------------------------------------------------------------------------------
// Infinite plane

VeraaInfinitePlaneObject::VeraaInfinitePlaneObject() {
  params.beginGroup("Infinite plane");
  params.addFloat("uvSize", "Texture size (cm per tile)", 100.0, 0.001, 1e7, 1.0);
  params.addFloat("displaySize", "Viewport display size", 2000.0, 1.0, 1e7, 10.0);
}

PolyMesh VeraaInfinitePlaneObject::build() const {
  PolyMesh m;
  float s = params.getFloat("displaySize") * 0.5f, uv = s / std::max(params.getFloat("uvSize"), 1e-3f);
  int a = m.addVertex(Vec3(-s, -s, 0)), b = m.addVertex(Vec3(s, -s, 0)), c = m.addVertex(Vec3(s, s, 0)),
      d = m.addVertex(Vec3(-s, s, 0));
  int ta = m.addUv(Vec2(-uv, -uv)), tb = m.addUv(Vec2(uv, -uv)), tc = m.addUv(Vec2(uv, uv)), td = m.addUv(Vec2(-uv, uv));
  m.addFace({a, b, c, d}, {ta, tb, tc, td}, 1u);
  return m;
}

// ---------------------------------------------------------------------------------------------
// Displacement modifier

VeraaDisplacementModifier::VeraaDisplacementModifier() {
  params.beginGroup("Displacement");
  params.addFloat("amount", "Amount", 2.0, -1e6, 1e6, 0.1);
  params.addFloat("shift", "Shift", 0.0, -1e6, 1e6, 0.1);
  params.addFile("bitmap", "Bitmap (when no map is set)", "Images (*.png *.jpg *.jpeg *.bmp *.tga *.tif *.tiff *.hdr *.exr)");
  params.addFloat("edgeLength", "Edge length (cm)", 1.0, 0.001, 1e5, 0.1, 3);
  params.addInt("maxSubdivs", "Max subdivision levels", 6, 0, 10);
  params.addBool("keepContinuity", "Keep continuity", true);
  params.addBool("useWaterLevel", "Water level", false);
  params.addFloat("waterLevel", "Water level value", 0.0, -1e6, 1e6, 0.01);
}

json VeraaDisplacementModifier::toJson() const {
  json j = Modifier::toJson();
  if (texmap.map) {
    j["texmap"] = texmap.map->toJson();
    if (!texmap.enabled) j["texmapEnabled"] = false;
  }
  return j;
}

void VeraaDisplacementModifier::fromJson(const json& j) {
  Modifier::fromJson(j);
  texmap = MapSlot();
  if (j.contains("texmap")) texmap.map = TexMap::fromJson(j["texmap"]);
  texmap.enabled = j.value("texmapEnabled", true);
}

// ---------------------------------------------------------------------------------------------
// Registration

void registerRenderObjectTypes() {
  const char* veraa = "Veraa";
  registerObjectType({"VeraaLight", "VeraaLight", "Lights", veraa, [] { return std::make_unique<VeraaLightObject>(); }});
  registerObjectType({"VeraaIES", "VeraaIES", "Lights", veraa, [] { return std::make_unique<VeraaIesObject>(); }});
  registerObjectType({"VeraaSun", "VeraaSun", "Lights", veraa, [] { return std::make_unique<VeraaSunObject>(false); }});
  registerObjectType(
      {"SunPositioner", "Sun Positioner", "Lights", "Photometric", [] { return std::make_unique<VeraaSunObject>(true); }});
  registerObjectType({"VeraaPhysicalCamera", "VeraaPhysicalCamera", "Cameras", veraa,
                      [] { return std::make_unique<VeraaPhysicalCameraObject>(); }});
  registerObjectType({"VeraaProxy", "VeraaProxy", "Geometry", veraa, [] { return std::make_unique<VeraaProxyObject>(); }});
  registerObjectType({"VeraaFur", "VeraaFur", "Geometry", veraa, [] { return std::make_unique<VeraaFurObject>(); }});
  registerObjectType({"VeraaInfinitePlane", "VeraaInfinitePlane", "Geometry", veraa,
                      [] { return std::make_unique<VeraaInfinitePlaneObject>(); }});
}

void registerRenderModifierTypes() {
  registerModifierType({"VeraaDisplacementMod", "VeraaDisplacementMod", "Rendering", true, false,
                        [] { return std::make_unique<VeraaDisplacementModifier>(); }});
}

}  // namespace veraa
