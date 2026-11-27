/* SPDX-License-Identifier: Apache-2.0 */
/* wasm.h's types as a C program sees them, through the shared library, with val.h's isoline_ functions:
 * every kind and its round trip, the kinds Isoline adds outside upstream's numbering, the limits wasm.h's
 * 32-bit wasm_limits_t cannot hold, the casts between extern types, and what a type that no module could
 * declare, or a NULL where a type is due, gives: NULL, a value val.h names, or an error, never an abort.
 *
 * Labelled memory and run under the build's cap (tests/c/CMakeLists.txt): it makes function types of a
 * hundred thousand parameters, and gives sizes no vector has. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <isoline/c/isoline.h>

#define C_TEST_SUITE "types"
/* The cases in groups that each fit a line. */
#define C_TEST_KIND_CASES(X) \
    X(unknownValkindIsNull) X(privateKindsOutsideUpstreamSpace) X(v128KindReported) X(exnrefKindReported)
#define C_TEST_FUNC_CASES(X) \
    X(functypeRoundTrip)     \
    X(functypeRefusesMalformedVectors) X(functypeRefusesAHoleInTheMiddle) X(functypeOfHugeVectors)
#define C_TEST_LIMIT_CASES(X) \
    X(limitsClampedFor64)     \
    X(limitsAtTheU32Boundary) \
    X(limitsMinimumAboveMaximumRefused) X(limitsPastTheAddressTypeRefused) X(limits64StructIsChecked)
#define C_TEST_TABLE_CASES(X) X(tableElementIsAReference) X(table64RoundTrip) X(memoryPageSizeAndAddressBits)
#define C_TEST_LEAK_CASES(X)  X(inlineHelpersLeakNothing)
#define C_TEST_OTHER_CASES(X) \
    X(globaltypeRoundTrip)    \
    X(externtypeCastsRefuseWrongKind) X(tagtypeRoundTrip) X(importAndExportTypes) X(nullTypesAreNamed)
#define C_TEST_CASES(X)  \
    C_TEST_KIND_CASES(X) \
    C_TEST_FUNC_CASES(X) \
    C_TEST_LIMIT_CASES(X) C_TEST_TABLE_CASES(X) C_TEST_LEAK_CASES(X) C_TEST_OTHER_CASES(X)
#include "IsolineCTest.h"

/* The kinds wasm_valtype_new() makes: upstream's six, then Isoline's two. */
static wasm_valkind_t const c_test_kinds[] = {
    WASM_I32,
    WASM_I64,
    WASM_F32,
    WASM_F64,
    WASM_EXTERNREF,
    WASM_FUNCREF,
    ISOLINE_VALKIND_V128,
    ISOLINE_VALKIND_EXNREF,
};
#define C_TEST_KIND_COUNT (sizeof c_test_kinds / sizeof c_test_kinds[0])

/* 64-bit limits, size first. */
static isoline_limits64_t c_test_limits(uint64_t minimum, uint64_t maximum, isoline_maximum_kind_t kind)
{
    isoline_limits64_t limits;
    memset(&limits, 0, sizeof limits);
    limits.struct_size = sizeof limits;
    limits.minimum = minimum;
    limits.maximum = maximum;
    limits.maximum_kind = kind;
    return limits;
}

/* A fresh extern type, of a function of no parameters and no results. */
static wasm_externtype_t* c_test_func_externtype(void)
{
    return wasm_functype_as_externtype(wasm_functype_new_0_0());
}

/* The code of @p error, which it deletes. */
static isoline_error_code_t c_test_code_of(isoline_error_t* error)
{
    isoline_error_code_t const code = isoline_error_code(error);
    isoline_error_delete(error);
    return code;
}

ISOLINE_TEST_CASE(unknownValkindIsNull)
{
    unsigned value = 0;
    size_t made = 0;
    CHECK(wasm_valtype_new(77) == NULL);
    /* Of all 256 values, exactly the eight kinds make a type, each of its own kind: no value is cast. */
    for (value = 0; value <= UINT8_MAX; ++value)
    {
        wasm_valtype_t* const type = wasm_valtype_new((wasm_valkind_t) value);
        size_t index = 0;
        int known = 0;
        for (index = 0; index < C_TEST_KIND_COUNT; ++index)
            known = known || c_test_kinds[index] == value;
        CHECK((type != NULL) == known);
        if (type != NULL)
        {
            CHECK(wasm_valtype_kind(type) == value);
            ++made;
        }
        wasm_valtype_delete(type);
    }
    CHECK(made == C_TEST_KIND_COUNT);
    CHECK(wasm_valtype_new(ISOLINE_VALKIND_INVALID) == NULL);
}

ISOLINE_TEST_CASE(privateKindsOutsideUpstreamSpace)
{
    /* Upstream numbers kinds 0-3 and from 128, and wasm.h counts a kind below 128 a number, every other a
     * reference. Isoline's kinds are none of upstream's, and each on the side wasm.h reads right: v128 is
     * 0x7B, its byte in the binary format; exnref is in 0xF0-0xFF, the range Isoline keeps for itself. */
    size_t index = 0;
    CHECK(ISOLINE_VALKIND_V128 == 0x7B);
    CHECK(wasm_valkind_is_num(ISOLINE_VALKIND_V128) && !wasm_valkind_is_ref(ISOLINE_VALKIND_V128));
    CHECK(ISOLINE_VALKIND_EXNREF >= 0xF0 && ISOLINE_VALKIND_EXNREF <= 0xFF);
    CHECK(wasm_valkind_is_ref(ISOLINE_VALKIND_EXNREF) && !wasm_valkind_is_num(ISOLINE_VALKIND_EXNREF));
    /* What NULL gives is 0xFF, which wasm.h calls a reference (val.h says so). */
    CHECK(ISOLINE_VALKIND_INVALID == 0xFF && wasm_valkind_is_ref(ISOLINE_VALKIND_INVALID));
    for (index = 0; index < C_TEST_KIND_COUNT; ++index)
    {
        wasm_valkind_t const kind = c_test_kinds[index];
        int const upstream = kind <= WASM_F64 || kind == WASM_EXTERNREF || kind == WASM_FUNCREF;
        int const isolines = kind == ISOLINE_VALKIND_V128 || kind == ISOLINE_VALKIND_EXNREF;
        CHECK(upstream != isolines);
    }
    CHECK(ISOLINE_EXTERNKIND_INVALID > WASM_EXTERN_TAG);
    CHECK(ISOLINE_MUTABILITY_INVALID != WASM_CONST && ISOLINE_MUTABILITY_INVALID != WASM_VAR);
}

