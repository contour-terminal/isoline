/* SPDX-License-Identifier: BSD-3-Clause
 *
 * shootout-memmove, from Sightglass (https://github.com/bytecodealliance/sightglass at f0d2040f,
 * benchmarks/shootout/src/memmove.c), which distributes it under The Computer Language Benchmarks
 * Game's 3-clause BSD licence (Copyright (c) 2004-2008 Brent Fulgham, 2005-2020 Isaac Gouy).
 * PROVENANCE.md has its licence chain and NOTICE the licences' texts.
 *
 * Adapted for Isoline (benchmarks/shim/benchmark.h): freestanding, without output, and with main()
 * turned into run(iterations), which repeats the measured loop and returns a checksum of its result.
 */
#include "benchmark.h"
#define STR_SIZE 10000

BENCHMARK_EXPORT int64_t run(int32_t iterations)
{
    benchmark_reset_arena();
    size_t str_size = STR_SIZE;
    char* str = calloc(str_size, (size_t)1U);
    str[str_size / 2U] = 1;
    for (int32_t i = 0; i < iterations; i++) {
        BLACK_BOX(str);
        for (size_t j = (size_t)0U; j < str_size - (size_t)1U; j++) {
            memmove(&str[j], str, str_size - j);
        }
        for (size_t j = (size_t)0U; j < str_size - (size_t)1U; j++) {
            memmove(str, &str[j], str_size - j);
        }
    }
    BLACK_BOX(str);
    return (int64_t) benchmark_mix_bytes(BENCHMARK_CHECKSUM_SEED, str, str_size);
}
