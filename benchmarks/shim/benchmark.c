/* SPDX-License-Identifier: Apache-2.0
 *
 * The out-of-line half of benchmark.h, linked into every guest. Its own translation unit, so that the
 * kernel's compiler sees neither through benchmark_black_box() nor into the allocator (LTO is off for
 * every build of the flag table). The mem functions are the bulk-memory instructions (the guests'
 * feature list has bulk memory on): the compiler lowers each builtin to memory.copy or memory.fill
 * rather than to a call to itself.
 */
#include "benchmark.h"

static _Alignas(16) unsigned char arena[BENCHMARK_ARENA_BYTES];
static size_t arenaUsed = 0;

int errno = 0;

void benchmark_black_box(void* value)
{
    (void) value;
}

void benchmark_reset_arena(void)
{
    arenaUsed = 0;
}

void* malloc(size_t size)
{
    size_t const aligned = (size + 15U) & ~(size_t) 15U;
    if (aligned < size || aligned > sizeof arena - arenaUsed)
        benchmark_trap();
    void* const block = &arena[arenaUsed];
    arenaUsed += aligned;
    return block;
}

void* calloc(size_t count, size_t size)
{
    if (size != 0 && count > SIZE_MAX / size)
        benchmark_trap();
    void* const block = malloc(count * size);
    __builtin_memset(block, 0, count * size);
    return block;
}

void free(void* pointer)
{
    (void) pointer;
}

void* memcpy(void* destination, void const* source, size_t size)
{
    __builtin_memcpy(destination, source, size);
    return destination;
}

void* memmove(void* destination, void const* source, size_t size)
{
    __builtin_memmove(destination, source, size);
    return destination;
}

void* memset(void* destination, int value, size_t size)
{
    __builtin_memset(destination, value, size);
    return destination;
}

int memcmp(void const* lhs, void const* rhs, size_t size)
{
    unsigned char const* const left = (unsigned char const*) lhs;
    unsigned char const* const right = (unsigned char const*) rhs;
    for (size_t i = 0; i < size; i++)
    {
        if (left[i] != right[i])
            return left[i] < right[i] ? -1 : 1;
    }
    return 0;
}

size_t strlen(char const* text)
{
    size_t length = 0;
    while (text[length] != '\0')
        length++;
    return length;
}

char* strchr(char const* text, int c)
{
    for (;; text++)
    {
        if (*text == (char) c)
            return (char*) text;
        if (*text == '\0')
            return NULL;
    }
}
