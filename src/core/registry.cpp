#include "registry.h"

namespace veraa {

void registerBuiltinObjectTypes() {
  registerPrimitiveTypes();
  registerCoreObjectTypes();
}

void registerBuiltinModifierTypes() {
  registerCoreModifierTypes();
  registerPolyModifierTypes();
  registerMeshModifierTypes();
}

}  // namespace veraa
