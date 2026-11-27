# SPDX-License-Identifier: Apache-2.0
#
# The in-tree consumers of module isoline, one target per kind of TU, since clang-tidy's lifted checks
# (E2) are a target's: one that imports and includes no Isoline header, one that includes Isoline headers
# and then imports, and an ordinary header TU that imports nothing. All three link into isoline_module_test,
# one program that both includes and imports Isoline (D7).

# The header side: an ordinary TU of the header build, not scanned, linted in full.
add_library(isoline_module_test_header_side OBJECT HeaderSide.cpp)
target_link_libraries(isoline_module_test_header_side PUBLIC isoline_core)
isoline_apply_target_policy(isoline_module_test_header_side TEST)

# Include, then import, in one TU: the supported order (D7).
add_library(isoline_module_test_mixed_inclusion OBJECT MixedInclusion_test.cpp)
target_include_directories(isoline_module_test_mixed_inclusion PRIVATE "${PROJECT_SOURCE_DIR}/tests/consumer")
target_link_libraries(isoline_module_test_mixed_inclusion PRIVATE Catch2::Catch2 isoline_core)
isoline_add_module_consumer(isoline_module_test_mixed_inclusion isoline)
isoline_apply_target_policy(isoline_module_test_mixed_inclusion TEST)
# E2 (D13), rows include-then-import of ISOLINE_MODULE_TIDY_FALSE_POSITIVES: readability-redundant-declaration,
# performance-trivially-destructible.
isoline_lift_module_tidy_checks(isoline_module_test_mixed_inclusion include-then-import)

add_executable(isoline_module_test ImportConsumer_test.cpp)
# The add module's bytes, which a consumer that only imports may include (tests/consumer/AddModule.hpp).
target_include_directories(isoline_module_test PRIVATE "${PROJECT_SOURCE_DIR}/tests/consumer")
target_link_libraries(isoline_module_test PRIVATE Catch2::Catch2WithMain isoline_module_test_header_side
                                                  isoline_module_test_mixed_inclusion)
isoline_add_module_consumer(isoline_module_test isoline)
isoline_apply_target_policy(isoline_module_test TEST)
# E2 (D13), row import-only of ISOLINE_MODULE_TIDY_FALSE_POSITIVES: readability-redundant-declaration.
isoline_lift_module_tidy_checks(isoline_module_test import-only)
add_test(NAME isoline_module_test COMMAND isoline_module_test)
set_tests_properties(isoline_module_test PROPERTIES LABELS "unit;modules" TIMEOUT 300)

# D7, import then include in one TU, as measured (2026-11-27), one row per compiler id the support table
# accepts: (compiler id, works|fails, a regular expression that a failing build's output matches).
# module-import-then-include builds ImportThenInclude.cpp and holds the outcome to its compiler's row, so a
# compiler that starts (or stops) accepting the order turns it red; docs/modules.md shows the same table.
# Every failure measured is the header redefining what the module's global module fragment already holds,
# and the first error names a different entity per version, so a row matches what those errors share:
#  - Clang 22.1.8: accepted.
#  - GCC: "error: redefinition of '...'", the entity varying by version: GCC 16.2.1 names
#    'constexpr bool std::__is_constant_evaluated()', GCC 15.2.0 (the gcc-modules lane)
#    'struct std::__byte_operand<bool>'.
#  - MSVC: the code varies by version: 19.51 reports warning C4348, "'std::enable_if': redefinition of default
#    parameter", an error under /WX; 19.44 (the windows-msvc-modules lane) error C2011, "'isoline::Feature':
#    'unsigned enum' type redefinition".
# The test runs the build with English diagnostics (LC_ALL=C for GCC, VSLANG=1033 for MSVC), which these
# patterns assume.
set(ISOLINE_MODULE_IMPORT_THEN_INCLUDE
    "Clang" "works" "-"
    "GNU" "fails" "error: redefinition of '"
    "MSVC" "fails" "(error|warning) C(2011|4348):")

# Its probe: out of `all`, since on a `fails` row it does not build; the CTest builds it by target.
add_library(isoline_module_import_then_include OBJECT EXCLUDE_FROM_ALL ImportThenInclude.cpp)
target_link_libraries(isoline_module_import_then_include PRIVATE isoline_core)
isoline_add_module_consumer(isoline_module_import_then_include isoline)
isoline_apply_target_policy(isoline_module_import_then_include)
# The row's diagnostic is GCC's first error; the hundreds that follow it cost the test about two minutes.
target_compile_options(isoline_module_import_then_include PRIVATE "$<$<CXX_COMPILER_ID:GNU>:-fmax-errors=1>")
# E2 (D13), row import-then-include of ISOLINE_MODULE_TIDY_FALSE_POSITIVES: readability-redundant-declaration.
isoline_lift_module_tidy_checks(isoline_module_import_then_include import-then-include)

