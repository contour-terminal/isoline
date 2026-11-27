# SPDX-License-Identifier: Apache-2.0
#
# The smallest C++ module project: one interface unit and one importer. tests/cmake/expect-no-module-scanning.cmake
# configures and builds it with a compiler the module support table refuses, expecting CMake to refuse
# its module scanning.
cmake_minimum_required(VERSION 3.28)
project(isoline_module_scanning_probe LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
add_executable(probe main.cpp)
target_sources(probe PRIVATE FILE_SET CXX_MODULES FILES probe.cppm)
