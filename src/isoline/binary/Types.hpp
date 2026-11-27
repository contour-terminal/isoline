// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace isoline::binary
{

/// The kind of a value type: a number, a vector, or a reference.
enum class ValueKind : uint8_t
{
    I32,
    I64,
    F32,
    F64,
    V128,
    Ref,
};

/// Whether a reference type admits null.
enum class Nullable : uint8_t
{
    No,
    Yes,
};

/// The heap a reference points into: an abstract heap, or a concrete type of the type section.
enum class AbstractHeapType : uint8_t
{
    Func,
    Extern,
    Exn,
    NoFunc,
    NoExtern,
    NoExn,
    /// The bottom of every hierarchy, 3.0's bot: the heap of what ref.as_non_null, br_on_null and
    /// br_on_non_null take from the polymorphic stack of unreachable code. Only validation produces
    /// it; it is never decoded, and no value has it.
    Bot,
    Concrete, ///< A defined type, named by HeapType::typeIndex.
};

/// The last AbstractHeapType enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastAbstractHeapType = AbstractHeapType::Concrete;

/// A heap type: an abstract heap, or a type index when the kind is Concrete.
///
/// In a module, a concrete type index indexes its type section; in the store (runtime::Store), it is
/// a TypeId, which every module's equal types share. Types order and compare member by member, an
/// abstract heap's typeIndex being 0 always, so that equal types are equal values.
struct HeapType
{
    AbstractHeapType kind = AbstractHeapType::Func; ///< The abstract heap, or Concrete.
    uint32_t typeIndex = 0;                         ///< The type index; 0 unless the kind is Concrete.

    auto operator<=>(HeapType const&) const = default;
};

/// A value type. Only references use nullable and heap; number and vector types leave them default.
struct ValueType
{
    ValueKind kind = ValueKind::I32;   ///< The type's kind.
    Nullable nullable = Nullable::Yes; ///< Whether a reference admits null; Ref only.
    HeapType heap {};                  ///< The heap a reference points into; Ref only.

    auto operator<=>(ValueType const&) const = default;
};

/// The 32-bit integer type.
inline constexpr auto I32 = ValueType { .kind = ValueKind::I32 };
/// The 64-bit integer type.
inline constexpr auto I64 = ValueType { .kind = ValueKind::I64 };
/// The 32-bit float type.
inline constexpr auto F32 = ValueType { .kind = ValueKind::F32 };
/// The 64-bit float type.
inline constexpr auto F64 = ValueType { .kind = ValueKind::F64 };
/// The 128-bit vector type.
inline constexpr auto V128 = ValueType { .kind = ValueKind::V128 };
/// funcref: a nullable reference to any function.
inline constexpr auto FuncRef = ValueType { .kind = ValueKind::Ref,
                                            .nullable = Nullable::Yes,
                                            .heap = { .kind = AbstractHeapType::Func } };
/// externref: a nullable reference to any host object.
inline constexpr auto ExternRef = ValueType { .kind = ValueKind::Ref,
                                              .nullable = Nullable::Yes,
                                              .heap = { .kind = AbstractHeapType::Extern } };
/// (ref func): a non-null reference to any function, the type of an element segment's function indices.
inline constexpr auto NonNullFuncRef = ValueType { .kind = ValueKind::Ref,
                                                   .nullable = Nullable::No,
                                                   .heap = { .kind = AbstractHeapType::Func } };
/// exnref: a nullable reference to any exception.
inline constexpr auto ExnRef = ValueType { .kind = ValueKind::Ref,
                                           .nullable = Nullable::Yes,
                                           .heap = { .kind = AbstractHeapType::Exn } };
/// nullexnref: the type of the null exception reference only.
inline constexpr auto NullExnRef = ValueType { .kind = ValueKind::Ref,
                                               .nullable = Nullable::Yes,
                                               .heap = { .kind = AbstractHeapType::NoExn } };

/// A reference to the concrete type at @p typeIndex: (ref $t) or (ref null $t).
/// @param typeIndex The type's index (a TypeId in the store).
/// @param nullable Whether the reference admits null.
/// @return The reference type.
[[nodiscard]] constexpr ValueType concreteRef(uint32_t typeIndex, Nullable nullable) noexcept
{
    return ValueType { .kind = ValueKind::Ref,
                       .nullable = nullable,
                       .heap = { .kind = AbstractHeapType::Concrete, .typeIndex = typeIndex } };
}

/// Whether @p type is a reference to a concrete type, (ref $t) or (ref null $t): the one form of value
/// type that names a type index.
/// @param type The type.
/// @return Whether its heap is Concrete.
[[nodiscard]] constexpr bool isConcreteRef(ValueType const& type) noexcept
{
    return type.kind == ValueKind::Ref && type.heap.kind == AbstractHeapType::Concrete;
}

/// @p type with its concrete type index, if it names one, replaced by @p map(index): how a module's
/// type indices become canonical ids or a store's TypeIds.
/// @param type The type.
/// @param map Maps a type index to the index or id that replaces it.
/// @return The type, rewritten.
template <typename Map>
[[nodiscard]] constexpr ValueType mapTypeIndex(ValueType type, Map const& map)
{
    if (isConcreteRef(type))
        type.heap.typeIndex = map(type.heap.typeIndex);
    return type;
}

/// Whether a value of @p type has a default: every number and vector type and every nullable
/// reference does; a non-null reference does not, so a local of it starts uninitialized.
/// @param type The type.
/// @return Whether it is defaultable.
[[nodiscard]] constexpr bool isDefaultable(ValueType const& type) noexcept
{
    return type.kind != ValueKind::Ref || type.nullable == Nullable::Yes;
}

/// A function type: what a function takes and what it returns.
struct FuncType
{
    std::vector<ValueType> params;  ///< The parameter types, in order.
    std::vector<ValueType> results; ///< The result types, in order.

    auto operator<=>(FuncType const&) const = default;
};

/// @p type with every concrete type index of its params and results replaced by @p map(index).
/// @param type The function type.
/// @param map Maps a type index to the index or id that replaces it.
/// @return The type, rewritten.
template <typename Map>
[[nodiscard]] FuncType mapTypeIndices(FuncType type, Map const& map)
{
    for (auto& param: type.params)
        param = mapTypeIndex(param, map);
    for (auto& result: type.results)
        result = mapTypeIndex(result, map);
    return type;
}

/// The type of the addresses a memory or table is indexed with (memory64 and table64 use I64).
enum class AddressType : uint8_t
{
    I32,
    I64,
};

/// The last AddressType enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastAddressType = AddressType::I64;

/// The size bounds of a memory (in pages) or a table (in elements).
struct Limits
{
    uint64_t min = 0;                           ///< The initial size.
    std::optional<uint64_t> max;                ///< The maximum size, if bounded.
    AddressType addressType = AddressType::I32; ///< The type its addresses take.
};

/// A table's type: its element reference type and its size bounds.
struct TableType
{
    ValueType element = FuncRef; ///< The reference type of its elements.
    Limits limits;               ///< Its size bounds, in elements.
};

/// The base-2 logarithm of a memory's page size when it declares none: 65536 bytes.
inline constexpr auto DefaultPageSizeLog2 = uint8_t { 16 };

/// The page sizes custom-page-sizes admits, as base-2 logarithms: 1 byte and 64 KiB. The validator
/// refuses a module's memory of any other, and api::Store::defineMemory() a host's.
inline constexpr auto ValidPageSizesLog2 = std::to_array<uint8_t>({ 0, DefaultPageSizeLog2 });

/// A page size's base-2 logarithm must be below this, so that the page size is a u64: the decoder
/// refuses a larger exponent as malformed, and runtime::Store::addMemory requires it of a host's memory.
inline constexpr auto PageSizeLog2Limit = uint8_t { 64 };

/// A memory's type: its size bounds and its page size.
struct MemoryType
{
    Limits limits; ///< Its size bounds, in pages.

    /// The base-2 logarithm of its page size in bytes: 16 unless custom-page-sizes declares another.
    /// The decoder admits values below PageSizeLog2Limit; the validator only 0 and 16.
    uint8_t pageSizeLog2 = DefaultPageSizeLog2;
};

/// The value type of the addresses, offsets and sizes of a memory or table of @p type.
/// @param type The memory's or table's address type.
/// @return I32 or I64.
[[nodiscard]] constexpr ValueType addressValueType(AddressType type) noexcept
{
    return type == AddressType::I64 ? I64 : I32;
}

/// The type of a copy's count between memories or tables of address types @p destination and
/// @p source: i64 only when both are, as memory64 and table64 define it.
/// @param destination The address type copied to.
/// @param source The address type copied from.
/// @return The narrower of the two.
[[nodiscard]] constexpr AddressType copyCountType(AddressType destination, AddressType source) noexcept
{
    return destination == AddressType::I64 && source == AddressType::I64 ? AddressType::I64
                                                                         : AddressType::I32;
}

/// The size of a page of a memory of @p type, in bytes.
///
/// Precondition: type.pageSizeLog2 is below PageSizeLog2Limit, as the decoder guarantees for a module's
/// memories and runtime::Store::addMemory checks for a host's.
/// @param type The memory's type.
/// @return 2 to the power of its pageSizeLog2.
[[nodiscard]] constexpr uint64_t pageSize(MemoryType const& type) noexcept
{
    return uint64_t { 1 } << type.pageSizeLog2;
}

/// The most pages a memory of @p type may declare: as many as its address type's largest byte
/// address needs, as custom-page-sizes' check_memorytype computes it.
/// @param type The memory's type.
/// @return For 64 KiB pages 2^16 with i32 addresses and 2^48 with i64 ones; for 1-byte pages
///         2^32 - 1 and 2^64 - 1, so that the size in bytes stays a value of the address type.
[[nodiscard]] constexpr uint64_t maxPages(MemoryType const& type) noexcept
{
    auto const largestAddress =
        type.limits.addressType == AddressType::I64 ? ~uint64_t { 0 } : uint64_t { 0xFFFF'FFFF };
    if (type.pageSizeLog2 == 0)
        return largestAddress;
    return (largestAddress >> type.pageSizeLog2) + 1;
}

/// The most elements a table of @p type may declare: the largest value its address type holds.
/// @param type The table's type.
/// @return 2^32 - 1 for i32 addresses, 2^64 - 1 for i64 ones.
[[nodiscard]] constexpr uint64_t maxTableSize(TableType const& type) noexcept
{
    return type.limits.addressType == AddressType::I64 ? ~uint64_t { 0 } : uint64_t { 0xFFFF'FFFF };
}

/// Whether a global can be assigned to.
enum class Mutability : uint8_t
{
    Const,
    Var,
};

/// A global's type: its value type and whether it is mutable.
struct GlobalType
{
    ValueType type;                            ///< The type of its value.
    Mutability mutability = Mutability::Const; ///< Whether global.set may assign it.
};

/// What an import or export names.
enum class ExternKind : uint8_t
{
    Func,
    Table,
    Memory,
    Global,
    Tag,
};

/// The last ExternKind enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastExternKind = ExternKind::Tag;

/// The number of ExternKind enumerators, one index space each, as LastExternKind counts them.
inline constexpr auto ExternKindCount = static_cast<std::size_t>(LastExternKind) + 1;

} // namespace isoline::binary
