# SPDX-License-Identifier: Apache-2.0

# The fuzz targets (README.md): the module generator, the seeds, and what each target does with one input, as
# a library the unit tests, the tools and the libFuzzer entry points share.

add_library(isoline_fuzz STATIC FuzzTargets.cpp ModuleGenerator.cpp Seeds.cpp)
target_include_directories(isoline_fuzz PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(isoline_fuzz PUBLIC isoline_differential isoline::text)
isoline_apply_target_policy(isoline_fuzz)

# The tools, one row each: its name and source.
#   isoline-fuzz-module <input> <directory>: the module a differential input generates, as <seed>.wasm, the
#     form of a differential regression.
#   isoline-fuzz-seeds <load|text> <directory> <max-len>: a target's generated seeds, for a campaign or CI.
set(isolineFuzzTools "isoline-fuzz-module|WriteModule.cpp" "isoline-fuzz-seeds|WriteSeeds.cpp")
foreach(row IN LISTS isolineFuzzTools)
    string(REPLACE "|" ";" fields "${row}")
    list(GET fields 0 tool)
    list(GET fields 1 source)
    add_executable(${tool} ${source})
    target_link_libraries(${tool} PRIVATE isoline_fuzz)
    isoline_apply_target_policy(${tool} TEST)
endforeach()

add_executable(isoline_fuzz_test FuzzTargets_test.cpp ModuleGenerator_test.cpp OracleReplay_test.cpp)
target_link_libraries(isoline_fuzz_test PRIVATE isoline_fuzz Catch2::Catch2WithMain)
# FuzzRegressions reads the committed inputs from the source tree.
target_compile_definitions(isoline_fuzz_test PRIVATE ISOLINE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
isoline_apply_target_policy(isoline_fuzz_test TEST)
math(EXPR isolineFuzzTestTimeout "900 * ${ISOLINE_TEST_TIMEOUT_FACTOR}")
add_test(NAME isoline_fuzz_test COMMAND isoline_fuzz_test "~[.] ~[memory] ~[differential]")
set_tests_properties(isoline_fuzz_test PROPERTIES LABELS unit TIMEOUT ${isolineFuzzTestTimeout})
# The hostile seeds lower within the compile budget, under the build's memory cap.
isoline_add_capped_test(NAME isoline_fuzz_test_memory TARGET isoline_fuzz_test LABELS unit ARGS "[memory]"
                        TIMEOUT ${isolineFuzzTestTimeout})
# The generator's import-free modules against Wasmtime, where ISOLINE_WASMTIME names it; skipped without it.
# Label oracle-nightly, which only the nightly differential run selects.
add_test(NAME isoline_fuzz_oracle COMMAND isoline_fuzz_test "GeneratedOracle.agreesWithWasmtime")
set_tests_properties(isoline_fuzz_oracle PROPERTIES LABELS oracle-nightly SKIP_RETURN_CODE 4 TIMEOUT 3600)

# The fuzz targets, one row each: its name, its source, and its committed inputs (regressions/<inputs>).
# Under ISOLINE_FUZZING (the clang-fuzz presets) -fsanitize=fuzzer links libFuzzer's main into them, and every
# other Isoline target is only instrumented for it (cmake/IsolinePedantic.cmake). In every other Clang build
# they link FuzzReplayMain.cpp instead, which runs the inputs it is given: so clang-tidy checks the entry
# points, and the unit run replays the committed inputs through them. libFuzzer and __builtin_trap are Clang's.
set(isolineFuzzTargets
    "isoline_differential_fuzzer|DifferentialFuzzer.cpp|differential"
    "isoline_load_fuzzer|LoadFuzzer.cpp|load"
    "isoline_text_fuzzer|TextFuzzer.cpp|text")
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT MSVC)
    foreach(row IN LISTS isolineFuzzTargets)
        string(REPLACE "|" ";" fields "${row}")
        list(GET fields 0 target)
        list(GET fields 1 source)
        list(GET fields 2 inputs)
        if(ISOLINE_FUZZING)
            add_executable(${target} ${source})
            target_compile_options(${target} PRIVATE -fsanitize=fuzzer)
            target_link_options(${target} PRIVATE -fsanitize=fuzzer)
        else()
            add_executable(${target} ${source} FuzzReplayMain.cpp)
            add_test(NAME ${target}.replay COMMAND ${target} "${CMAKE_CURRENT_SOURCE_DIR}/regressions/${inputs}")
            set_tests_properties(${target}.replay PROPERTIES LABELS unit TIMEOUT ${isolineFuzzTestTimeout})
        endif()
        target_link_libraries(${target} PRIVATE isoline_fuzz)
        isoline_apply_target_policy(${target} TEST)
    endforeach()
endif()
