// Scene exchange formats: FBX (import via ufbx, ASCII export), glTF 2.0 (.gltf/.glb, import via
// cgltf, export), STL and PLY (import/export), 3DS (import) and OBJ (export with MTL; import is in
// obj_import.h), and 3ds Max .max scenes (import, see io_max.cpp). Veraa is Z-up in centimeters; files are converted on the way in and out.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "progress.h"

namespace veraa {

class Scene;

struct ImportOptions {
  float scale = 1.0f;          // extra multiplier after unit conversion
  bool convertUnits = true;    // use the file's units (glTF meters, FBX unit scale) -> centimeters
  bool convertAxes = true;     // Y-up formats (glTF, FBX with Y up) -> Z-up
  bool importMaterials = true;
  bool importLights = true;
  bool importCameras = true;
  bool weld = true;            // weld identical vertex positions (STL, glTF)
  bool yUpOverride = false;    // STL/PLY/OBJ: treat the file as Y-up
  ImportProgress* progress = nullptr;  // optional progress report and cancellation (not all formats)
};

struct ImportResult {
  std::vector<uint32_t> nodeIds;  // created nodes
  size_t triangles = 0;
  std::vector<std::string> warnings;
};

struct ExportOptions {
  float scale = 1.0f;               // extra multiplier after unit conversion
  bool yUp = true;                  // glTF is always Y-up; OBJ/STL/PLY use this flag
  bool binary = true;               // STL/PLY binary; glTF: .glb is binary regardless
  bool exportMaterials = true;
  std::vector<uint32_t> nodeIds;    // export only these (and their descendants); empty = all
};

// Lower-case extensions (without dot) the dispatchers accept.
std::vector<std::string> importFormats();
std::vector<std::string> exportFormats();

// Picks the reader/writer from the file extension. OBJ import goes through importObj.
bool importScene(Scene& scene, const std::string& path, const ImportOptions& options, ImportResult* result,
                 std::string* error);
bool exportScene(const Scene& scene, const std::string& path, const ExportOptions& options, std::string* error);

bool importFbx(Scene& scene, const std::string& path, const ImportOptions& options, ImportResult* result, std::string* error);
bool importGltf(Scene& scene, const std::string& path, const ImportOptions& options, ImportResult* result, std::string* error);
bool importStl(Scene& scene, const std::string& path, const ImportOptions& options, ImportResult* result, std::string* error);
bool importPly(Scene& scene, const std::string& path, const ImportOptions& options, ImportResult* result, std::string* error);
bool import3ds(Scene& scene, const std::string& path, const ImportOptions& options, ImportResult* result, std::string* error);
// 3ds Max scene. The first warning is always a summary line (file version and what was
// imported); the rest list what was skipped or approximated.
bool importMax(Scene& scene, const std::string& path, const ImportOptions& options, ImportResult* result, std::string* error);

bool exportFbx(const Scene& scene, const std::string& path, const ExportOptions& options, std::string* error);
bool exportGltf(const Scene& scene, const std::string& path, const ExportOptions& options, std::string* error);
bool exportStl(const Scene& scene, const std::string& path, const ExportOptions& options, std::string* error);
bool exportPly(const Scene& scene, const std::string& path, const ExportOptions& options, std::string* error);
bool exportObj(const Scene& scene, const std::string& path, const ExportOptions& options, std::string* error);

}  // namespace veraa
