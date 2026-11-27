# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.25)

# The version literal is the source of truth: isoline::version() is generated from it.
project(isoline
    VERSION 0.1.0
    DESCRIPTION "A C++23 WebAssembly runtime"
    HOMEPAGE_URL "https://github.com/contour-terminal/isoline"
    LANGUAGES CXX)

# Directory-scoped rather than per target, so that a dependency fetched with ISOLINE_FETCH_DEPS
# (Catch2) is compiled against the same standard library surface as the code that links it.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(IsolineOptions)
include(IsolineDependencies)
include(IsolinePedantic)
include(IsolineCappedTest)

if(ISOLINE_TESTING)
    enable_testing()
endif()

add_subdirectory(src/isoline)

if(ISOLINE_TESTING)
    add_subdirectory(tests)
endif()
