/* SPDX-License-Identifier: Apache-2.0
 *
 * micro-call_indirect: calls through a table of four functions of one type, chosen by the data, so that
 * what is timed is call_indirect's bounds and signature check besides the call itself.
 */
#include "benchmark.h"

typedef uint32_t (*Step)(uint32_t state, uint32_t k);

__attribute__((noinline)) static uint32_t stepAdd(uint32_t state, uint32_t k) { return state + k + 1U; }
__attribute__((noinline)) static uint32_t stepXor(uint32_t state, uint32_t k) { return state ^ (k * 2654435761U); }
__attribute__((noinline)) static uint32_t stepMul(uint32_t state, uint32_t k) { return (state * 1664525U) + k; }
__attribute__((noinline)) static uint32_t stepRot(uint32_t state, uint32_t k)
{
    return ((state << 7) | (state >> 25)) + k;
}

static Step steps[4] = { stepAdd, stepXor, stepMul, stepRot };

BENCHMARK_EXPORT int64_t run(int32_t iterations)
{
    benchmark_reset_arena();
    uint32_t state = 1;
    BLACK_BOX(state);
    BLACK_BOX(steps);
    for (int32_t i = 0; i < iterations; i++)
    {
        /* One iteration is 100'000 indirect calls. */
        for (uint32_t k = 0; k < 100000U; k++)
            state = steps[(state >> 3) & 3U](state, k);
        BLACK_BOX(state);
    }
    return (int64_t) benchmark_mix(BENCHMARK_CHECKSUM_SEED, state);
}