ISOLINE_TEST_CASE(v128KindReported)
{
    wasm_valtype_t* const type = wasm_valtype_new(ISOLINE_VALKIND_V128);
    wasm_valtype_t* copy = NULL;
    REQUIRE(type != NULL);
    CHECK(wasm_valtype_kind(type) == ISOLINE_VALKIND_V128);
    /* A vector is no reference, whatever wasm_valkind_is_ref() makes of its number. */
    CHECK(isoline_valtype_heap(type) == ISOLINE_HEAP_NOT_REFERENCE);
    CHECK(isoline_valtype_nullability(type) == ISOLINE_NOT_NULLABLE);
    CHECK(isoline_valtype_concrete_index(type) == UINT32_MAX);
    copy = wasm_valtype_copy(type);
    wasm_valtype_delete(type);
    REQUIRE(copy != NULL);
    CHECK(wasm_valtype_kind(copy) == ISOLINE_VALKIND_V128);
    wasm_valtype_delete(copy);
}

ISOLINE_TEST_CASE(exnrefKindReported)
{
    wasm_valtype_t* const exnref = wasm_valtype_new(ISOLINE_VALKIND_EXNREF);
    wasm_valtype_t* const funcref = wasm_valtype_new(WASM_FUNCREF);
    wasm_valtype_t* const externref = wasm_valtype_new(WASM_EXTERNREF);
    wasm_valtype_t* const number = wasm_valtype_new(WASM_I64);
    REQUIRE(exnref != NULL && funcref != NULL && externref != NULL && number != NULL);
    CHECK(wasm_valtype_kind(exnref) == ISOLINE_VALKIND_EXNREF);
    CHECK(isoline_valtype_heap(exnref) == ISOLINE_HEAP_EXN);
    CHECK(isoline_valtype_nullability(exnref) == ISOLINE_NULLABLE);
    CHECK(isoline_valtype_heap(funcref) == ISOLINE_HEAP_FUNC
          && isoline_valtype_nullability(funcref) == ISOLINE_NULLABLE);
    CHECK(isoline_valtype_heap(externref) == ISOLINE_HEAP_EXTERN);
    CHECK(isoline_valtype_heap(number) == ISOLINE_HEAP_NOT_REFERENCE);
    CHECK(isoline_valtype_nullability(number) == ISOLINE_NOT_NULLABLE);
    CHECK(isoline_valtype_concrete_index(funcref) == UINT32_MAX);
    wasm_valtype_delete(exnref);
    wasm_valtype_delete(funcref);
    wasm_valtype_delete(externref);
    wasm_valtype_delete(number);
}

ISOLINE_TEST_CASE(functypeRoundTrip)
{
    wasm_valtype_vec_t params;
    wasm_valtype_vec_t results;
    wasm_functype_t* type = NULL;
    wasm_functype_t* copy = NULL;
    wasm_valtype_vec_t const* read = NULL;
    size_t index = 0;
    wasm_valtype_vec_new_uninitialized(&params, C_TEST_KIND_COUNT);
    REQUIRE(params.size == C_TEST_KIND_COUNT);
    for (index = 0; index < C_TEST_KIND_COUNT; ++index)
        params.data[index] = wasm_valtype_new(c_test_kinds[index]);
    wasm_valtype_vec_new_uninitialized(&results, 1);
    REQUIRE(results.size == 1);
    results.data[0] = wasm_valtype_new(ISOLINE_VALKIND_V128);

    type = wasm_functype_new(&params, &results);
    REQUIRE(type != NULL);
    /* The function type took the vectors' elements, and left the vectors empty. */
    CHECK(params.size == 0 && params.data == NULL);
    CHECK(results.size == 0 && results.data == NULL);

    /* A copy is deep: it outlives its original. */
    copy = wasm_functype_copy(type);
    wasm_functype_delete(type);
    REQUIRE(copy != NULL);
    read = wasm_functype_params(copy);
    REQUIRE(read != NULL && read->size == C_TEST_KIND_COUNT);
    for (index = 0; index < C_TEST_KIND_COUNT; ++index)
        CHECK(wasm_valtype_kind(read->data[index]) == c_test_kinds[index]);
    read = wasm_functype_results(copy);
    REQUIRE(read != NULL && read->size == 1);
    CHECK(wasm_valtype_kind(read->data[0]) == ISOLINE_VALKIND_V128);
    CHECK(wasm_externtype_kind(wasm_functype_as_externtype(copy)) == WASM_EXTERN_FUNC);
    wasm_functype_delete(copy);

    /* wasm.h's own shorthands make the same, of no parameters and no results. */
    type = wasm_functype_new_0_0();
    REQUIRE(type != NULL);
    CHECK(wasm_functype_params(type)->size == 0 && wasm_functype_results(type)->size == 0);
    wasm_functype_delete(type);
}

ISOLINE_TEST_CASE(functypeRefusesMalformedVectors)
{
    /* Refused, wasm_functype_new() takes its vectors all the same and deletes them, as wasm.h's `own` says: a
     * vector not yet filled (an element NULL) is deleted, and left empty. A vector it cannot walk, one that
     * claims elements and has no data, or one of a size no vector has, it leaves untouched. */
    wasm_valtype_vec_t partial;
    wasm_valtype_vec_t empty;
    wasm_valtype_vec_t other;
    wasm_valtype_vec_t noData = { 3, NULL };
    wasm_valtype_t* one = wasm_valtype_new(WASM_I32);
    wasm_valtype_vec_t lying = { SIZE_MAX, NULL };
    lying.data = &one;
    wasm_valtype_vec_new_uninitialized(&partial, 2);
    REQUIRE(partial.size == 2);
    partial.data[0] = wasm_valtype_new(WASM_F32);
    wasm_valtype_vec_new_empty(&empty);

    CHECK(wasm_functype_new(&partial, &empty) == NULL);
    CHECK(partial.size == 0 && partial.data == NULL);
    CHECK(wasm_functype_new(&empty, &noData) == NULL);
    CHECK(noData.size == 3 && noData.data == NULL);
    /* The size is checked before any element is read: one element exists, SIZE_MAX are claimed. */
    CHECK(wasm_functype_new(&lying, &empty) == NULL);
    CHECK(lying.size == SIZE_MAX && lying.data == &one);
    /* A NULL vector beside a well-formed one: the well-formed one is taken, and deleted. */
    wasm_valtype_vec_new_uninitialized(&other, 1);
    REQUIRE(other.size == 1);
    other.data[0] = wasm_valtype_new(WASM_I64);
    CHECK(wasm_functype_new(NULL, &other) == NULL);
    CHECK(other.size == 0 && other.data == NULL);
    CHECK(wasm_functype_new(&empty, NULL) == NULL);

    /* Only what was left untouched is still the caller's. */
    wasm_valtype_delete(one);
}

