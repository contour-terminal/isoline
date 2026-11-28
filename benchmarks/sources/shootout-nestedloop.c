/* SPDX-License-Identifier: BSD-3-Clause
 *
 * shootout-nestedloop, from Sightglass (https://github.com/bytecodealliance/sightglass at f0d2040f,
 * benchmarks/shootout/src/nestedloop.c), which distributes it under The Computer Language Benchmarks
 * Game's 3-clause BSD licence (Copyright (c) 2004-2008 Brent Fulgham, 2005-2020 Isaac Gouy).
 * PROVENANCE.md has its licence chain and NOTICE the licences' texts.
 *
 * Adapted for Isoline (benchmarks/shim/benchmark.h): freestanding, without output, and with main()
 * turned into run(iterations), which repeats the measured loop and returns a checksum of its result.
 */
#include "benchmark.h"

BENCHMARK_EXPORT int64_t run(int32_t iterations)
{
    benchmark_reset_arena();
    /* One iteration is the six nested loops at length 10; Sightglass runs them once at 15. */
    int n = 10;
    BLACK_BOX(n);
    /* `x` is `volatile` so that the increment is a real load/store: otherwise the compiler
       closes the whole loop nest into a single multiply and the benchmark measures nothing. */
    volatile int x = 0;
    BLACK_BOX(x);
    for (int32_t i = 0; i < iterations; i++) {
        for (int a = 0; a < n; a++) {
            for (int b = 0; b < n; b++) {
                for (int c = 0; c < n; c++) {
                    for (int d = 0; d < n; d++) {
                        for (int e = 0; e < n; e++) {
                            for (int f = 0; f < n; f++) {
                                x++;
                            }
                        }
                    }
                }
            }
        }
    }
    BLACK_BOX(x);
    return (int64_t) benchmark_mix(BENCHMARK_CHECKSUM_SEED, (uint64_t) x);
}
