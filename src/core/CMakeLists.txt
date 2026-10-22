add_library(veraa_core STATIC
  math.cpp
  params.cpp
  mesh.cpp
  object.cpp
  primitives.cpp
  modifier.cpp
  material.cpp
  scene.cpp
  obj_import.cpp
)
target_include_directories(veraa_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/third_party
)
