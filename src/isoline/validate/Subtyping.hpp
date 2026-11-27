// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Assert.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <isoline/binary/Types.hpp>
#include <isoline/meta/EnumRows.hpp>

namespace isoline::validate
{

/// Where an abstract heap type stands in its hierarchy.
enum class HeapRank : uint8_t
{
    Top,      ///< The hierarchy's top: every heap of the hierarchy is a subtype of it.
    Defined,  ///< A concrete type: a subtype of the top, and of an equivalent concrete type.
    Bottom,   ///< The hierarchy's bottom: a subtype of every heap of the hierarchy.
    BelowAll, ///< Below every hierarchy (bot): a subtype of every heap of every hierarchy.
};

/// The last HeapRank enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastHeapRank = HeapRank::BelowAll;

/// One row of HeapTable: an abstract heap type, the top of its hierarchy, and its rank there.
struct HeapRow
{
    binary::AbstractHeapType kind; ///< The heap type; equal to the row's index.
    binary::AbstractHeapType top;  ///< The top of the hierarchy it belongs to.
    HeapRank rank;                 ///< Where it stands in that hierarchy.
};

/// The heap hierarchies without GC, one row per AbstractHeapType in enumerator order: func with the
/// concrete (function) types beneath it and nofunc at the bottom, extern over noextern, exn over
/// noexn; and bot beneath them all, which belongs to none and is its own row's top. A heap is a
/// subtype of another of its own hierarchy when they are equal, the other is the top, or it is the
/// bottom; two concrete types when they are equivalent; bot of every heap.
inline constexpr auto HeapTable = std::to_array<HeapRow>({
    { .kind = binary::AbstractHeapType::Func, .top = binary::AbstractHeapType::Func, .rank = HeapRank::Top },
    { .kind = binary::AbstractHeapType::Extern,
      .top = binary::AbstractHeapType::Extern,
      .rank = HeapRank::Top },
    { .kind = binary::AbstractHeapType::Exn, .top = binary::AbstractHeapType::Exn, .rank = HeapRank::Top },
    { .kind = binary::AbstractHeapType::NoFunc,
      .top = binary::AbstractHeapType::Func,
      .rank = HeapRank::Bottom },
    { .kind = binary::AbstractHeapType::NoExtern,
      .top = binary::AbstractHeapType::Extern,
      .rank = HeapRank::Bottom },
    { .kind = binary::AbstractHeapType::NoExn,
      .top = binary::AbstractHeapType::Exn,
      .rank = HeapRank::Bottom },
    { .kind = binary::AbstractHeapType::Bot,
      .top = binary::AbstractHeapType::Bot,
      .rank = HeapRank::BelowAll },
    { .kind = binary::AbstractHeapType::Concrete,
      .top = binary::AbstractHeapType::Func,
      .rank = HeapRank::Defined },
});

static_assert(meta::firstMisplacedRow<binary::LastAbstractHeapType>(HeapTable, &HeapRow::kind)
                  == meta::NoMisplacedRow,
              "HeapTable must hold one row per AbstractHeapType, at the index of its enumerator");

/// The row of HeapTable describing @p kind.
/// @param kind The abstract heap type.
/// @return Its row.
[[nodiscard]] constexpr HeapRow const& heapRow(binary::AbstractHeapType kind) noexcept
{
    return HeapTable[static_cast<std::size_t>(kind)];
}

/// The top of the hierarchy @p kind belongs to: func, extern or exn.
///
/// Precondition (checked): @p kind is not bot, which belongs to no hierarchy.
/// @param kind The abstract heap type.
/// @return The top of its hierarchy.
[[nodiscard]] constexpr binary::AbstractHeapType topOf(binary::AbstractHeapType kind) noexcept
{
    Require(kind != binary::AbstractHeapType::Bot);
    return heapRow(kind).top;
}

/// Whether @p sub matches @p super in a module: numbers and vectors when equal; references when
/// @p sub admits null only if @p super does and its heap is a subtype of @p super's (HeapTable),
/// two concrete types when their canonical ids are equal (no declared subtypes without GC).
///
/// Precondition (checked): every concrete type index is below @p canonical's size.
/// @param sub The type a value has.
/// @param super The type required of it.
/// @param canonical The module's canonical type ids (binary::canonicalTypeIds).
/// @return Whether a value of @p sub may stand where @p super is required.
[[nodiscard]] bool isSubtype(binary::ValueType sub,
                             binary::ValueType super,
                             std::span<uint32_t const> canonical) noexcept;

/// Whether @p sub matches @p super in the store, where every concrete type index is a TypeId, the
/// same for equal types: as isSubtype(), with concrete types compared by their ids.
/// @param sub The type a value has, in store form.
/// @param super The type required of it, in store form.
/// @return Whether a value of @p sub may stand where @p super is required.
[[nodiscard]] bool isStoreSubtype(binary::ValueType sub, binary::ValueType super) noexcept;

} // namespace isoline::validate
