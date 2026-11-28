/* SPDX-License-Identifier: BSD-3-Clause
 *
 * shootout-ackermann, from Sightglass (https://github.com/bytecodealliance/sightglass at f0d2040f,
 * benchmarks/shootout/src/ackermann.c), which distributes it under The Computer Language Benchmarks
 * Game's 3-clause BSD licence (Copyright (c) 2004-2008 Brent Fulgham, 2005-2020 Isaac Gouy).
 * PROVENANCE.md has its licence chain and NOTICE the licences' texts.
 *
 * Adapted for Isoline (benchmarks/shim/benchmark.h): freestanding, without output, and with main()
 * turned into run(iterations), which repeats the measured loop and returns a checksum of its result.
 */
#include "benchmark.h"
int ackermann(int M, int N)
{
    if (M == 0)
    {
        return N + 1;
    }
    if (N == 0)
    {
        return ackermann(M - 1, 1);
    }
    return ackermann(M - 1, ackermann(M, (N - 1)));
}

BENCHMARK_EXPORT int64_t run(int32_t iterations)
{
    benchmark_reset_arena();
    /* One iteration is one of Sightglass's repeated calls: A(3, 7). */
    int m = 3;
    int n = 7;
    uint64_t checksum = BENCHMARK_CHECKSUM_SEED;
    for (int32_t i = 0; i < iterations; i++)
    {
        BLACK_BOX(m);
        BLACK_BOX(n);
        int result = ackermann(m, n);
        BLACK_BOX(result);
        checksum = benchmark_mix(checksum, (uint64_t) result);
    }
    return (int64_t) checksum;
}
