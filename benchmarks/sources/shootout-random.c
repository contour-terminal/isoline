/* SPDX-License-Identifier: BSD-3-Clause
 *
 * shootout-random, from Sightglass (https://github.com/bytecodealliance/sightglass at f0d2040f,
 * benchmarks/shootout/src/random.c), which distributes it under The Computer Language Benchmarks
 * Game's 3-clause BSD licence (Copyright (c) 2004-2008 Brent Fulgham, 2005-2020 Isaac Gouy).
 * PROVENANCE.md has its licence chain and NOTICE the licences' texts.
 *
 * Adapted for Isoline (benchmarks/shim/benchmark.h): freestanding, without output, and with main()
 * turned into run(iterations), which repeats the measured loop and returns a checksum of its result.
 */
#include "benchmark.h"
#define IA 3877
#define IC 29573
#define IM 139968

static inline double
gen_random(double max, long ia, long ic, long im)
{
    static long last = 42;
    last = (last * ia + ic) % im;
    return max * last / im;
}

BENCHMARK_EXPORT int64_t run(int32_t iterations)
{
    benchmark_reset_arena();
    /* One iteration is 100'000 numbers; Sightglass draws 12'900'000 at once. */
    long ia = IA;
    long ic = IC;
    long im = IM;
    int n = 100000;
    BLACK_BOX(n);
    for (int32_t i = 0; i < iterations; i++) {
        int k = n;
        while (k--) {
            gen_random(100.0, ia, ic, im);
        }
    }
    double res = gen_random(100.0, ia, ic, im);
    BLACK_BOX(res);
    return (int64_t) benchmark_mix_bytes(BENCHMARK_CHECKSUM_SEED, &res, sizeof res);
}
