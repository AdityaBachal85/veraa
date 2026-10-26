// Materials. A material has a registered type (VeraaMtl, VeraaBlendMtl, Multi/Sub-Object, ...),
// typed parameters, texture maps plugged into its parameters and, for compound types,
// sub-materials. The tree is copied deeply, so a Material can be passed around by value.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "params.h"
#include "texmap.h"

namespace veraa {

class Material;

// A sub-material of a compound material (Blend coats, Multi/Sub-Object IDs, Two-Sided back, ...).
struct SubMaterialSlot {
  std::unique_ptr<Material> material;  // null = "None"
  std::string label;                   // "Base material", "Coat 1", "Front", ...
  int matId = 1;                       // Multi/Sub-Object: material ID (1-based, like the UI)
  bool enabled = true;

  SubMaterialSlot() = default;
  SubMaterialSlot(const SubMaterialSlot& o);
  SubMaterialSlot& operator=(const SubMaterialSlot& o);
  SubMaterialSlot(SubMaterialSlot&&) noexcept = default;
  SubMaterialSlot& operator=(SubMaterialSlot&&) noexcept = default;
  bool active() const { return enabled && material != nullptr; }
};

class Material {
 public:
  Material();  // VeraaMtl
  explicit Material(const std::string& materialType);
  Material(const Material& o);
  Material& operator=(const Material& o);
  Material(Material&&) noexcept;
  Material& operator=(Material&&) noexcept;
  ~Material();

  // Default material type (kept for older callers).
  static std::string type() { return "VeraaMtl"; }

  json toJson() const;
  // Reads a material saved by toJson(); the material takes the saved type.
  void fromJson(const json& j);

  uint32_t id = 0;  // stable within a scene; nodes refer to materials by id (0 for sub-materials)
  std::string name = "Material";
  std::string materialType = "VeraaMtl";
  ParamBlock params;
  MapSlots maps;  // maps plugged into parameters, keyed by parameter key (or map-only slots: bump, ...)
  std::vector<SubMaterialSlot> subs;

  // Map plugged into a parameter (null when none or disabled).
  const TexMap* mapFor(const std::string& key) const;
  // Keys that accept a map (parameters and map-only slots such as "bump" and "displacement").
  const std::vector<std::string>& mappableKeys() const;
  // Changes the type: parameters with the same key keep their values, maps of keys that still
  // exist are kept, sub-materials are kept by position.
  void setMaterialType(const std::string& type);

  // Converts the legacy file parameters (diffuseMap, reflectMap, opacityMap, bumpMap, with
  // mapTileU/V) into Bitmap map slots. Returns true if anything changed.
  bool upgradeLegacyMaps();
  // Image file to show on viewport geometry: the first Bitmap that feeds the diffuse color
  // (searching through compound materials and maps), or the legacy diffuseMap. Unresolved.
  std::string viewportTexture() const;
  // Representative diffuse color for viewports.
  Vec3 viewportColor() const;

  // Multi/Sub-Object: the sub-material for a face material ID (0-based face ID as stored in
  // meshes; ID n+1 in the UI). Wraps around like 3ds Max when no slot matches. Null if none.
  const Material* subForFaceId(int faceMatId) const;
  // Every image file used by this material tree (for asset tracking and the bitmap cache).
  void collectFiles(std::vector<std::string>& files) const;
};

// ---- Material type registry ----

struct MaterialTypeInfo {
  std::string type;          // stored in files
  std::string displayName;   // shown in the material browser
  std::string category;      // browser group: "Materials", "Compound"
  std::function<void(Material&)> define;  // parameters and default sub-material slots
  std::vector<std::string> mappable;      // keys that accept maps
  std::string description;
};

void registerMaterialType(MaterialTypeInfo info);
const std::vector<MaterialTypeInfo>& materialTypes();
const MaterialTypeInfo* findMaterialType(const std::string& type);
std::string materialDisplayName(const std::string& type);

// VeraaMtl parameter keys (also used by the renderer):
//  Diffuse:      diffuse, roughness
//  Reflection:   reflect, reflectGlossiness, useRoughness, fresnel, lockIor, fresnelIor, metalness,
//                anisotropy, anisoRotation
//  Refraction:   refract, refractGlossiness, ior, abbe, fogColor, fogMultiplier, thinWalled,
//                affectShadows, exitColorOn, exitColor
//  Metal:        metalFresnel (0 artist F0, 1 custom n/k, 2+ measured metals), metalNr/Ng/Nb, metalKr/Kg/Kb
//  Thin film:    thinFilm, thinFilmLayer (0 reflection, 1 coat), thinFilmThickness (nm), thinFilmIor
//  Translucency: translucency (0 none, 1 thin, 2 volumetric), translucencyColor
//  Coat:         coatAmount, coatColor, coatGlossiness, coatIor        (map-only slot: coatBump)
//  Sheen:        sheenColor, sheenGlossiness
//  Self-illum:   selfIllum, selfIllumMultiplier, selfIllumGI
//  Opacity:      opacity, opacityMode (0 stochastic, 1 clip)
//  Bump:         bumpAmount, displacementAmount, displacementShift     (map-only: bump, displacement)
//  Legacy:       diffuseMap, diffuseMapAmount, reflectMap, opacityMap, bumpMap, mapTileU, mapTileV

}  // namespace veraa
