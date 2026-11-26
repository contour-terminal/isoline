// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <type_traits>
#include <variant>

#include <isoline/binary/Types.hpp>

namespace isoline::runtime
{

/// A 32-bit integer, as its bits; whether it is signed is the instruction's to decide.
struct I32
{
    uint32_t bits = 0; ///< The value's bits.

    bool operator==(I32 const&) const = default;
};

/// A 64-bit integer, as its bits.
struct I64
{
    uint64_t bits = 0; ///< The value's bits.

    bool operator==(I64 const&) const = default;
};

/// A 32-bit float, as its IEEE 754 bits: floats are carried as bits always, so a NaN's payload and
/// sign survive every move, as the spec requires of everything that is not arithmetic.
struct F32
{
    uint32_t bits = 0; ///< The value's IEEE 754 bits.

    bool operator==(F32 const&) const = default;
};

/// A 64-bit float, as its IEEE 754 bits.
struct F64
{
    uint64_t bits = 0; ///< The value's IEEE 754 bits.

    bool operator==(F64 const&) const = default;
};

/// A 128-bit vector, as two 64-bit halves, the low half first.
struct V128
{
    std::array<uint64_t, 2> lanes {}; ///< The low and the high 64 bits.

    bool operator==(V128 const&) const = default;
};

/// The vector whose bytes, lowest lane byte first, are @p bytes: as memory and v128.const hold one.
///
/// Composed byte by byte, so the result is the same on hosts of either byte order.
/// @param bytes The 16 bytes, byte 0 the lowest byte of lane 0.
/// @return The vector.
[[nodiscard]] constexpr V128 v128FromBytes(std::span<std::byte const, 16> bytes) noexcept
{
    auto vector = V128 {};
    for (auto const index: std::views::iota(std::size_t { 0 }, bytes.size()))
        vector.lanes[index / 8] |= uint64_t { std::to_integer<uint8_t>(bytes[index]) } << (8U * (index % 8));
    return vector;
}

/// The bytes of @p vector, lowest lane byte first: as memory and v128.const hold it.
///
/// Decomposed byte by byte, so the result is the same on hosts of either byte order.
/// @param vector The vector.
/// @return Its 16 bytes, byte 0 the lowest byte of lane 0.
[[nodiscard]] constexpr std::array<std::byte, 16> v128ToBytes(V128 vector) noexcept
{
    auto bytes = std::array<std::byte, 16> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, bytes.size()))
        bytes[index] = static_cast<std::byte>(vector.lanes[index / 8] >> (8U * (index % 8)));
    return bytes;
}

/// What a reference refers to.
enum class RefKind : uint8_t
{
    Null,   ///< Nothing: the null reference of its heap.
    Func,   ///< A function instance, by its store address.
    Extern, ///< A host object, by an address the embedder assigns.
};

/// A reference value.
struct Ref
{
    RefKind kind = RefKind::Null; ///< What it refers to.
    binary::HeapType heap {};     ///< The heap it belongs to; a null reference keeps its type's heap.
    uint32_t address = 0;         ///< The referent's address; meaningful unless kind is Null.

    bool operator==(Ref const&) const = default;
};

/// A WebAssembly value.
using Value = std::variant<I32, I64, F32, F64, V128, Ref>;

/// The kind of @p value: which value type it belongs to, a reference's heap aside.
/// @param value Any value.
/// @return The ValueKind of its alternative.
[[nodiscard]] constexpr binary::ValueKind kindOf(Value const& value) noexcept
{
    return std::visit(
        []<typename T>(T const&) {
            if constexpr (std::is_same_v<T, I32>)
                return binary::ValueKind::I32;
            else if constexpr (std::is_same_v<T, I64>)
                return binary::ValueKind::I64;
            else if constexpr (std::is_same_v<T, F32>)
                return binary::ValueKind::F32;
            else if constexpr (std::is_same_v<T, F64>)
                return binary::ValueKind::F64;
            else if constexpr (std::is_same_v<T, V128>)
                return binary::ValueKind::V128;
            else
            {
                static_assert(std::is_same_v<T, Ref>, "every Value alternative has a ValueKind");
                return binary::ValueKind::Ref;
            }
        },
        value);
}

/// The value a local or a table slot of @p type starts with: zero, or the null reference.
/// @param type The value's type.
/// @return Its default value.
[[nodiscard]] constexpr Value defaultValue(binary::ValueType const& type) noexcept
{
    switch (type.kind)
    {
        case binary::ValueKind::I32: return I32 {};
        case binary::ValueKind::I64: return I64 {};
        case binary::ValueKind::F32: return F32 {};
        case binary::ValueKind::F64: return F64 {};
        case binary::ValueKind::V128: return V128 {};
        case binary::ValueKind::Ref: break;
    }
    return Ref { .kind = RefKind::Null, .heap = type.heap, .address = 0 };
}

} // namespace isoline::runtime
