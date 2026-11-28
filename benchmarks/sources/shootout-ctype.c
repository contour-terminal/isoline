/* SPDX-License-Identifier: BSD-3-Clause
 *
 * shootout-ctype, from Sightglass (https://github.com/bytecodealliance/sightglass at f0d2040f,
 * benchmarks/shootout/src/ctype.c), which distributes it under The Computer Language Benchmarks
 * Game's 3-clause BSD licence (Copyright (c) 2004-2008 Brent Fulgham, 2005-2020 Isaac Gouy).
 * PROVENANCE.md has its licence chain and NOTICE the licences' texts.
 *
 * Adapted for Isoline (benchmarks/shim/benchmark.h): freestanding, without output, and with main()
 * turned into run(iterations), which repeats the measured loop and returns a checksum of its result.
 */
#include "benchmark.h"
#define STR_SIZE 50000

BENCHMARK_EXPORT int64_t run(int32_t iterations)
{
    benchmark_reset_arena();
    size_t str_size = STR_SIZE;
    char* str = malloc(str_size);
    size_t ret = (size_t)0U;
    memset(str, 'x', str_size);
    str[str_size - 1U] = 0;
    for (int32_t i = 0; i < iterations; i++)
    {
        BLACK_BOX(str);
        for (size_t j = 0U; j < str_size; j++)
        {
            ret += isalpha((int)(unsigned char)str[j]);
            ret += isalnum((int)(unsigned char)str[j]);
            ret += isdigit((int)(unsigned char)str[j]);
            ret += isspace((int)(unsigned char)str[j]);
            ret += isprint((int)(unsigned char)str[j]);
            if (isupper((int)(unsigned char)str[j]))
            {
                str[j] = (char)tolower((int)(unsigned char)str[j]);
            }
            else if (islower((int)(unsigned char)str[j]))
            {
                str[j] = (char)toupper((int)(unsigned char)str[j]);
            }
        }
    }
    BLACK_BOX(ret);
    return (int64_t) benchmark_mix(benchmark_mix(BENCHMARK_CHECKSUM_SEED, ret), (unsigned char) str[0]);
}
