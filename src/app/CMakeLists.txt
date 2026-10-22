find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets OpenGL OpenGLWidgets)

set(CMAKE_AUTOMOC ON)

add_executable(Veraa
  main.cpp
  theme.cpp
  icons.cpp
  document.cpp
  editorstate.cpp
  viewcamera.cpp
  glscene.cpp
  gizmo.cpp
  viewport.cpp
  viewportpanel.cpp
  paramform.cpp
  commandpanel.cpp
  sceneexplorer.cpp
  materialeditor.cpp
  rendersetup.cpp
  framebuffer.cpp
  dialogs.cpp
  mainwindow.cpp
  selftest.cpp
)

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
