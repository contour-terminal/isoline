/* SPDX-License-Identifier: Apache-2.0
 *
 * micro-call: direct calls of a small function the compiler may not inline, so that what is timed is
 * the call, its frame and its return.
 */
#include "benchmark.h"

__attribute__((noinline)) static uint32_t step(uint32_t state, uint32_t k)
{
    return (state * 1664525U) + 1013904223U + k;
}

BENCHMARK_EXPORT int64_t run(int32_t iterations)
{
    benchmark_reset_arena();
    uint32_t state = 1;
    BLACK_BOX(state);
    for (int32_t i = 0; i < iterations; i++)
    {
        /* One iteration is 100'000 calls. */
        for (uint32_t k = 0; k < 100000U; k++)
            state = step(state, k);
        BLACK_BOX(state);
    }
    return (int64_t) benchmark_mix(BENCHMARK_CHECKSUM_SEED, state);
}
