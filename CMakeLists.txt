cmake_minimum_required(VERSION 3.21)
project(Veraa VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Put every executable and DLL in one folder so the app runs straight from the build tree.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(VERAA_BUILD_APP "Build the Veraa desktop application (needs Qt 6)" ON)
option(VERAA_BUILD_TESTS "Build the test suite" ON)

if(MSVC)
  add_compile_options(/W3 /permissive- /utf-8 /MP /EHsc)
  add_compile_definitions(_USE_MATH_DEFINES NOMINMAX _CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
endif()

find_package(Threads REQUIRED)
find_package(embree 4 REQUIRED)
find_package(OpenImageDenoise 2 QUIET)
if(OpenImageDenoise_FOUND)
  message(STATUS "Veraa: Intel Open Image Denoise found, denoiser enabled")
else()
  message(STATUS "Veraa: Intel Open Image Denoise not found, denoiser disabled")
endif()

add_subdirectory(src/core)
add_subdirectory(src/render)
add_subdirectory(src/cli)

if(VERAA_BUILD_APP)
  add_subdirectory(src/app)
endif()

if(VERAA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
