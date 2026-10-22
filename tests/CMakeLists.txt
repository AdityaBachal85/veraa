add_executable(core_tests core_tests.cpp)
target_link_libraries(core_tests PRIVATE veraa_core)
add_test(NAME core_tests COMMAND core_tests)
