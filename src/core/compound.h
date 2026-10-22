// Compound Objects: Loft, Scatter, Connect, ShapeMerge, Terrain, Conform and BlobMesh.
// A compound object keeps copies of its operands (meshes or shapes) so it evaluates without the
// scene; each operand remembers the node it came from so the editor can refresh it.
// Registered by registerCompoundTypes() under Geometry > Compound Objects.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "object.h"

namespace veraa {

struct CompoundOperand {
  std::string role;        // "path", "shape", "source", "distribution", "target", ...
  uint32_t sourceId = 0;   // node the operand was picked from (0 = none)
  std::string name;        // that node's name, for display
  bool isShape = false;
  PolyMesh mesh;           // geometry operand
  BezierShape shape;       // shape operand
  float percent = 0.0f;    // Loft shapes: position on the path in percent
};

// How an operand's geometry is stored: in the operand's own object space (Loft shapes, Scatter
// source) or in the compound's object space (everything else).
enum class OperandSpace { Local, Compound };

struct CompoundRole {
  std::string role;
  std::string label;    // button text, e.g. "Pick Shape"
  bool shape = false;   // picks shapes (true) or geometry (false)
  bool multiple = false;
  OperandSpace space = OperandSpace::Compound;
};

class CompoundObject : public GeometryObject {
 public:
  json toJson() const override;
  void fromJson(const json& j) override;

  std::vector<const CompoundOperand*> operandsWithRole(const std::string& role) const;
  const CompoundOperand* firstOperand(const std::string& role) const;
  // Replaces (single roles) or adds (multiple roles) an operand.
  void setOperand(CompoundOperand op);
  // The pickable roles of this type, in UI order.
  virtual std::vector<CompoundRole> roles() const = 0;
  const CompoundRole* findRole(const std::string& role) const;

  std::vector<CompoundOperand> operands;
};

// ---- Building blocks (exposed for tests) ----

// Marching-tetrahedra metaball surface of spheres (centers, radius) blended with tension (0..1].
PolyMesh metaballMesh(const std::vector<Vec3>& centers, float radius, float tension, float cellSize);
// Triangulated irregular network through the points (Delaunay in XY).
PolyMesh terrainMesh(const std::vector<Vec3>& points, bool solid, float baseZ, int zones);
// Boundary loops of a mesh (edges used by one face), each in face winding order.
std::vector<std::vector<int>> boundaryLoops(const PolyMesh& m);

void registerCompoundTypes();

}  // namespace veraa
