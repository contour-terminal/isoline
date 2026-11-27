# SPDX-License-Identifier: Apache-2.0
#
# A fixture project for the header check's seal (cmake/IsolineLanguageFeatures.cmake): a target passed
# to isoline_apply_language_features() after isoline_add_public_header_check() has run must fail the
# configure, naming it. tests/CMakeLists.txt configures it twice: with ISOLINE_FIXTURE_ORDER=late, the
# configure must fail with that message; with early, the same targets in order must configure.
# The features are OFF, so no probe compiles: the seal does not depend on the path.

cmake_minimum_required(VERSION 3.25)
project(isoline_header_check_sealed LANGUAGES C CXX)

set(ISOLINE_FIXTURE_ORDER "late" CACHE STRING "late or early: whether the second target registers after the check")
list(APPEND CMAKE_MODULE_PATH "${ISOLINE_SOURCE_DIR}/cmake")
include(IsolineOptions)
include(IsolinePedantic)
include(IsolineLanguageFeatures)

add_library(fixture_checked STATIC Fixture.cpp)
target_sources(fixture_checked PUBLIC FILE_SET HEADERS BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}" FILES Fixture.hpp)
isoline_apply_language_features(fixture_checked)

add_library(fixture_late STATIC Fixture.cpp)
if(ISOLINE_FIXTURE_ORDER STREQUAL "early")
    isoline_apply_language_features(fixture_late)
endif()

isoline_add_public_header_check(fixture_headers_cxx23)

if(ISOLINE_FIXTURE_ORDER STREQUAL "late")
    isoline_apply_language_features(fixture_late)
endif()
