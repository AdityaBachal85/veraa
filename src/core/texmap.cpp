#include "texmap.h"

#include <algorithm>

#include "registry.h"

namespace veraa {

namespace {

const char* kImageFilter = "Images (*.png *.jpg *.jpeg *.bmp *.tga *.tif *.tiff *.hdr *.exr)";

std::vector<TexMapTypeInfo>& registry() {
  static std::vector<TexMapTypeInfo> types;
  return types;
}

void ensureBuiltins() {
  static bool done = false;
  if (done) return;
  done = true;
  registerTexMapTypes();
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// MapSlot / TexMap

MapSlot::MapSlot(const MapSlot& o)
    : map(o.map ? std::make_unique<TexMap>(*o.map) : nullptr), amount(o.amount), enabled(o.enabled) {}

MapSlot& MapSlot::operator=(const MapSlot& o) {
  if (this != &o) {
    map = o.map ? std::make_unique<TexMap>(*o.map) : nullptr;
    amount = o.amount;
    enabled = o.enabled;
  }
  return *this;
}

TexMap::TexMap(const std::string& t) : type(t) {
  if (const TexMapTypeInfo* info = findTexMapType(t)) {
    if (info->define) info->define(params);
    name = info->displayName;
  }
}

const TexMap* TexMap::mapFor(const std::string& key) const {
  auto it = maps.find(key);
  return it != maps.end() && it->second.active() ? it->second.map.get() : nullptr;
}

TexMap* TexMap::mapFor(const std::string& key) {
  auto it = maps.find(key);
  return it != maps.end() && it->second.active() ? it->second.map.get() : nullptr;
}

const std::vector<std::string>& TexMap::mappableKeys() const {
  static const std::vector<std::string> none;
  const TexMapTypeInfo* info = findTexMapType(type);
  return info ? info->mappable : none;
}

json mapSlotsToJson(const MapSlots& slots) {
  json j = json::object();
  for (const auto& [key, slot] : slots) {
    if (!slot.map) continue;
    json s{{"map", slot.map->toJson()}};
    if (slot.amount != 1.0f) s["amount"] = slot.amount;
    if (!slot.enabled) s["enabled"] = false;
    j[key] = std::move(s);
  }
  return j;
}

MapSlots mapSlotsFromJson(const json& j) {
  MapSlots slots;
  if (!j.is_object()) return slots;
  for (auto it = j.begin(); it != j.end(); ++it) {
    if (!it->is_object() || !it->contains("map")) continue;
    MapSlot s;
    s.map = TexMap::fromJson((*it)["map"]);
    if (!s.map) continue;
    s.amount = it->value("amount", 1.0f);
    s.enabled = it->value("enabled", true);
    slots[it.key()] = std::move(s);
  }
  return slots;
}

json TexMap::toJson() const {
  json j{{"type", type}, {"name", name}, {"params", params.toJson()}};
  json m = mapSlotsToJson(maps);
  if (!m.empty()) j["maps"] = std::move(m);
  return j;
}

std::unique_ptr<TexMap> TexMap::fromJson(const json& j) {
  if (!j.is_object()) return nullptr;
  std::string type = j.value("type", std::string());
  if (!findTexMapType(type)) return nullptr;
  auto m = std::make_unique<TexMap>(type);
  m->name = j.value("name", m->name);
  if (j.contains("params")) m->params.fromJson(j["params"]);
  if (j.contains("maps")) m->maps = mapSlotsFromJson(j["maps"]);
  return m;
}

// ---------------------------------------------------------------------------------------------
// Registry

void registerTexMapType(TexMapTypeInfo info) {
  auto& reg = registry();
  for (auto& t : reg)
    if (t.type == info.type) {
      t = std::move(info);
      return;
    }
  reg.push_back(std::move(info));
}

const std::vector<TexMapTypeInfo>& texMapTypes() {
  ensureBuiltins();
  return registry();
}

const TexMapTypeInfo* findTexMapType(const std::string& type) {
  for (const auto& t : texMapTypes())
    if (t.type == type) return &t;
  return nullptr;
}

std::unique_ptr<TexMap> createTexMap(const std::string& type) {
  if (!findTexMapType(type)) return nullptr;
  return std::make_unique<TexMap>(type);
}

const std::vector<std::string>& compositeBlendModes() {
  static const std::vector<std::string> modes = {
      "Normal",      "Average",    "Add",         "Subtract",   "Darken",     "Multiply",   "Color Burn",
      "Linear Burn", "Lighten",    "Screen",      "Color Dodge", "Linear Dodge", "Overlay", "Soft Light",
      "Hard Light",  "Difference", "Exclusion",   "Hue",        "Saturation", "Color",      "Value"};
  return modes;
}

// ---------------------------------------------------------------------------------------------
// Shared parameter groups

void addCoords2D(ParamBlock& p, float defaultTiling) {
  p.beginGroup("Coordinates");
  p.addEnum("mapping", "Mapping", {"Explicit map channel", "Planar from object XY", "Planar from world XY",
                                   "Spherical environment", "Cylindrical (object Z)"},
            0);
  p.addInt("uvChannel", "Map channel", 1, 1, 99);
  p.addFloat("uOffset", "U offset", 0.0, -1e6, 1e6, 0.01, 3);
  p.addFloat("vOffset", "V offset", 0.0, -1e6, 1e6, 0.01, 3);
  p.addFloat("uTiling", "U tiling", defaultTiling, -1e6, 1e6, 0.1, 3);
  p.addFloat("vTiling", "V tiling", defaultTiling, -1e6, 1e6, 0.1, 3);
  p.addFloat("wAngle", "W angle", 0.0, -36000, 36000, 1.0, 1);
  p.addBool("uWrap", "U tile", true);
  p.addBool("vWrap", "V tile", true);
  p.addBool("uMirror", "U mirror", false);
  p.addBool("vMirror", "V mirror", false);
  p.addBool("realWorld", "Real-world scale", false);
  p.addFloat("realWidth", "Real width", 100.0, 0.001, 1e7, 1.0);
  p.addFloat("realHeight", "Real height", 100.0, 0.001, 1e7, 1.0);
}

void addCoords3D(ParamBlock& p) {
  p.beginGroup("Coordinates");
  p.addEnum("source", "Source", {"Object XYZ", "World XYZ", "Explicit map channel"}, 0);
  p.addFloat("offsetX", "Offset X", 0.0, -1e7, 1e7, 1.0);
  p.addFloat("offsetY", "Offset Y", 0.0, -1e7, 1e7, 1.0);
  p.addFloat("offsetZ", "Offset Z", 0.0, -1e7, 1e7, 1.0);
  p.addFloat("tilingX", "Tiling X", 1.0, -1e6, 1e6, 0.1, 3);
  p.addFloat("tilingY", "Tiling Y", 1.0, -1e6, 1e6, 0.1, 3);
  p.addFloat("tilingZ", "Tiling Z", 1.0, -1e6, 1e6, 0.1, 3);
}

void addOutput(ParamBlock& p) {
  p.beginGroup("Output");
  p.addFloat("outputAmount", "Output amount", 1.0, 0.0, 1000.0, 0.05);
  p.addFloat("rgbOffset", "RGB offset", 0.0, -100.0, 100.0, 0.01, 3);
  p.addFloat("rgbLevel", "RGB level", 1.0, -100.0, 100.0, 0.05);
  p.addBool("invert", "Invert", false);
  p.addBool("clampOutput", "Clamp", false);
}

// ---------------------------------------------------------------------------------------------
// Built-in map types

void registerTexMapTypes() {
  auto add = [](const char* type, const char* name, const char* cat, std::vector<std::string> mappable,
                std::function<void(ParamBlock&)> define) {
    registerTexMapType({type, name, cat, std::move(define), std::move(mappable)});
  };

  add("Bitmap", "Bitmap", "2D Maps", {}, [](ParamBlock& p) {
    p.beginGroup("Bitmap");
    p.addFile("file", "File", kImageFilter);
    p.addEnum("colorSpace", "Color space", {"Automatic", "sRGB", "Linear"}, 0);
    p.addEnum("alphaSource", "Alpha source", {"Image alpha", "RGB intensity", "None (opaque)"}, 0);
    p.addEnum("monoOutput", "Mono output", {"Automatic", "RGB intensity", "Alpha"}, 0);
    p.addEnum("filtering", "Filtering", {"Bilinear", "Smooth (mipmapped)", "Nearest"}, 1);
    p.addFloat("blur", "Blur", 1.0, 0.01, 100.0, 0.1);
    addCoords2D(p);
    addOutput(p);
  });

  add("Checker", "Checker", "2D Maps", {"color1", "color2"}, [](ParamBlock& p) {
    p.beginGroup("Checker");
    p.addColor("color1", "Color 1", Vec3(0.0f));
    p.addColor("color2", "Color 2", Vec3(1.0f));
    p.addFloat("soften", "Soften", 0.0, 0.0, 5.0, 0.01, 3);
    addCoords2D(p);
    addOutput(p);
  });

  add("Gradient", "Gradient", "2D Maps", {"color1", "color2", "color3"}, [](ParamBlock& p) {
    p.beginGroup("Gradient");
    p.addColor("color1", "Color 1", Vec3(0.0f));
    p.addColor("color2", "Color 2", Vec3(0.5f));
    p.addColor("color3", "Color 3", Vec3(1.0f));
    p.addFloat("color2Pos", "Color 2 position", 0.5, 0.0, 1.0, 0.01);
    p.addEnum("gradientType", "Type", {"Linear", "Radial"}, 0);
    p.addFloat("noiseAmount", "Noise amount", 0.0, 0.0, 1.0, 0.01);
    p.addFloat("noiseSize", "Noise size", 1.0, 0.001, 1e4, 0.1);
    addCoords2D(p);
    addOutput(p);
  });

  add("GradientRamp", "Gradient Ramp", "2D Maps",
      {"color1", "color2", "color3", "color4", "color5", "color6", "source"}, [](ParamBlock& p) {
        p.beginGroup("Gradient Ramp");
        p.addEnum("gradientType", "Gradient type",
                  {"Linear (U)", "Linear (V)", "Radial", "Diagonal", "Box", "Spiral", "Facing ratio", "Height (world Z)",
                   "Mapped (source map)"},
                  0);
        p.addEnum("interpolation", "Interpolation", {"Linear", "Smooth", "Solid"}, 0);
        p.addInt("keyCount", "Flags", 3, 2, 6);
        const Vec3 colors[6] = {Vec3(0.0f), Vec3(0.5f), Vec3(1.0f), Vec3(1, 0, 0), Vec3(0, 1, 0), Vec3(0, 0, 1)};
        const double pos[6] = {0.0, 0.5, 1.0, 1.0, 1.0, 1.0};
        for (int i = 0; i < 6; ++i) {
          std::string n = std::to_string(i + 1);
          p.addFloat("pos" + n, "Flag " + n + " position", pos[i], 0.0, 1.0, 0.01, 3);
          p.addColor("color" + n, "Flag " + n + " color", colors[i]);
        }
        p.addColor("source", "Source (mapped)", Vec3(0.5f));
        p.addFloat("heightMin", "Height min", 0.0, -1e7, 1e7, 1.0);
        p.addFloat("heightMax", "Height max", 100.0, -1e7, 1e7, 1.0);
        p.addFloat("noiseAmount", "Noise amount", 0.0, 0.0, 1.0, 0.01);
        p.addFloat("noiseSize", "Noise size", 1.0, 0.001, 1e4, 0.1);
        addCoords2D(p);
        addOutput(p);
      });

  add("Tiles", "Tiles", "2D Maps", {"tileColor", "groutColor"}, [](ParamBlock& p) {
    p.beginGroup("Tiles");
    p.addEnum("pattern", "Pattern",
              {"Running bond", "Stack bond", "Half running bond", "Common Flemish bond", "English bond",
               "Fine running bond"},
              0);
    p.addColor("tileColor", "Tile color", Vec3(0.55f, 0.27f, 0.18f));
    p.addColor("groutColor", "Grout color", Vec3(0.75f, 0.74f, 0.7f));
    p.addInt("tilesH", "Horizontal count", 4, 1, 1000);
    p.addInt("tilesV", "Vertical count", 8, 1, 1000);
    p.addFloat("colorVariance", "Color variance", 0.1, 0.0, 1.0, 0.01);
    p.addFloat("fadeVariance", "Fade variance", 0.05, 0.0, 1.0, 0.01);
    p.addFloat("groutH", "Horizontal gap (%)", 2.0, 0.0, 50.0, 0.1);
    p.addFloat("groutV", "Vertical gap (%)", 2.0, 0.0, 50.0, 0.1);
    p.addFloat("rowShift", "Row shift", 0.5, 0.0, 1.0, 0.05);
    p.addFloat("randomShift", "Random shift", 0.0, 0.0, 1.0, 0.05);
    p.addInt("seed", "Random seed", 1, 0, 100000);
    addCoords2D(p);
    addOutput(p);
  });

  add("Noise", "Noise", "3D Maps", {"color1", "color2"}, [](ParamBlock& p) {
    p.beginGroup("Noise");
    p.addColor("color1", "Color 1", Vec3(0.0f));
    p.addColor("color2", "Color 2", Vec3(1.0f));
    p.addEnum("noiseType", "Noise type", {"Regular", "Fractal", "Turbulence"}, 0);
    p.addFloat("size", "Size", 25.0, 0.001, 1e6, 1.0);
    p.addFloat("levels", "Levels", 3.0, 1.0, 10.0, 0.5, 1);
    p.addFloat("phase", "Phase", 0.0, -1e4, 1e4, 0.1);
    p.addFloat("lowThreshold", "Low threshold", 0.0, 0.0, 1.0, 0.01);
    p.addFloat("highThreshold", "High threshold", 1.0, 0.0, 1.0, 0.01);
    addCoords3D(p);
    addOutput(p);
  });

  add("Cellular", "Cellular", "3D Maps", {"cellColor", "divColor1", "divColor2"}, [](ParamBlock& p) {
    p.beginGroup("Cellular");
    p.addColor("cellColor", "Cell color", Vec3(1.0f));
    p.addColor("divColor1", "Division color 1", Vec3(0.5f));
    p.addColor("divColor2", "Division color 2", Vec3(0.0f));
    p.addEnum("cellType", "Cell type", {"Circular", "Chips"}, 0);
    p.addFloat("size", "Size", 5.0, 0.001, 1e6, 0.5);
    p.addFloat("spread", "Spread", 0.5, 0.001, 1.0, 0.01);
    p.addFloat("colorVariance", "Cell color variation", 0.0, 0.0, 1.0, 0.01);
    p.addBool("fractal", "Fractal", false);
    p.addFloat("iterations", "Iterations", 3.0, 1.0, 10.0, 0.5, 1);
    p.addFloat("roughness", "Roughness", 0.0, 0.0, 1.0, 0.05);
    addCoords3D(p);
    addOutput(p);
  });

  add("Falloff", "Falloff", "Other", {"frontColor", "sideColor"}, [](ParamBlock& p) {
    p.beginGroup("Falloff");
    p.addColor("frontColor", "Front color", Vec3(0.0f));
    p.addColor("sideColor", "Side color", Vec3(1.0f));
    p.addEnum("falloffType", "Falloff type",
              {"Towards / Away", "Perpendicular / Parallel", "Fresnel", "Shadow / Light", "Distance blend"}, 1);
    p.addEnum("direction", "Direction", {"Viewing direction", "Object Z axis", "World Z axis"}, 0);
    p.addFloat("ior", "Index of refraction", 1.6, 1.0, 10.0, 0.01, 3);
    p.addFloat("nearDistance", "Near distance", 0.0, 0.0, 1e7, 1.0);
    p.addFloat("farDistance", "Far distance", 1000.0, 0.0, 1e7, 1.0);
    p.addFloat("curve", "Mix curve (gamma)", 1.0, 0.05, 20.0, 0.05);
  });

  add("Mix", "Mix", "Compositors", {"color1", "color2", "mixAmount"}, [](ParamBlock& p) {
    p.beginGroup("Mix");
    p.addColor("color1", "Color 1", Vec3(0.0f));
    p.addColor("color2", "Color 2", Vec3(1.0f));
    p.addFloat("mixAmount", "Mix amount", 0.5, 0.0, 1.0, 0.01, 3);
    p.addBool("useCurve", "Use curve", false);
    p.addFloat("curveLower", "Lower", 0.3, 0.0, 1.0, 0.01);
    p.addFloat("curveUpper", "Upper", 0.7, 0.0, 1.0, 0.01);
  });

  add("Composite", "Composite", "Compositors",
      {"layer1Color", "layer1Mask", "layer2Color", "layer2Mask", "layer3Color", "layer3Mask", "layer4Color",
       "layer4Mask"},
      [](ParamBlock& p) {
        p.beginGroup("Composite");
        p.addInt("layerCount", "Layers", 2, 1, 4);
        for (int i = 1; i <= 4; ++i) {
          std::string n = std::to_string(i);
          p.beginGroup("Layer " + n);
          p.addBool("layer" + n + "Enabled", "Enabled", true);
          p.addColor("layer" + n + "Color", "Color", i == 1 ? Vec3(0.5f) : Vec3(1.0f));
          p.addFloat("layer" + n + "Mask", "Mask", 1.0, 0.0, 1.0, 0.01, 3);
          p.addEnum("layer" + n + "Mode", "Blend mode", compositeBlendModes(), 0);
          p.addFloat("layer" + n + "Opacity", "Opacity (%)", 100.0, 0.0, 100.0, 1.0, 1);
        }
      });

  add("ColorCorrection", "Color Correction", "Color Modifiers", {"color"}, [](ParamBlock& p) {
    p.beginGroup("Color Correction");
    p.addColor("color", "Input", Vec3(0.5f));
    p.addFloat("hueShift", "Hue shift", 0.0, -180.0, 180.0, 1.0, 1);
    p.addFloat("saturation", "Saturation", 0.0, -100.0, 100.0, 1.0, 1);
    p.addColor("tint", "Tint", Vec3(1.0f));
    p.addFloat("tintStrength", "Tint strength", 0.0, 0.0, 100.0, 1.0, 1);
    p.addFloat("brightness", "Brightness", 0.0, -100.0, 100.0, 1.0, 1);
    p.addFloat("contrast", "Contrast", 0.0, -100.0, 100.0, 1.0, 1);
    p.addFloat("gamma", "Gamma", 1.0, 0.01, 10.0, 0.01, 3);
    p.addFloat("exposure", "Exposure (EV)", 0.0, -20.0, 20.0, 0.1);
    p.addBool("invert", "Invert", false);
  });

  add("Output", "Output", "Color Modifiers", {"color"}, [](ParamBlock& p) {
    p.beginGroup("Output");
    p.addColor("color", "Input", Vec3(0.5f));
    addOutput(p);
  });

  add("AmbientOcclusion", "Ambient Occlusion (Dirt)", "Other", {"unoccludedColor", "occludedColor", "radius"},
      [](ParamBlock& p) {
        p.beginGroup("Ambient Occlusion");
        p.addColor("unoccludedColor", "Unoccluded color", Vec3(1.0f));
        p.addColor("occludedColor", "Occluded color", Vec3(0.0f));
        p.addFloat("radius", "Radius", 10.0, 0.0, 1e7, 0.5);
        p.addFloat("distribution", "Distribution", 1.0, 0.0, 100.0, 0.1);
        p.addFloat("falloff", "Falloff", 0.0, 0.0, 1.0, 0.05);
        p.addInt("samples", "Subdivs (samples)", 8, 1, 256);
        p.addEnum("mode", "Mode", {"Ambient occlusion", "Reflection occlusion", "Inner (edges/creases)"}, 0);
        p.addBool("onlySameObject", "Only same object", false);
        p.addBool("ignoreSelf", "Ignore self", false);
      });

  add("NormalBump", "Normal Bump", "Other", {"normal", "bump"}, [](ParamBlock& p) {
    p.beginGroup("Normal Bump");
    p.addColor("normal", "Normal", Vec3(0.5f, 0.5f, 1.0f));
    p.addFloat("normalAmount", "Normal amount", 1.0, -100.0, 100.0, 0.05);
    p.addEnum("method", "Method", {"Tangent", "Local XYZ", "World XYZ"}, 0);
    p.addBool("flipRed", "Flip red (X)", false);
    p.addBool("flipGreen", "Flip green (Y)", false);
    p.addBool("swapRG", "Swap red and green", false);
    p.addColor("bump", "Additional bump", Vec3(0.0f));
    p.addFloat("bumpAmount", "Bump amount", 1.0, -100.0, 100.0, 0.05);
  });

  add("Triplanar", "Triplanar", "3D Maps", {"texture", "textureY", "textureZ"}, [](ParamBlock& p) {
    p.beginGroup("Triplanar");
    p.addColor("texture", "Texture (X)", Vec3(0.5f));
    p.addBool("sameOnAll", "Same texture on all axes", true);
    p.addColor("textureY", "Texture Y", Vec3(0.5f));
    p.addColor("textureZ", "Texture Z", Vec3(0.5f));
    p.addFloat("size", "Size", 100.0, 0.001, 1e7, 1.0);
    p.addFloat("blend", "Blend", 0.2, 0.0, 1.0, 0.01);
    p.addEnum("space", "Space", {"Object", "World"}, 0);
    p.addBool("randomOffset", "Random offset per object", false);
    p.addBool("randomRotation", "Random rotation per object", false);
    p.addInt("seed", "Seed", 0, 0, 100000);
  });

  add("Edges", "Edges (Wireframe)", "Other", {"edgeColor", "faceColor"}, [](ParamBlock& p) {
    p.beginGroup("Edges");
    p.addColor("edgeColor", "Edge color", Vec3(0.0f));
    p.addColor("faceColor", "Face color", Vec3(1.0f));
    p.addFloat("thickness", "Thickness (world units)", 0.5, 0.0, 1e5, 0.05, 3);
    p.addBool("hiddenEdges", "Show hidden (triangle) edges", false);
  });

  add("ColorVariation", "Color Variation (Multi Color)", "Other", {"color1", "color2", "color3", "color4"},
      [](ParamBlock& p) {
        p.beginGroup("Color Variation");
        p.addEnum("mode", "Vary by", {"Object", "Element", "Material ID", "Instance"}, 0);
        p.addInt("colorCount", "Colors", 2, 1, 4);
        p.addColor("color1", "Color 1", Vec3(0.8f, 0.3f, 0.2f));
        p.addColor("color2", "Color 2", Vec3(0.2f, 0.5f, 0.8f));
        p.addColor("color3", "Color 3", Vec3(0.3f, 0.7f, 0.3f));
        p.addColor("color4", "Color 4", Vec3(0.9f, 0.8f, 0.3f));
        p.addFloat("hueVariation", "Hue variation (deg)", 0.0, 0.0, 180.0, 1.0, 1);
        p.addFloat("satVariation", "Saturation variation", 0.0, 0.0, 1.0, 0.01);
        p.addFloat("valueVariation", "Value variation", 0.0, 0.0, 1.0, 0.01);
        p.addInt("seed", "Seed", 0, 0, 100000);
      });

  add("Distance", "Distance", "Other", {"nearColor", "farColor"}, [](ParamBlock& p) {
    p.beginGroup("Distance");
    p.addString("objects", "Objects (names, comma separated)", "");
    p.addFloat("distance", "Distance", 50.0, 0.001, 1e7, 1.0);
    p.addColor("nearColor", "Near color", Vec3(1.0f));
    p.addColor("farColor", "Far color", Vec3(0.0f));
    p.addColor("insideColor", "Inside color", Vec3(1.0f));
    p.addBool("useInside", "Use inside color", false);
  });
}

}  // namespace veraa