ISOLINE_TEST_CASE(functypeRefusesAHoleInTheMiddle)
{
    /* A NULL element between two filled ones: refused, and every element is deleted, the one after the hole
     * as well as the one before it. LeakSanitizer (the build's ASan) proves the deletion: a walk that stopped
     * at the hole would leak the last element. */
    wasm_valtype_vec_t holed;
    wasm_valtype_vec_t results;
    wasm_valtype_vec_new_uninitialized(&holed, 3);
    REQUIRE(holed.size == 3);
    holed.data[0] = wasm_valtype_new(WASM_F32);
    holed.data[1] = NULL;
    holed.data[2] = wasm_valtype_new(WASM_I64);
    wasm_valtype_vec_new_uninitialized(&results, 1);
    REQUIRE(results.size == 1);
    results.data[0] = wasm_valtype_new(WASM_F64);

    CHECK(wasm_functype_new(&holed, &results) == NULL);
    CHECK(holed.size == 0 && holed.data == NULL);
    CHECK(results.size == 0 && results.data == NULL);
}

ISOLINE_TEST_CASE(functypeOfHugeVectors)
{
    /* A hundred thousand parameters and results each: made, copied and deleted, nothing quadratic. */
    size_t const count = 100000;
    wasm_valkind_t const alternating[2] = { WASM_I64, ISOLINE_VALKIND_EXNREF };
    wasm_valtype_vec_t params;
    wasm_valtype_vec_t results;
    wasm_functype_t* type = NULL;
    wasm_functype_t* copy = NULL;
    size_t index = 0;
    wasm_valtype_vec_new_uninitialized(&params, count);
    wasm_valtype_vec_new_uninitialized(&results, count);
    REQUIRE(params.size == count && results.size == count);
    for (index = 0; index < count; ++index)
    {
        params.data[index] = wasm_valtype_new(alternating[index % 2]);
        results.data[index] = wasm_valtype_new(WASM_F64);
    }
    type = wasm_functype_new(&params, &results);
    REQUIRE(type != NULL);
    copy = wasm_functype_copy(type);
    wasm_functype_delete(type);
    REQUIRE(copy != NULL);
    CHECK(wasm_functype_params(copy)->size == count && wasm_functype_results(copy)->size == count);
    CHECK(wasm_valtype_kind(wasm_functype_params(copy)->data[count - 1]) == ISOLINE_VALKIND_EXNREF);
    wasm_functype_delete(copy);
}

ISOLINE_TEST_CASE(limitsClampedFor64)
{
    /* 2^33 pages of a 64-bit memory: wasm_limits_t holds no such size, and says wasm_limits_max_default;
     * isoline_memorytype_limits64() gives it exactly. */
    uint64_t const pages = (uint64_t) 1 << 33;
    isoline_limits64_t const limits = c_test_limits(pages, 0, ISOLINE_MAXIMUM_NONE);
    isoline_limits64_t read = c_test_limits(0, 0, ISOLINE_MAXIMUM_NONE);
    wasm_memorytype_t* type = NULL;
    wasm_memorytype_t* copy = NULL;
    REQUIRE(isoline_memorytype_new(&limits, 64, 16, &type) == NULL);
    REQUIRE(type != NULL);
    CHECK(wasm_memorytype_limits(type)->min == wasm_limits_max_default);
    CHECK(wasm_memorytype_limits(type)->max == wasm_limits_max_default);
    CHECK(isoline_memorytype_limits64(type, &read) == NULL);
    CHECK(read.minimum == pages && read.maximum_kind == ISOLINE_MAXIMUM_NONE && read.maximum == 0);
    CHECK(isoline_memorytype_address_bits(type) == 64);
    copy = wasm_memorytype_copy(type);
    wasm_memorytype_delete(type);
    REQUIRE(copy != NULL);
    read.minimum = 0;
    CHECK(isoline_memorytype_limits64(copy, &read) == NULL);
    CHECK(read.minimum == pages && isoline_memorytype_address_bits(copy) == 64);
    wasm_memorytype_delete(copy);
}

