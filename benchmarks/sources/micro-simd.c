/* SPDX-License-Identifier: Apache-2.0
 *
 * micro-simd: 128-bit integer and float lane arithmetic, shuffles and lane moves, so that what is timed
 * is the SIMD operations. The only guest built with simd128: it has no WAMR column, as Contour's WAMR
 * is built without SIMD.
 */
#include "benchmark.h"

#include <wasm_simd128.h>

BENCHMARK_EXPORT int64_t run(int32_t iterations)
{
    benchmark_reset_arena();
    v128_t integers = wasm_i32x4_make(1, 2, 3, 4);
    v128_t floats = wasm_f32x4_make(0.5f, 1.5f, 2.5f, 3.5f);
    v128_t const increment = wasm_i32x4_splat(0x01010101);
    v128_t const scale = wasm_f32x4_splat(0.999f);
    BLACK_BOX(integers);
    BLACK_BOX(floats);
    for (int32_t i = 0; i < iterations; i++)
    {
        /* One iteration is 25'000 rounds. */
        for (int32_t k = 0; k < 25000; k++)
        {
            integers = wasm_i32x4_add(integers, increment);
            integers = wasm_v128_xor(integers, wasm_i32x4_shl(integers, 3));
            integers = wasm_i8x16_shuffle(integers, integers, 1, 2, 3, 0, 5, 6, 7, 4, 9, 10, 11, 8, 13, 14, 15, 12);
            floats = wasm_f32x4_add(wasm_f32x4_mul(floats, scale), wasm_f32x4_convert_i32x4(wasm_i32x4_shr(integers, 24)));
            floats = wasm_f32x4_min(floats, wasm_f32x4_splat(1.0e6f));
        }
        BLACK_BOX(integers);
    }
    uint64_t checksum = BENCHMARK_CHECKSUM_SEED;
    checksum = benchmark_mix(checksum, (uint64_t) wasm_i64x2_extract_lane(integers, 0));
    checksum = benchmark_mix(checksum, (uint64_t) wasm_i64x2_extract_lane(integers, 1));
    checksum = benchmark_mix(checksum, (uint64_t) wasm_i64x2_extract_lane(floats, 0));
    return (int64_t) benchmark_mix(checksum, (uint64_t) wasm_i64x2_extract_lane(floats, 1));
}
