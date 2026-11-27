/* SPDX-License-Identifier: Apache-2.0 */
/* Vectors and names as a C program sees them (wasm.h's WASM_DECLARE_VEC families), through the shared
 * library: sizes a host can pass that no vector can have, allocations that fail, vectors whose data is
 * missing, and vectors deleted twice. Every refusal ends in the empty vector, { 0, NULL }, and never in an
 * abort.
 *
 * Labelled memory and run under the build's cap (tests/c/CMakeLists.txt): a size check that is lost would
 * otherwise ask the host for exabytes. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <isoline/c/isoline.h>

#define C_TEST_SUITE "vectors"
/* The cases in groups that each fit a line. */
#define C_TEST_SIZE_CASES(X)   X(overflowingSizeIsEmpty) X(allocationFailureIsEmpty) X(newEmptyIsEmpty)
#define C_TEST_MAKE_CASES(X)   X(uninitializedIsZeroed) X(newCopiesTheElements) X(newWithNullDataIsEmpty)
#define C_TEST_COPY_CASES(X)   X(copyIsElementwise) X(copyIsDeep) X(copyOfNullDataIsEmpty)
#define C_TEST_DELETE_CASES(X) X(deleteNullDataIsSafe) X(deleteTwiceIsSafe) X(nullVectorIsSafe)
#define C_TEST_CASES(X)  \
    C_TEST_SIZE_CASES(X) \
    C_TEST_MAKE_CASES(X) C_TEST_COPY_CASES(X) C_TEST_DELETE_CASES(X) X(namesAreByteVectors)
#include "IsolineCTest.h"

/* A vector the library has not written: what a refusal must overwrite. */
#define C_TEST_GARBAGE ((void*) (uintptr_t) 0x1)

ISOLINE_TEST_CASE(overflowingSizeIsEmpty)
{
    wasm_byte_vec_t bytes = { 7, (wasm_byte_t*) C_TEST_GARBAGE };
    wasm_val_vec_t values = { 7, (wasm_val_t*) C_TEST_GARBAGE };
    wasm_valtype_vec_t types = { 7, (wasm_valtype_t**) C_TEST_GARBAGE };

    /* SIZE_MAX bytes, and SIZE_MAX / 8 + 1 values of 16 bytes each, whose byte count wraps to 0. */
    wasm_byte_vec_new_uninitialized(&bytes, SIZE_MAX);
    CHECK(bytes.size == 0 && bytes.data == NULL);
    wasm_val_vec_new_uninitialized(&values, SIZE_MAX / 8 + 1);
    CHECK(values.size == 0 && values.data == NULL);
    wasm_valtype_vec_new_uninitialized(&types, SIZE_MAX / sizeof(wasm_valtype_t*) + 1);
    CHECK(types.size == 0 && types.data == NULL);

    /* The same sizes given with data: nothing of the data is read, since no array of that size exists. The
     * pointer is read through a volatile, so the compiler cannot see it is no array and warn of the read that
     * must not happen. */
    values.size = 7;
    values.data = (wasm_val_t*) C_TEST_GARBAGE;
    {
        wasm_val_t const* volatile garbage = (wasm_val_t const*) C_TEST_GARBAGE;
        wasm_val_vec_new(&values, SIZE_MAX / 8 + 1, garbage);
    }
    CHECK(values.size == 0 && values.data == NULL);
    {
        wasm_val_vec_t const lying = { SIZE_MAX, (wasm_val_t*) C_TEST_GARBAGE };
        wasm_val_vec_copy(&values, &lying);
        CHECK(values.size == 0 && values.data == NULL);
    }
}

ISOLINE_TEST_CASE(allocationFailureIsEmpty)
{
    /* PTRDIFF_MAX bytes overflow nothing, and no host has them: the allocation fails, and the vector is
     * empty. */
    wasm_byte_vec_t bytes = { 7, (wasm_byte_t*) C_TEST_GARBAGE };
    wasm_byte_vec_new_uninitialized(&bytes, (size_t) PTRDIFF_MAX);
    CHECK(bytes.size == 0 && bytes.data == NULL);
    /* The process carries on, and a vector that fits is made as usual. */
    wasm_byte_vec_new_uninitialized(&bytes, 3);
    CHECK(bytes.size == 3 && bytes.data != NULL);
    wasm_byte_vec_delete(&bytes);
}

