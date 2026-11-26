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
