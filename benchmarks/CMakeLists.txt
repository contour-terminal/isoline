# SPDX-License-Identifier: Apache-2.0
#
# The benchmark harness (docs/performance.md): the guests (sources/, built by build-guests.sh into guests/ and
# pinned in guests.lock), the runners scripts/benchmark.py times, and the tests that keep the guests honest.
#
# The guests' tests are part of every testing build: they run the committed guests on every engine of the
# build. The runners are built only with ISOLINE_BENCHMARKS, in the trees scripts/benchmark.py configures with
# its flag table: isoline-benchmark (every Isoline engine and dispatch), isoline-benchmark-simd-lanes (the
# SIMD lane kernels under callgrind) and the WAMR runners (wamr/), which download WAMR's pinned source.

add_library(isoline_benchmarks STATIC GuestRun.cpp)
target_include_directories(isoline_benchmarks PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(isoline_benchmarks PUBLIC isoline::core)
isoline_apply_target_policy(isoline_benchmarks)

if(ISOLINE_BENCHMARKS)
    # The WAMR runners are C: enabled here only, so that a build without benchmarks needs no C compiler.
    enable_language(C)

    add_executable(isoline-benchmark BenchmarkRunner.cpp)
    target_link_libraries(isoline-benchmark PRIVATE isoline_benchmarks)
    isoline_apply_target_policy(isoline-benchmark)

    add_executable(isoline-benchmark-simd-lanes SimdLanes.cpp)
    target_link_libraries(isoline-benchmark-simd-lanes PRIVATE isoline::core)
    isoline_apply_target_policy(isoline-benchmark-simd-lanes)

    add_subdirectory(wamr)
endif()

if(ISOLINE_TESTING)
    add_executable(isoline_benchmark_test BenchmarkGuests_test.cpp)
    target_link_libraries(isoline_benchmark_test PRIVATE isoline_benchmarks Catch2::Catch2WithMain)
    # The committed guests and their lock, read from the source tree.
    target_compile_definitions(isoline_benchmark_test PRIVATE ISOLINE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    isoline_apply_target_policy(isoline_benchmark_test TEST)
    math(EXPR isolineBenchmarkTimeout "600 * ${ISOLINE_TEST_TIMEOUT_FACTOR}")
    add_test(NAME isoline_benchmark_test COMMAND isoline_benchmark_test)
    set_tests_properties(isoline_benchmark_test PROPERTIES LABELS benchmark TIMEOUT ${isolineBenchmarkTimeout})
    # wamr-classic's options (wamr/ClassicOptions.cmake): Contour's with the fast interpreter off, and a
    # refusal where there is no fast interpreter row to turn off.
    add_test(NAME wamr-classic-options
             COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_SOURCE_DIR}/wamr/ClassicOptions_test.cmake")
    set_tests_properties(wamr-classic-options PROPERTIES LABELS lint PASS_REGULAR_EXPRESSION "wamr-classic-options: ok")
    add_test(NAME wamr-classic-options-refuses
             COMMAND "${CMAKE_COMMAND}" -DCASE=missing -P "${CMAKE_CURRENT_SOURCE_DIR}/wamr/ClassicOptions_test.cmake")
    set_tests_properties(wamr-classic-options-refuses PROPERTIES LABELS lint
                         PASS_REGULAR_EXPRESSION "have no \"WAMR_BUILD_FAST_INTERP 1\" row")
endif()
