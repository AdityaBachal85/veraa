#include "registry.h"

namespace veraa {

void registerBuiltinObjectTypes() {
  registerPrimitiveTypes();
  registerCoreObjectTypes();
  registerShapeTypes();
  registerShapeGeometryTypes();
  registerCompoundTypes();
  registerAecTypes();
}

void registerBuiltinModifierTypes() {
  registerCoreModifierTypes();
  registerShapeModifierTypes();
}

}  // namespace veraa