ISOLINE_TEST_CASE(limitsAtTheU32Boundary)
{
    /* Sizes up to UINT32_MAX - 1 fit wasm_limits_t exactly; UINT32_MAX itself is wasm_limits_max_default,
     * which wasm.h also says for no maximum; one more is clamped to it. */
    uint64_t const sizes[] = { 0xFFFFFFFEu, 0xFFFFFFFFu, (uint64_t) 0xFFFFFFFFu + 1 };
    uint32_t const reported[] = { 0xFFFFFFFEu, 0xFFFFFFFFu, 0xFFFFFFFFu };
    size_t index = 0;
    for (index = 0; index < sizeof sizes / sizeof sizes[0]; ++index)
    {
        isoline_limits64_t const limits = c_test_limits(sizes[index], sizes[index], ISOLINE_MAXIMUM_GIVEN);
        isoline_limits64_t read = c_test_limits(0, 0, ISOLINE_MAXIMUM_NONE);
        wasm_memorytype_t* memory = NULL;
        wasm_tabletype_t* table = NULL;
        REQUIRE(isoline_memorytype_new(&limits, 64, 16, &memory) == NULL);
        CHECK(wasm_memorytype_limits(memory)->min == reported[index]);
        CHECK(wasm_memorytype_limits(memory)->max == reported[index]);
        CHECK(isoline_memorytype_limits64(memory, &read) == NULL);
        CHECK(read.minimum == sizes[index] && read.maximum == sizes[index]
              && read.maximum_kind == ISOLINE_MAXIMUM_GIVEN);
        wasm_memorytype_delete(memory);
        REQUIRE(isoline_tabletype_new(wasm_valtype_new(WASM_FUNCREF), &limits, 64, &table) == NULL);
        CHECK(wasm_tabletype_limits(table)->min == reported[index]);
        CHECK(wasm_tabletype_limits(table)->max == reported[index]);
        CHECK(isoline_tabletype_limits64(table, &read) == NULL);
        CHECK(read.minimum == sizes[index] && read.maximum == sizes[index]);
        wasm_tabletype_delete(table);
    }
    {
        /* A 32-bit table reaches UINT32_MAX elements; through wasm.h, a maximum of UINT32_MAX is none. */
        wasm_limits_t const largest = { 0xFFFFFFFFu, 0xFFFFFFFEu };
        wasm_limits_t const unbounded = { 1, wasm_limits_max_default };
        isoline_limits64_t read = c_test_limits(0, 0, ISOLINE_MAXIMUM_NONE);
        wasm_tabletype_t* table = NULL;
        CHECK(wasm_tabletype_new(wasm_valtype_new(WASM_FUNCREF), &largest) == NULL);
        table = wasm_tabletype_new(wasm_valtype_new(WASM_FUNCREF), &unbounded);
        REQUIRE(table != NULL);
        CHECK(isoline_tabletype_limits64(table, &read) == NULL);
        CHECK(read.minimum == 1 && read.maximum_kind == ISOLINE_MAXIMUM_NONE);
        CHECK(isoline_tabletype_address_bits(table) == 32);
        wasm_tabletype_delete(table);
    }
}

ISOLINE_TEST_CASE(limitsMinimumAboveMaximumRefused)
{
    wasm_limits_t const inverted = { 2, 1 };
    isoline_limits64_t const inverted64 = c_test_limits(2, 1, ISOLINE_MAXIMUM_GIVEN);
    wasm_valtype_t* const element = wasm_valtype_new(WASM_FUNCREF);
    wasm_memorytype_t* memory = NULL;
    wasm_tabletype_t* table = NULL;
    CHECK(wasm_memorytype_new(&inverted) == NULL);
    CHECK(c_test_code_of(isoline_memorytype_new(&inverted64, 64, 16, &memory))
          == ISOLINE_ERROR_INVALID_ARGUMENT);
    CHECK(memory == NULL);
    /* An isoline_ function leaves its arguments untouched on an error: the element is still the caller's. */
    CHECK(c_test_code_of(isoline_tabletype_new(element, &inverted64, 32, &table))
          == ISOLINE_ERROR_INVALID_ARGUMENT);
    CHECK(table == NULL);
    CHECK(wasm_valtype_kind(element) == WASM_FUNCREF);
    /* wasm_tabletype_new() takes it all the same, and deletes it: the caller does not. */
    CHECK(wasm_tabletype_new(element, &inverted) == NULL);
}

ISOLINE_TEST_CASE(limitsPastTheAddressTypeRefused)
{
    /* A 32-bit memory of 64 KiB pages reaches 65536 of them, a 64-bit one 2^48; one of 1-byte pages reaches
     * 2^32 - 1 and 2^64 - 1. A size past that is refused, one at it made. */
    wasm_limits_t const tooMany = { 65537, wasm_limits_max_default };
    wasm_limits_t const allPages = { 65536, 65536 };
    wasm_limits_t const maximumPast = { 0, 65537 };
    isoline_limits64_t const past48 = c_test_limits(((uint64_t) 1 << 48) + 1, 0, ISOLINE_MAXIMUM_NONE);
    isoline_limits64_t const at48 = c_test_limits((uint64_t) 1 << 48, 0, ISOLINE_MAXIMUM_NONE);
    isoline_limits64_t const everyByte = c_test_limits(UINT64_MAX, UINT64_MAX, ISOLINE_MAXIMUM_GIVEN);
    isoline_limits64_t const past32 = c_test_limits((uint64_t) 1 << 32, 0, ISOLINE_MAXIMUM_NONE);
    wasm_memorytype_t* memory = wasm_memorytype_new(&allPages);
    wasm_tabletype_t* table = NULL;
    CHECK(memory != NULL);
    wasm_memorytype_delete(memory);
    memory = NULL;
    CHECK(wasm_memorytype_new(&tooMany) == NULL);
    CHECK(wasm_memorytype_new(&maximumPast) == NULL);
    CHECK(c_test_code_of(isoline_memorytype_new(&past48, 64, 16, &memory)) == ISOLINE_ERROR_INVALID_ARGUMENT);
    CHECK(isoline_memorytype_new(&at48, 64, 16, &memory) == NULL);
    wasm_memorytype_delete(memory);
    CHECK(isoline_memorytype_new(&everyByte, 64, 0, &memory) == NULL);
    CHECK(isoline_memorytype_page_size_log2(memory) == 0);
    wasm_memorytype_delete(memory);
    CHECK(c_test_code_of(isoline_memorytype_new(&everyByte, 32, 0, &memory))
          == ISOLINE_ERROR_INVALID_ARGUMENT);
    /* A 32-bit table holds at most 2^32 - 1 elements, a 64-bit one 2^64 - 1. */
    {
        wasm_valtype_t* const element = wasm_valtype_new(WASM_EXTERNREF);
        CHECK(c_test_code_of(isoline_tabletype_new(element, &past32, 32, &table))
              == ISOLINE_ERROR_INVALID_ARGUMENT);
        CHECK(isoline_tabletype_new(element, &past32, 64, &table) == NULL);
        CHECK(isoline_tabletype_address_bits(table) == 64);
        wasm_tabletype_delete(table);
    }
}

