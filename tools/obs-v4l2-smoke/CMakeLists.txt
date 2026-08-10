cmake_minimum_required(VERSION 3.20)
project(openlens_obs_v4l2_smoke LANGUAGES C)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBOBS REQUIRED IMPORTED_TARGET libobs)

add_executable(openlens-obs-v4l2-smoke obs-v4l2-smoke.c)
target_compile_features(openlens-obs-v4l2-smoke PRIVATE c_std_17)
target_compile_options(openlens-obs-v4l2-smoke PRIVATE -Wall -Wextra -Werror)
target_link_libraries(openlens-obs-v4l2-smoke PRIVATE PkgConfig::LIBOBS)
