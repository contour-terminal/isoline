/* SPDX-License-Identifier: Apache-2.0 */
/* The ABI layout rows (src/isoline/c/abi/layout.def), held to what this C compiler lays out. Their C++
   twin is src/isoline/c/abi/Layout_test.cpp. */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <isoline/c/isoline.h>

#define C_TEST_SUITE    "layout"
#define C_TEST_CASES(X) X(rowsMatch) X(noImplicitPadding) X(boolIsOneByte)
#include "IsolineCTest.h"

/* layout.rowsMatch: every row is a compile-time assertion, so a row the headers disagree with is a
   translation unit that does not compile, and the case below only records that it did. */
#define ISOLINE_LAYOUT_SCALAR(type, size) _Static_assert(sizeof(type) == (size), #type ": the size row");
#define ISOLINE_LAYOUT_STRUCT(type, size, align)                    \
    _Static_assert(sizeof(type) == (size), #type ": the size row"); \
    _Static_assert(_Alignof(type) == (align), #type ": the alignment row");
#define ISOLINE_LAYOUT_FIELD(type, field, offset, size)                                     \
    _Static_assert(offsetof(type, field) == (offset), #type "." #field ": the offset row"); \
    _Static_assert(sizeof(((type*) 0)->field) == (size), #type "." #field ": the size row");
#define ISOLINE_LAYOUT_UPSTREAM_PADDING(type, offset, size)
#include <isoline/c/abi/layout.def>
#undef ISOLINE_LAYOUT_SCALAR
#undef ISOLINE_LAYOUT_STRUCT
#undef ISOLINE_LAYOUT_FIELD
#undef ISOLINE_LAYOUT_UPSTREAM_PADDING

/** A struct row: the struct's name and size. */
struct layout_struct_row
{
    char const* type;
    size_t size;
};

/** A field row or an upstream padding row: the struct it belongs to, its offset and its size. */
struct layout_part_row
{
    char const* type;
    size_t offset;
    size_t size;
    int isUpstreamPadding;
};

#define ISOLINE_LAYOUT_SCALAR(type, size)
#define ISOLINE_LAYOUT_STRUCT(type, size, align) { #type, (size) },
#define ISOLINE_LAYOUT_FIELD(type, field, offset, size)
#define ISOLINE_LAYOUT_UPSTREAM_PADDING(type, offset, size)
static struct layout_struct_row const layoutStructs[] = {
#include <isoline/c/abi/layout.def>
};
#undef ISOLINE_LAYOUT_SCALAR
#undef ISOLINE_LAYOUT_STRUCT
#undef ISOLINE_LAYOUT_FIELD
#undef ISOLINE_LAYOUT_UPSTREAM_PADDING

#define ISOLINE_LAYOUT_SCALAR(type, size)
#define ISOLINE_LAYOUT_STRUCT(type, size, align)
#define ISOLINE_LAYOUT_FIELD(type, field, offset, size)     { #type, (offset), (size), 0 },
#define ISOLINE_LAYOUT_UPSTREAM_PADDING(type, offset, size) { #type, (offset), (size), 1 },
static struct layout_part_row const layoutParts[] = {
#include <isoline/c/abi/layout.def>
};
#undef ISOLINE_LAYOUT_SCALAR
#undef ISOLINE_LAYOUT_STRUCT
#undef ISOLINE_LAYOUT_FIELD
#undef ISOLINE_LAYOUT_UPSTREAM_PADDING

static size_t const layoutStructCount = sizeof layoutStructs / sizeof layoutStructs[0];
static size_t const layoutPartCount = sizeof layoutParts / sizeof layoutParts[0];

ISOLINE_TEST_CASE(rowsMatch)
{
    /* The assertions above held, or this would not have compiled. */
    CHECK(layoutStructCount > 0);
}

ISOLINE_TEST_CASE(noImplicitPadding)
{
    size_t structIndex = 0;
    size_t partIndex = 0;
    for (structIndex = 0; structIndex < layoutStructCount; ++structIndex)
    {
        /* Its rows tile it: each starts where the one before it ended, and the last ends at its size. */
        struct layout_struct_row const* const row = &layoutStructs[structIndex];
        size_t end = 0;
        for (partIndex = 0; partIndex < layoutPartCount; ++partIndex)
        {
            struct layout_part_row const* const part = &layoutParts[partIndex];
            if (strcmp(part->type, row->type) != 0)
                continue;
            if (part->offset != end)
                fprintf(stderr,
                        "%s: a row starts at %zu, where the rows before it end at %zu\n",
                        row->type,
                        part->offset,
                        end);
            CHECK(part->offset == end);
            end = part->offset + part->size;
        }
        if (end != row->size)
            fprintf(stderr, "%s: its rows end at %zu of its %zu bytes\n", row->type, end, row->size);
        CHECK(end == row->size);
    }
    for (partIndex = 0; partIndex < layoutPartCount; ++partIndex)
    {
        struct layout_part_row const* const part = &layoutParts[partIndex];
        int named = 0;
        for (structIndex = 0; structIndex < layoutStructCount; ++structIndex)
            named |= strcmp(layoutStructs[structIndex].type, part->type) == 0;
        /* Every field or padding row belongs to a struct row, and only wasm.h's structs have padding. */
        CHECK(named);
        if (part->isUpstreamPadding)
            CHECK(strncmp(part->type, "wasm_", 5) == 0);
    }
}

ISOLINE_TEST_CASE(boolIsOneByte)
{
    /* abi.h asserts both at compile time too; a binding's c_bool, C.bool or bool relies on them. */
    CHECK(sizeof(bool) == 1);
    CHECK(sizeof(wasm_valkind_t) == 1);
}

int main(int argc, char** argv)
{
    return c_test_run_all(argc, argv);
}
