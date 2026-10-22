add_executable(core_tests core_tests.cpp)
target_link_libraries(core_tests PRIVATE veraa_core)
add_test(NAME core_tests COMMAND core_tests)

add_executable(render_tests render_tests.cpp)
target_link_libraries(render_tests PRIVATE veraa_render)
target_compile_definitions(render_tests PRIVATE VERAA_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_test(NAME render_tests COMMAND render_tests)

add_executable(animation_tests animation_tests.cpp)
target_link_libraries(animation_tests PRIVATE veraa_core)
add_test(NAME animation_tests COMMAND animation_tests)
