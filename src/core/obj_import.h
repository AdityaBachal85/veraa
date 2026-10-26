// Wavefront OBJ (+ MTL) import.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "progress.h"

namespace veraa {

class Scene;

struct ObjImportOptions {
  bool yUp = true;          // most OBJ files are Y-up; Veraa is Z-up
  float scale = 1.0f;       // unit conversion multiplier
  bool importMaterials = true;
  ImportProgress* progress = nullptr;  // optional progress report and cancellation
};

struct ObjImportResult {
  std::vector<uint32_t> nodeIds;  // created nodes
  size_t triangles = 0;
  std::vector<std::string> warnings;
};

// Adds one node per (object/group, material) pair. Returns false and sets error on failure.
bool importObj(Scene& scene, const std::string& path, const ObjImportOptions& options, ObjImportResult* result,
               std::string* error);

}  // namespace veraa
