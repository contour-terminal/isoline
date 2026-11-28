/* SPDX-License-Identifier: BSD-3-Clause
 *
 * shootout-fib2, from Sightglass (https://github.com/bytecodealliance/sightglass at f0d2040f,
 * benchmarks/shootout/src/fib2.c), which distributes it under The Computer Language Benchmarks
 * Game's 3-clause BSD licence (Copyright (c) 2004-2008 Brent Fulgham, 2005-2020 Isaac Gouy).
 * PROVENANCE.md has its licence chain and NOTICE the licences' texts.
 *
 * Adapted for Isoline (benchmarks/shim/benchmark.h): freestanding, without output, and with main()
 * turned into run(iterations), which repeats the measured loop and returns a checksum of its result.
 */
#include "benchmark.h"
static unsigned long
fib2(unsigned long n)
{
    if (n < 2) {
        return 1;
    }
    return fib2(n - 2) + fib2(n - 1);
}

BENCHMARK_EXPORT int64_t run(int32_t iterations)
{
    benchmark_reset_arena();
    /* One iteration is fib2(25); Sightglass computes fib2(32) once. */
    int n = 25;
    uint64_t checksum = BENCHMARK_CHECKSUM_SEED;
    for (int32_t i = 0; i < iterations; i++)
    {
        BLACK_BOX(n);
        unsigned long res = fib2((unsigned long) n);
        BLACK_BOX(res);
        checksum = benchmark_mix(checksum, res);
    }
    return (int64_t) checksum;
}
