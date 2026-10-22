# Vendored C libraries: ufbx (FBX reader, MIT/public domain) and miniz (zip, MIT).
enable_language(C)
add_library(veraa_thirdparty STATIC
  ufbx/ufbx.c
  miniz/miniz.c
)
target_include_directories(veraa_thirdparty PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(veraa_thirdparty PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Third-party code is compiled as-is; its warnings are not ours to fix.
if(MSVC)
  target_compile_options(veraa_thirdparty PRIVATE /W0)
  target_compile_definitions(veraa_thirdparty PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(veraa_thirdparty PRIVATE -w)
endif()
