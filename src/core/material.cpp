#include "material.h"

#include <algorithm>

#include "registry.h"

namespace veraa {

namespace {

const char* kImageFilter = "Images (*.png *.jpg *.jpeg *.bmp *.tga *.tif *.tiff *.hdr *.exr)";

std::vector<MaterialTypeInfo>& registry() {
  static std::vector<MaterialTypeInfo> types;
  return types;
}

void ensureBuiltins() {
  static bool done = false;
  if (done) return;
  done = true;
  registerMaterialTypes();
}

void defineFromType(Material& m) {
  m.params = ParamBlock();
  m.subs.clear();
  const MaterialTypeInfo* info = findMaterialType(m.materialType);
  if (!info) {
    m.materialType = "VeraaMtl";
    info = findMaterialType(m.materialType);
  }
  if (info && info->define) info->define(m);
}

SubMaterialSlot subSlot(const std::string& label, std::unique_ptr<Material> m = nullptr, int matId = 1) {
  SubMaterialSlot s;
  s.label = label;
  s.material = std::move(m);
  s.matId = matId;
  return s;
}

std::unique_ptr<Material> coloredMtl(const std::string& name, const Vec3& color) {
  auto m = std::make_unique<Material>("VeraaMtl");
  m->name = name;
  m->params.setColor("diffuse", color);
  return m;
}

const std::string* bitmapFileIn(const TexMap* m, int depth = 0) {
  if (!m || depth > 16) return nullptr;
  if (m->type == "Bitmap" && !m->params.getString("file").empty()) return &m->params.getString("file");
  // Prefer the first color input of compositing maps.
  for (const auto& key : m->mappableKeys())
    if (const std::string* f = bitmapFileIn(m->mapFor(key), depth + 1)) return f;
  return nullptr;
}

void collectMapFiles(const TexMap* m, std::vector<std::string>& files) {
  if (!m) return;
  for (const auto& d : m->params.defs())
    if (d.type == ParamType::File && !m->params.getString(d.key).empty()) files.push_back(m->params.getString(d.key));
  for (const auto& [key, slot] : m->maps) collectMapFiles(slot.map.get(), files);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// SubMaterialSlot

SubMaterialSlot::SubMaterialSlot(const SubMaterialSlot& o)
    : material(o.material ? std::make_unique<Material>(*o.material) : nullptr),
      label(o.label),
      matId(o.matId),
      enabled(o.enabled) {}

SubMaterialSlot& SubMaterialSlot::operator=(const SubMaterialSlot& o) {
  if (this != &o) {
    material = o.material ? std::make_unique<Material>(*o.material) : nullptr;
    label = o.label;
    matId = o.matId;
    enabled = o.enabled;
  }
  return *this;
}

// ---------------------------------------------------------------------------------------------
// Material

Material::Material() : Material(std::string("VeraaMtl")) {}

Material::Material(const std::string& t) : materialType(t) { defineFromType(*this); }

Material::Material(const Material& o) = default;
Material& Material::operator=(const Material& o) = default;
Material::Material(Material&&) noexcept = default;
Material& Material::operator=(Material&&) noexcept = default;
Material::~Material() = default;

const TexMap* Material::mapFor(const std::string& key) const {
  auto it = maps.find(key);
  return it != maps.end() && it->second.active() ? it->second.map.get() : nullptr;
}

const std::vector<std::string>& Material::mappableKeys() const {
  static const std::vector<std::string> none;
  const MaterialTypeInfo* info = findMaterialType(materialType);
  return info ? info->mappable : none;
}

void Material::setMaterialType(const std::string& type) {
  if (type == materialType || !findMaterialType(type)) return;
  json oldParams = params.toJson();
  std::vector<SubMaterialSlot> oldSubs = std::move(subs);
  materialType = type;
  defineFromType(*this);
  params.fromJson(oldParams);
  const auto& keys = mappableKeys();
  for (auto it = maps.begin(); it != maps.end();)
    it = std::find(keys.begin(), keys.end(), it->first) == keys.end() ? maps.erase(it) : std::next(it);
  for (size_t i = 0; i < subs.size() && i < oldSubs.size(); ++i)
    if (oldSubs[i].material) subs[i].material = std::move(oldSubs[i].material);
}

json Material::toJson() const {
  json j{{"id", id}, {"name", name}, {"type", materialType}, {"params", params.toJson()}};
  json m = mapSlotsToJson(maps);
  if (!m.empty()) j["maps"] = std::move(m);
  if (!subs.empty()) {
    json s = json::array();
    for (const auto& slot : subs) {
      json js{{"label", slot.label}, {"matId", slot.matId}, {"enabled", slot.enabled}};
      js["material"] = slot.material ? slot.material->toJson() : json();
      s.push_back(std::move(js));
    }
    j["subs"] = std::move(s);
  }
  return j;
}

void Material::fromJson(const json& j) {
  std::string t = j.value("type", std::string("VeraaMtl"));
  if (!findMaterialType(t)) t = "VeraaMtl";
  if (t != materialType || !subs.empty() || !maps.empty()) {
    materialType = t;
    maps.clear();
    defineFromType(*this);
  }
  id = j.value("id", id);
  name = j.value("name", name);
  if (j.contains("params")) params.fromJson(j["params"]);
  if (j.contains("maps")) maps = mapSlotsFromJson(j["maps"]);
  if (j.contains("subs") && j["subs"].is_array()) {
    const json& js = j["subs"];
    // Variable-length compound types (Multi/Sub-Object) take the saved count.
    if (subs.empty() || materialType == "MultiSubObject") subs.resize(js.size());
    for (size_t i = 0; i < js.size() && i < subs.size(); ++i) {
      const json& s = js[i];
      if (!s.is_object()) continue;
      subs[i].label = s.value("label", subs[i].label);
      subs[i].matId = s.value("matId", subs[i].matId);
      subs[i].enabled = s.value("enabled", true);
      if (s.contains("material") && s["material"].is_object()) {
        auto m = std::make_unique<Material>();
        m->fromJson(s["material"]);
        subs[i].material = std::move(m);
      } else {
        subs[i].material.reset();
      }
    }
  }
  upgradeLegacyMaps();
}

bool Material::upgradeLegacyMaps() {
  if (materialType != "VeraaMtl" || !params.has("diffuseMap")) return false;
  bool changed = false;
  float tu = params.getFloat("mapTileU"), tv = params.getFloat("mapTileV");
  auto upgrade = [&](const char* fileKey, const char* slotKey, float amount, int monoOutput) {
    const std::string file = params.getString(fileKey);
    if (file.empty()) return;
    if (!mapFor(slotKey)) {
      auto bmp = createTexMap("Bitmap");
      bmp->params.setString("file", file);
      bmp->params.setNumber("uTiling", tu);
      bmp->params.setNumber("vTiling", tv);
      if (monoOutput >= 0) bmp->params.setNumber("monoOutput", monoOutput);
      size_t slash = file.find_last_of("/\\");
      bmp->name = slash == std::string::npos ? file : file.substr(slash + 1);
      MapSlot s;
      s.map = std::move(bmp);
      s.amount = amount;
      maps[slotKey] = std::move(s);
    }
    params.setString(fileKey, "");
    changed = true;
  };
  upgrade("diffuseMap", "diffuse", params.getFloat("diffuseMapAmount"), -1);
  upgrade("reflectMap", "reflect", 1.0f, -1);
  upgrade("opacityMap", "opacity", 1.0f, 0);  // automatic: alpha when the image has one
  upgrade("bumpMap", "bump", 1.0f, 1);
  return changed;
}

std::string Material::viewportTexture() const {
  if (materialType == "VeraaMtl") {
    if (const std::string* f = bitmapFileIn(mapFor("diffuse"))) return *f;
    if (params.has("diffuseMap") && !params.getString("diffuseMap").empty()) return params.getString("diffuseMap");
    return {};
  }
  const char* colorKeys[] = {"baseColor", "color", "sssColor"};
  for (const char* k : colorKeys)
    if (const std::string* f = bitmapFileIn(mapFor(k))) return *f;
  for (const auto& s : subs)
    if (s.active()) {
      std::string f = s.material->viewportTexture();
      if (!f.empty()) return f;
    }
  return {};
}

Vec3 Material::viewportColor() const {
  for (const char* k : {"diffuse", "baseColor", "sssColor", "color"})
    if (params.has(k) && params.def(k)->type == ParamType::Color) {
      Vec3 c = params.getColor(k);
      if (std::string(k) == "diffuse" && params.has("metalness") && params.getFloat("metalness") > 0.5f &&
          maxComponent(params.getColor("reflect")) > 0.0f)
        c = c * params.getColor("reflect");
      return c;
    }
  for (const auto& s : subs)
    if (s.active()) return s.material->viewportColor();
  return Vec3(0.5f);
}

const Material* Material::subForFaceId(int faceMatId) const {
  if (subs.empty()) return nullptr;
  int id = faceMatId + 1;
  for (const auto& s : subs)
    if (s.matId == id) return s.active() ? s.material.get() : nullptr;
  // 3ds Max wraps IDs beyond the list around (ID modulo the number of slots).
  int n = static_cast<int>(subs.size());
  int idx = ((faceMatId % n) + n) % n;
  return subs[idx].active() ? subs[idx].material.get() : nullptr;
}

void Material::collectFiles(std::vector<std::string>& files) const {
  for (const auto& d : params.defs())
    if (d.type == ParamType::File && !params.getString(d.key).empty()) files.push_back(params.getString(d.key));
  for (const auto& [key, slot] : maps) collectMapFiles(slot.map.get(), files);
  for (const auto& s : subs)
    if (s.material) s.material->collectFiles(files);
}

// ---------------------------------------------------------------------------------------------
// Registry

void registerMaterialType(MaterialTypeInfo info) {
  auto& reg = registry();
  for (auto& t : reg)
    if (t.type == info.type) {
      t = std::move(info);
      return;
    }
  reg.push_back(std::move(info));
}

const std::vector<MaterialTypeInfo>& materialTypes() {
  ensureBuiltins();
  return registry();
}

const MaterialTypeInfo* findMaterialType(const std::string& type) {
  for (const auto& t : materialTypes())
    if (t.type == type) return &t;
  return nullptr;
}

std::string materialDisplayName(const std::string& type) {
  const MaterialTypeInfo* info = findMaterialType(type);
  return info ? info->displayName : type;
}

// ---------------------------------------------------------------------------------------------
// Built-in material types

namespace {

void defineVeraaMtl(Material& m) {
  ParamBlock& p = m.params;
  p.beginGroup("Diffuse");
  p.addColor("diffuse", "Diffuse", Vec3(0.5f));
  p.addFloat("roughness", "Roughness", 0.0, 0.0, 1.0, 0.05);

  p.beginGroup("Reflection");
  p.addColor("reflect", "Reflect", Vec3(0.0f));
  p.addFloat("reflectGlossiness", "Glossiness", 1.0, 0.0, 1.0, 0.01, 3);
  p.addBool("useRoughness", "Use roughness", false).tooltip =
      "Interpret the glossiness values as roughness (0 = mirror)";
  p.addBool("fresnel", "Fresnel reflections", true);
  p.addBool("lockIor", "Lock Fresnel IOR to refraction IOR", false);
  p.addFloat("fresnelIor", "Fresnel IOR", 1.6, 1.0, 50.0, 0.01, 3);
  p.addFloat("metalness", "Metalness", 0.0, 0.0, 1.0, 0.05);
  p.addFloat("anisotropy", "Anisotropy", 0.0, -1.0, 1.0, 0.05);
  p.addFloat("anisoRotation", "Anisotropy rotation", 0.0, -360.0, 360.0, 1.0, 1);
  // Metal Fresnel: index 0 uses the diffuse color as F0 (artist), 1 the complex IOR below, 2+ a
  // measured metal (render::metalPresets()).
  p.addEnum("metalFresnel", "Metal Fresnel",
            {"Base color (artist)", "Complex IOR (n, k)", "Gold", "Silver", "Copper", "Aluminium", "Chromium", "Iron",
             "Titanium", "Nickel", "Cobalt", "Brass"},
            0)
      .tooltip = "How the metal part (metalness) reflects: from the diffuse color, or physically from a complex "
                 "index of refraction (measured metals or custom n, k). The reflect color still tints it";
  p.addFloat("metalNr", "Metal n (red)", 0.18, 0.01, 10.0, 0.01, 4);
  p.addFloat("metalNg", "Metal n (green)", 0.42, 0.01, 10.0, 0.01, 4);
  p.addFloat("metalNb", "Metal n (blue)", 1.37, 0.01, 10.0, 0.01, 4);
  p.addFloat("metalKr", "Metal k (red)", 3.42, 0.0, 20.0, 0.01, 4);
  p.addFloat("metalKg", "Metal k (green)", 2.35, 0.0, 20.0, 0.01, 4);
  p.addFloat("metalKb", "Metal k (blue)", 1.77, 0.0, 20.0, 0.01, 4);

  p.beginGroup("Thin film");
  p.addBool("thinFilm", "Thin film (iridescence)", false).tooltip =
      "A transparent film on the reflection or the coat (soap, oil, anodized or heat-tinted metal)";
  p.addEnum("thinFilmLayer", "Film on", {"Reflection", "Coat"}, 0);
  p.addFloat("thinFilmThickness", "Thickness (nm)", 400.0, 0.0, 2000.0, 10.0, 1).tooltip =
      "Film thickness in nanometers; a map scales it (black = no film, white = this thickness)";
  p.addFloat("thinFilmIor", "Film IOR", 1.45, 1.0, 4.0, 0.01, 3);

  p.beginGroup("Refraction");
  p.addColor("refract", "Refract", Vec3(0.0f));
  p.addFloat("refractGlossiness", "Glossiness", 1.0, 0.0, 1.0, 0.01, 3);
  p.addFloat("ior", "IOR", 1.6, 1.0, 50.0, 0.01, 3);
  p.addFloat("abbe", "Abbe number (dispersion, 0 = off)", 0.0, 0.0, 150.0, 1.0, 1).tooltip =
      "Lower values give stronger rainbow dispersion (diamond ~ 55, flint glass ~ 30)";
  p.addColor("fogColor", "Fog color", Vec3(1.0f));
  p.addFloat("fogMultiplier", "Fog multiplier", 1.0, 0.0, 1e4, 0.1);
  p.addBool("thinWalled", "Thin-walled", false);
  p.addBool("affectShadows", "Affect shadows", true);
  p.addBool("exitColorOn", "Use exit color", false);
  p.addColor("exitColor", "Exit color", Vec3(0.0f));

  p.beginGroup("Translucency");
  p.addEnum("translucency", "Translucency", {"None", "Thin (diffuse transmission)", "Volumetric (subsurface)"}, 0);
  p.addColor("translucencyColor", "Translucency color", Vec3(1.0f));

  p.beginGroup("Coat");
  p.addFloat("coatAmount", "Coat amount", 0.0, 0.0, 1.0, 0.05);
  p.addColor("coatColor", "Coat color", Vec3(1.0f));
  p.addFloat("coatGlossiness", "Coat glossiness", 1.0, 0.0, 1.0, 0.01, 3);
  p.addFloat("coatIor", "Coat IOR", 1.6, 1.0, 50.0, 0.01, 3);

  p.beginGroup("Sheen");
  p.addColor("sheenColor", "Sheen color", Vec3(0.0f));
  p.addFloat("sheenGlossiness", "Sheen glossiness", 0.8, 0.0, 1.0, 0.01, 3);

  p.beginGroup("Self-illumination");
  p.addColor("selfIllum", "Self-illumination", Vec3(0.0f));
  p.addFloat("selfIllumMultiplier", "Multiplier", 1.0, 0.0, 1e6, 0.1);
  p.addBool("selfIllumGI", "Affects GI", true);

  p.beginGroup("Opacity");
  p.addFloat("opacity", "Opacity", 1.0, 0.0, 1.0, 0.05);
  p.addEnum("opacityMode", "Opacity mode", {"Stochastic", "Clip"}, 0);

  p.beginGroup("Bump and displacement");
  p.addFloat("bumpAmount", "Bump amount", 1.0, -100.0, 100.0, 0.1);
  p.addFloat("displacementAmount", "Displacement amount", 2.0, -1e5, 1e5, 0.1);
  p.addFloat("displacementShift", "Displacement shift", 0.0, -1e5, 1e5, 0.1);

  // Older scenes stored maps as file names; they are converted to Bitmap maps on load.
  p.beginGroup("Legacy");
  p.addFile("diffuseMap", "Diffuse map", kImageFilter);
  p.addFloat("diffuseMapAmount", "Map amount", 1.0, 0.0, 1.0, 0.05);
  p.addFile("reflectMap", "Reflect map", kImageFilter);
  p.addFile("opacityMap", "Opacity map", kImageFilter);
  p.addFile("bumpMap", "Bump map", kImageFilter);
  p.addFloat("mapTileU", "Map tiling U", 1.0, -1e4, 1e4, 0.1);
  p.addFloat("mapTileV", "Map tiling V", 1.0, -1e4, 1e4, 0.1);
}

void defineBlend(Material& m) {
  ParamBlock& p = m.params;
  p.beginGroup("Blend");
  p.addBool("additive", "Additive (shellac) mode", false);
  m.subs.push_back(subSlot("Base material", coloredMtl("Base", Vec3(0.5f))));
  for (int i = 1; i <= 9; ++i) {
    std::string n = std::to_string(i);
    p.beginGroup("Coat " + n);
    p.addColor("blend" + n, "Blend amount", Vec3(0.5f));
    m.subs.push_back(subSlot("Coat " + n));
  }
}

void defineMultiSub(Material& m) {
  const Vec3 colors[] = {{0.8f, 0.25f, 0.2f}, {0.25f, 0.6f, 0.3f}, {0.2f, 0.4f, 0.8f}, {0.85f, 0.75f, 0.3f}};
  for (int i = 0; i < 4; ++i)
    m.subs.push_back(subSlot("ID " + std::to_string(i + 1), coloredMtl("Sub " + std::to_string(i + 1), colors[i]), i + 1));
}

void defineLightMtl(Material& m) {
  ParamBlock& p = m.params;
  p.beginGroup("Light");
  p.addColor("color", "Color", Vec3(1.0f));
  p.addFloat("multiplier", "Multiplier", 1.0, 0.0, 1e7, 0.1);
  p.addFloat("opacity", "Opacity", 1.0, 0.0, 1.0, 0.05);
  p.addBool("twoSided", "Emit light on back side", false);
  p.addBool("compensateExposure", "Compensate camera exposure", false);
  p.addBool("directIllumination", "Direct illumination (sample as a light)", false).tooltip =
      "Treat the object as a mesh light: less noise in the light it casts";
}

void defineTwoSided(Material& m) {
  ParamBlock& p = m.params;
  p.beginGroup("Two-sided");
  p.addColor("translucency", "Translucency", Vec3(0.5f));
  p.addBool("forceSingleSided", "Force single-sided sub-materials", true);
  m.subs.push_back(subSlot("Front material", coloredMtl("Front", Vec3(0.6f, 0.75f, 0.35f))));
  m.subs.push_back(subSlot("Back material"));
}

void defineCarPaint(Material& m) {
  ParamBlock& p = m.params;
  p.beginGroup("Base layer");
  p.addColor("baseColor", "Base color", Vec3(0.45f, 0.03f, 0.03f));
  p.addFloat("baseReflection", "Base reflection", 0.5, 0.0, 1.0, 0.05);
  p.addFloat("baseGlossiness", "Base glossiness", 0.6, 0.0, 1.0, 0.01, 3);
  p.beginGroup("Flake layer");
  p.addColor("flakeColor", "Flake color", Vec3(0.9f, 0.35f, 0.25f));
  p.addFloat("flakeGlossiness", "Flake glossiness", 0.8, 0.0, 1.0, 0.01, 3);
  p.addFloat("flakeOrientation", "Flake orientation", 0.3, 0.0, 1.0, 0.01, 3);
  p.addFloat("flakeDensity", "Flake density", 0.5, 0.0, 4.0, 0.05);
  p.addFloat("flakeSize", "Flake size (cm)", 0.05, 0.0001, 100.0, 0.01, 4);
  p.addInt("flakeSeed", "Flake seed", 1, 0, 100000);
  p.beginGroup("Coat layer");
  p.addColor("coatColor", "Coat color", Vec3(1.0f));
  p.addFloat("coatStrength", "Coat strength (reflection at 0°)", 0.05, 0.0, 1.0, 0.01, 3);
  p.addFloat("coatGlossiness", "Coat glossiness", 1.0, 0.0, 1.0, 0.01, 3);
  p.beginGroup("Bump");
  p.addFloat("bumpAmount", "Bump amount", 1.0, -100.0, 100.0, 0.1);
}

void defineSss(Material& m) {
  ParamBlock& p = m.params;
  p.beginGroup("Subsurface");
  p.addColor("sssColor", "Subsurface color", Vec3(0.87f, 0.6f, 0.5f));
  p.addFloat("scatterRadius", "Scatter radius (cm)", 1.0, 0.0001, 1e5, 0.1, 3);
  p.addColor("radiusColor", "Scatter radius color", Vec3(1.0f, 0.45f, 0.25f));
  p.addFloat("phase", "Phase function", 0.0, -0.9, 0.9, 0.05);
  p.addFloat("ior", "IOR", 1.4, 1.0, 5.0, 0.01, 3);
  p.beginGroup("Diffuse layer");
  p.addColor("diffuseColor", "Diffuse color", Vec3(0.8f));
  p.addFloat("diffuseAmount", "Diffuse amount", 0.0, 0.0, 1.0, 0.05);
  p.beginGroup("Specular layer");
  p.addColor("specularColor", "Specular color", Vec3(1.0f));
  p.addFloat("specularAmount", "Specular amount", 1.0, 0.0, 1.0, 0.05);
  p.addFloat("specularGlossiness", "Specular glossiness", 0.6, 0.0, 1.0, 0.01, 3);
  p.beginGroup("Bump");
  p.addFloat("bumpAmount", "Bump amount", 1.0, -100.0, 100.0, 0.1);
}

void defineOverride(Material& m) {
  m.subs.push_back(subSlot("Base material", coloredMtl("Base", Vec3(0.5f))));
  m.subs.push_back(subSlot("GI material"));
  m.subs.push_back(subSlot("Reflect material"));
  m.subs.push_back(subSlot("Refract material"));
  m.subs.push_back(subSlot("Shadow material"));
}

void defineWrapper(Material& m) {
  ParamBlock& p = m.params;
  p.beginGroup("Surface");
  p.addFloat("generateGI", "Generate GI", 1.0, 0.0, 100.0, 0.1);
  p.addFloat("receiveGI", "Receive GI", 1.0, 0.0, 100.0, 0.1);
  p.beginGroup("Matte properties");
  p.addBool("matteSurface", "Matte surface", false);
  p.addFloat("alphaContribution", "Alpha contribution", 1.0, -1.0, 1.0, 0.1);
  p.addBool("shadows", "Shadows", true);
  p.addBool("shadowsAffectAlpha", "Affect alpha", true);
  p.addColor("shadowColor", "Shadow color", Vec3(0.0f));
  p.addFloat("shadowBrightness", "Shadow brightness", 1.0, 0.0, 10.0, 0.05);
  p.addFloat("reflectionAmount", "Reflection amount", 1.0, 0.0, 1.0, 0.05);
  p.addFloat("refractionAmount", "Refraction amount", 1.0, 0.0, 1.0, 0.05);
  m.subs.push_back(subSlot("Base material", coloredMtl("Base", Vec3(0.5f))));
}

}  // namespace

void registerMaterialTypes() {
  registerMaterialType({"VeraaMtl", "VeraaMtl", "Materials", defineVeraaMtl,
                        {"diffuse", "roughness", "reflect", "reflectGlossiness", "fresnelIor", "metalness",
                         "anisotropy", "anisoRotation", "thinFilmThickness", "refract", "refractGlossiness", "ior", "fogColor",
                         "translucencyColor", "coatAmount", "coatColor", "coatGlossiness", "coatBump", "sheenColor",
                         "sheenGlossiness", "selfIllum", "opacity", "bump", "displacement"},
                        "General purpose physically based material: diffuse, reflection, refraction, coat, sheen"});
  std::vector<std::string> blendMaps;
  for (int i = 1; i <= 9; ++i) blendMaps.push_back("blend" + std::to_string(i));
  registerMaterialType({"VeraaBlendMtl", "VeraaBlendMtl", "Compound", defineBlend, blendMaps,
                        "Layers up to nine coat materials over a base material"});
  registerMaterialType({"MultiSubObject", "Multi/Sub-Object", "Compound", defineMultiSub, {},
                        "Assigns sub-materials by face material ID"});
  registerMaterialType({"VeraaLightMtl", "VeraaLightMtl", "Materials", defineLightMtl, {"color", "opacity"},
                        "Self-illuminated surface; can act as a mesh light"});
  registerMaterialType({"VeraaTwoSidedMtl", "VeraaTwoSidedMtl", "Compound", defineTwoSided, {"translucency"},
                        "Thin translucent surfaces (leaves, paper, curtains) with different front and back"});
  registerMaterialType({"VeraaCarPaintMtl", "VeraaCarPaintMtl", "Materials", defineCarPaint,
                        {"baseColor", "baseReflection", "baseGlossiness", "flakeColor", "coatColor", "coatStrength",
                         "coatGlossiness", "bump"},
                        "Three-layer car paint: base, metallic flakes and clear coat"});
  registerMaterialType({"VeraaSSSMtl", "VeraaSSSMtl", "Materials", defineSss,
                        {"sssColor", "radiusColor", "diffuseColor", "diffuseAmount", "specularColor",
                         "specularGlossiness", "bump"},
                        "Random-walk subsurface scattering for skin, wax, marble, milk"});
  registerMaterialType({"VeraaOverrideMtl", "VeraaOverrideMtl", "Compound", defineOverride, {},
                        "Different materials for GI, reflections, refractions and shadows"});
  registerMaterialType({"VeraaWrapperMtl", "VeraaWrapperMtl (Matte/Shadow)", "Compound", defineWrapper, {},
                        "GI control and matte/shadow-catcher surfaces for compositing"});
}

}  // namespace veraa
