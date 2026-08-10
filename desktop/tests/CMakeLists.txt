add_executable(protocol_tests protocol_tests.cpp)
target_link_libraries(protocol_tests PRIVATE openlens_core)
target_compile_definitions(protocol_tests PRIVATE OPENLENS_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_test(NAME protocol_tests COMMAND protocol_tests)

add_executable(core_tests core_tests.cpp)
target_link_libraries(core_tests PRIVATE openlens_core)
add_test(NAME core_tests COMMAND core_tests)
