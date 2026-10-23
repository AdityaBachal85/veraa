#include "registry.h"

namespace veraa {

// Registration order is the order of the Create panel's categories and subcategories.
void registerBuiltinObjectTypes() {
  registerPrimitiveTypes();
  registerExtendedPrimitiveTypes();
  registerShapeGeometryTypes();
  registerCoreObjectTypes();
  registerShapeTypes();
  registerCompoundTypes();
  registerCompoundObjectTypes();
  registerParticleTypes();
  registerPatchGridTypes();
  registerAecTypes();
  registerRiggingTypes();
  registerGroupObjectTypes();
  registerRenderObjectTypes();
}

void registerBuiltinModifierTypes() {
  registerCoreModifierTypes();
  registerPolyModifierTypes();
  registerMeshModifierTypes();
  registerShapeModifierTypes();
  registerRiggingModifiers();
  registerParticleModifiers();
  registerDynamicsModifiers();
  registerRenderModifierTypes();
}

void registerBuiltinSceneModules() {
  registerAnimationModule();
}

}  // namespace veraa
