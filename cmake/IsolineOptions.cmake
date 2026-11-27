# SPDX-License-Identifier: Apache-2.0
#
# Every switch the build reads, in one place. The defaults describe a consumer build (Isoline added
# by a parent project): no tests, no -Werror, no clang-tidy. The presets in CMakePresets.json turn
# the developer settings on.

include_guard(GLOBAL)

option(ISOLINE_TESTING "Build Isoline's tests and register them with CTest" ${PROJECT_IS_TOP_LEVEL})
option(ISOLINE_WERROR "Treat compiler warnings in Isoline's own targets as errors" OFF)
option(ISOLINE_CLANG_TIDY "Run clang-tidy on Isoline's own targets as part of the build" OFF)
option(ISOLINE_COVERAGE "Instrument Isoline's own targets for source-based coverage (Clang)" OFF)
option(ISOLINE_FETCH_DEPS
       "Fetch a dependency (Catch2) with CPM when neither the parent project nor find_package() provides it"
       OFF)

set(ISOLINE_SANITIZE "" CACHE STRING
    "Sanitizer mode for Isoline's own targets: empty, address (ASan + UBSan) or thread")
set_property(CACHE ISOLINE_SANITIZE PROPERTY STRINGS "" address thread)

# The fast interpreter's tail-call dispatch (fastinterpreter/CMakeLists.txt): AUTO compiles it where a probe
# row below passes, OFF never does, which leaves the portable Switch dispatch alone.
set(ISOLINE_FAST_DISPATCH_TAILCALL AUTO CACHE STRING
    "The fast interpreter's TailCall dispatch: AUTO (where a probe row passes) or OFF")
set_property(CACHE ISOLINE_FAST_DISPATCH_TAILCALL PROPERTY STRINGS AUTO OFF)

# The tail-call probe rows, tried in order: a spelling of a guaranteed tail call, and the compiler families
# (CMAKE_CXX_COMPILER_ID) that have it. The probe compiles the real TailCallControl.cpp with each spelling,
# with the target policy's own flags and warnings as errors, and the first that compiles is the build's.
# MSVC has no guaranteed tail call, and GCC has one from 15 on: no row passes on MSVC or GCC 14, nor on GCC
# with AddressSanitizer, which keeps the handler's locals addressable across the tail call.
# GCC's row comes first: GCC 15 and later accept Clang's spelling too, and Clang rejects GCC's, so each
# compiler takes its own row.
set(ISOLINE_TAILCALL_PROBE_ROWS "gnu::musttail" "clang::musttail")
set(ISOLINE_TAILCALL_PROBE_FAMILIES "GNU" "Clang AppleClang")
