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
  Qt6::Widgets
  Qt6::OpenGL
  Qt6::OpenGLWidgets
)

target_compile_definitions(Veraa PRIVATE QT_NO_KEYWORDS VERAA_VERSION="${PROJECT_VERSION}")
