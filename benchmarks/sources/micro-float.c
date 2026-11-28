/* SPDX-License-Identifier: Apache-2.0
 *
 * micro-float: f64 and f32 arithmetic, a division and a square root among them, so that what is timed
 * is the float operations (and, in the deterministic profile, the NaN canonicalization after each).
 */
#include "benchmark.h"

BENCHMARK_EXPORT int64_t run(int32_t iterations)
{
    benchmark_reset_arena();
    double x = 0.5;
    double y = 1.25;
    float z = 0.75f;
    BLACK_BOX(x);
    BLACK_BOX(y);
    BLACK_BOX(z);
    for (int32_t i = 0; i < iterations; i++)
    {
        /* One iteration is 25'000 steps of a bounded recurrence. */
        for (int32_t k = 0; k < 25000; k++)
        {
            x = (x * 0.999) + (y * 0.001) + 0.0001;
            y = __builtin_sqrt((y * y) + 1.0) / (1.0 + x);
            z = (z * 0.5f) + (float) (x - y) * 0.25f;
        }
        BLACK_BOX(x);
    }
    uint64_t checksum = benchmark_mix_bytes(BENCHMARK_CHECKSUM_SEED, &x, sizeof x);
    checksum = benchmark_mix_bytes(checksum, &y, sizeof y);
    return (int64_t) benchmark_mix_bytes(checksum, &z, sizeof z);
}
