// SPDX-License-Identifier: Apache-2.0
//
// The ABI layout rows (abi/layout.def), held to what this C++ compiler lays out: a C++ caller of the C
// API sees the structs as a C caller does (tests/c/layout_test.c is the C twin). Each check is a
// static_assert, so a row the headers disagree with does not compile.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <utility>

#include <isoline/c/isoline.h>

namespace
{

// layout.rowsMatch: each row against the compiler.
#define ISOLINE_LAYOUT_SCALAR(type, size) static_assert(sizeof(type) == (size), #type ": the size row");
#define ISOLINE_LAYOUT_STRUCT(type, size, align)                   \
    static_assert(sizeof(type) == (size), #type ": the size row"); \
    static_assert(alignof(type) == (align), #type ": the alignment row");
#define ISOLINE_LAYOUT_FIELD(type, field, offset, size)                                    \
    static_assert(offsetof(type, field) == (offset), #type "." #field ": the offset row"); \
    static_assert(sizeof(decltype(type::field)) == (size), #type "." #field ": the size row");
#define ISOLINE_LAYOUT_UPSTREAM_PADDING(type, offset, size)
#define ISOLINE_LAYOUT_FIELD_TYPE(type, field, fieldType) \
    static_assert(std::is_same_v<decltype(type::field), fieldType>, #type "." #field ": the type row");
#define ISOLINE_LAYOUT_UNION_MEMBER_TYPE(type, field, member, memberType)                   \
    static_assert(std::is_same_v<decltype(std::declval<type&>().field.member), memberType>, \
                  #type "." #field "." #member ": the type row");
#include <isoline/c/abi/layout.def>
#undef ISOLINE_LAYOUT_SCALAR
#undef ISOLINE_LAYOUT_STRUCT
#undef ISOLINE_LAYOUT_FIELD
#undef ISOLINE_LAYOUT_UPSTREAM_PADDING
#undef ISOLINE_LAYOUT_FIELD_TYPE
#undef ISOLINE_LAYOUT_UNION_MEMBER_TYPE

/// A struct row: the struct's name and size.
struct StructRow
{
    std::string_view type;
    std::size_t size;
};

/// What a part of a struct is: one of its fields, or padding wasm.h's layout has.
enum class PartKind : std::uint8_t
{
    Field,
    UpstreamPadding,
};

/// A field row or an upstream padding row: the struct it belongs to, its offset, its size and which it is.
struct PartRow
{
    std::string_view type;
    std::size_t offset;
    std::size_t size;
    PartKind kind;
};

constexpr auto StructRows = std::to_array<StructRow>({
#define ISOLINE_LAYOUT_SCALAR(type, size)
#define ISOLINE_LAYOUT_STRUCT(name, bytes, alignment) { .type = #name, .size = (bytes) },
#define ISOLINE_LAYOUT_FIELD(type, field, offset, size)
#define ISOLINE_LAYOUT_UPSTREAM_PADDING(type, offset, size)
#include <isoline/c/abi/layout.def>
#undef ISOLINE_LAYOUT_SCALAR
#undef ISOLINE_LAYOUT_STRUCT
#undef ISOLINE_LAYOUT_FIELD
#undef ISOLINE_LAYOUT_UPSTREAM_PADDING
});

constexpr auto PartRows = std::to_array<PartRow>({
#define ISOLINE_LAYOUT_SCALAR(type, size)
#define ISOLINE_LAYOUT_STRUCT(type, size, align)
#define ISOLINE_LAYOUT_FIELD(name, field, at, bytes) \
    { .type = #name, .offset = (at), .size = (bytes), .kind = PartKind::Field },
#define ISOLINE_LAYOUT_UPSTREAM_PADDING(name, at, bytes) \
    { .type = #name, .offset = (at), .size = (bytes), .kind = PartKind::UpstreamPadding },
#include <isoline/c/abi/layout.def>
#undef ISOLINE_LAYOUT_SCALAR
#undef ISOLINE_LAYOUT_STRUCT
#undef ISOLINE_LAYOUT_FIELD
#undef ISOLINE_LAYOUT_UPSTREAM_PADDING
});

/// Whether the field and padding rows of @p row tile it: each starts where the one before it ends, the
/// first at 0, and the last ends at its size.
[[nodiscard]] constexpr auto partsTile(StructRow const& row) noexcept -> bool
{
    auto end = std::size_t { 0 };
    for (auto const& part: PartRows)
    {
        if (part.type != row.type)
            continue;
        if (part.offset != end)
            return false;
        end = part.offset + part.size;
    }
    return end == row.size;
}

/// Whether every struct's rows tile all of it, tail included: no padding the rows do not name.
[[nodiscard]] constexpr auto noStructHasImplicitPadding() noexcept -> bool
{
    return std::ranges::all_of(StructRows, partsTile);
}

/// Whether every part row belongs to a struct row, and only wasm.h's structs have padding rows.
[[nodiscard]] constexpr auto partRowsAreWellFormed() noexcept -> bool
{
    return std::ranges::all_of(PartRows, [](PartRow const& part) {
        auto const named =
            std::ranges::any_of(StructRows, [&](StructRow const& row) { return row.type == part.type; });
        return named && (part.kind == PartKind::Field || part.type.starts_with("wasm_"));
    });
}

static_assert(noStructHasImplicitPadding(), "layout.def: a struct has padding no row names");
static_assert(partRowsAreWellFormed(), "layout.def: a part row names no struct, or pads an isoline_ struct");

} // namespace

TEST_CASE("layout.rowsMatch", "[c-api]")
{
    // The assertions above held, or this would not have compiled.
    CHECK_FALSE(StructRows.empty());
}

TEST_CASE("layout.noImplicitPadding", "[c-api]")
{
    // Asserted above at compile time; a failing row stops the build there.
    CHECK(noStructHasImplicitPadding());
    CHECK(partRowsAreWellFormed());
}

TEST_CASE("layout.boolIsOneByte", "[c-api]")
{
    // abi.h asserts both at compile time too; a binding's c_bool, C.bool or bool relies on them.
    CHECK(sizeof(bool) == 1);
    CHECK(sizeof(wasm_valkind_t) == 1);
}
