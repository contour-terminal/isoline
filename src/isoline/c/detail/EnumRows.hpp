// SPDX-License-Identifier: Apache-2.0
//
// The C API's enumerations against the C++ ones they stand for, one row per value (plan P17a, I1): a kind of
// value, a heap, a kind of extern and a mutability. Every conversion between the two (detail/Convert.hpp)
// reads these rows and nothing else, so a new kind is a row here, and a C value without a row is refused,
// never cast.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

#include <isoline/binary/Types.hpp>
#include <isoline/c/isoline.h>

namespace isoline::c::detail
{

/// Whose numbering a C value belongs to.
enum class KindOrigin : uint8_t
{
    Upstream, ///< wasm.h's own: its value is wasm.h's.
    /// Isoline's, for a kind wasm.h has no number for, on the side of 128 wasm.h's wasm_valkind_is_num() and
    /// wasm_valkind_is_ref() read right: a number type's byte in the binary format (v128's 0x7B), a reference
    /// type's from the range Isoline keeps for itself (IsolineKindFirst to IsolineKindLast).
    Isoline,
};

/// The values of wasm_valkind_t that Isoline numbers reference kinds from, upstream's numbering reaching none
/// of them.
inline constexpr auto IsolineKindFirst = wasm_valkind_t { 0xF0 };
/// The last value of Isoline's range.
inline constexpr auto IsolineKindLast = wasm_valkind_t { 0xFF };

/// One kind of value wasm_valtype_new() makes, and the type it makes.
struct ValueKindRow
{
    wasm_valkind_t kind;    ///< The C kind.
    binary::ValueType type; ///< The type: a number, the vector, or a nullable abstract reference.
    KindOrigin origin;      ///< Whose number the kind is.
};

/// The kinds of value: wasm.h's six and Isoline's two. A reference type's kind is the row of its hierarchy's
/// top (HeapRows), so (ref func) and (ref $t) report WASM_FUNCREF.
inline constexpr auto ValueKindRows = std::to_array<ValueKindRow>({
    { WASM_I32, binary::I32, KindOrigin::Upstream },
    { WASM_I64, binary::I64, KindOrigin::Upstream },
    { WASM_F32, binary::F32, KindOrigin::Upstream },
    { WASM_F64, binary::F64, KindOrigin::Upstream },
    { WASM_EXTERNREF, binary::ExternRef, KindOrigin::Upstream },
    { WASM_FUNCREF, binary::FuncRef, KindOrigin::Upstream },
    { ISOLINE_VALKIND_V128, binary::V128, KindOrigin::Isoline },
    { ISOLINE_VALKIND_EXNREF, binary::ExnRef, KindOrigin::Isoline },
});

/// One heap of a reference type.
struct HeapRow
{
    binary::AbstractHeapType heap; ///< The C++ heap.
    isoline_heap_t cHeap;          ///< Its C constant.
    binary::AbstractHeapType top;  ///< The top of its hierarchy: what its kind is the row of.
};

/// The heaps a value type of the C API may have: every heap a module declares. The validator's bottom heap
/// (AbstractHeapType::Bot), which no module may declare, has no row, and so no C form.
inline constexpr auto HeapRows = std::to_array<HeapRow>({
    { binary::AbstractHeapType::Func, ISOLINE_HEAP_FUNC, binary::AbstractHeapType::Func },
    { binary::AbstractHeapType::Extern, ISOLINE_HEAP_EXTERN, binary::AbstractHeapType::Extern },
    { binary::AbstractHeapType::Exn, ISOLINE_HEAP_EXN, binary::AbstractHeapType::Exn },
    { binary::AbstractHeapType::Concrete, ISOLINE_HEAP_CONCRETE, binary::AbstractHeapType::Func },
    { binary::AbstractHeapType::NoFunc, ISOLINE_HEAP_NOFUNC, binary::AbstractHeapType::Func },
    { binary::AbstractHeapType::NoExtern, ISOLINE_HEAP_NOEXTERN, binary::AbstractHeapType::Extern },
    { binary::AbstractHeapType::NoExn, ISOLINE_HEAP_NOEXN, binary::AbstractHeapType::Exn },
});

/// One kind of extern.
struct ExternKindRow
{
    binary::ExternKind kind; ///< The C++ kind.
    wasm_externkind_t cKind; ///< wasm.h's.
};

/// The kinds of extern, one row per binary::ExternKind, in enumerator order (rowAt()).
inline constexpr auto ExternKindRows = std::to_array<ExternKindRow>({
    { binary::ExternKind::Func, WASM_EXTERN_FUNC },
    { binary::ExternKind::Table, WASM_EXTERN_TABLE },
    { binary::ExternKind::Memory, WASM_EXTERN_MEMORY },
    { binary::ExternKind::Global, WASM_EXTERN_GLOBAL },
    { binary::ExternKind::Tag, WASM_EXTERN_TAG },
});
static_assert(ExternKindRows.size() == binary::ExternKindCount, "one row per binary::ExternKind");

/// One mutability of a global.
struct MutabilityRow
{
    binary::Mutability mutability; ///< The C++ mutability.
    wasm_mutability_t cMutability; ///< wasm.h's.
};

/// The last binary::Mutability enumerator. The switch lists every enumerator, so -Wswitch names this
/// function when one is added, and the row count below follows from what it returns.
[[nodiscard]] consteval auto lastMutability() noexcept -> binary::Mutability
{
    constexpr auto Last = binary::Mutability::Var;
    switch (Last)
    {
        case binary::Mutability::Const:
        case binary::Mutability::Var: break;
    }
    return Last;
}

/// The mutabilities, one row per binary::Mutability, in enumerator order (rowAt()).
inline constexpr auto MutabilityRows = std::to_array<MutabilityRow>({
    { binary::Mutability::Const, WASM_CONST },
    { binary::Mutability::Var, WASM_VAR },
});
static_assert(MutabilityRows.size() == std::to_underlying(lastMutability()) + 1,
              "one row per binary::Mutability");

/// One address type of a memory or table, as the C API gives its width.
struct AddressTypeRow
{
    binary::AddressType addressType; ///< The C++ address type.
    uint32_t bits;                   ///< Its width, as isoline_memorytype_address_bits() reports it.
};

/// The last binary::AddressType enumerator, checked as lastMutability() is: -Wswitch names this function
/// when an enumerator is added.
[[nodiscard]] consteval auto lastAddressType() noexcept -> binary::AddressType
{
    constexpr auto Last = binary::AddressType::I64;
    switch (Last)
    {
        case binary::AddressType::I32:
        case binary::AddressType::I64: break;
    }
    return Last;
}

/// The address types, in enumerator order (rowAt()): 32-bit, and 64-bit for memory64 and table64.
inline constexpr auto AddressTypeRows = std::to_array<AddressTypeRow>({
    { binary::AddressType::I32, 32 },
    { binary::AddressType::I64, 64 },
});
static_assert(AddressTypeRows.size() == std::to_underlying(lastAddressType()) + 1,
              "one row per binary::AddressType");

/// The row of @p rows whose @p column is @p value.
/// @param rows The rows.
/// @param column The member compared.
/// @param value The value looked for.
/// @return The row, or null when none has it.
template <typename Row, std::size_t Count, typename Value>
[[nodiscard]] constexpr auto findRow(std::array<Row, Count> const& rows,
                                     Value Row::* column,
                                     Value value) noexcept -> Row const*
{
    auto const found = std::ranges::find(rows, value, column);
    return found == rows.end() ? nullptr : &*found;
}

/// Whether each row of @p rows sits at the index of the enumerator its @p column holds: the row of
/// enumerator n is rows[n]. With a row count of one past the enumeration's last (each table's static_assert),
/// that makes every enumerator's row the one rowAt() reads.
/// @param rows The rows.
/// @param column The enumerator column.
/// @return Whether the rows are in enumerator order, one each.
template <typename Row, std::size_t Count, typename Enum>
[[nodiscard]] consteval auto rowsAtTheirIndex(std::array<Row, Count> const& rows, Enum Row::* column) -> bool
{
    auto index = std::size_t { 0 };
    for (auto const& row: rows)
    {
        if (static_cast<std::size_t>(row.*column) != index++)
            return false;
    }
    return true;
}

static_assert(rowsAtTheirIndex(ExternKindRows, &ExternKindRow::kind));
static_assert(rowsAtTheirIndex(MutabilityRows, &MutabilityRow::mutability));
static_assert(rowsAtTheirIndex(AddressTypeRows, &AddressTypeRow::addressType));

/// The row of the C++ enumerator @p value in @p rows, read by its index: never a search, and never a miss,
/// since the static_asserts above put every enumerator's row at its index. For C++ values only, which name
/// an enumerator; a C value, which may name none, is looked up with findRow() and refused without a row.
/// @param rows The rows, in enumerator order.
/// @param value The enumerator.
/// @return Its row.
template <typename Row, std::size_t Count, typename Enum>
    requires std::is_enum_v<Enum>
[[nodiscard]] constexpr auto rowAt(std::array<Row, Count> const& rows, Enum value) noexcept -> Row const&
{
    return rows[static_cast<std::size_t>(value)];
}

/// The C kinds of the value kinds whose wasm_val_t holds a reference (of.ref), which it owns: the rows of
/// reference types, and only those.
inline constexpr auto ReferenceValueKinds = [] {
    constexpr auto Count = std::ranges::count(
        ValueKindRows, binary::ValueKind::Ref, [](auto const& row) { return row.type.kind; });
    auto kinds = std::array<wasm_valkind_t, static_cast<std::size_t>(Count)> {};
    auto next = kinds.begin();
    for (auto const& row: ValueKindRows)
        if (row.type.kind == binary::ValueKind::Ref)
            *next++ = row.kind;
    return kinds;
}();

} // namespace isoline::c::detail
