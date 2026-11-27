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
option(ISOLINE_EXPECT_MODULE_SURFACE
       "Fail the configure, rather than leave it out, where the reflected module surface's test cannot run \
(cmake/IsolineModuleSurface.cmake); the native CI rows set it"
       OFF)
option(ISOLINE_FETCH_DEPS
       "Fetch a dependency (Catch2) with CPM when neither the parent project nor find_package() provides it"
       OFF)

option(ISOLINE_C_API "Build the C API (wasm.h + isoline.h): the isoline_c shared and static libraries"
       ${PROJECT_IS_TOP_LEVEL})

set(ISOLINE_EXPECT_LANGUAGE_FEATURES "" CACHE STRING
    "Fail the configure unless every C++26 language feature takes this path: empty (any), native or fallback")
set_property(CACHE ISOLINE_EXPECT_LANGUAGE_FEATURES PROPERTY STRINGS "" native fallback)

set(ISOLINE_SANITIZE "" CACHE STRING
    "Sanitizer mode for Isoline's own targets: empty, address (ASan + UBSan) or thread")
set_property(CACHE ISOLINE_SANITIZE PROPERTY STRINGS "" address thread)

# The C++26 language features' options, ISOLINE_<FEATURE> (AUTO, ON or OFF, default AUTO), are defined by
# cmake/IsolineLanguageFeatures.cmake from its feature rows, so that a feature is one row there; see
# docs/language-features.md.

# The fast interpreter's tail-call dispatch (fastinterpreter/CMakeLists.txt): AUTO compiles it where a probe
# row below passes, OFF never does, which leaves the portable Switch dispatch alone.
set(ISOLINE_FAST_DISPATCH_TAILCALL AUTO CACHE STRING
    "The fast interpreter's TailCall dispatch: AUTO (where a probe row passes) or OFF")
set_property(CACHE ISOLINE_FAST_DISPATCH_TAILCALL PROPERTY STRINGS AUTO OFF)

# The tail-call probe rows: a spelling of a guaranteed tail call, and the compiler families (CMAKE_CXX_COMPILER_ID)
# that have it. The probe tries, in order, the rows whose families name the build's compiler: it compiles the
# real TailCallControl.cpp with each spelling, with the target policy's own flags and warnings as errors, and
# the first that compiles is the build's. A compiler no row names is not probed, and gets Switch alone: MSVC,
# which has no guaranteed tail call. GCC has one from 15 on, so GCC 14 passes no row, nor does GCC with
# AddressSanitizer, which keeps the handler's locals addressable across the tail call. Each compiler takes its
# own spelling, though GCC 15 and later accept Clang's too.
set(ISOLINE_TAILCALL_PROBE_ROWS "gnu::musttail" "clang::musttail")
set(ISOLINE_TAILCALL_PROBE_FAMILIES "GNU" "Clang AppleClang")
