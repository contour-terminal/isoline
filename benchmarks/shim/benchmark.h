/* SPDX-License-Identifier: Apache-2.0
 *
 * The freestanding environment every benchmark guest is built against (benchmarks/build-guests.sh): no
 * libc, no WASI, no import. A guest exports `run(i32 iterations) -> i64 checksum`, which repeats its
 * kernel `iterations` times and returns a checksum of what the kernel computed, so that every engine's
 * result can be compared (BenchmarkGuests.checksumsAgreeAcrossEngines) and no optimizer can drop the
 * work. `run(0)` does the setup alone: its time is the cold-start row (load, lowering, instantiation).
 *
 * What a kernel takes from libc is here instead: a bump allocator over a static arena (free does
 * nothing; each run() starts it afresh), the mem and str functions (benchmark.c, as the compiler calls
 * them by name), the ASCII ctype functions, and a trap in place of assert() and abort(). Output is
 * removed: a kernel reports through its checksum.
 */
#ifndef ISOLINE_BENCHMARK_H
    #define ISOLINE_BENCHMARK_H 1

    #include <stddef.h>
    #include <stdint.h>

    /** Marks the guest's entry point: exported as `run`. */
    #define BENCHMARK_EXPORT __attribute__((export_name("run")))

    /** The arena malloc() allocates from: the largest kernel's working set with room to spare. */
    #define BENCHMARK_ARENA_BYTES (4U * 1024U * 1024U)

    /** Ends the guest with a trap: what assert() and abort() do in a guest that has no output. */
    #define benchmark_trap() __builtin_trap()

    /** Traps unless @p condition holds; the guests' assert(). */
    #define benchmark_require(condition) \
        do                               \
        {                                \
            if (!(condition))            \
                benchmark_trap();        \
        } while (0)

/**
 * Keeps the compiler from knowing a value: the pointer is passed through a function it cannot see
 * through, as Sightglass's black_box() does, so the work that produced the value stays.
 */
void benchmark_black_box(void* value);
    #define BLACK_BOX(X) benchmark_black_box((void*) &(X))

/** Starts the allocator afresh: every run() calls it first, as nothing outlives a run. */
void benchmark_reset_arena(void);

/** A bump allocation of @p size bytes, 16-byte aligned; traps when the arena is exhausted. */
void* malloc(size_t size);

/** A zeroed bump allocation of @p count elements of @p size bytes. */
void* calloc(size_t count, size_t size);

/** Does nothing: the arena is released as a whole by the next run(). */
void free(void* pointer);

void* memcpy(void* destination, void const* source, size_t size);
void* memmove(void* destination, void const* source, size_t size);
void* memset(void* destination, int value, size_t size);
int memcmp(void const* lhs, void const* rhs, size_t size);
size_t strlen(char const* text);
char* strchr(char const* text, int c);

/** Where a kernel reports an error, as libc's errno; nothing reads it. */
extern int errno;
#define EINVAL 22
#define ERANGE 34

/** Folds @p value into the checksum @p checksum (an FNV-1a step over the value's 64 bits). */
static inline uint64_t benchmark_mix(uint64_t checksum, uint64_t value)
{
    checksum ^= value;
    return checksum * UINT64_C(0x100000001b3);
}

    /** The checksum every guest starts from: FNV-1a's offset basis. */
    #define BENCHMARK_CHECKSUM_SEED UINT64_C(0xcbf29ce484222325)

/** Folds @p size bytes at @p bytes into @p checksum. */
static inline uint64_t benchmark_mix_bytes(uint64_t checksum, void const* bytes, size_t size)
{
    unsigned char const* const begin = (unsigned char const*) bytes;
    for (size_t i = 0; i < size; i++)
        checksum = benchmark_mix(checksum, begin[i]);
    return checksum;
}

/* The ASCII ctype functions, as the C locale defines them. */
static inline int isupper(int c)
{
    return c >= 'A' && c <= 'Z';
}
static inline int islower(int c)
{
    return c >= 'a' && c <= 'z';
}
static inline int isalpha(int c)
{
    return isupper(c) || islower(c);
}
static inline int isdigit(int c)
{
    return c >= '0' && c <= '9';
}
static inline int isalnum(int c)
{
    return isalpha(c) || isdigit(c);
}
static inline int isspace(int c)
{
    return c == ' ' || (c >= '\t' && c <= '\r');
}
static inline int isprint(int c)
{
    return c >= 0x20 && c < 0x7f;
}
static inline int toupper(int c)
{
    return islower(c) ? c - 'a' + 'A' : c;
}
static inline int tolower(int c)
{
    return isupper(c) ? c - 'A' + 'a' : c;
}

#endif
