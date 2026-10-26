#include "rendersettings.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace veraa {

const std::vector<RenderElementInfo>& renderElementInfos() {
  static const std::vector<RenderElementInfo> elements = {
      {"Lighting", "Lighting (direct diffuse)", true},
      {"GI", "Global illumination", true},
      {"Specular", "Specular (direct highlights)", true},
      {"Reflection", "Reflection", true},
      {"Refraction", "Refraction", true},
      {"SSS", "Subsurface scattering", true},
      {"SelfIllumination", "Self-illumination", true},
      {"Atmosphere", "Atmosphere", true},
      {"Background", "Background", true},
      {"DiffuseFilter", "Diffuse filter (albedo)", false},
      {"Normals", "Normals", false},
      {"ZDepth", "Z-depth", false},
      {"ObjectID", "Object ID (color)", false},
      {"MaterialID", "Material ID (color)", false},
      {"WireColor", "Wire color", false},
      {"Alpha", "Alpha", false},
  };
  return elements;
}

void defineAdvancedRenderSettings(ParamBlock& p) {
  defineSamplingQualitySettings(p);
  p.beginGroup("Image sampler");
  p.addEnum("samplerType", "Sampler", {"Progressive", "Bucket"}, 0);
  p.addInt("minSamples", "Min samples per pixel", 16, 1, 100000);
  p.addFloat("noiseThreshold", "Noise threshold (0 = off)", 0.01, 0.0, 1.0, 0.001, 4).tooltip =
      "Pixels stop sampling once their estimated relative noise falls below this value";
  p.addInt("bucketSize", "Bucket size (pixels)", 48, 8, 512);
  p.addBool("renderRegion", "Render region", false);
  p.addFloat("regionX0", "Region left", 0.25, 0.0, 1.0, 0.01, 4);
  p.addFloat("regionY0", "Region top", 0.25, 0.0, 1.0, 0.01, 4);
  p.addFloat("regionX1", "Region right", 0.75, 0.0, 1.0, 0.01, 4);
  p.addFloat("regionY1", "Region bottom", 0.75, 0.0, 1.0, 0.01, 4);
  p.addString("renderSelected", "Render only these node ids", "");

  p.beginGroup("Global illumination");
  p.addBool("caustics", "Refractive caustics", false);
  p.addFloat("clampDirect", "Clamp direct (0 = off)", 0.0, 0.0, 1e7, 1.0, 1);
  p.addFloat("giMultiplier", "GI multiplier", 1.0, 0.0, 100.0, 0.05);

  p.beginGroup("Exposure");
  p.addEnum("exposureControl", "Exposure control", {"None", "Physical (EV100)"}, 0);
  p.addFloat("ev100", "Exposure value (EV100)", 14.0, -10.0, 30.0, 0.1, 2);
  p.addFloat("whiteBalanceK", "White balance (K)", 6500.0, 1000.0, 40000.0, 100.0, 0);

  p.beginGroup("Atmosphere");
  p.addBool("fogOn", "Fog / volumetrics", false);
  p.addEnum("fogType", "Fog type", {"Uniform", "Height (exponential)"}, 0);
  p.addColor("fogColor", "Fog color", Vec3(0.85f, 0.88f, 0.92f));
  p.addFloat("fogDistance", "Fog distance (cm)", 5000.0, 0.1, 1e9, 10.0);
  p.addFloat("fogHeight", "Fog height (cm)", 200.0, 0.1, 1e8, 10.0);
  p.addFloat("fogFalloff", "Height falloff", 1.0, 0.01, 100.0, 0.1);
  p.addFloat("fogAnisotropy", "Phase anisotropy", 0.3, -0.95, 0.95, 0.05);
  p.addBool("fogAffectBackground", "Affect background", true);
  p.addColor("fogEmission", "Emission", Vec3(0.0f));

  p.beginGroup("Lens effects");
  p.addBool("lensEffects", "Bloom / glare", false);
  p.addFloat("bloomIntensity", "Bloom intensity", 0.15, 0.0, 10.0, 0.01, 3);
  p.addFloat("bloomSize", "Bloom size", 0.04, 0.001, 1.0, 0.005, 3);
  p.addFloat("glareIntensity", "Glare intensity", 0.08, 0.0, 10.0, 0.01, 3);
  p.addFloat("glareSize", "Glare size", 0.15, 0.001, 1.0, 0.01, 3);
  p.addInt("glareRays", "Glare rays", 6, 0, 16);
  p.addFloat("glareRotation", "Glare rotation", 15.0, -360.0, 360.0, 1.0, 1);
  p.addFloat("vignetting", "Vignetting", 0.0, 0.0, 1.0, 0.05);

  p.beginGroup("Color corrections");
  p.addBool("ccOn", "Color corrections", false);
  p.addFloat("ccWhiteBalanceK", "White balance (K)", 6500.0, 1000.0, 40000.0, 100.0, 0);
  p.addFloat("ccTint", "Tint (green-magenta)", 0.0, -1.0, 1.0, 0.01);
  p.addFloat("ccHue", "Hue shift", 0.0, -180.0, 180.0, 1.0, 1);
  p.addFloat("ccHighlightBurn", "Highlight burn", 1.0, 0.0, 1.0, 0.05);
  p.addString("ccCurve", "Curve", "0,0;1,1");
  p.addFloat("ccLevelsBlack", "Levels black", 0.0, 0.0, 1.0, 0.01, 3);
  p.addFloat("ccLevelsWhite", "Levels white", 1.0, 0.0, 1.0, 0.01, 3);
  p.addFloat("ccLevelsGamma", "Levels gamma", 1.0, 0.1, 10.0, 0.01, 3);
  p.addFile("ccLutFile", "LUT file", "Cube LUT (*.cube)");
  p.addFloat("ccLutStrength", "LUT strength", 1.0, 0.0, 1.0, 0.05);

  p.beginGroup("Render elements");
  for (const auto& e : renderElementInfos()) p.addBool(std::string("elem") + e.key, e.label, false);
  p.addFloat("zDepthMin", "Z-depth min (cm)", 0.0, 0.0, 1e9, 10.0);
  p.addFloat("zDepthMax", "Z-depth max (cm)", 2000.0, 0.0, 1e9, 10.0);
  p.addBool("lightMix", "Light Mix", false);

  defineRealismRenderSettings(p);
}

