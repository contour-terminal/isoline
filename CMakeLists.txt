# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.25)

# The version literal is the source of truth: isoline::version() is generated from it.
project(isoline
    VERSION 0.1.0
    DESCRIPTION "A C++23 WebAssembly runtime"
    HOMEPAGE_URL "https://github.com/contour-terminal/isoline"
    LANGUAGES C CXX)

# Directory-scoped rather than per target, so that a dependency fetched with ISOLINE_FETCH_DEPS
# (Catch2) is compiled against the same standard library surface as the code that links it.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# C is the language of the C API's tests and of every header it installs (src/isoline/c/*.h).
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

# The compiler cache, only when Isoline is the build being run: an embedder's launcher is its own.
# Before the dependencies are resolved, so that a fetched Catch2 is compiled through it too. The module
# is a verbatim copy; see cmake/portable/README.md.
if(PROJECT_IS_TOP_LEVEL)
    include("${PROJECT_SOURCE_DIR}/cmake/portable/CompileCache.cmake")
    # The module sets CMAKE_CXX_COMPILER_LAUNCHER as a normal variable, so CMakeCache.txt would not
    # otherwise show which launcher this tree uses. Recorded for CI and for people; nothing reads it.
    set(ISOLINE_CXX_COMPILER_LAUNCHER "${CMAKE_CXX_COMPILER_LAUNCHER}" CACHE INTERNAL
        "The compiler launcher cmake/portable/CompileCache.cmake selected for this build tree")
    if(CMAKE_CXX_COMPILER_LAUNCHER)
        # The launcher may be `cmake -E env <settings> <launcher>`; the program is its last word.
        list(GET CMAKE_CXX_COMPILER_LAUNCHER -1 _isolineLauncher)
        message(STATUS "[isoline] Compiler launcher: ${_isolineLauncher}")
        unset(_isolineLauncher)
    else()
        message(STATUS "[isoline] Compiler launcher: none (compiles uncached)")
    endif()
endif()

include(IsolineOptions)
include(IsolineDependencies)
include(IsolineCoverage)
include(IsolinePedantic)
include(IsolineLanguageFeatures)
include(IsolineCappedTest)
include(IsolineModules)
include(IsolineModuleSurface)
include(IsolineCExports)

if(ISOLINE_TESTING)
    enable_testing()
endif()

add_subdirectory(src/isoline)

# Every installed header of every target isoline_apply_language_features() saw parses as C++23 on its
# own, whatever the library compiles as, and, where contracts are native, also as the libraries'
# consumers compile it (C++26, the installed contracts): one check, after all of them are defined.
if(ISOLINE_TESTING)
    isoline_add_public_header_check(isoline_public_headers_cxx23 CONSUMER_CHECK isoline_public_headers_cxx26)
endif()

if(ISOLINE_TESTING)
    add_subdirectory(tests)
    # Once every directory has added its test binaries.
    isoline_write_coverage_binaries()
    # A coverage build's report and ratchet, which reads that list.
    isoline_add_coverage_check()
endif()

# The benchmark harness: its guests' tests in every testing build, its runners with ISOLINE_BENCHMARKS.
if(ISOLINE_TESTING OR ISOLINE_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

# Last, because it installs and exports library targets every directory above has created.
include(IsolineInstall)
