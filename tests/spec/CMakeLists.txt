# SPDX-License-Identifier: Apache-2.0

# The official WebAssembly spec suite, imported by scripts/import-spec-tests.py into one directory
# per .wast file: the verbatim .wast script with its oracle manifest and the oracle's .wasm files. The
# import is committed, so running it needs neither the network nor wasm-tools.

add_library(isoline_spectest STATIC ComponentSpectest.cpp ComponentValues.cpp Hex.cpp Oracle.cpp SpecCheck.cpp WastReader.cpp)
target_include_directories(isoline_spectest PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
# The component model's suites run through the same reader and runner (D12), so the harness links it.
target_link_libraries(isoline_spectest PUBLIC isoline::core isoline::text isoline::component isoline::component_text)
isoline_apply_target_policy(isoline_spectest)

add_executable(isoline_spec_runner SpecRunner.cpp)
target_link_libraries(isoline_spec_runner PRIVATE isoline_spectest)
# TEST: it is run by ctest alone, so a failing spec file must end it rather than open a dialog.
isoline_apply_target_policy(isoline_spec_runner TEST)

add_executable(isoline_spectest_test
               ComponentSpectest_test.cpp
               ComponentValues_test.cpp
               CoreTrapWording_test.cpp
               Oracle_test.cpp
               SpecCheck_test.cpp
               SpecFeatures_test.cpp
               WastReader_test.cpp)
target_link_libraries(isoline_spectest_test PRIVATE isoline_spectest Catch2::Catch2WithMain)
# WastReader.ordinalsMatchWasmTools reads its fixture, tests/spec/ordinals, from the source tree.
target_compile_definitions(isoline_spectest_test PRIVATE ISOLINE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
isoline_apply_target_policy(isoline_spectest_test TEST)
add_test(NAME isoline_spectest_test COMMAND isoline_spectest_test)
set_tests_properties(isoline_spectest_test PROPERTIES LABELS unit TIMEOUT 600)

# The feature set each suite source runs with: the 3.0 release its own standard, a proposal's suite
# every feature Isoline knows, as its tests build on 3.0.
set(specFeatures_wasm3 wasm3)
set(specFeatures_wide-arithmetic all)
set(specFeatures_custom-page-sizes all)
set(specFeatures_compact-imports all)

# One test per imported .wast file, in files.txt's order, named spec.<source>.<path with / as .>,
# which fails unless the file's script skips exactly as many commands as files.txt pins. An `exclude`
# row names a file that is deliberately not imported.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS files.txt)
file(STRINGS files.txt specFiles REGEX "^[^#]" ENCODING UTF-8)
foreach(entry IN LISTS specFiles)
    if(entry MATCHES "^[^ ]+ +[^ ]+ +exclude .+$")
        continue()
    endif()
    if(NOT entry MATCHES "^([^ ]+) +([^ ]+) +([0-9]+)$")
        message(FATAL_ERROR "tests/spec/files.txt: expected '<source> <path> <skips>' "
                            "or '<source> <path> exclude <reason>', got '${entry}'")
    endif()
    set(source "${CMAKE_MATCH_1}")
    set(path "${CMAKE_MATCH_2}")
    set(skips "${CMAKE_MATCH_3}")
    if(NOT DEFINED specFeatures_${source})
        message(FATAL_ERROR "tests/spec/files.txt: no feature set for the suite source '${source}'")
    endif()
    string(REPLACE "/" "." testName "spec.${source}.${path}")
    add_test(NAME ${testName}
             COMMAND isoline_spec_runner "${CMAKE_CURRENT_SOURCE_DIR}/${source}/${path}" "${skips}"
                     --features "${specFeatures_${source}}")
    set_tests_properties(${testName} PROPERTIES LABELS spec)
endforeach()
