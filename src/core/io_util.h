// Shared helpers for the exchange formats (internal to veraa_core).
#pragma once

#include <array>
#include <string>
#include <vector>

#include "io_formats.h"
#include "scene.h"

namespace veraa::io {

std::string lowerExtension(const std::string& path);
std::string directoryOf(const std::string& path);
std::string fileNameOf(const std::string& path);
std::string baseNameOf(const std::string& path);  // file name without extension
// Joins dir and a relative path (absolute paths are returned unchanged).
std::string joinPath(const std::string& dir, const std::string& rel);
// Path of `target` relative to `dir` when it is inside it, otherwise the absolute path.
std::string relativePath(const std::string& dir, const std::string& target);

// Nodes to export (the listed ones and their descendants, or everything), in scene order.
std::vector<const Node*> exportNodes(const Scene& scene, const ExportOptions& options);
// A node's mesh in node space (modifier stack result with the pivot offset applied).
PolyMesh nodeSpaceMesh(const Node& n);

// Creates an Editable Poly node holding `mesh` (name made unique). Returns null on failure.
Node* addMeshNode(Scene& scene, const std::string& name, PolyMesh mesh);
// Same, sharing an existing mesh (copy-on-write) with other nodes.
Node* addMeshNode(Scene& scene, const std::string& name, std::shared_ptr<const PolyMesh> mesh);
// Creates a material with a diffuse color/texture and returns its id.
uint32_t addMaterial(Scene& scene, const std::string& name, const Vec3& diffuse, const std::string& diffuseMap,
                     float opacity);
// Unique node name based on `name` (or "Object").
std::string uniqueName(const Scene& scene, const std::string& name);

// Euler XYZ (degrees) aiming local -Z along dir with +Z kept up (lights, cameras).
Vec3 lookAlong(const Vec3& dir);

// Fan-triangulates polygons (for STL and glTF, which need triangles).
std::vector<std::array<int, 3>> fanTriangles(const PolyMesh& mesh, std::vector<int>* faceOfTriangle = nullptr);

}  // namespace veraa::io
