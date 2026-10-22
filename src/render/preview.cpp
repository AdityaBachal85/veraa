#include "renderer.h"

namespace veraa {

namespace {

// Euler angles that point a node's local -Z axis from `from` toward `to` (local +Y stays up).
Vec3 eulerLookingAt(const Vec3& from, const Vec3& to) {
  Vec3 dir = normalize(to - from);
  Vec3 right = cross(dir, Vec3(0, 0, 1));
  if (length2(right) < 1e-10f) right = Vec3(1, 0, 0);
  right = normalize(right);
  Vec3 up = cross(right, dir);
  Mat4 m;
  Vec3 cols[3] = {right, up, -dir};
  for (int c = 0; c < 3; ++c) {
    m.m[0][c] = cols[c].x;
    m.m[1][c] = cols[c].y;
    m.m[2][c] = cols[c].z;
  }
  return eulerDegFromMatrix(m);
}

}  // namespace

std::vector<uint8_t> renderMaterialPreview(const Material& material, const Scene* sceneForPaths, int size,
                                           int samples) {
  size = std::max(size, 8);
  Scene scene;
  if (sceneForPaths) scene.filePath = sceneForPaths->filePath;
  ParamBlock& rs = scene.renderSettings;
  rs.setNumber("width", size);
  rs.setNumber("height", size);
  rs.setNumber("samples", std::max(samples, 1));
  rs.setNumber("maxDepth", 8);
  rs.setNumber("envMode", 1);
  rs.setNumber("envIntensity", 0.5);
  rs.setNumber("denoise", 1);

  Material* mat = scene.addMaterial(std::make_unique<Material>(material));
  Node* sphere = scene.createNode("Sphere");
  sphere->object->params.setNumber("radius", 40);
  sphere->object->params.setNumber("segments", 64);
  sphere->transform.position = Vec3(0, 0, 40);
  sphere->materialId = mat->id;
  sphere->invalidate();

  Material* floorMat = scene.createMaterial("Preview floor");
  floorMat->params.setString("diffuseMap", ":checker");
  floorMat->params.setColor("diffuse", Vec3(1.0f));
  floorMat->params.setNumber("mapTileU", 6);
  floorMat->params.setNumber("mapTileV", 6);
  Node* floor = scene.createNode("Plane");
  floor->object->params.setNumber("length", 3000);
  floor->object->params.setNumber("width", 3000);
  floor->object->params.setNumber("lengthSegs", 1);
  floor->object->params.setNumber("widthSegs", 1);
  floor->materialId = floorMat->id;
  floor->invalidate();

  Node* key = scene.createNode("RectLight");
  key->object->params.setNumber("width", 150);
  key->object->params.setNumber("length", 150);
  key->object->params.setNumber("intensity", 6.0);
  key->object->params.setNumber("invisible", 1);
  key->transform.position = Vec3(-140, -160, 230);
  key->transform.rotation = eulerLookingAt(key->transform.position, Vec3(0, 0, 40));

  RenderView view;
  view.position = Vec3(0, -185, 85);
  view.target = Vec3(0, 0, 36);
  view.up = Vec3(0, 0, 1);
  view.fovYDeg = 30.0f;

  Renderer r;
  std::vector<uint8_t> rgba;
  if (!r.setScene(scene)) return std::vector<uint8_t>(static_cast<size_t>(size) * size * 4, 0);
  r.setView(view);
  r.renderBlocking();
  DisplaySettings d = displaySettingsFromScene(scene);
  d.denoise = Renderer::denoiserAvailable();
  r.resolve(d, rgba);
  if (rgba.size() != static_cast<size_t>(size) * size * 4) rgba.assign(static_cast<size_t>(size) * size * 4, 0);
  return rgba;
}

}  // namespace veraa
