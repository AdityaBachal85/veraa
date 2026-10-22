add_executable(core_tests core_tests.cpp)
target_link_libraries(core_tests PRIVATE veraa_core)
add_test(NAME core_tests COMMAND core_tests)

add_executable(render_tests render_tests.cpp)
target_link_libraries(render_tests PRIVATE veraa_render)
target_compile_definitions(render_tests PRIVATE VERAA_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_test(NAME render_tests COMMAND render_tests)

add_executable(rendercore_tests rendercore_tests.cpp)
target_link_libraries(rendercore_tests PRIVATE veraa_core)
target_compile_definitions(rendercore_tests PRIVATE VERAA_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_test(NAME rendercore_tests COMMAND rendercore_tests)
