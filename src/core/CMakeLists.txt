# Clipper2 (polygon booleans and offsets, Boost license), built without project warnings.
add_library(veraa_clipper2 STATIC
  ${PROJECT_SOURCE_DIR}/third_party/clipper2/src/clipper.engine.cpp
  ${PROJECT_SOURCE_DIR}/third_party/clipper2/src/clipper.offset.cpp
  ${PROJECT_SOURCE_DIR}/third_party/clipper2/src/clipper.rectclip.cpp
)
target_include_directories(veraa_clipper2 SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party/clipper2/include)
if(MSVC)
  target_compile_options(veraa_clipper2 PRIVATE /w)
else()
  target_compile_options(veraa_clipper2 PRIVATE -w)
endif()

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
  meshbuild.cpp
  shape2d.cpp
  shapes.cpp
  font.cpp
  shape_mesh.cpp
  spline_ops.cpp
  shape_modifiers.cpp
  compound.cpp
  aec_walls.cpp
  aec_doors.cpp
  aec_stairs.cpp
  patch_nurbs.cpp
)
target_include_directories(veraa_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/third_party
)
target_link_libraries(veraa_core PUBLIC veraa_clipper2)
# Lets the font search find the bundled fonts when running from the build tree.
target_compile_definitions(veraa_core PRIVATE VERAA_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
