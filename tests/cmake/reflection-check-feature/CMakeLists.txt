# SPDX-License-Identifier: Apache-2.0
#
# A fixture project for isoline_add_reflection_check()'s refusals (cmake/IsolineLanguageFeatures.cmake), one
# per CASE, each of which must fail the configure naming what it refuses. tests/CMakeLists.txt configures it
# once per case. The features are OFF, so no probe compiles: each refusal comes before the path is read.
#  - feature (the default): a feature no row names, whatever the cache holds (FEATURE "annotation"; the row
#    is "annotations").
#  - test-and-program: TEST and PROGRAM, which exclude each other.
#  - header-sets-only-and-exclude: EXCLUDE, which filters the private headers HEADER_SETS_ONLY leaves out.

cmake_minimum_required(VERSION 3.25)
project(isoline_reflection_check_feature LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${ISOLINE_SOURCE_DIR}/cmake")
include(IsolineOptions)
include(IsolinePedantic)
include(IsolineLanguageFeatures)

if(NOT DEFINED CASE OR CASE STREQUAL "feature")
    # A stale cache entry for the misspelt feature: it must not make the name known.
    set(ISOLINE_ANNOTATION_PATH native CACHE INTERNAL "a stale entry")
    isoline_add_reflection_check(fixture_check FEATURE annotation SOURCES Fixture.cpp)
elseif(CASE STREQUAL "test-and-program")
    isoline_add_reflection_check(fixture_check TEST PROGRAM SOURCES Fixture.cpp)
elseif(CASE STREQUAL "header-sets-only-and-exclude")
    isoline_add_reflection_check(fixture_check SOURCES Fixture.cpp UMBRELLA Umbrella.hpp HEADER_TARGETS fixture
                                 HEADER_SETS_ONLY EXCLUDE "/testing/")
else()
    message(FATAL_ERROR "reflection-check-feature: no case ${CASE}")
endif()
