#include "rendersettings.h"

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

}  // namespace veraa
