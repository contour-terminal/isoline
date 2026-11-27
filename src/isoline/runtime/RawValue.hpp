// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <isoline/binary/Types.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/runtime/ValueWords.hpp>

namespace isoline::runtime
{

/// A value as two 64-bit words in the encoding every engine shares (encodeWords()): what a global's cell
/// holds, and what crosses between generated code and the runtime. It carries no type: decodeValue() takes
/// it from the global or the signature the value belongs to. The encoding is an exact bijection with no
/// padding, so operator== is well defined and equal values give equal cells.
struct alignas(16) RawValue
{
    /// The value's words: a number's in words[0], words[1] 0. A 32-bit number (i32, f32) is the low half
    /// of words[0], zero-extended: the value's bits, not its bytes, so code that loads it as 32 bits in
    /// place finds it at offset 0 of the cell on a little-endian host only, and at offset 4 on a big-endian
    /// one.
    std::array<uint64_t, 2> words {};

    /// Whether both hold the same words, hence the same value of any one type.
    [[nodiscard]] bool operator==(RawValue const&) const = default;
};

/// The raw cell of @p value (encodeWords()).
/// @param value The value.
/// @return Its two words.
[[nodiscard]] inline RawValue encodeValue(Value const& value) noexcept
{
    auto raw = RawValue {};
    encodeWords(value, raw.words);
    return raw;
}

/// The value of @p type that encodeValue() made @p raw of (decodeWords()).
/// @param raw The cell.
/// @param type The value's type; a reference's heap comes from the cell, not from it.
/// @return The value.
[[nodiscard]] inline Value decodeValue(RawValue const& raw, binary::ValueType const& type) noexcept
{
    return decodeWords(type, raw.words);
}

/// Where a Ref's fields lie in its object representation: what code that reads or writes a Ref in place
/// (rather than through its two words) addresses it by.
struct RefLayout
{
    std::size_t size;                ///< sizeof(Ref).
    std::size_t kindOffset;          ///< The offset of Ref::kind, one byte.
    std::size_t heapKindOffset;      ///< The offset of Ref::heap's kind, one byte.
    std::size_t heapTypeIndexOffset; ///< The offset of Ref::heap's type index, four bytes.
    std::size_t addressOffset;       ///< The offset of Ref::address, four bytes.
};

static_assert(std::is_standard_layout_v<Ref> && std::is_trivially_copyable_v<Ref>,
              "RefLayoutOfRef names offsets, which only a standard-layout, trivially copyable Ref has");
static_assert(std::is_standard_layout_v<binary::HeapType>, "Ref's heap is addressed by offsets too");

/// The layout of Ref on this host, from sizeof and offsetof.
inline constexpr auto RefLayoutOfRef =
    RefLayout { .size = sizeof(Ref),
                .kindOffset = offsetof(Ref, kind),
                .heapKindOffset = offsetof(Ref, heap) + offsetof(binary::HeapType, kind),
                .heapTypeIndexOffset = offsetof(Ref, heap) + offsetof(binary::HeapType, typeIndex),
                .addressOffset = offsetof(Ref, address) };

static_assert(sizeof(RawValue) == 16 && alignof(RawValue) == 16, "a RawValue is two words, 16-aligned");

} // namespace isoline::runtime
