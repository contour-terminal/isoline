/* SPDX-License-Identifier: BSD-3-Clause
 *
 * shootout-sieve, from Sightglass (https://github.com/bytecodealliance/sightglass at f0d2040f,
 * benchmarks/shootout/src/sieve.c), which distributes it under The Computer Language Benchmarks
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
    static char flags[8192 + 1];
    unsigned long res = 0;
    for (int32_t n = 0; n < iterations; n++)
    {
        int count = 0;
        for (long i = 2; i <= 8192; i++)
        {
            flags[i] = 1;
        }
        for (long i = 2; i <= 8192; i++)
        {
            if (flags[i])
            {
                /* remove all multiples of prime: i */
                for (long k = i + i; k <= 8192; k += i)
                {
                    flags[k] = 0;
                }
                count++;
            }
        }
        res += count;
    }
    BLACK_BOX(res);
    return (int64_t) benchmark_mix(BENCHMARK_CHECKSUM_SEED, res);
}
