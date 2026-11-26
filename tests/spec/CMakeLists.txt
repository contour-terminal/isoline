# SPDX-License-Identifier: Apache-2.0

# The official WebAssembly spec suite, imported by scripts/import-spec-tests.py into one directory
# per .wast file: an .isotest script and the .wasm files it names. The import is committed, so
# running it needs neither the network nor wasm-tools.

add_library(isoline_isotest STATIC IsotestReader.cpp SpecCheck.cpp)
target_include_directories(isoline_isotest PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(isoline_isotest PUBLIC isoline::core)
isoline_apply_target_policy(isoline_isotest)

add_executable(isoline_spec_runner SpecRunner.cpp)
target_link_libraries(isoline_spec_runner PRIVATE isoline_isotest)
isoline_apply_target_policy(isoline_spec_runner)

add_executable(isoline_isotest_test IsotestReader_test.cpp SpecCheck_test.cpp SpecFeatures_test.cpp)
target_link_libraries(isoline_isotest_test PRIVATE isoline_isotest Catch2::Catch2WithMain)
isoline_apply_target_policy(isoline_isotest_test TEST)
add_test(NAME isoline_isotest_test COMMAND isoline_isotest_test)
set_tests_properties(isoline_isotest_test PROPERTIES LABELS unit)

# The feature set each suite source runs with: the 3.0 release its own standard, a proposal's suite
# every feature Isoline knows, as its tests build on 3.0.
set(specFeatures_wasm3 wasm3)
set(specFeatures_wide-arithmetic all)
set(specFeatures_custom-page-sizes all)
set(specFeatures_compact-imports all)

# One test per imported .wast file, in files.txt's order, named spec.<source>.<path with / as .>,
# which fails unless the file's script skips exactly as many commands as files.txt pins. An
# `exclude` row names a file that is deliberately not imported.
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
