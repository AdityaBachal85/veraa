// Constructive solid geometry on polygon meshes (BSP-tree clipping) and the Boolean compound
// object that keeps its operands and re-evaluates the result.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "object.h"

namespace veraa {

enum class CsgOp { Union = 0, Intersection = 1, SubtractAB = 2, SubtractBA = 3, Merge = 4 };

// Boolean of two closed meshes given in the same space. The result is welded by position;
// UVs, material IDs and smoothing groups are carried over from the operands.
PolyMesh csgMesh(const PolyMesh& a, const PolyMesh& b, CsgOp op);

// Compound object: operand A (its own mesh) combined with any number of operands, each with
// its own operation and placement relative to the object.
class BooleanObject : public GeometryObject {
 public:
  BooleanObject();
  std::string type() const override { return "Boolean"; }
  std::unique_ptr<SceneObject> clone() const override { return std::make_unique<BooleanObject>(*this); }
  PolyMesh build() const override;
  json toJson() const override;
  void fromJson(const json& j) override;

  struct Operand {
    std::string name;
    PolyMesh mesh;     // in the operand's own object space
    Mat4 transform;    // operand object space -> this object's space
    CsgOp op = CsgOp::SubtractAB;
    uint32_t materialId = 0;
  };
  std::string nameA = "A";
  PolyMesh meshA;
  std::vector<Operand> operands;
};

void registerCompoundObjectTypes();  // Boolean

}  // namespace veraa
