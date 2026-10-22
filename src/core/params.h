// Named, typed parameters. Objects, modifiers, materials and render settings all keep
// their editable values in a ParamBlock, so the UI can build forms automatically and
// files can be saved/loaded generically.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "math.h"

namespace veraa {

using json = nlohmann::json;

enum class ParamType { Float, Int, Bool, Color, Enum, String, File };

struct ParamDef {
  std::string key;          // stable identifier used in files, e.g. "radius"
  std::string label;        // shown in the UI, e.g. "Radius"
  ParamType type = ParamType::Float;
  std::string group;        // UI rollout title, e.g. "Parameters"
  double minValue = -1e9;
  double maxValue = 1e9;
  double step = 1.0;        // spinner step
  int decimals = 2;         // spinner precision
  std::vector<std::string> items;  // Enum choices
  std::string fileFilter;          // File dialog filter, e.g. "Images (*.png *.jpg)"
  std::string tooltip;

  // Default value.
  double defNumber = 0.0;
  Vec3 defColor;
  std::string defString;
};

struct ParamValue {
  double number = 0.0;  // Float, Int, Bool, Enum
  Vec3 color;           // Color
  std::string text;     // String, File
};

class ParamBlock {
 public:
  // Definition helpers. Each returns the stored definition so callers can tweak extra fields.
  ParamDef& addFloat(const std::string& key, const std::string& label, double def, double minV, double maxV,
                     double step = 0.1, int decimals = 2);
  ParamDef& addInt(const std::string& key, const std::string& label, int def, int minV, int maxV);
  ParamDef& addBool(const std::string& key, const std::string& label, bool def);
  ParamDef& addColor(const std::string& key, const std::string& label, const Vec3& def);
  ParamDef& addEnum(const std::string& key, const std::string& label, std::vector<std::string> items, int def);
  ParamDef& addString(const std::string& key, const std::string& label, const std::string& def);
  ParamDef& addFile(const std::string& key, const std::string& label, const std::string& filter);
  // Subsequent add* calls are placed in this UI group.
  void beginGroup(const std::string& group) { currentGroup_ = group; }

  bool has(const std::string& key) const { return values_.count(key) != 0; }
  const ParamDef* def(const std::string& key) const;
  const std::vector<ParamDef>& defs() const { return defs_; }

  float getFloat(const std::string& key) const;
  int getInt(const std::string& key) const;
  bool getBool(const std::string& key) const;
  Vec3 getColor(const std::string& key) const;
  const std::string& getString(const std::string& key) const;

  // Setters clamp to the definition's range. Unknown keys are ignored. Return true if the value changed.
  bool setNumber(const std::string& key, double v);
  bool setColor(const std::string& key, const Vec3& c);
  bool setString(const std::string& key, const std::string& s);

  json toJson() const;
  // Reads values for known keys; missing keys keep their current values.
  void fromJson(const json& j);

 private:
  ParamDef& add(ParamDef d);
  std::vector<ParamDef> defs_;
  std::unordered_map<std::string, ParamValue> values_;
  std::string currentGroup_ = "Parameters";
};

json vec3ToJson(const Vec3& v);
Vec3 vec3FromJson(const json& j, const Vec3& fallback = Vec3());

}  // namespace veraa