ISOLINE_TEST_CASE(newEmptyIsEmpty)
{
    wasm_extern_vec_t externs = { 7, (wasm_extern_t**) C_TEST_GARBAGE };
    wasm_extern_vec_new_empty(&externs);
    CHECK(externs.size == 0 && externs.data == NULL);
    wasm_extern_vec_delete(&externs);
    /* Size 0 is the empty vector however it is asked for. */
    wasm_extern_vec_new_uninitialized(&externs, 0);
    CHECK(externs.size == 0 && externs.data == NULL);
}

ISOLINE_TEST_CASE(uninitializedIsZeroed)
{
    /* An uninitialized vector of pointers holds NULLs, and one of values holds zeros, which own nothing: the
     * vector can be deleted before the caller has filled it. */
    size_t index = 0;
    wasm_functype_vec_t types;
    wasm_val_vec_t values;
    wasm_byte_vec_t bytes;
    wasm_functype_vec_new_uninitialized(&types, 4);
    REQUIRE(types.size == 4 && types.data != NULL);
    for (index = 0; index < types.size; ++index)
        CHECK(types.data[index] == NULL);
    wasm_functype_vec_delete(&types);

    wasm_val_vec_new_uninitialized(&values, 4);
    REQUIRE(values.size == 4 && values.data != NULL);
    for (index = 0; index < values.size; ++index)
        CHECK(values.data[index].kind == WASM_I32 && values.data[index].of.i64 == 0);
    wasm_val_vec_delete(&values);

    wasm_byte_vec_new_uninitialized(&bytes, 4);
    REQUIRE(bytes.size == 4 && bytes.data != NULL);
    for (index = 0; index < bytes.size; ++index)
        CHECK(bytes.data[index] == 0);
    wasm_byte_vec_delete(&bytes);
}

ISOLINE_TEST_CASE(newCopiesTheElements)
{
    wasm_val_t source[2];
    wasm_val_vec_t values;
    source[0].kind = WASM_I64;
    source[0].of.i64 = -5;
    source[1].kind = WASM_F64;
    source[1].of.f64 = 2.5;
    wasm_val_vec_new(&values, 2, source);
    REQUIRE(values.size == 2 && values.data != NULL);
    /* The vector holds its own array: the caller's may change or go. */
    CHECK(values.data != source);
    source[0].of.i64 = 9;
    CHECK(values.data[0].kind == WASM_I64 && values.data[0].of.i64 == -5);
    CHECK(values.data[1].kind == WASM_F64 && values.data[1].of.f64 == 2.5);
    wasm_val_vec_delete(&values);
}

ISOLINE_TEST_CASE(newWithNullDataIsEmpty)
{
    /* A size with no data to go with it is refused, never read. */
    wasm_byte_vec_t bytes = { 7, (wasm_byte_t*) C_TEST_GARBAGE };
    wasm_byte_vec_new(&bytes, 5, NULL);
    CHECK(bytes.size == 0 && bytes.data == NULL);
    /* Size 0 needs no data. */
    wasm_byte_vec_new(&bytes, 0, NULL);
    CHECK(bytes.size == 0 && bytes.data == NULL);
}

ISOLINE_TEST_CASE(copyIsElementwise)
{
    wasm_byte_t const text[] = { 'w', 'a', '\0', 's', 'm' };
    wasm_byte_vec_t original;
    wasm_byte_vec_t copy;
    wasm_byte_vec_new(&original, sizeof text, text);
    wasm_byte_vec_copy(&copy, &original);
    REQUIRE(copy.size == sizeof text && copy.data != NULL);
    CHECK(copy.data != original.data);
    /* The copy survives its original. */
    wasm_byte_vec_delete(&original);
    CHECK(memcmp(copy.data, text, sizeof text) == 0);
    wasm_byte_vec_delete(&copy);

    /* An empty vector copies to the empty vector. */
    wasm_byte_vec_new_empty(&original);
    wasm_byte_vec_copy(&copy, &original);
    CHECK(copy.size == 0 && copy.data == NULL);
}

