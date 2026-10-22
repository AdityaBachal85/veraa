add_library(veraa_core STATIC
  vmath.cpp
  params.cpp
  mesh.cpp
  object.cpp
  primitives.cpp
  modifier.cpp
  material.cpp
  scene.cpp
  obj_import.cpp
  shape.cpp
  registry.cpp
  io_common.cpp
  io_fbx.cpp
  io_gltf.cpp
  io_mesh_formats.cpp
)
target_include_directories(veraa_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/third_party
)

# ufbx (FBX import) and miniz (archives) are compiled as C in third_party.
target_link_libraries(veraa_core PRIVATE veraa_thirdparty)
