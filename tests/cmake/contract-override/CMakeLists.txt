# SPDX-License-Identifier: Apache-2.0
#
# A parent project for the contracts' refusals (cmake/IsolineLanguageFeatures.cmake): it brings its own
# core-cpp, as Contour does, and adds Isoline as a subproject with contracts native. Run.cmake configures
# it once per ISOLINE_FIXTURE_OVERRIDE, each a way a parent can override the semantic Isoline's contracts
# are checked under, then runs the build-time check of the compile commands; one of the two must fail,
# naming where the override came from. With `none` it overrides nothing, and both must pass. Nothing is
# compiled: the targets of this core-cpp have no sources, only the names Isoline links.

cmake_minimum_required(VERSION 3.25)

set(ISOLINE_FIXTURE_OVERRIDE "none" CACHE STRING "where the override comes from (tests/CMakeLists.txt)")

# A parent that is not a C++ project: Isoline enables C++ for itself, and the scan must use the compiler
# of Isoline's directories, not the top level's, which has none.
if(ISOLINE_FIXTURE_OVERRIDE STREQUAL "c-parent")
    project(isoline_contract_override LANGUAGES C)
else()
    project(isoline_contract_override LANGUAGES CXX)
endif()

# The parent's core-cpp: Isoline uses a core::base it finds defined (cmake/IsolineDependencies.cmake).
add_library(fixture_core_base INTERFACE)
add_library(core::base ALIAS fixture_core_base)
add_library(fixture_core_cli INTERFACE)
add_library(core::cli ALIAS fixture_core_cli)
add_library(fixture_core_platform INTERFACE)
add_library(core::platform ALIAS fixture_core_platform)

if(ISOLINE_FIXTURE_OVERRIDE MATCHES "^(interface-option|c-parent)$")
    # A usage requirement of a library Isoline links.
    target_compile_options(fixture_core_base INTERFACE -fcontract-evaluation-semantic=ignore)
elseif(ISOLINE_FIXTURE_OVERRIDE STREQUAL "transitive-option")
    # A usage requirement of a library that one Isoline links links in turn.
    add_library(fixture_core_detail INTERFACE)
    target_compile_options(fixture_core_detail INTERFACE "$<$<CONFIG:Debug>:-fcontract-evaluation-semantic=observe>")
    target_link_libraries(fixture_core_base INTERFACE "$<BUILD_INTERFACE:fixture_core_detail>")
elseif(ISOLINE_FIXTURE_OVERRIDE STREQUAL "nested-genex-link")
    # The same, linked under nested generator expressions.
    add_library(fixture_core_nested INTERFACE)
    target_compile_options(fixture_core_nested INTERFACE -fcontract-evaluation-semantic=ignore)
    target_link_libraries(fixture_core_base INTERFACE "$<$<AND:$<CONFIG:Debug>,$<BOOL:1>>:fixture_core_nested>")
elseif(ISOLINE_FIXTURE_OVERRIDE STREQUAL "direct-link")
    # A library linked only directly, to whoever links core::base.
    add_library(fixture_core_direct INTERFACE)
    target_compile_options(fixture_core_direct INTERFACE -fno-contracts)
    set_property(TARGET fixture_core_base APPEND PROPERTY INTERFACE_LINK_LIBRARIES_DIRECT fixture_core_direct)
elseif(ISOLINE_FIXTURE_OVERRIDE STREQUAL "config-flags")
    # The parent's flags for one configuration.
    string(APPEND CMAKE_CXX_FLAGS_DEBUG " -fcontract-evaluation-semantic=ignore")
elseif(ISOLINE_FIXTURE_OVERRIDE STREQUAL "add-definitions")
    # A flag given as a definition, which reaches every directory below.
    add_definitions(-fcontract-evaluation-semantic=observe)
endif()

if(ISOLINE_FIXTURE_OVERRIDE STREQUAL "parent-directory")
    # An intermediate directory's flags, which the top-level directory never sees.
    add_subdirectory(parent-directory)
else()
    add_subdirectory("${ISOLINE_SOURCE_DIR}" isoline)
endif()

set(isolineExtern "${ISOLINE_SOURCE_DIR}/src/isoline/api/Extern.cpp")
if(ISOLINE_FIXTURE_OVERRIDE STREQUAL "target-option")
    # An option added to Isoline's library after Isoline has configured.
    target_compile_options(isoline_core PRIVATE -fcontracts-definition-check=off)
elseif(ISOLINE_FIXTURE_OVERRIDE STREQUAL "target-flags")
    # The same through the library's legacy flags string.
    set_property(TARGET isoline_core APPEND_STRING PROPERTY COMPILE_FLAGS " -fno-contracts")
elseif(ISOLINE_FIXTURE_OVERRIDE STREQUAL "source-option")
    # An option of one of the library's sources.
    set_source_files_properties("${isolineExtern}" TARGET_DIRECTORY isoline_core
                                PROPERTIES COMPILE_OPTIONS -fcontract-evaluation-semantic=quick_enforce)
elseif(ISOLINE_FIXTURE_OVERRIDE STREQUAL "missing-response-file")
    # A response file that is not there: the check cannot know what it holds.
    target_compile_options(isoline_core PRIVATE "@${CMAKE_BINARY_DIR}/missing.rsp")
elseif(ISOLINE_FIXTURE_OVERRIDE STREQUAL "edited-response-file")
    # A response file Run.cmake writes clean, then edits after a first check that passed.
    target_compile_options(isoline_core PRIVATE "@${CMAKE_BINARY_DIR}/flags.rsp")
elseif(ISOLINE_FIXTURE_OVERRIDE STREQUAL "specs-file")
    # A GCC specs file that adds the semantic to the compiler proper's command line, which no option spells.
    file(WRITE "${CMAKE_BINARY_DIR}/override.specs" "*cc1plus:\n+ -fcontract-evaluation-semantic=ignore\n")
    target_compile_options(isoline_core PRIVATE "-specs=${CMAKE_BINARY_DIR}/override.specs")
elseif(ISOLINE_FIXTURE_OVERRIDE STREQUAL "generated-flag")
    # A flag a generator expression builds, which no text of the configure spells.
    set_source_files_properties("${isolineExtern}" TARGET_DIRECTORY isoline_core
                                PROPERTIES COMPILE_OPTIONS "-fcontract-evaluation-semantic=$<LOWER_CASE:IGNORE>")
endif()
