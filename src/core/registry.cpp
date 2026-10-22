#include "registry.h"

namespace veraa {

void registerBuiltinObjectTypes() {
  registerPrimitiveTypes();
  registerCoreObjectTypes();
  registerShapeTypes();
  registerShapeGeometryTypes();
}

void registerBuiltinModifierTypes() {
  registerCoreModifierTypes();
  registerShapeModifierTypes();
}

}  // namespace veraa
