// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <span>

#include <isoline/binary/Types.hpp>
#include <isoline/runtime/Value.hpp>

namespace isoline::runtime
{

/// Encodes @p value as two 64-bit words, an exact bijection: decodeWords() gives it back bit for bit, a
/// typed null's heap included, and two values equal by operator== give equal words.
///
/// A 32-bit value is zero-extended into the first word, a 64-bit one stored as its bits, a vector as
/// lane 0 then lane 1, and a reference as its tag word `kind | heap.kind << 8 | heap.typeIndex << 32`,
/// then its address. A number leaves the second word 0. Words are written as whole integers, never
/// through the value's bytes, so a reference's padding never enters them and the encoding is the same on
/// every host. It is the encoding every engine shares: the fast interpreter's slots and the raw cells of
/// globals.
/// @param value The value.
/// @param words The two words it goes into.
void encodeWords(Value const& value, std::span<uint64_t, 2> words) noexcept;

/// The value of @p type that encodeWords() encoded into @p words.
/// @param type The value's type; a reference's heap comes from the words, not from it.
/// @param words The two words.
/// @return The value.
[[nodiscard]] Value decodeWords(binary::ValueType const& type, std::span<uint64_t const, 2> words) noexcept;

} // namespace isoline::runtime
