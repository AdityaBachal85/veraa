#include "registry.h"

namespace veraa {

void registerBuiltinObjectTypes() {
  registerPrimitiveTypes();
  registerCoreObjectTypes();
  registerShapeTypes();
  registerShapeGeometryTypes();
  registerCompoundTypes();
  registerPatchGridTypes();
  registerAecTypes();
}

void registerBuiltinModifierTypes() {
  registerCoreModifierTypes();
  registerShapeModifierTypes();
}

}  // namespace veraa