// ---------------------------------------------------------------------------------------------
// Realism settings: color mapping parameters and the physical sky. Kept in their own block.

void defineRealismRenderSettings(ParamBlock& p) {
  p.beginGroup("Color mapping");
  p.addFloat("cmDarkMultiplier", "Dark multiplier", 1.0, 0.0, 100.0, 0.05, 3).tooltip =
      "Multiplier for dark values (all color mapping types except the gamma types)";
  p.addFloat("cmBrightMultiplier", "Bright multiplier", 1.0, 0.0, 100.0, 0.05, 3).tooltip =
      "Multiplier for bright values";
  p.addFloat("cmGamma", "Gamma", 1.0, 0.05, 10.0, 0.05, 3).tooltip =
      "Gamma applied after the color mapping curve (the curve itself for the gamma types). The sRGB display "
      "transform is always applied afterwards, so 1.0 is neutral";
  p.addFloat("cmBurn", "Burn value (Reinhard)", 0.0, 0.0, 1.0, 0.05, 3).tooltip =
      "Reinhard only: 1 = linear, 0 = full highlight compression";
  p.addBool("cmAffectBackground", "Affect background", true).tooltip =
      "When off, pixels showing the environment are not color mapped";
  p.addBool("cmAdaptationOnly", "Don't affect colors (adaptation only)", false).tooltip =
      "Keeps the image linear: the color mapping is not applied to the output";

  p.beginGroup("Environment");
  // Scenes saved before this setting existed keep the Preetham sky (Scene::fromJson).
  p.addEnum("skyModel", "Sky model (standard sun)", {"Preetham et al.", "Physical atmosphere", "Hosek-Wilkie 2012"}, 1)
      .tooltip = "Physical sky model used with a standard Sun or without a sun. A VeraaSun or Sun Positioner "
                 "uses its own sky settings";
  p.addFloat("skyHorizonBlur", "Horizon blur (deg)", 1.5, 0.0, 30.0, 0.1, 2).tooltip =
      "Width of the soft transition from the horizon haze to the ground below the horizon";
}

const std::vector<std::string>& renderLookPresetNames() {
  static const std::vector<std::string> names = {"Interior (archviz)", "Exterior daylight", "Product studio"};
  return names;
}

bool applyRenderLookPreset(ParamBlock& p, const std::string& name) {
  auto common = [&](int tonemap, float burn, float contrast, float saturation) {
    p.setNumber("tonemap", tonemap);
    p.setNumber("cmBurn", burn);
    p.setNumber("cmDarkMultiplier", 1.0);
    p.setNumber("cmBrightMultiplier", 1.0);
    p.setNumber("cmGamma", 1.0);
    p.setNumber("cmAffectBackground", 1);
    p.setNumber("cmAdaptationOnly", 0);
    p.setNumber("contrast", contrast);
    p.setNumber("saturation", saturation);
    p.setNumber("denoise", 1);
  };
  if (name == "Interior (archviz)") {
    // Soft Reinhard roll-off for bright windows and light fixtures, physical exposure for a lit
    // interior and a warm-neutral white balance between 3000 K lamps and daylight.
    common(1, 0.35f, 1.05f, 1.0f);
    p.setNumber("exposureControl", 1);
    p.setNumber("ev100", 8.0);
    p.setNumber("whiteBalanceK", 4700.0);
    p.setNumber("clampIndirect", 8.0);
    return true;
  }
  if (name == "Exterior daylight") {
    // Filmic (AgX-style) view for the sun's dynamic range; sunny-16 exposure.
    common(8, 0.0f, 1.08f, 1.05f);
    p.setNumber("exposureControl", 1);
    p.setNumber("ev100", 14.7);
    p.setNumber("whiteBalanceK", 6000.0);
    return true;
  }
  if (name == "Product studio") {
    common(0, 0.0f, 1.0f, 1.0f);
    return true;
  }
  return false;
}


