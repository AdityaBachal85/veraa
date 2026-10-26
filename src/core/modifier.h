// Modifier stack entries (Bend, Taper, Smooth, UVW Map, Extrude, ...). Each modifier takes the
// geometry produced by the object and the modifiers below it, and changes it.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "geomstate.h"
#include "mesh.h"
#include "params.h"
#include "shape.h"

namespace veraa {

class Modifier {
 public:
  virtual ~Modifier() = default;
  virtual std::string type() const = 0;
  virtual std::unique_ptr<Modifier> clone() const = 0;
  // Mesh modifiers override this. The default does nothing.
  virtual void apply(PolyMesh& mesh) const {}
  // Override for modifiers that work on splines or need the full state. The default applies
  // apply(mesh) when the state is a mesh.
  virtual void applyState(GeomState& state) const {
    if (!state.isShape) applyWithSelection(state);
  }
  // Runs apply(mesh) and, when a sub-object selection comes from below, keeps unselected
  // vertices in place (blending by weight). Topology changes drop the selection.
  void applyWithSelection(GeomState& state) const;

  virtual json toJson() const;
  virtual void fromJson(const json& j);

  // True when the result changes with scene time (re-evaluated on every time change).
  virtual bool timeDependent() const { return false; }
  // Other nodes this modifier reads (bones, targets, paths); the node re-evaluates when they move.
  virtual std::vector<uint32_t> referencedNodes() const { return {}; }

  // Bumped by touch(); nodes sharing this modifier (instances) re-evaluate when it changes.
  uint64_t revision() const { return revision_; }
  void touch();

  ParamBlock params;
  bool enabled = true;
  std::string displayName;  // optional user rename shown in the stack

 private:
  uint64_t revision_ = nextRevisionStamp();
};

// ---- Modifier type registry ----

struct ModifierTypeInfo {
  std::string type;         // stable name stored in files, e.g. "UVWMap"
  std::string displayName;  // e.g. "UVW Map"
  std::string category;     // Modifiers menu group, e.g. "Parametric Deformers", "Mesh Editing"
  bool acceptsMeshes = true;
  bool acceptsShapes = false;
  std::function<std::unique_ptr<Modifier>()> create;
};

void registerModifierType(ModifierTypeInfo info);
const std::vector<ModifierTypeInfo>& modifierTypeInfos();
const ModifierTypeInfo* findModifierType(const std::string& type);

std::unique_ptr<Modifier> createModifier(const std::string& type);
// Modifier type names, sorted alphabetically by display name (the Modifier List order).
const std::vector<std::string>& modifierTypes();
std::string modifierDisplayName(const std::string& type);

// Catmull-Clark subdivision (exposed for tests and the Editable Poly tools).
PolyMesh subdivideCatmullClark(const PolyMesh& mesh);

// Classic 3D Perlin noise in roughly [-1, 1].
float perlinNoise(float x, float y, float z);

}  // namespace veraa
