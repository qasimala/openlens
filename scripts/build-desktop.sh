#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cmake_bin="${CMAKE_BIN:-$(command -v cmake || true)}"
ninja_bin="${NINJA_BIN:-$(command -v ninja || true)}"
ctest_bin="${CTEST_BIN:-$(command -v ctest || true)}"

if [[ -z "$cmake_bin" && -x /home/qasim/Android/Sdk/cmake/3.22.1/bin/cmake ]]; then
  cmake_bin=/home/qasim/Android/Sdk/cmake/3.22.1/bin/cmake
fi
if [[ -z "$ninja_bin" && -x /home/qasim/Android/Sdk/cmake/3.22.1/bin/ninja ]]; then
  ninja_bin=/home/qasim/Android/Sdk/cmake/3.22.1/bin/ninja
fi
if [[ -z "$ctest_bin" && -n "$cmake_bin" && -x "$(dirname "$cmake_bin")/ctest" ]]; then
  ctest_bin="$(dirname "$cmake_bin")/ctest"
fi
if [[ -z "$cmake_bin" || -z "$ninja_bin" || -z "$ctest_bin" ]]; then
  echo "CMake, CTest, and Ninja are required." >&2
  exit 2
fi

build_dir="$repo_root/build/desktop-debug"
"$cmake_bin" -S "$repo_root" -B "$build_dir" -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$ninja_bin" -DCMAKE_BUILD_TYPE=Debug -DOPENLENS_BUILD_TESTS=ON
"$cmake_bin" --build "$build_dir"
"$ctest_bin" --test-dir "$build_dir" --output-on-failure
