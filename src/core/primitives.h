// Standard and extended primitives (Box, Sphere, Cylinder, ...). Internal to the core library;
// use createObject() from object.h.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "object.h"

namespace veraa {

std::unique_ptr<SceneObject> createPrimitive(const std::string& type);
const std::vector<std::string>& primitiveTypes();

}  // namespace veraa
