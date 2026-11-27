# SPDX-License-Identifier: Apache-2.0

# The differential harness: every engine of the build runs the same module and every observable is
# compared exactly (DifferentialRunner.hpp), and Wasmtime is the oracle of results and traps
# (WasmtimeOracle.hpp). Runtime level: it links the engines, not the api.

# The platform's half of ProcessRunner, one row per platform.
if(WIN32)
    set(isolineProcessRunnerSource ProcessRunnerWindows.cpp)
else()
    set(isolineProcessRunnerSource ProcessRunnerPosix.cpp)
endif()

add_library(isoline_differential STATIC
            Arguments.cpp Comparison.cpp Corpus.cpp DifferentialRunner.cpp ProcessRunner.cpp Reach.cpp
            ${isolineProcessRunnerSource} RecordingHost.cpp WasmtimeOracle.cpp)
target_include_directories(isoline_differential PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
# The text format compiles the .wat modules a replay or a test is given.
target_link_libraries(isoline_differential PUBLIC isoline::core isoline::text)
isoline_apply_target_policy(isoline_differential)

# isoline-differential-replay <module> <seed>: prints what every engine observed of one module, the
# lowered listing, and the first divergence (exit status 1), for minimizing and reproducing.
add_executable(isoline-differential-replay Replay.cpp)
target_link_libraries(isoline-differential-replay PRIVATE isoline_differential)
isoline_apply_target_policy(isoline-differential-replay TEST)

# A child for ProcessRunner's test: it prints its arguments and exits with the status its first names.
add_executable(isoline_differential_child testing/Child.cpp)
isoline_apply_target_policy(isoline_differential_child TEST)

add_executable(isoline_differential_test
               CorpusReplay_test.cpp DifferentialRunner_test.cpp ProcessRunner_test.cpp WasmtimeOracle_test.cpp)
target_link_libraries(isoline_differential_test PRIVATE isoline_differential Catch2::Catch2WithMain)
# The tests read their modules from the source tree: tests/spec's binaries, the edge modules, the
# regressions, and the corpus under out/.
target_compile_definitions(isoline_differential_test PRIVATE ISOLINE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
isoline_apply_target_policy(isoline_differential_test TEST)

# The unit run: every case but the corpus replay and the [memory] cases. The spec binaries run on every
# engine, both lanes and both observation modes, which takes a while under a sanitizer or an emulator.
math(EXPR isolineDifferentialTimeout "900 * ${ISOLINE_TEST_TIMEOUT_FACTOR}")
# ProcessRunner's case starts the child itself, so a cross toolchain's ISOLINE_EMULATED_CHILD_ENVIRONMENT
# (the qemu sysroot) goes with it.
set(isolineDifferentialEnvironment "ISOLINE_DIFFERENTIAL_CHILD=$<TARGET_FILE:isoline_differential_child>")
list(APPEND isolineDifferentialEnvironment ${ISOLINE_EMULATED_CHILD_ENVIRONMENT})
add_test(NAME isoline_differential_test COMMAND isoline_differential_test "~[.] ~[memory] ~[differential]")
set_tests_properties(isoline_differential_test PROPERTIES
                     LABELS unit TIMEOUT ${isolineDifferentialTimeout}
                     ENVIRONMENT "${isolineDifferentialEnvironment}")
# The 64 MiB fill, under the build's memory cap.
isoline_add_capped_test(NAME isoline_differential_test_memory TARGET isoline_differential_test LABELS unit
                        ARGS "[memory]")

# The generated corpus (scripts/make-differential-corpus.py, never committed) on every engine, and Wasmtime
# as the oracle where scripts/fetch-tools.py has fetched it. Label differential, which the test presets
# leave out: CI's differential job runs it, as `ctest -L differential` does locally. It skips (exit status
# 4) while out/corpus/differential does not exist.
math(EXPR isolineCorpusTimeout "7200 * ${ISOLINE_TEST_TIMEOUT_FACTOR}")
add_test(NAME isoline_differential_corpus COMMAND isoline_differential_test "[differential]")
set_tests_properties(isoline_differential_corpus PROPERTIES
                     LABELS differential TIMEOUT ${isolineCorpusTimeout} SKIP_RETURN_CODE 4)
