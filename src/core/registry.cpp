#include "registry.h"

namespace veraa {

void registerBuiltinObjectTypes() {
  registerPrimitiveTypes();
  registerCoreObjectTypes();
  registerRenderObjectTypes();
}

void registerBuiltinModifierTypes() {
  registerCoreModifierTypes();
  registerRenderModifierTypes();
}

}  // namespace veraa
