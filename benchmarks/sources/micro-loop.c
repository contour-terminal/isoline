/* SPDX-License-Identifier: Apache-2.0
 *
 * micro-loop: a counted loop and nothing else, so that what is timed is the back-edge (and the poll an
 * interrupting engine makes at it) and a few integer operations on locals.
 */
#include "benchmark.h"

BENCHMARK_EXPORT int64_t run(int32_t iterations)
{
    benchmark_reset_arena();
    uint32_t state = 1;
    BLACK_BOX(state);
    for (int32_t i = 0; i < iterations; i++)
    {
        /* One iteration is 100'000 back-edges of an xorshift step. */
        for (int32_t k = 0; k < 100000; k++)
        {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
        }
        BLACK_BOX(state);
    }
    return (int64_t) benchmark_mix(BENCHMARK_CHECKSUM_SEED, state);
}
