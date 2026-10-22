#include "registry.h"

namespace veraa {

void registerBuiltinObjectTypes() {
  registerPrimitiveTypes();
  registerCoreObjectTypes();
  registerRiggingTypes();
  registerParticleTypes();
}

void registerBuiltinModifierTypes() {
  registerCoreModifierTypes();
  registerRiggingModifiers();
  registerParticleModifiers();
  registerDynamicsModifiers();
}

void registerBuiltinSceneModules() {
  registerAnimationModule();
}

}  // namespace veraa