ISOLINE_TEST_CASE(copyIsDeep)
{
    /* A vector of own pointers copies its elements too: the copy's are its own, and outlive the original's. A
     * shallow copy would leave the copy's pointers dangling, which the sanitizers report. */
    wasm_valtype_vec_t original;
    wasm_valtype_vec_t copy;
    wasm_valtype_vec_new_uninitialized(&original, 3);
    REQUIRE(original.size == 3);
    original.data[0] = wasm_valtype_new(WASM_F64);
    original.data[1] = NULL;
    original.data[2] = wasm_valtype_new(ISOLINE_VALKIND_EXNREF);
    wasm_valtype_vec_copy(&copy, &original);
    REQUIRE(copy.size == 3 && copy.data != NULL);
    CHECK(copy.data[0] != original.data[0] && copy.data[2] != original.data[2]);
    /* A slot not yet filled is copied as one. */
    CHECK(copy.data[1] == NULL);
    wasm_valtype_vec_delete(&original);
    CHECK(wasm_valtype_kind(copy.data[0]) == WASM_F64);
    CHECK(wasm_valtype_kind(copy.data[2]) == ISOLINE_VALKIND_EXNREF);
    wasm_valtype_vec_delete(&copy);
}

ISOLINE_TEST_CASE(copyOfNullDataIsEmpty)
{
    /* A vector that claims elements and has no data (a zeroed struct given a size, or a C binding's mistake)
     * copies to the empty vector, and nothing is read. */
    wasm_valtype_vec_t const broken = { 3, NULL };
    wasm_valtype_vec_t copy = { 7, (wasm_valtype_t**) C_TEST_GARBAGE };
    wasm_valtype_vec_copy(&copy, &broken);
    CHECK(copy.size == 0 && copy.data == NULL);
    /* Copying from NULL is the same. */
    copy.size = 7;
    wasm_valtype_vec_copy(&copy, NULL);
    CHECK(copy.size == 0 && copy.data == NULL);
}

ISOLINE_TEST_CASE(deleteNullDataIsSafe)
{
    /* Nothing to free and no element to delete: a no-op that leaves the empty vector behind. */
    wasm_exporttype_vec_t zeroed = { 0, NULL };
    wasm_exporttype_vec_t sized = { 3, NULL };
    wasm_val_vec_t values = { 2, NULL };
    wasm_exporttype_vec_delete(&zeroed);
    CHECK(zeroed.size == 0 && zeroed.data == NULL);
    wasm_exporttype_vec_delete(&sized);
    CHECK(sized.size == 0 && sized.data == NULL);
    wasm_val_vec_delete(&values);
    CHECK(values.size == 0 && values.data == NULL);
}

ISOLINE_TEST_CASE(deleteTwiceIsSafe)
{
    /* Delete leaves the empty vector behind, so deleting again frees nothing twice. */
    wasm_importtype_vec_t types;
    wasm_importtype_vec_new_uninitialized(&types, 2);
    REQUIRE(types.size == 2);
    wasm_importtype_vec_delete(&types);
    CHECK(types.size == 0 && types.data == NULL);
    wasm_importtype_vec_delete(&types);
    CHECK(types.size == 0 && types.data == NULL);
}

ISOLINE_TEST_CASE(nullVectorIsSafe)
{
    /* Where no vector is given there is nothing to write, and nothing happens. */
    wasm_byte_t byte = 1;
    wasm_byte_vec_t source = { 1, NULL };
    source.data = &byte;
    wasm_byte_vec_new_empty(NULL);
    wasm_byte_vec_new_uninitialized(NULL, 3);
    wasm_byte_vec_new(NULL, 1, &byte);
    wasm_byte_vec_copy(NULL, &source);
    wasm_byte_vec_delete(NULL);
    wasm_frame_vec_delete(NULL);
    CHECK(source.size == 1 && source.data == &byte);
}

ISOLINE_TEST_CASE(namesAreByteVectors)
{
    /* wasm_name_t is wasm.h's byte vector, its functions the byte vector's: a name is its bytes, with no
     * terminator unless one is asked for. */
    wasm_name_t name;
    wasm_name_t terminated;
    wasm_name_t copy;
    wasm_name_new_from_string(&name, "memory");
    CHECK(name.size == 6 && memcmp(name.data, "memory", 6) == 0);
    wasm_name_new_from_string_nt(&terminated, "memory");
    CHECK(terminated.size == 7 && terminated.data[6] == '\0');
    wasm_name_copy(&copy, &name);
    wasm_name_delete(&name);
    CHECK(copy.size == 6 && memcmp(copy.data, "memory", 6) == 0);
    wasm_name_delete(&copy);
    wasm_name_delete(&terminated);
    wasm_name_new_empty(&name);
    CHECK(name.size == 0 && name.data == NULL);
}

int main(int argc, char** argv)
{
    return c_test_run_all(argc, argv);
}
