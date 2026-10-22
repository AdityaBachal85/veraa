// Texture maps (Bitmap, Checker, Noise, Falloff, Tiles, ...). A map has typed parameters and
// can hold further maps in its own parameter slots, forming a tree like the 3ds Max
// material/map tree. Materials and lights plug maps into their parameters through MapSlot.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "params.h"

namespace veraa {

class TexMap;

// A map plugged into a parameter (of a material or another map), with the 3ds Max style
// "amount" that blends between the parameter's own value and the map.
struct MapSlot {
  std::unique_ptr<TexMap> map;
  float amount = 1.0f;
  bool enabled = true;

  MapSlot() = default;
  MapSlot(const MapSlot& o);
  MapSlot& operator=(const MapSlot& o);
  MapSlot(MapSlot&&) = default;
  MapSlot& operator=(MapSlot&&) = default;
  bool active() const { return enabled && map != nullptr; }
};

using MapSlots = std::map<std::string, MapSlot>;

class TexMap {
 public:
  // Creates a map of a registered type with its default parameters.
  explicit TexMap(const std::string& type = "Bitmap");
  TexMap(const TexMap& o) = default;
  TexMap& operator=(const TexMap& o) = default;

  std::string type;  // "Bitmap", "Checker", ...
  std::string name;  // shown in editors, e.g. "Map #3"
  ParamBlock params;
  MapSlots maps;     // sub-maps keyed by parameter key

  // Map plugged into a parameter, or null.
  const TexMap* mapFor(const std::string& key) const;
  TexMap* mapFor(const std::string& key);
  // Parameters that accept a sub-map.
  const std::vector<std::string>& mappableKeys() const;

  json toJson() const;
  // Returns null for unknown map types.
  static std::unique_ptr<TexMap> fromJson(const json& j);
};

// ---- Map type registry ----

struct TexMapTypeInfo {
  std::string type;         // stable name stored in files
  std::string displayName;  // e.g. "Gradient Ramp"
  std::string category;     // browser group: "2D Maps", "3D Maps", "Compositors", "Color Modifiers", "Other"
  std::function<void(ParamBlock&)> define;
  std::vector<std::string> mappable;  // parameter keys that accept sub-maps
};

void registerTexMapType(TexMapTypeInfo info);
const std::vector<TexMapTypeInfo>& texMapTypes();
const TexMapTypeInfo* findTexMapType(const std::string& type);
std::unique_ptr<TexMap> createTexMap(const std::string& type);

// Helpers shared by map definitions (also used by renderer code to read keys consistently).
// 2D placement: mapping, uvChannel, uOffset, vOffset, uTiling, vTiling, wAngle, uWrap, vWrap,
// uMirror, vMirror, realWorld, realWidth, realHeight.
void addCoords2D(ParamBlock& p, float defaultTiling = 1.0f);
// 3D placement: source (0 object, 1 world, 2 UV), offsetX/Y/Z, tilingX/Y/Z.
void addCoords3D(ParamBlock& p);
// Output: outputAmount, rgbOffset, rgbLevel, invert, clampOutput.
void addOutput(ParamBlock& p);

json mapSlotsToJson(const MapSlots& slots);
MapSlots mapSlotsFromJson(const json& j);

// Blend modes used by the Composite map (index = "layerNMode" value).
const std::vector<std::string>& compositeBlendModes();

}  // namespace veraa
