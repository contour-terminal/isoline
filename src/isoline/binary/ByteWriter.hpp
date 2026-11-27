// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <string_view>
#include <vector>

// The binary format's primitives, written: the inverse of ByteReader's reads, which the module encoder
// and the name section's share.

namespace isoline::binary
{

/// The bytes being written.
using Bytes = std::vector<std::byte>;

/// The low seven bits of a LEB128 byte, which carry the value.
inline constexpr auto LebPayload = uint8_t { 0b0111'1111 };

/// The bit of a LEB128 byte that says another byte follows.
inline constexpr auto LebContinuation = uint8_t { 0b1000'0000 };

/// The bit of a signed LEB128 byte's payload that holds the value's sign once it is the last byte.
inline constexpr auto LebSign = uint8_t { 0b0100'0000 };

/// Writes one byte.
inline void writeByte(Bytes& out, uint8_t byte)
{
    out.push_back(std::byte { byte });
}

/// Writes @p bytes as they are.
inline void writeBytes(Bytes& out, std::span<std::byte const> bytes)
{
    out.insert(out.end(), bytes.begin(), bytes.end());
}

/// Writes @p value as an unsigned LEB128 in its shortest form.
inline void writeUnsigned(Bytes& out, uint64_t value)
{
    do
    {
        auto byte = static_cast<uint8_t>(value & LebPayload);
        value >>= 7U;
        if (value != 0)
            byte |= LebContinuation;
        writeByte(out, byte);
    } while (value != 0);
}

/// Writes @p value as a signed LEB128 in its shortest form.
inline void writeSigned(Bytes& out, int64_t value)
{
    while (true)
    {
        auto const byte = static_cast<uint8_t>(static_cast<uint64_t>(value) & LebPayload);
        value >>= 7; // arithmetic: the sign is kept
        auto const signClear = (byte & LebSign) == 0;
        if ((value == 0 && signClear) || (value == -1 && !signClear))
        {
            writeByte(out, byte);
            return;
        }
        writeByte(out, static_cast<uint8_t>(byte | LebContinuation));
    }
}

/// Writes the low @p width bytes of @p bits, least significant first.
inline void writeFixed(Bytes& out, uint64_t bits, std::size_t width)
{
    for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, width))
    {
        writeByte(out, static_cast<uint8_t>(bits & 0xFFU));
        bits >>= 8U;
    }
}

/// Writes a length or vector count, as an unsigned LEB128.
inline void writeLength(Bytes& out, std::size_t length)
{
    writeUnsigned(out, length);
}

/// Writes a name: its byte length, then its bytes.
inline void writeName(Bytes& out, std::string_view name)
{
    writeLength(out, name.size());
    writeBytes(out, std::as_bytes(std::span { name }));
}

/// Writes a vector: its length, then each item by @p writeOne.
template <typename T, typename WriteOne>
void writeVector(Bytes& out, std::span<T const> items, WriteOne writeOne)
{
    writeLength(out, items.size());
    for (auto const& item: items)
        writeOne(item);
}

} // namespace isoline::binary
