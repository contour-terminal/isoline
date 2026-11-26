// SPDX-License-Identifier: Apache-2.0
#pragma once

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
    Concrete, ///< A defined type, named by HeapType::typeIndex.
};

/// A heap type: an abstract heap, or a type index when the kind is Concrete.
struct HeapType
{
    AbstractHeapType kind = AbstractHeapType::Func; ///< The abstract heap, or Concrete.
    uint32_t typeIndex = 0;                         ///< The type index; meaningful only for Concrete.

    bool operator==(HeapType const&) const = default;
};

/// A value type. Only references use nullable and heap; number and vector types leave them default.
struct ValueType
{
    ValueKind kind = ValueKind::I32;   ///< The type's kind.
    Nullable nullable = Nullable::Yes; ///< Whether a reference admits null; Ref only.
    HeapType heap {};                  ///< The heap a reference points into; Ref only.

    bool operator==(ValueType const&) const = default;
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

/// A function type: what a function takes and what it returns.
struct FuncType
{
    std::vector<ValueType> params;  ///< The parameter types, in order.
    std::vector<ValueType> results; ///< The result types, in order.

    bool operator==(FuncType const&) const = default;
};

/// The type of the addresses a memory or table is indexed with (memory64 and table64 use I64).
enum class AddressType : uint8_t
{
    I32,
    I64,
};

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

/// A memory's type. Its page size is fixed at 65536 bytes.
struct MemoryType
{
    Limits limits; ///< Its size bounds, in pages.
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

/// The most pages a memory of @p type may declare: what its address type reaches in 64 KiB pages.
/// @param type The memory's type.
/// @return 2^16 for i32 addresses, 2^48 for i64 ones.
[[nodiscard]] constexpr uint64_t maxPages(MemoryType const& type) noexcept
{
    return type.limits.addressType == AddressType::I64 ? uint64_t { 1 } << 48 : uint64_t { 1 } << 16;
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

} // namespace isoline::binary
