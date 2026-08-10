find_package(Qt6 6.5 QUIET COMPONENTS Widgets Concurrent)
if(NOT Qt6_FOUND)
  message(WARNING "Qt 6.5+ Widgets/Concurrent not found; OpenLens Desktop disabled")
  return()
endif()

qt_standard_project_setup()
qt_add_executable(openlens-desktop
  src/main.cpp
  src/main_window.cpp
  src/main_window.hpp
)
target_link_libraries(openlens-desktop PRIVATE openlens_core Qt6::Widgets Qt6::Concurrent)
target_compile_definitions(openlens-desktop PRIVATE OPENLENS_VERSION="${PROJECT_VERSION}")
set_target_properties(openlens-desktop PROPERTIES
  AUTOMOC ON
  OUTPUT_NAME "openlens-desktop"
)

if(OPENLENS_BUILD_TESTS)
  add_test(NAME desktop_gui_smoke
    COMMAND openlens-desktop --smoke-test -platform offscreen)
endif()
