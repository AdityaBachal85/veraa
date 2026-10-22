// Inverse kinematics solvers working on joint positions.
#pragma once

#include <vector>

#include "vmath.h"

namespace veraa {

// FABRIK: moves joints (joints[0] stays fixed) so the last joint reaches `goal`, keeping segment
// lengths. Returns true when the goal was reached within `tolerance`.
bool solveFabrik(std::vector<Vec3>& joints, const Vec3& goal, int iterations = 20, float tolerance = 0.01f);

// Analytic two-bone (limb) solver. root stays fixed; mid and end are updated. The bend stays in
// the plane of the current pose (or poleHint when the pose is straight).
bool solveTwoBone(const Vec3& root, Vec3& mid, Vec3& end, const Vec3& goal, const Vec3& poleHint);

// Rotates joints[1..n-2] about the axis from joints[0] to joints.back() (the swivel angle).
void applySwivel(std::vector<Vec3>& joints, float degrees);

// Shortest rotation taking unit vector a onto unit vector b, as a rotation matrix.
Mat4 rotationBetween(const Vec3& a, const Vec3& b);

}  // namespace veraa