// ---------------------------------------------------------------------------------------------
// Sampling quality: path guiding, light sampling, glossy filter and quality presets.

void defineSamplingQualitySettings(ParamBlock& p) {
  p.beginGroup("Global illumination");
  p.addFloat("filterGlossy", "Glossy filter (0 = off)", 1.0, 0.0, 10.0, 0.05, 2).tooltip =
      "Blurs sharp reflections reached after diffuse bounces: removes fireflies from small bright lights seen in "
      "them, at the cost of slightly softer indirect reflections";
  p.addEnum("lightSampling", "Light sampling", {"Light tree", "All lights", "One light by power"}, 0).tooltip =
      "Light tree picks the lights that matter at each point, so scenes with many lights stay clean";
  p.beginGroup("Path guiding");
  p.addBool("pathGuiding", "Path guiding", false).tooltip =
      "Learns where indirect light comes from while rendering and sends more rays there (interiors lit through "
      "windows, light bouncing off bright surfaces). On in the High, Very high and Supreme presets";
  p.addInt("guidingTrainingSamples", "Training samples per pixel", 64, 1, 4096).tooltip =
      "Passes that refine the learned light distribution; the passes after them use the final one";
}

namespace {

struct QualityPreset {
  const char* name;
  int samples, minSamples;
  double noiseThreshold;
  int maxDepth;
  double clampIndirect;
  bool guiding;
  int trainingSamples;
  double filterGlossy;
};

const QualityPreset kQualityPresets[] = {
    {"Draft", 64, 4, 0.05, 4, 5.0, false, 16, 1.5},
    {"Medium", 256, 8, 0.02, 6, 10.0, false, 32, 1.0},
    {"High", 1024, 16, 0.01, 8, 20.0, true, 64, 0.75},
    {"Very high", 4096, 32, 0.005, 12, 50.0, true, 128, 0.5},
    {"Supreme", 16384, 64, 0.0025, 16, 100.0, true, 256, 0.3},
};
constexpr int kQualityPresetCount = static_cast<int>(sizeof(kQualityPresets) / sizeof(kQualityPresets[0]));

std::string presetKey(const std::string& name) {
  std::string k;
  for (char c : name)
    if (std::isalnum(static_cast<unsigned char>(c))) k += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return k;
}

}  // namespace

const std::vector<std::string>& renderQualityPresetNames() {
  static const std::vector<std::string> names = [] {
    std::vector<std::string> n;
    for (const auto& q : kQualityPresets) n.push_back(q.name);
    return n;
  }();
  return names;
}

int renderQualityPresetIndex(const std::string& name) {
  std::string k = presetKey(name);
  for (int i = 0; i < kQualityPresetCount; ++i)
    if (presetKey(kQualityPresets[i].name) == k) return i;
  return -1;
}

bool applyRenderQualityPreset(ParamBlock& p, int preset) {
  if (preset < 0 || preset >= kQualityPresetCount) return false;
  const QualityPreset& q = kQualityPresets[preset];
  p.setNumber("samples", q.samples);
  p.setNumber("minSamples", q.minSamples);
  p.setNumber("noiseThreshold", q.noiseThreshold);
  p.setNumber("maxDepth", q.maxDepth);
  p.setNumber("clampIndirect", q.clampIndirect);
  p.setNumber("pathGuiding", q.guiding ? 1 : 0);
  p.setNumber("guidingTrainingSamples", q.trainingSamples);
  p.setNumber("filterGlossy", q.filterGlossy);
  p.setNumber("lightSampling", 0);
  p.setNumber("denoise", 1);
  return true;
}

int matchingRenderQualityPreset(const ParamBlock& p) {
  auto same = [](double a, double b) { return std::fabs(a - b) <= 1e-4 * std::max(1.0, std::fabs(b)); };
  for (int i = 0; i < kQualityPresetCount; ++i) {
    const QualityPreset& q = kQualityPresets[i];
    if (p.getInt("samples") == q.samples && p.getInt("minSamples") == q.minSamples &&
        same(p.getFloat("noiseThreshold"), q.noiseThreshold) && p.getInt("maxDepth") == q.maxDepth &&
        same(p.getFloat("clampIndirect"), q.clampIndirect) && p.getBool("pathGuiding") == q.guiding &&
        p.getInt("guidingTrainingSamples") == q.trainingSamples && same(p.getFloat("filterGlossy"), q.filterGlossy) &&
        p.getInt("lightSampling") == 0 && p.getBool("denoise"))
      return i;
  }
  return -1;
}

}  // namespace veraa
