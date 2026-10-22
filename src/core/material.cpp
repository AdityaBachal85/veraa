#include "material.h"

namespace veraa {

namespace {
const char* kImageFilter = "Images (*.png *.jpg *.jpeg *.bmp *.tga *.hdr *.exr)";
}

Material::Material() {
  params.beginGroup("Diffuse");
  params.addColor("diffuse", "Diffuse", Vec3(0.5f));
  params.addFile("diffuseMap", "Diffuse map", kImageFilter);
  params.addFloat("diffuseMapAmount", "Map amount", 1.0, 0.0, 1.0, 0.05);
  params.addFloat("roughness", "Roughness", 0.0, 0.0, 1.0, 0.05);

  params.beginGroup("Reflection");
  params.addColor("reflect", "Reflect", Vec3(0.0f));
  params.addFile("reflectMap", "Reflect map", kImageFilter);
  params.addFloat("reflectGlossiness", "Glossiness", 1.0, 0.0, 1.0, 0.01, 3);
  params.addBool("fresnel", "Fresnel reflections", true);
  params.addFloat("fresnelIor", "Fresnel IOR", 1.6, 1.0, 50.0, 0.01, 3);
  params.addFloat("metalness", "Metalness", 0.0, 0.0, 1.0, 0.05);

  params.beginGroup("Refraction");
  params.addColor("refract", "Refract", Vec3(0.0f));
  params.addFloat("refractGlossiness", "Glossiness", 1.0, 0.0, 1.0, 0.01, 3);
  params.addFloat("ior", "IOR", 1.6, 1.0, 50.0, 0.01, 3);
  params.addColor("fogColor", "Fog color", Vec3(1.0f));
  params.addFloat("fogMultiplier", "Fog multiplier", 1.0, 0.0, 1e4, 0.1);
  params.addBool("thinWalled", "Thin-walled", false);
  params.addBool("affectShadows", "Affect shadows", true);

  params.beginGroup("Self-illumination");
  params.addColor("selfIllum", "Self-illumination", Vec3(0.0f));
  params.addFloat("selfIllumMultiplier", "Multiplier", 1.0, 0.0, 1e6, 0.1);

  params.beginGroup("Maps");
  params.addFloat("opacity", "Opacity", 1.0, 0.0, 1.0, 0.05);
  params.addFile("opacityMap", "Opacity map", kImageFilter);
  params.addFile("bumpMap", "Bump map", kImageFilter);
  params.addFloat("bumpAmount", "Bump amount", 1.0, -100.0, 100.0, 0.1);
  params.addFloat("mapTileU", "Map tiling U", 1.0, -1e4, 1e4, 0.1);
  params.addFloat("mapTileV", "Map tiling V", 1.0, -1e4, 1e4, 0.1);
}

json Material::toJson() const {
  return json{{"id", id}, {"name", name}, {"type", type()}, {"params", params.toJson()}};
}

void Material::fromJson(const json& j) {
  id = j.value("id", id);
  name = j.value("name", name);
  if (j.contains("params")) params.fromJson(j["params"]);
}

}  // namespace veraa
