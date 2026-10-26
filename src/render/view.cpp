#include "motionhook.h"
#include "postprocess.h"
#include "renderer.h"
#include "renderobjects.h"

namespace veraa {

RenderView viewFromCamera(const Node& node, float aspect) {
  RenderView v;
  Mat4 M = node.worldMatrix();
  v.position = M.translation();
  Vec3 forward = normalize(M.transformDir(Vec3(0, 0, -1)));
  v.up = normalize(M.transformDir(Vec3(0, 1, 0)));
  const CameraObject* cam = node.camera();
  if (!cam) {
    v.target = v.position + forward * v.focusDistance;
    return v;
  }
  const ParamBlock& p = cam->params;
  v.fovYDeg = cam->verticalFovDeg(aspect);
  v.focusDistance = std::max(p.getFloat("focusDistance"), 0.01f);
  if (cam->type() == "VeraaPhysicalCamera") {
    const auto* pc = static_cast<const VeraaPhysicalCameraObject*>(cam);
    const Scene* scene = node.scene();
    float fps = scene ? scene->time.fps : 30.0f;
    v.apertureRadius = p.getBool("dof") ? pc->apertureRadius() : 0.0f;
    v.blades = p.getInt("blades");
    v.bladeRotationDeg = p.getFloat("bladeRotation");
    v.bokehAnisotropy = p.getFloat("bokehAnisotropy");
    v.exposure = pc->exposureMultiplier(fps);
    v.whiteBalance = pc->whiteBalance();
    v.vignetting = p.getBool("vignettingOn") ? p.getFloat("vignetting") * 0.5f : 0.0f;
    v.distortion = p.getFloat("distortion");
    v.shiftX = p.getFloat("horizontalShift") * 2.0f;
    v.shiftY = p.getFloat("verticalShift") * 2.0f;
    if (p.getBool("autoVerticalTilt") && std::fabs(forward.z) < 0.95f) {
      // Keep verticals straight: level the camera and shift the lens instead of tilting.
      float pitch = std::asin(clampf(forward.z, -1.0f, 1.0f));
      Vec3 level = normalize(Vec3(forward.x, forward.y, 0.0f));
      float tanHalf = std::tan(radians(v.fovYDeg) * 0.5f);
      v.shiftY += std::tan(pitch) / std::max(tanHalf, 1e-4f);
      forward = level;
      v.up = Vec3(0, 0, 1);
    }
    if (p.getBool("clipping")) {
      v.nearClip = p.getFloat("nearClip");
      v.farClip = p.getFloat("farClip");
    }
    if (p.getBool("motionBlur") && scene) {
      double frame = scene->time.currentFrame;
      double dur = pc->shutterSeconds(fps) * fps;  // frames
      v.motionBlur = true;
      v.frame = frame;
      v.shutterOpen = frame;
      v.shutterClose = frame + dur;
      Mat4 m1;
      if (nodeMatrixAtTime(*scene, node, v.shutterClose, true, m1)) {
        Mat4 m0 = M;
        nodeMatrixAtTime(*scene, node, v.shutterOpen, true, m0);
        v.position = m0.translation();
        Vec3 f0 = normalize(m0.transformDir(Vec3(0, 0, -1)));
        v.up = normalize(m0.transformDir(Vec3(0, 1, 0)));
        forward = f0;
        v.cameraMoves = true;
        v.positionClose = m1.translation();
        v.targetClose = v.positionClose + normalize(m1.transformDir(Vec3(0, 0, -1))) * v.focusDistance;
        v.upClose = normalize(m1.transformDir(Vec3(0, 1, 0)));
      }
    }
  } else {
    v.apertureRadius = p.getBool("dof") ? p.getFloat("aperture") : 0.0f;
  }
  v.target = v.position + forward * v.focusDistance;
  return v;
}

RenderView defaultViewForScene(const Scene& scene, float aspect) {
  RenderView v;
  Aabb b = scene.bounds(true);
  if (!b.valid()) return v;
  Vec3 c = b.center();
  float r = std::max(b.radius(), 1.0f);
  v.fovYDeg = 40.0f;
  float halfY = radians(v.fovYDeg * 0.5f);
  float halfX = std::atan(std::tan(halfY) * std::max(aspect, 1e-3f));
  float half = std::min(halfX, halfY);
  float dist = r / std::sin(half) * 1.05f;
  Vec3 dir = normalize(Vec3(0.9f, -1.6f, 0.95f));  // front-right, above (front view looks along +Y)
  v.position = c + dir * dist;
  v.target = c;
  v.up = Vec3(0, 0, 1);
  v.focusDistance = dist;
  return v;
}

DisplaySettings displaySettingsFromScene(const Scene& scene) {
  const ParamBlock& p = scene.renderSettings;
  DisplaySettings d;
  d.exposureEV = p.getFloat("exposure");
  d.tonemap = p.getInt("tonemap");
  d.mapDark = p.getFloat("cmDarkMultiplier");
  d.mapBright = p.getFloat("cmBrightMultiplier");
  d.mapGamma = p.getFloat("cmGamma");
  d.mapBurn = p.getFloat("cmBurn");
  d.mapAffectBackground = p.getBool("cmAffectBackground");
  d.mapAdaptationOnly = p.getBool("cmAdaptationOnly");
  d.contrast = p.getFloat("contrast");
  d.saturation = p.getFloat("saturation");
  d.denoise = p.getBool("denoise");
  d.corrections = p.getBool("ccOn");
  d.whiteBalanceK = p.getFloat("ccWhiteBalanceK");
  d.tint = p.getFloat("ccTint");
  d.hueShift = p.getFloat("ccHue");
  d.highlightBurn = p.getFloat("ccHighlightBurn");
  d.curve = render::parseCurve(p.getString("ccCurve"));
  d.levelsBlack = p.getFloat("ccLevelsBlack");
  d.levelsWhite = p.getFloat("ccLevelsWhite");
  d.levelsGamma = p.getFloat("ccLevelsGamma");
  d.lutFile = p.getString("ccLutFile").empty() ? std::string() : scene.resolvePath(p.getString("ccLutFile"));
  d.lutStrength = p.getFloat("ccLutStrength");
  d.lensEffects = p.getBool("lensEffects");
  d.bloomIntensity = p.getFloat("bloomIntensity");
  d.bloomSize = p.getFloat("bloomSize");
  d.glareIntensity = p.getFloat("glareIntensity");
  d.glareSize = p.getFloat("glareSize");
  d.glareRays = p.getInt("glareRays");
  d.glareRotation = p.getFloat("glareRotation");
  d.vignetting = p.getFloat("vignetting");
  // Saved Light Mix (written by the frame buffer).
  auto it = scene.extensions.find("lightMix");
  if (it != scene.extensions.end() && it->is_object()) {
    d.useLightMix = it->value("enabled", false);
    if (it->contains("groups") && (*it)["groups"].is_array())
      for (const auto& g : (*it)["groups"]) {
        LightMixGroup m;
        m.name = g.value("name", std::string());
        if (g.contains("color")) m.color = vec3FromJson(g["color"], Vec3(1.0f));
        m.intensity = g.value("intensity", 1.0f);
        m.enabled = g.value("enabled", true);
        d.lightMix.push_back(m);
      }
  }
  return d;
}

}  // namespace veraa
