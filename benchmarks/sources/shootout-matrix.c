/* SPDX-License-Identifier: BSD-3-Clause
 *
 * shootout-matrix, from Sightglass (https://github.com/bytecodealliance/sightglass at f0d2040f,
 * benchmarks/shootout/src/matrix.c), which distributes it under The Computer Language Benchmarks
 * Game's 3-clause BSD licence (Copyright (c) 2004-2008 Brent Fulgham, 2005-2020 Isaac Gouy).
 * PROVENANCE.md has its licence chain and NOTICE the licences' texts.
 *
 * Adapted for Isoline (benchmarks/shim/benchmark.h): freestanding, without output, and with main()
 * turned into run(iterations), which repeats the measured loop and returns a checksum of its result.
 */
#include "benchmark.h"
#define SIZE 10

static int**
mkmatrix(int rows, int cols)
{
    int** m = calloc(rows, sizeof(int*));
    benchmark_require(m != NULL);

    int i, j, count = 1;
    for (i = 0; i < rows; i++) {
        m[i] = calloc(cols, sizeof(int));
        benchmark_require(m[i] != NULL);
        for (j = 0; j < cols; j++) {
            m[i][j] = count++;
        }
    }
    return m;
}

static void
freematrix(int rows, int** m)
{
    while (--rows > -1) {
        free(m[rows]);
    }
    free(m);
}

static int**
mmult(int rows, int cols, int** m1, int** m2, int** m3)
{
    int i, j, k, val;

    for (i = 0; i < rows; i++) {
        for (j = 0; j < cols; j++) {
            val = 0;
            for (k = 0; k < cols; k++) {
                val += m1[i][k] * m2[k][j];
            }
            m3[i][j] = val;
        }
    }
    return m3;
}

BENCHMARK_EXPORT int64_t run(int32_t iterations)
{
    benchmark_reset_arena();
    int rows = SIZE;
    int cols = SIZE;
    int** m1 = mkmatrix(rows, cols);
    int** m2 = mkmatrix(rows, cols);
    int** mm = mkmatrix(rows, cols);
    for (int32_t i = 0; i < iterations; i++) {
        mm = mmult(rows, cols, m1, m2, mm);
    }
    int res = mm[0][0] + mm[2][3] + mm[3][2] + mm[4][4];
    BLACK_BOX(res);
    freematrix(rows, m1);
    freematrix(rows, m2);
    freematrix(rows, mm);
    return (int64_t) benchmark_mix(BENCHMARK_CHECKSUM_SEED, (uint64_t) res);
}
