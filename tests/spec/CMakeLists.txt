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

add_executable(isoline_isotest_test IsotestReader_test.cpp SpecCheck_test.cpp)
target_link_libraries(isoline_isotest_test PRIVATE isoline_isotest Catch2::Catch2WithMain)
isoline_apply_target_policy(isoline_isotest_test TEST)
add_test(NAME isoline_isotest_test COMMAND isoline_isotest_test)
set_tests_properties(isoline_isotest_test PROPERTIES LABELS unit)

# One test per imported .wast file, in files.txt's order, which fails unless the file's script
# skips exactly as many commands as files.txt pins.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS files.txt)
file(STRINGS files.txt specFiles REGEX "^[^#]")
foreach(entry IN LISTS specFiles)
    if(NOT entry MATCHES "^([^ ]+) +([0-9]+)$")
        message(FATAL_ERROR "tests/spec/files.txt: expected '<name> <skips>', got '${entry}'")
    endif()
    set(name "${CMAKE_MATCH_1}")
    add_test(NAME spec.${name}
             COMMAND isoline_spec_runner "${CMAKE_CURRENT_SOURCE_DIR}/${name}" "${CMAKE_MATCH_2}")
    set_tests_properties(spec.${name} PROPERTIES LABELS spec)
endforeach()
