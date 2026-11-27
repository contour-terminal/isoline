# SPDX-License-Identifier: Apache-2.0
#
# A fixture project for isoline_add_reflection_check()'s FEATURE (cmake/IsolineLanguageFeatures.cmake): a
# feature no row names must fail the configure, naming it, whatever the cache holds. tests/CMakeLists.txt
# configures it with FEATURE "annotation" (the row is "annotations"). The features are OFF, so no probe
# compiles: the refusal comes before the path is read.

cmake_minimum_required(VERSION 3.25)
project(isoline_reflection_check_feature LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${ISOLINE_SOURCE_DIR}/cmake")
include(IsolineOptions)
include(IsolinePedantic)
include(IsolineLanguageFeatures)

# A stale cache entry for the misspelt feature: it must not make the name known.
set(ISOLINE_ANNOTATION_PATH native CACHE INTERNAL "a stale entry")

isoline_add_reflection_check(fixture_check FEATURE annotation SOURCES Fixture.cpp)