ISOLINE_TEST_CASE(limits64StructIsChecked)
{
    isoline_limits64_t const good = c_test_limits(1, 2, ISOLINE_MAXIMUM_GIVEN);
    isoline_limits64_t limits = good;
    wasm_memorytype_t* memory = NULL;
    /* Larger than this header's, as a later minor's caller passes it: its own fields must be zero. */
    struct
    {
        isoline_limits64_t limits;
        uint64_t later;
    } larger;
    /* The library's first published size is 32: below it, and above 4096, is refused. */
    size_t const sizes[] = { 0, 31, 4097 };
    /* The largest struct_size accepted, its bytes past this header's fields all zero. */
    static union {
        isoline_limits64_t limits;
        unsigned char bytes[4096];
    } largest;
    size_t index = 0;
    for (index = 0; index < sizeof sizes / sizeof sizes[0]; ++index)
    {
        limits = good;
        limits.struct_size = sizes[index];
        CHECK(c_test_code_of(isoline_memorytype_new(&limits, 32, 16, &memory))
              == ISOLINE_ERROR_INVALID_ARGUMENT);
    }
    limits = good;
    limits.reserved[6] = 1;
    CHECK(c_test_code_of(isoline_memorytype_new(&limits, 32, 16, &memory)) == ISOLINE_ERROR_INVALID_ARGUMENT);
    limits = good;
    limits.maximum_kind = 7;
    CHECK(c_test_code_of(isoline_memorytype_new(&limits, 32, 16, &memory)) == ISOLINE_ERROR_INVALID_ARGUMENT);
    /* An unknown maximum_kind is refused even with no maximum to go with it: it is never read as NONE. */
    limits = c_test_limits(1, 0, ISOLINE_MAXIMUM_NONE);
    limits.maximum_kind = 2;
    CHECK(c_test_code_of(isoline_memorytype_new(&limits, 32, 16, &memory)) == ISOLINE_ERROR_INVALID_ARGUMENT);
    limits = c_test_limits(1, 5, ISOLINE_MAXIMUM_NONE);
    CHECK(c_test_code_of(isoline_memorytype_new(&limits, 32, 16, &memory)) == ISOLINE_ERROR_INVALID_ARGUMENT);
    CHECK(c_test_code_of(isoline_memorytype_new(NULL, 32, 16, &memory)) == ISOLINE_ERROR_INVALID_ARGUMENT);
    CHECK(c_test_code_of(isoline_memorytype_new(&good, 32, 16, NULL)) == ISOLINE_ERROR_INVALID_ARGUMENT);
    CHECK(memory == NULL);

    memset(&larger, 0, sizeof larger);
    larger.limits = good;
    larger.limits.struct_size = sizeof larger;
    larger.later = 1;
    CHECK(c_test_code_of(isoline_memorytype_new(&larger.limits, 32, 16, &memory))
          == ISOLINE_ERROR_INVALID_ARGUMENT);
    larger.later = 0;
    REQUIRE(isoline_memorytype_new(&larger.limits, 32, 16, &memory) == NULL);
    wasm_memorytype_delete(memory);
    memory = NULL;
    CHECK(sizeof(isoline_limits64_t) == 32);
    memset(&largest, 0, sizeof largest);
    largest.limits = good;
    largest.limits.struct_size = sizeof largest;
    REQUIRE(isoline_memorytype_new(&largest.limits, 32, 16, &memory) == NULL);

    /* Written, the fields past this header's are zeroed; a struct_size refused writes nothing. */
    memset(&larger, 0xAB, sizeof larger);
    larger.limits.struct_size = sizeof larger;
    CHECK(isoline_memorytype_limits64(memory, &larger.limits) == NULL);
    CHECK(larger.limits.minimum == 1 && larger.limits.maximum == 2
          && larger.limits.maximum_kind == ISOLINE_MAXIMUM_GIVEN);
    CHECK(larger.later == 0);
    for (index = 0; index < sizeof larger.limits.reserved; ++index)
        CHECK(larger.limits.reserved[index] == 0);
    /* Written into a struct of the first published size, only that prefix is written: the bytes past
     * struct_size are the caller's. */
    memset(&larger, 0xAB, sizeof larger);
    larger.limits.struct_size = 32;
    CHECK(isoline_memorytype_limits64(memory, &larger.limits) == NULL);
    CHECK(larger.limits.minimum == 1 && larger.limits.maximum == 2
          && larger.limits.maximum_kind == ISOLINE_MAXIMUM_GIVEN);
    CHECK(larger.later == UINT64_C(0xABABABABABABABAB));
    limits = c_test_limits(9, 9, ISOLINE_MAXIMUM_GIVEN);
    limits.struct_size = 8;
    CHECK(c_test_code_of(isoline_memorytype_limits64(memory, &limits)) == ISOLINE_ERROR_INVALID_ARGUMENT);
    CHECK(limits.minimum == 9);
    CHECK(c_test_code_of(isoline_memorytype_limits64(memory, NULL)) == ISOLINE_ERROR_INVALID_ARGUMENT);
    wasm_memorytype_delete(memory);
}

ISOLINE_TEST_CASE(tableElementIsAReference)
{
    wasm_limits_t const limits = { 1, 10 };
    isoline_limits64_t const limits64 = c_test_limits(1, 10, ISOLINE_MAXIMUM_GIVEN);
    wasm_valtype_t* const number = wasm_valtype_new(WASM_I32);
    wasm_tabletype_t* table = NULL;
    isoline_error_t* error = NULL;
    error = isoline_tabletype_new(number, &limits64, 32, &table);
    CHECK(isoline_error_code(error) == ISOLINE_ERROR_INVALID_ARGUMENT);
    /* detail: api::InvalidTypeReason::NonReferenceElement, the fourth, plus one. */
    CHECK(isoline_error_detail(error) == 4);
    isoline_error_delete(error);
    CHECK(table == NULL);
    /* Refused by wasm.h's constructor, the element types are taken, and deleted. */
    CHECK(wasm_tabletype_new(number, &limits) == NULL);
    CHECK(wasm_tabletype_new(wasm_valtype_new(ISOLINE_VALKIND_V128), &limits) == NULL);
    CHECK(wasm_tabletype_new(wasm_valtype_new(WASM_FUNCREF), NULL) == NULL);

    table = wasm_tabletype_new(wasm_valtype_new(ISOLINE_VALKIND_EXNREF), &limits);
    REQUIRE(table != NULL);
    CHECK(wasm_valtype_kind(wasm_tabletype_element(table)) == ISOLINE_VALKIND_EXNREF);
    CHECK(wasm_tabletype_limits(table)->min == 1 && wasm_tabletype_limits(table)->max == 10);
    wasm_tabletype_delete(table);
    CHECK(wasm_tabletype_new(NULL, &limits) == NULL);
    {
        /* An index width there is none of, as for memories. */
        wasm_valtype_t* const element = wasm_valtype_new(WASM_FUNCREF);
        table = NULL;
        CHECK(c_test_code_of(isoline_tabletype_new(element, &limits64, 48, &table))
              == ISOLINE_ERROR_INVALID_ARGUMENT);
        CHECK(table == NULL);
        wasm_valtype_delete(element);
    }
    CHECK(c_test_code_of(isoline_tabletype_new(NULL, &limits64, 32, &table))
          == ISOLINE_ERROR_INVALID_ARGUMENT);
}

