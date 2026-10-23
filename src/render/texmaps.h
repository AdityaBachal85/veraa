// Evaluation of texture map trees (core TexMap) at shading points.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "shadecontext.h"
#include "texmap.h"
#include "texture.h"

namespace veraa {
class Scene;

namespace render {

class MapNode {
 public:
  virtual ~MapNode() = default;
  virtual Vec3 color(const ShadeContext& sc) const = 0;
  // Scalar use (glossiness, amounts, bump height): the RGB average.
  virtual float value(const ShadeContext& sc) const {
    Vec3 c = color(sc);
    return (c.x + c.y + c.z) * (1.0f / 3.0f);
  }
  // Opacity use: Bitmaps return their alpha channel when they have one.
  virtual float opacity(const ShadeContext& sc) const { return clampf(value(sc), 0.0f, 1.0f); }
  // Normal Bump maps perturb the normal directly instead of acting as a height field.
  virtual bool isNormalMap() const { return false; }
  virtual Vec3 perturbNormal(const ShadeContext& sc) const { return sc.ns; }
};
using MapPtr = std::shared_ptr<const MapNode>;

struct MapCompileContext {
  const Scene* scene = nullptr;
  TextureCache* cache = nullptr;
  std::vector<std::string>* warnings = nullptr;
  // Set by maps that need extra data from the renderer.
  bool needsTracer = false;
  bool needsElements = false;
  bool needsEdges = false;
  // Distance map object lists; the index is the field id passed to Tracer::distanceTo.
  std::vector<std::vector<std::string>> distanceFields;
};

MapPtr compileMap(const TexMap* map, MapCompileContext& ctx);

// A material or map parameter: its own value, optionally replaced by a map (blended by amount).
struct ColorInput {
  Vec3 value;
  MapPtr map;
  float amount = 1.0f;
  bool mapped() const { return map != nullptr; }
  Vec3 eval(const ShadeContext& sc) const {
    if (!map) return value;
    Vec3 m = map->color(sc);
    return amount >= 1.0f ? m : lerp(value, m, amount);
  }
};

struct FloatInput {
  float value = 0.0f;
  MapPtr map;
  float amount = 1.0f;
  bool mapped() const { return map != nullptr; }
  float eval(const ShadeContext& sc) const {
    if (!map) return value;
    float m = map->value(sc);
    return amount >= 1.0f ? m : value + (m - value) * amount;
  }
};

ColorInput colorInput(const ParamBlock& p, const MapSlots& maps, const std::string& key, MapCompileContext& ctx);
FloatInput floatInput(const ParamBlock& p, const MapSlots& maps, const std::string& key, MapCompileContext& ctx);
// A map-only slot (bump, displacement): null when empty.
MapPtr slotMap(const MapSlots& maps, const std::string& key, MapCompileContext& ctx, float* amount = nullptr);

// HSV helpers (h in [0, 1)).
Vec3 rgbToHsv(const Vec3& c);
Vec3 hsvToRgb(const Vec3& hsv);
uint32_t hashU32(uint32_t x);
float hashFloat(uint32_t x);  // [0, 1)

// Evaluates a map over a square UV swatch (map previews in the material editor). RGBA8.
std::vector<uint8_t> renderMapSwatch(const TexMap& map, const Scene* sceneForPaths, int size);

}  // namespace render
}  // namespace veraa