set(importThenIncludeRow "")
set(rows "${ISOLINE_MODULE_IMPORT_THEN_INCLUDE}")
while(rows)
    list(POP_FRONT rows compilerId expected diagnostic)
    if(compilerId STREQUAL CMAKE_CXX_COMPILER_ID)
        set(importThenIncludeRow "${expected};${diagnostic}")
    endif()
endwhile()
if(NOT importThenIncludeRow)
    message(FATAL_ERROR "[isoline] ISOLINE_MODULE_IMPORT_THEN_INCLUDE has no row for ${CMAKE_CXX_COMPILER_ID}: "
                        "measure tests/modules/ImportThenInclude.cpp on it and add one")
endif()
list(GET importThenIncludeRow 0 importThenIncludeExpected)
list(GET importThenIncludeRow 1 importThenIncludeDiagnostic)
add_test(NAME module-import-then-include
         COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIR=${PROJECT_BINARY_DIR}" "-DTARGET=isoline_module_import_then_include"
                 "-DCONFIG=$<CONFIG>" "-DCOMPILER=${CMAKE_CXX_COMPILER_ID}"
                 "-DCOMPILER_VERSION=${CMAKE_CXX_COMPILER_VERSION}" "-DEXPECTED=${importThenIncludeExpected}"
                 "-DDIAGNOSTIC=${importThenIncludeDiagnostic}"
                 -P "${PROJECT_SOURCE_DIR}/tests/cmake/expect-import-then-include.cmake")
# It builds in the build tree, which clang-tidy-module-tripwire and cmake-precompile-tripwire read: the
# three take turns (RESOURCE_LOCK build-tree).
set_tests_properties(module-import-then-include PROPERTIES LABELS modules TIMEOUT 600 RESOURCE_LOCK build-tree
                                                       ENVIRONMENT "LC_ALL=C;VSLANG=1033")

# clang-tidy-module-tripwire's sources (E2), one per kind: compiled, so that their module maps exist, and
# linted as their kind's rows say.
add_library(isoline_module_tidy_tripwire OBJECT tidy-tripwire/ImportOnly.cpp)
isoline_add_module_consumer(isoline_module_tidy_tripwire isoline)
isoline_apply_target_policy(isoline_module_tidy_tripwire)
# E2 (D13), row import-only of ISOLINE_MODULE_TIDY_FALSE_POSITIVES: readability-redundant-declaration.
isoline_lift_module_tidy_checks(isoline_module_tidy_tripwire import-only)

add_library(isoline_module_tidy_tripwire_include_then_import OBJECT tidy-tripwire/IncludeThenImport.cpp)
target_link_libraries(isoline_module_tidy_tripwire_include_then_import PRIVATE isoline_core)
isoline_add_module_consumer(isoline_module_tidy_tripwire_include_then_import isoline)
isoline_apply_target_policy(isoline_module_tidy_tripwire_include_then_import)
# E2 (D13), rows include-then-import of ISOLINE_MODULE_TIDY_FALSE_POSITIVES: readability-redundant-declaration,
# performance-trivially-destructible.
isoline_lift_module_tidy_checks(isoline_module_tidy_tripwire_include_then_import include-then-import)

# The import-then-include source, built only where its compiler's row says the order works: elsewhere the
# TU does not compile, so clang-tidy, which runs on Clang only (the same LLVM build, D13), never sees it.
set(importThenIncludeTidySource "")
if(importThenIncludeExpected STREQUAL "works")
    add_library(isoline_module_tidy_tripwire_import_then_include OBJECT tidy-tripwire/ImportThenInclude.cpp)
    target_link_libraries(isoline_module_tidy_tripwire_import_then_include PRIVATE isoline_core)
    isoline_add_module_consumer(isoline_module_tidy_tripwire_import_then_include isoline)
    isoline_apply_target_policy(isoline_module_tidy_tripwire_import_then_include)
    # E2 (D13), row import-then-include of ISOLINE_MODULE_TIDY_FALSE_POSITIVES: readability-redundant-declaration.
    isoline_lift_module_tidy_checks(isoline_module_tidy_tripwire_import_then_include import-then-include)
    set(importThenIncludeTidySource
        "import-then-include" "${CMAKE_CURRENT_SOURCE_DIR}/tidy-tripwire/ImportThenInclude.cpp")
endif()

# The tripwires of E1 and E2: one source of each kind ISOLINE_MODULE_TIDY_FALSE_POSITIVES names.
get_property(isolineInterfaceUnit GLOBAL PROPERTY ISOLINE_MODULE_isoline_SOURCE)
isoline_add_module_exception_tripwires(
    TIDY_SOURCES
        "interface-unit" "${isolineInterfaceUnit}"
        "import-only" "${CMAKE_CURRENT_SOURCE_DIR}/tidy-tripwire/ImportOnly.cpp"
        "include-then-import" "${CMAKE_CURRENT_SOURCE_DIR}/tidy-tripwire/IncludeThenImport.cpp"
        ${importThenIncludeTidySource})
