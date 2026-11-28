/* SPDX-License-Identifier: BSD-3-Clause AND ISC
 *
 * shootout-gimli, from Sightglass (https://github.com/bytecodealliance/sightglass at f0d2040f,
 * benchmarks/shootout/src/gimli.c), which distributes it under The Computer Language Benchmarks
 * Game's 3-clause BSD licence (Copyright (c) 2004-2008 Brent Fulgham, 2005-2020 Isaac Gouy).
 * It reproduces libhydrogen's gimli_core() (impl/gimli-core/portable.h),
 * Copyright (c) 2013-2026 Frank Denis, under the ISC licence.
 * PROVENANCE.md has its licence chain and NOTICE the licences' texts.
 *
 * Adapted for Isoline (benchmarks/shim/benchmark.h): freestanding, without output, and with main()
 * turned into run(iterations), which repeats the measured loop and returns a checksum of its result.
 */
#include "benchmark.h"
#include <stdint.h>

#define gimli_BLOCKBYTES 48
#define ROTL32(x, b) (uint32_t)(((x) << (b)) | ((x) >> (32 - (b))))

static void
gimli_core(uint32_t state[gimli_BLOCKBYTES / 4])
{
    unsigned int round;
    unsigned int column;
    uint32_t     x;
    uint32_t     y;
    uint32_t     z;

    for (round = 24; round > 0; round--) {
        for (column = 0; column < 4; column++) {
            x = ROTL32(state[column], 24);
            y = ROTL32(state[4 + column], 9);
            z = state[8 + column];

            state[8 + column] = x ^ (z << 1) ^ ((y & z) << 2);
            state[4 + column] = y ^ x ^ ((x | z) << 1);
            state[column] = z ^ y ^ ((x & y) << 3);
        }
        switch (round & 3) {
        case 0:
            x = state[0];
            state[0] = state[1];
            state[1] = x;
            x = state[2];
            state[2] = state[3];
            state[3] = x;
            state[0] ^= ((uint32_t)0x9e377900 | round);
            break;
        case 2:
            x = state[0];
            state[0] = state[2];
            state[2] = x;
            x = state[1];
            state[1] = state[3];
            state[3] = x;
        }
    }
}

BENCHMARK_EXPORT int64_t run(int32_t iterations)
{
    benchmark_reset_arena();
    uint32_t state[gimli_BLOCKBYTES / 4] = { 0 };
    BLACK_BOX(state);
    for (int32_t i = 0; i < iterations; i++) {
        gimli_core(state);
    }
    BLACK_BOX(state[0]);
    return (int64_t) benchmark_mix_bytes(BENCHMARK_CHECKSUM_SEED, state, sizeof state);
}