ISOLINE_TEST_CASE(table64RoundTrip)
{
    isoline_limits64_t const limits = c_test_limits((uint64_t) 1 << 40, UINT64_MAX, ISOLINE_MAXIMUM_GIVEN);
    isoline_limits64_t read = c_test_limits(0, 0, ISOLINE_MAXIMUM_NONE);
    wasm_tabletype_t* table = NULL;
    wasm_tabletype_t* copy = NULL;
    REQUIRE(isoline_tabletype_new(wasm_valtype_new(WASM_EXTERNREF), &limits, 64, &table) == NULL);
    copy = wasm_tabletype_copy(table);
    wasm_tabletype_delete(table);
    REQUIRE(copy != NULL);
    CHECK(isoline_tabletype_address_bits(copy) == 64);
    CHECK(isoline_tabletype_limits64(copy, &read) == NULL);
    CHECK(read.minimum == ((uint64_t) 1 << 40) && read.maximum == UINT64_MAX);
    CHECK(wasm_tabletype_limits(copy)->min == wasm_limits_max_default);
    CHECK(wasm_valtype_kind(wasm_tabletype_element(copy)) == WASM_EXTERNREF);
    wasm_tabletype_delete(copy);
}

ISOLINE_TEST_CASE(memoryPageSizeAndAddressBits)
{
    isoline_limits64_t const limits = c_test_limits(1, 0, ISOLINE_MAXIMUM_NONE);
    uint32_t const refusedPageSizes[] = { 1, 12, 15, 17, 64, 255, 256, UINT32_MAX };
    uint32_t const refusedBits[] = { 0, 16, 31, 33, 48, 128, UINT32_MAX };
    wasm_memorytype_t* memory = NULL;
    isoline_error_t* error = NULL;
    size_t index = 0;
    wasm_limits_t const pages = { 1, 2 };
    for (index = 0; index < sizeof refusedPageSizes / sizeof refusedPageSizes[0]; ++index)
    {
        error = isoline_memorytype_new(&limits, 32, refusedPageSizes[index], &memory);
        CHECK(isoline_error_code(error) == ISOLINE_ERROR_INVALID_ARGUMENT);
        /* detail: api::InvalidTypeReason::PageSize, the second, plus one. */
        CHECK(isoline_error_detail(error) == 2);
        isoline_error_delete(error);
    }
    for (index = 0; index < sizeof refusedBits / sizeof refusedBits[0]; ++index)
        CHECK(c_test_code_of(isoline_memorytype_new(&limits, refusedBits[index], 16, &memory))
              == ISOLINE_ERROR_INVALID_ARGUMENT);
    CHECK(memory == NULL);
    /* wasm.h's memory type is 32-bit, of 64 KiB pages. */
    memory = wasm_memorytype_new(&pages);
    REQUIRE(memory != NULL);
    CHECK(isoline_memorytype_address_bits(memory) == 32);
    CHECK(isoline_memorytype_page_size_log2(memory) == 16);
    CHECK(wasm_memorytype_limits(memory)->min == 1 && wasm_memorytype_limits(memory)->max == 2);
    wasm_memorytype_delete(memory);
}

ISOLINE_TEST_CASE(globaltypeRoundTrip)
{
    wasm_globaltype_t* type = NULL;
    wasm_globaltype_t* copy = NULL;
    /* A mutability no constant names is refused, never cast, and the content is taken and deleted. */
    CHECK(wasm_globaltype_new(wasm_valtype_new(WASM_F32), 2) == NULL);
    CHECK(wasm_globaltype_new(wasm_valtype_new(WASM_F32), ISOLINE_MUTABILITY_INVALID) == NULL);
    CHECK(wasm_globaltype_new(NULL, WASM_VAR) == NULL);
    type = wasm_globaltype_new(wasm_valtype_new(ISOLINE_VALKIND_V128), WASM_VAR);
    REQUIRE(type != NULL);
    copy = wasm_globaltype_copy(type);
    wasm_globaltype_delete(type);
    REQUIRE(copy != NULL);
    CHECK(wasm_globaltype_mutability(copy) == WASM_VAR);
    CHECK(wasm_valtype_kind(wasm_globaltype_content(copy)) == ISOLINE_VALKIND_V128);
    CHECK(wasm_externtype_kind(wasm_globaltype_as_externtype(copy)) == WASM_EXTERN_GLOBAL);
    wasm_globaltype_delete(copy);
    type = wasm_globaltype_new(wasm_valtype_new(WASM_I32), WASM_CONST);
    CHECK(wasm_globaltype_mutability(type) == WASM_CONST);
    wasm_globaltype_delete(type);
}

