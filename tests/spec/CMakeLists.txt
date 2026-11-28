# SPDX-License-Identifier: Apache-2.0

# The official WebAssembly spec suite, imported by scripts/import-spec-tests.py into one directory
# per .wast file: the verbatim .wast script with its oracle manifest and the oracle's .wasm files. The
# import is committed, so running it needs neither the network nor wasm-tools.

add_library(isoline_spectest STATIC Hex.cpp Oracle.cpp SpecCheck.cpp SpecEngine.cpp WastReader.cpp)
target_include_directories(isoline_spectest PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(isoline_spectest PUBLIC isoline::core isoline::text)
isoline_apply_target_policy(isoline_spectest)

add_executable(isoline_spec_runner SpecRunner.cpp)
target_link_libraries(isoline_spec_runner PRIVATE isoline_spectest)
# TEST: it is run by ctest alone, so a failing spec file must end it rather than open a dialog.
isoline_apply_target_policy(isoline_spec_runner TEST)

add_executable(isoline_spectest_test Oracle_test.cpp SpecCheck_test.cpp SpecEngine_test.cpp SpecFeatures_test.cpp
                                     WastReader_test.cpp)
target_link_libraries(isoline_spectest_test PRIVATE isoline_spectest Catch2::Catch2WithMain)
# WastReader.ordinalsMatchWasmTools reads its fixture, tests/spec/ordinals, from the source tree.
target_compile_definitions(isoline_spectest_test PRIVATE ISOLINE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
isoline_apply_target_policy(isoline_spectest_test TEST)
add_test(NAME isoline_spectest_test COMMAND isoline_spectest_test)
math(EXPR isolineSpecUnitTestTimeout "600 * ${ISOLINE_TEST_TIMEOUT_FACTOR}")
set_tests_properties(isoline_spectest_test PROPERTIES LABELS unit TIMEOUT ${isolineSpecUnitTestTimeout})

# The feature set each suite source runs with: the 3.0 release its own standard, a proposal's suite
# every feature Isoline knows, as its tests build on 3.0.
set(specFeatures_wasm3 wasm3)
set(specFeatures_wide-arithmetic all)
set(specFeatures_custom-page-sizes all)
set(specFeatures_compact-imports all)

# The bound of one spec file's test: four times the slowest one under ASan (return_call_ref, 30 s), so that a
# hang fails instead of running to the preset's limit; ISOLINE_TEST_TIMEOUT_FACTOR multiplies it under an
# emulator, as it does each unit-test area's.
math(EXPR isolineSpecTestTimeout "120 * ${ISOLINE_TEST_TIMEOUT_FACTOR}")

## @brief Registers the test of one imported .wast file on one engine: the reference interpreter as
## spec.<source>.<path with / as .> (label spec), the interpreter under a dispatch as
## spec.interpreter.<dispatch>.<source>.<path> (labels spec and interpreter). Each fails unless the file's script skips
## exactly as many commands as files.txt pins, the same on every engine.
## @param engine reference or interpreter.
## @param dispatch The interpreter's dispatch (switch or tailcall); empty for the reference.
function(isoline_add_spec_test engine dispatch source path skips)
    string(REPLACE "/" "." testName "${source}.${path}")
    set(engineArguments)
    if(engine STREQUAL "reference")
        set(testName "spec.${testName}")
        set(labels spec)
    else()
        set(testName "spec.${engine}.${dispatch}.${testName}")
        set(engineArguments --engine ${engine} --dispatch ${dispatch})
        set(labels spec ${engine})
    endif()
    add_test(NAME ${testName}
             COMMAND isoline_spec_runner "${CMAKE_CURRENT_SOURCE_DIR}/${source}/${path}" "${skips}"
                     --features "${specFeatures_${source}}" ${engineArguments})
    set_tests_properties(${testName} PROPERTIES LABELS "${labels}" TIMEOUT ${isolineSpecTestTimeout})
endfunction()

get_property(specDispatches GLOBAL PROPERTY ISOLINE_INTERPRETER_DISPATCH_NAMES)
if(NOT specDispatches)
    message(FATAL_ERROR "tests/spec: no interpreter dispatch is compiled, so no spec.interpreter.* test would run; "
                        "src/isoline/interpreter/CMakeLists.txt must set ISOLINE_INTERPRETER_DISPATCH_NAMES first")
endif()

# One test per imported .wast file and engine, in files.txt's order (isoline_add_spec_test()). An `exclude`
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
    isoline_add_spec_test(reference "" "${source}" "${path}" "${skips}")
    foreach(dispatch IN LISTS specDispatches)
        isoline_add_spec_test(interpreter "${dispatch}" "${source}" "${path}" "${skips}")
    endforeach()
endforeach()
