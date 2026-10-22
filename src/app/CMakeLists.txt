find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets OpenGL OpenGLWidgets)

set(CMAKE_AUTOMOC ON)

# Every .cpp in this folder is part of the app, so feature modules only add files.
file(GLOB VERAA_APP_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/*.h)
add_executable(Veraa ${VERAA_APP_SOURCES})

set_target_properties(Veraa PROPERTIES
  WIN32_EXECUTABLE ON
  MACOSX_BUNDLE ON
)

target_link_libraries(Veraa PRIVATE
  veraa_core
  veraa_render
  veraa_thirdparty
  Qt6::Widgets
  Qt6::OpenGL
  Qt6::OpenGLWidgets
)

target_compile_definitions(Veraa PRIVATE QT_NO_KEYWORDS VERAA_VERSION="${PROJECT_VERSION}")
# Lets bundled assets (IES files, HDRIs) be found when running from a build tree.
target_compile_definitions(Veraa PRIVATE VERAA_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

# Python scripting (Scripting menu, listener, veraa module). Enabled when Python development
# files are found; set VERAA_WITH_PYTHON=OFF to build without it.
option(VERAA_WITH_PYTHON "Embed Python for scripting" ON)
if(VERAA_WITH_PYTHON)
  find_package(Python3 3.8 COMPONENTS Development.Embed)
  if(Python3_Development.Embed_FOUND)
    message(STATUS "Veraa: Python ${Python3_VERSION} found, scripting enabled")
    target_link_libraries(Veraa PRIVATE Python3::Python)
    target_compile_definitions(Veraa PRIVATE VERAA_WITH_PYTHON)
  else()
    message(STATUS "Veraa: Python development files not found, scripting disabled")
  endif()
endif()