ISOLINE_TEST_CASE(externtypeCastsRefuseWrongKind)
{
    wasm_limits_t const limits = { 1, 2 };
    wasm_externtype_t* types[5];
    wasm_externkind_t const kinds[5] = {
        WASM_EXTERN_FUNC, WASM_EXTERN_GLOBAL, WASM_EXTERN_TABLE, WASM_EXTERN_MEMORY, WASM_EXTERN_TAG,
    };
    size_t index = 0;
    types[0] = wasm_functype_as_externtype(wasm_functype_new_0_0());
    types[1] = wasm_globaltype_as_externtype(wasm_globaltype_new(wasm_valtype_new(WASM_F32), WASM_CONST));
    types[2] = wasm_tabletype_as_externtype(wasm_tabletype_new(wasm_valtype_new(WASM_FUNCREF), &limits));
    types[3] = wasm_memorytype_as_externtype(wasm_memorytype_new(&limits));
    types[4] = wasm_tagtype_as_externtype(wasm_tagtype_new(wasm_functype_new_0_0()));
    for (index = 0; index < 5; ++index)
    {
        wasm_externtype_t* const type = types[index];
        wasm_externtype_t const* const constant = type;
        REQUIRE(type != NULL);
        CHECK(wasm_externtype_kind(type) == kinds[index]);
        /* Each cast gives the same object for its own kind, and NULL for every other. */
        CHECK((wasm_externtype_as_functype(type) != NULL) == (kinds[index] == WASM_EXTERN_FUNC));
        CHECK((wasm_externtype_as_globaltype(type) != NULL) == (kinds[index] == WASM_EXTERN_GLOBAL));
        CHECK((wasm_externtype_as_tabletype(type) != NULL) == (kinds[index] == WASM_EXTERN_TABLE));
        CHECK((wasm_externtype_as_memorytype(type) != NULL) == (kinds[index] == WASM_EXTERN_MEMORY));
        CHECK((wasm_externtype_as_tagtype(type) != NULL) == (kinds[index] == WASM_EXTERN_TAG));
        CHECK((wasm_externtype_as_functype_const(constant) != NULL) == (kinds[index] == WASM_EXTERN_FUNC));
        CHECK((wasm_externtype_as_globaltype_const(constant) != NULL)
              == (kinds[index] == WASM_EXTERN_GLOBAL));
        CHECK((wasm_externtype_as_tabletype_const(constant) != NULL) == (kinds[index] == WASM_EXTERN_TABLE));
        CHECK((wasm_externtype_as_memorytype_const(constant) != NULL)
              == (kinds[index] == WASM_EXTERN_MEMORY));
        CHECK((wasm_externtype_as_tagtype_const(constant) != NULL) == (kinds[index] == WASM_EXTERN_TAG));
    }
    /* A cast there and back is the same object. */
    CHECK(wasm_functype_as_externtype(wasm_externtype_as_functype(types[0])) == types[0]);
    CHECK(wasm_functype_as_externtype_const(wasm_externtype_as_functype_const(types[0])) == types[0]);
    CHECK(wasm_globaltype_as_externtype_const(wasm_externtype_as_globaltype(types[1])) == types[1]);
    CHECK(wasm_tabletype_as_externtype_const(wasm_externtype_as_tabletype(types[2])) == types[2]);
    CHECK(wasm_memorytype_as_externtype_const(wasm_externtype_as_memorytype(types[3])) == types[3]);
    CHECK(wasm_tagtype_as_externtype_const(wasm_externtype_as_tagtype(types[4])) == types[4]);
    /* A copy through the extern type is of the kind it was. */
    for (index = 0; index < 5; ++index)
    {
        wasm_externtype_t* const copy = wasm_externtype_copy(types[index]);
        wasm_externtype_delete(types[index]);
        CHECK(wasm_externtype_kind(copy) == kinds[index]);
        wasm_externtype_delete(copy);
    }
    CHECK(wasm_externtype_as_functype(NULL) == NULL && wasm_externtype_as_tagtype_const(NULL) == NULL);
    CHECK(wasm_functype_as_externtype(NULL) == NULL && wasm_memorytype_as_externtype_const(NULL) == NULL);
}

ISOLINE_TEST_CASE(tagtypeRoundTrip)
{
    wasm_tagtype_t* type = NULL;
    wasm_tagtype_t* copy = NULL;
    wasm_functype_t const* payload = NULL;
    /* A tag's function type returns nothing: one that returns a value is refused, and taken and deleted. */
    CHECK(wasm_tagtype_new(wasm_functype_new_0_1(wasm_valtype_new(WASM_I32))) == NULL);
    CHECK(wasm_tagtype_new(NULL) == NULL);

    type = wasm_tagtype_new(
        wasm_functype_new_2_0(wasm_valtype_new(WASM_I64), wasm_valtype_new(ISOLINE_VALKIND_EXNREF)));
    REQUIRE(type != NULL);
    copy = wasm_tagtype_copy(type);
    wasm_tagtype_delete(type);
    REQUIRE(copy != NULL);
    payload = wasm_tagtype_functype(copy);
    REQUIRE(payload != NULL && wasm_functype_params(payload)->size == 2);
    CHECK(wasm_valtype_kind(wasm_functype_params(payload)->data[0]) == WASM_I64);
    CHECK(wasm_valtype_kind(wasm_functype_params(payload)->data[1]) == ISOLINE_VALKIND_EXNREF);
    CHECK(wasm_functype_results(payload)->size == 0);
    CHECK(wasm_externtype_kind(wasm_tagtype_as_externtype_const(copy)) == WASM_EXTERN_TAG);
    wasm_tagtype_delete(copy);
}

ISOLINE_TEST_CASE(importAndExportTypes)
{
    wasm_name_t module;
    wasm_name_t name;
    wasm_name_t lying = { SIZE_MAX, NULL };
    wasm_importtype_t* import = NULL;
    wasm_importtype_t* importCopy = NULL;
    wasm_exporttype_t* exported = NULL;
    wasm_exporttype_t* exportCopy = NULL;
    wasm_byte_t text = 'x';
    lying.data = &text;
    /* Refused, the constructors take and delete what they were given: names (left empty) and types. A name
     * they cannot walk, of a size no vector has, they leave untouched. */
    wasm_name_new_from_string(&name, "f");
    CHECK(wasm_importtype_new(NULL, &name, c_test_func_externtype()) == NULL);
    CHECK(name.size == 0 && name.data == NULL);
    wasm_name_new_from_string(&module, "env");
    wasm_name_new_from_string(&name, "f");
    CHECK(wasm_importtype_new(&module, &name, NULL) == NULL);
    CHECK(module.size == 0 && name.size == 0);
    wasm_name_new_from_string(&module, "env");
    CHECK(wasm_importtype_new(&module, &lying, c_test_func_externtype()) == NULL);
    CHECK(module.size == 0 && lying.size == SIZE_MAX && lying.data == &text);
    wasm_name_new_from_string(&name, "f");
    CHECK(wasm_importtype_new(&lying, &name, c_test_func_externtype()) == NULL);
    CHECK(name.size == 0 && lying.size == SIZE_MAX);
    CHECK(wasm_exporttype_new(&lying, c_test_func_externtype()) == NULL);
    CHECK(wasm_exporttype_new(NULL, c_test_func_externtype()) == NULL);
    wasm_name_new_from_string(&name, "f");
    CHECK(wasm_exporttype_new(&name, NULL) == NULL);
    CHECK(name.size == 0);

    wasm_name_new_from_string(&module, "env");
    wasm_name_new_from_string(&name, "f");
    import = wasm_importtype_new(&module, &name, c_test_func_externtype());
    REQUIRE(import != NULL);
    CHECK(module.size == 0 && module.data == NULL && name.size == 0 && name.data == NULL);
    importCopy = wasm_importtype_copy(import);
    wasm_importtype_delete(import);
    REQUIRE(importCopy != NULL);
    CHECK(wasm_importtype_module(importCopy)->size == 3
          && memcmp(wasm_importtype_module(importCopy)->data, "env", 3) == 0);
    CHECK(wasm_importtype_name(importCopy)->size == 1 && wasm_importtype_name(importCopy)->data[0] == 'f');
    CHECK(wasm_externtype_kind(wasm_importtype_type(importCopy)) == WASM_EXTERN_FUNC);

    wasm_name_new_from_string(&name, "memory");
    exported = wasm_exporttype_new(&name, wasm_externtype_copy(wasm_importtype_type(importCopy)));
    REQUIRE(exported != NULL);
    exportCopy = wasm_exporttype_copy(exported);
    wasm_exporttype_delete(exported);
    REQUIRE(exportCopy != NULL);
    CHECK(wasm_exporttype_name(exportCopy)->size == 6);
    CHECK(wasm_externtype_kind(wasm_exporttype_type(exportCopy)) == WASM_EXTERN_FUNC);
    wasm_exporttype_delete(exportCopy);
    wasm_importtype_delete(importCopy);
}

