// Central list of built-in object and modifier types. Each feature module provides one
// register function and adds a single line to registry.cpp, so modules never edit each
// other's factories.
#pragma once

namespace veraa {

void registerBuiltinObjectTypes();    // called once, on first registry access
void registerBuiltinModifierTypes();  // called once, on first registry access
void registerBuiltinSceneModules();   // called once, when the first Scene is created

// Module register functions.
void registerPrimitiveTypes();      // primitives.cpp: Standard Primitives, Chamfer Box
void registerCoreObjectTypes();     // object.cpp: Editable Poly/Spline, lights, camera
void registerCoreModifierTypes();   // modifier.cpp: Bend, Taper, Twist, Noise, Smooth, ...
void registerAnimationModule();     // animation.cpp: keyframe tracks, controllers, constraints
void registerRiggingTypes();        // rigging.cpp: Bones, IK chains, Skin, Morpher, Path Deform
void registerParticleTypes();       // particles.cpp: Spray, Snow, Super Spray, space warps
void registerRiggingModifiers();    // rigging.cpp: Skin, Morpher, Path Deform
void registerParticleModifiers();   // particles.cpp: Space Warp Binding
void registerDynamicsModifiers();   // dynamics.cpp: Rigid Body
void registerShapeTypes();          // shapes.cpp: Splines and Extended Splines
void registerShapeGeometryTypes();  // shape_modifiers.cpp: TextPlus
void registerShapeModifierTypes();  // shape_modifiers.cpp: Extrude, Lathe, Sweep, Bevel, Edit Spline, ...
void registerCompoundTypes();       // compound.cpp: Loft, Scatter, Connect, ShapeMerge, Terrain, Conform, BlobMesh
void registerPatchGridTypes();      // patch_nurbs.cpp: Quad Patch, Tri Patch (NURBS curves: via registerShapeTypes)
void registerAecTypes();            // aec_*.cpp: Doors, Windows, Wall, Railing, Foliage, Stairs

}  // namespace veraa
