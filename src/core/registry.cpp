#include "registry.h"

namespace veraa {

// Registration order is the order of the Create panel's categories and subcategories.
void registerBuiltinObjectTypes() {
  registerPrimitiveTypes();
  registerShapeGeometryTypes();
  registerCoreObjectTypes();
  registerShapeTypes();
  registerCompoundTypes();
  registerParticleTypes();
  registerPatchGridTypes();
  registerAecTypes();
  registerRiggingTypes();
}

void registerBuiltinModifierTypes() {
  registerCoreModifierTypes();
  registerShapeModifierTypes();
  registerRiggingModifiers();
  registerParticleModifiers();
  registerDynamicsModifiers();
}

void registerBuiltinSceneModules() {
  registerAnimationModule();
}

}  // namespace veraa
