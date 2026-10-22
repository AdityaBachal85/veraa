#include "registry.h"

namespace veraa {

void registerBuiltinObjectTypes() {
  registerPrimitiveTypes();
  registerExtendedPrimitiveTypes();
  registerCoreObjectTypes();
  registerCompoundObjectTypes();
}

void registerBuiltinModifierTypes() {
  registerCoreModifierTypes();
  registerPolyModifierTypes();
  registerMeshModifierTypes();
}

}  // namespace veraa
