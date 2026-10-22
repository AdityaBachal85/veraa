#include "renderer.h"

namespace veraa {

RenderView viewFromCamera(const Node& node, float aspect) {
  RenderView v;
  Mat4 M = node.worldMatrix();
  v.position = M.translation();
  Vec3 forward = normalize(M.transformDir(Vec3(0, 0, -1)));
  v.up = normalize(M.transformDir(Vec3(0, 1, 0)));
  if (const CameraObject* cam = node.camera()) {
    const ParamBlock& p = cam->params;
    v.fovYDeg = cam->verticalFovDeg(aspect);
    v.focusDistance = std::max(p.getFloat("focusDistance"), 0.01f);
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
  d.contrast = p.getFloat("contrast");
  d.saturation = p.getFloat("saturation");
  d.denoise = p.getBool("denoise");
  return d;
}

}  // namespace veraa
