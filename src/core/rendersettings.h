// Render settings added by the rendering module (sampler, GI options, render elements,
// atmosphere, exposure, lens effects and frame buffer color corrections). Called at the end of
// defineRenderSettings(), so every Scene has them.
#pragma once

#include <string>
#include <vector>

#include "params.h"

namespace veraa {

void defineAdvancedRenderSettings(ParamBlock& p);
// Color mapping parameters and physical sky settings (called by defineAdvancedRenderSettings).
void defineRealismRenderSettings(ParamBlock& p);

// Render elements (AOVs). The key of element i is renderElementKeys()[i]; the setting
// "elem" + key enables it (e.g. "elemReflection").
struct RenderElementInfo {
  const char* key;
  const char* label;
  bool sumsToBeauty;  // part of the beauty decomposition
};
const std::vector<RenderElementInfo>& renderElementInfos();

// Keys:
//  Sampler:     samplerType (0 progressive, 1 bucket), minSamples, noiseThreshold (0 = off),
//               bucketSize, renderRegion, regionX0, regionY0, regionX1, regionY1 (0..1 of the
//               image, top-left origin), renderSelected (string: comma separated node ids)
//  GI:          caustics, clampDirect, giMultiplier
//  Exposure:    exposureControl (0 none, 1 physical EV100), ev100, whiteBalanceK
//  Atmosphere:  fogOn, fogType (0 uniform, 1 height), fogColor, fogDistance, fogHeight,
//               fogFalloff, fogAnisotropy, fogAffectBackground, fogEmission
//  Lens:        lensEffects, bloomIntensity, bloomSize, glareIntensity, glareSize, glareRays,
//               glareRotation, vignetting
//  Corrections: ccOn, ccWhiteBalanceK, ccTint, ccHue, ccHighlightBurn, ccCurve (points
//               "x,y;x,y"), ccLevelsBlack, ccLevelsWhite, ccLevelsGamma, ccLutFile, ccLutStrength
//  Elements:    elem<Key> (bool) per renderElementInfos(), zDepthMin, zDepthMax, lightMix
//  Mapping:     cmDarkMultiplier, cmBrightMultiplier, cmGamma, cmBurn, cmAffectBackground,
//               cmAdaptationOnly (the type is "tonemap")
//  Sky:         skyModel (0 Preetham, 1 physical atmosphere, 2 Hosek-Wilkie; standard sun),
//               skyHorizonBlur (degrees)
//  Batch:       stored in Scene::extensions["batchRender"]

}  // namespace veraa
