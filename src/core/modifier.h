// Modifier stack entries (Bend, Taper, Smooth, UVW Map, ...). Each modifier takes the mesh
// produced by the object and the modifiers below it, and changes it in place.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "mesh.h"
#include "params.h"

namespace veraa {

class Modifier {
 public:
  virtual ~Modifier() = default;
  virtual std::string type() const = 0;
  virtual std::unique_ptr<Modifier> clone() const = 0;
  virtual void apply(PolyMesh& mesh) const = 0;

  json toJson() const;
  void fromJson(const json& j);

  ParamBlock params;
  bool enabled = true;
};

std::unique_ptr<Modifier> createModifier(const std::string& type);
// Modifier types in the order shown in the Modifier List.
const std::vector<std::string>& modifierTypes();
std::string modifierDisplayName(const std::string& type);

// Catmull-Clark subdivision (exposed for tests and the future Editable Poly tools).
PolyMesh subdivideCatmullClark(const PolyMesh& mesh);

// Classic 3D Perlin noise in roughly [-1, 1].
float perlinNoise(float x, float y, float z);

}  // namespace veraa
