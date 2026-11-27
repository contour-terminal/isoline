# SPDX-License-Identifier: Apache-2.0
#
# A fixture project for the declaration-contracts proof (isoline_add_declaration_contracts_proof(),
# cmake/IsolineLanguageFeatures.cmake): a test helper's header, under a testing/ directory, declares an
# ISOLINE_PRE on a member function that may throw. The proof reads the test helpers' headers too, so building
# fixture_proof must fail naming the noexcept rule (tests/CMakeLists.txt,
# language-features-contracts-proof-reads-testing; where contracts are native). The two meta/ headers are
# the proof's own seam, which it leaves out.

cmake_minimum_required(VERSION 3.25)
project(isoline_contracts_proof_testing LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${ISOLINE_SOURCE_DIR}/cmake")
include(IsolineOptions)
include(IsolinePedantic)
include(IsolineLanguageFeatures)

add_library(fixture_core STATIC src/isoline/fixture/Api.cpp)
target_sources(fixture_core PUBLIC FILE_SET HEADERS BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
               FILES src/isoline/fixture/Api.hpp)
target_include_directories(fixture_core PUBLIC "${PROJECT_BINARY_DIR}/include")
isoline_apply_language_features(fixture_core)
isoline_add_declaration_contracts_proof(fixture_proof SOURCES Proof.cpp HEADER_TARGETS fixture_core
                                        LINK_LIBRARIES fixture_core)
if(NOT TARGET fixture_proof)
    message(FATAL_ERROR "contracts-proof-testing: contracts are not native, so there is no proof to build")
endif()
