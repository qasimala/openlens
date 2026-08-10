add_executable(openlens src/main.cpp)
target_link_libraries(openlens PRIVATE openlens_core)
target_compile_definitions(openlens PRIVATE OPENLENS_VERSION="${PROJECT_VERSION}")
