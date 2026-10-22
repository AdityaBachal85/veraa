// Central list of built-in object and modifier types. Each feature module provides one
// register function and adds a single line to registry.cpp, so modules never edit each
// other's factories.
#pragma once

namespace veraa {

void registerBuiltinObjectTypes();    // called once, on first registry access
void registerBuiltinModifierTypes();  // called once, on first registry access

// Module register functions.
void registerPrimitiveTypes();      // primitives.cpp: Standard Primitives, Chamfer Box
void registerCoreObjectTypes();     // object.cpp: Editable Poly/Spline, lights, camera
void registerCoreModifierTypes();   // modifier.cpp: Bend, Taper, Twist, Noise, Smooth, ...
void registerPolyModifierTypes();   // polymods.cpp: Edit Poly, Poly/Mesh/Vol. Select, Turn to Poly

}  // namespace veraa
