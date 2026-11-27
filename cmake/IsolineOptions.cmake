# SPDX-License-Identifier: Apache-2.0
#
# Every switch the build reads, in one place. The defaults describe a consumer build (Isoline added
# by a parent project): no tests, no install rules, no -Werror, no clang-tidy. The presets in
# CMakePresets.json turn the developer settings on.

include_guard(GLOBAL)

option(ISOLINE_TESTING "Build Isoline's tests and register them with CTest" ${PROJECT_IS_TOP_LEVEL})
option(ISOLINE_WERROR "Treat compiler warnings in Isoline's own targets as errors" OFF)
option(ISOLINE_CLANG_TIDY "Run clang-tidy on Isoline's own targets as part of the build" OFF)
option(ISOLINE_COVERAGE "Instrument Isoline's own targets for source-based coverage (Clang)" OFF)
option(ISOLINE_INSTALL
       "Install Isoline's libraries and headers as the CMake package isoline (cmake/IsolineInstall.cmake)"
       ${PROJECT_IS_TOP_LEVEL})
option(ISOLINE_MODULES
       "Build the C++ module isoline too; the configure fails on a toolchain that cannot (IsolineModules.cmake)"
       OFF)
option(ISOLINE_EXPECT_IMPORT_STD
       "Fail, rather than skip, the install test of import std; where the toolchain provides no std module"
       OFF)
option(ISOLINE_FETCH_DEPS
       "Fetch a dependency (Catch2) with CPM when neither the parent project nor find_package() provides it"
       OFF)

option(ISOLINE_C_API "Build the C API (wasm.h + isoline.h): the isoline_c shared and static libraries"
       ${PROJECT_IS_TOP_LEVEL})

set(ISOLINE_SANITIZE "" CACHE STRING
    "Sanitizer mode for Isoline's own targets: empty, address (ASan + UBSan) or thread")
set_property(CACHE ISOLINE_SANITIZE PROPERTY STRINGS "" address thread)

# The C++26 language features' options, ISOLINE_<FEATURE> (AUTO, ON or OFF, default AUTO), are defined by
# cmake/IsolineLanguageFeatures.cmake from its feature rows, so that a feature is one row there; see
# docs/language-features.md.