ISOLINE_TEST_CASE(inlineHelpersLeakNothing)
{
    /* wasm.h's inline helpers pass their arguments, temporaries, straight on: wasm_functype_new_1_1() makes
     * two vectors of them and gives both to wasm_functype_new(). When one argument is NULL
     * (wasm_valtype_new() of a kind there is none of) the function type is refused, and everything it was
     * given is deleted, the other valtype and both vectors: LeakSanitizer holds this program to it. */
    wasm_limits_t const limits = { 1, 2 };
    CHECK(wasm_functype_new_1_1(wasm_valtype_new(WASM_I32), wasm_valtype_new(77)) == NULL);
    CHECK(wasm_functype_new_2_1(
              wasm_valtype_new(77), wasm_valtype_new(WASM_F64), wasm_valtype_new(WASM_FUNCREF))
          == NULL);
    CHECK(wasm_functype_new_3_0(wasm_valtype_new(WASM_I32), wasm_valtype_new(ISOLINE_VALKIND_V128), NULL)
          == NULL);
    CHECK(wasm_functype_new_0_2(wasm_valtype_new(ISOLINE_VALKIND_EXNREF), wasm_valtype_new(200)) == NULL);
    /* And the refusals of the other constructors, whose arguments are temporaries too. */
    CHECK(wasm_globaltype_new(wasm_valtype_new(WASM_I32), 9) == NULL);
    CHECK(wasm_tabletype_new(wasm_valtype_new(WASM_I32), &limits) == NULL);
    CHECK(wasm_tagtype_new(wasm_functype_new_0_1(wasm_valtype_new(WASM_I32))) == NULL);
    CHECK(wasm_exporttype_new(NULL, c_test_func_externtype()) == NULL);
    CHECK(wasm_functype_as_externtype(wasm_functype_new_1_1(wasm_valtype_new(77), wasm_valtype_new(WASM_I32)))
          == NULL);
}

ISOLINE_TEST_CASE(nullTypesAreNamed)
{
    /* Every accessor given NULL gives NULL or the value val.h names for it; delete and copy of NULL do
     * nothing. */
    isoline_limits64_t limits = c_test_limits(0, 0, ISOLINE_MAXIMUM_NONE);
    CHECK(wasm_valtype_kind(NULL) == ISOLINE_VALKIND_INVALID);
    CHECK(wasm_valtype_copy(NULL) == NULL);
    CHECK(isoline_valtype_heap(NULL) == ISOLINE_HEAP_NOT_REFERENCE);
    CHECK(isoline_valtype_nullability(NULL) == ISOLINE_NOT_NULLABLE);
    CHECK(isoline_valtype_concrete_index(NULL) == UINT32_MAX);
    CHECK(wasm_functype_params(NULL) == NULL && wasm_functype_results(NULL) == NULL);
    CHECK(wasm_globaltype_content(NULL) == NULL);
    CHECK(wasm_globaltype_mutability(NULL) == ISOLINE_MUTABILITY_INVALID);
    CHECK(wasm_tabletype_element(NULL) == NULL && wasm_tabletype_limits(NULL) == NULL);
    CHECK(wasm_memorytype_limits(NULL) == NULL && wasm_memorytype_new(NULL) == NULL);
    CHECK(isoline_memorytype_page_size_log2(NULL) == UINT32_MAX);
    CHECK(isoline_memorytype_address_bits(NULL) == 0 && isoline_tabletype_address_bits(NULL) == 0);
    CHECK(c_test_code_of(isoline_memorytype_limits64(NULL, &limits)) == ISOLINE_ERROR_INVALID_ARGUMENT);
    CHECK(c_test_code_of(isoline_tabletype_limits64(NULL, &limits)) == ISOLINE_ERROR_INVALID_ARGUMENT);
    CHECK(wasm_tagtype_functype(NULL) == NULL);
    CHECK(wasm_externtype_kind(NULL) == ISOLINE_EXTERNKIND_INVALID);
    CHECK(wasm_importtype_module(NULL) == NULL && wasm_importtype_name(NULL) == NULL);
    CHECK(wasm_importtype_type(NULL) == NULL);
    CHECK(wasm_exporttype_name(NULL) == NULL && wasm_exporttype_type(NULL) == NULL);
    CHECK(wasm_functype_copy(NULL) == NULL && wasm_externtype_copy(NULL) == NULL);
    CHECK(wasm_importtype_copy(NULL) == NULL && wasm_exporttype_copy(NULL) == NULL);
    wasm_valtype_delete(NULL);
    wasm_functype_delete(NULL);
    wasm_globaltype_delete(NULL);
    wasm_tabletype_delete(NULL);
    wasm_memorytype_delete(NULL);
    wasm_tagtype_delete(NULL);
    wasm_externtype_delete(NULL);
    wasm_importtype_delete(NULL);
    wasm_exporttype_delete(NULL);
}

int main(int argc, char** argv)
{
    return c_test_run_all(argc, argv);
}
