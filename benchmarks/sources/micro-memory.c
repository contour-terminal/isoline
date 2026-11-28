/* SPDX-License-Identifier: Apache-2.0
 *
 * micro-memory: loads and stores of every width over a 64 KiB buffer, in a data-dependent order, so
 * that what is timed is the bounds-checked memory access.
 */
#include "benchmark.h"

#define BUFFER_BYTES 65536U

BENCHMARK_EXPORT int64_t run(int32_t iterations)
{
    benchmark_reset_arena();
    unsigned char* buffer = calloc(BUFFER_BYTES, 1U);
    BLACK_BOX(buffer);
    uint32_t index = 0;
    uint64_t sum = 0;
    for (int32_t i = 0; i < iterations; i++)
    {
        /* One iteration is 25'000 rounds of four loads and four stores. */
        for (uint32_t k = 0; k < 25000U; k++)
        {
            uint32_t const at = index & (BUFFER_BYTES - 8U);
            uint8_t byte;
            uint16_t half;
            uint32_t word;
            uint64_t wide;
            memcpy(&byte, buffer + at, sizeof byte);
            memcpy(&half, buffer + at + 1U, sizeof half);
            memcpy(&word, buffer + (at ^ 4U), sizeof word);
            memcpy(&wide, buffer + (at & ~7U), sizeof wide);
            sum += byte + half + word + wide;
            byte = (uint8_t) (byte + 1U);
            half = (uint16_t) (half + k);
            word += k * 3U;
            wide ^= sum;
            memcpy(buffer + at, &byte, sizeof byte);
            memcpy(buffer + at + 1U, &half, sizeof half);
            memcpy(buffer + (at ^ 4U), &word, sizeof word);
            memcpy(buffer + (at & ~7U), &wide, sizeof wide);
            index = (index * 1103515245U) + 12345U + (uint32_t) sum;
        }
        BLACK_BOX(buffer);
    }
    return (int64_t) benchmark_mix(benchmark_mix_bytes(BENCHMARK_CHECKSUM_SEED, buffer, 256U), sum);
}
