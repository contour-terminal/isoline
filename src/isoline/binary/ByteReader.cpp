// SPDX-License-Identifier: Apache-2.0
#include <isoline/binary/ByteReader.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <ranges>
#include <span>
#include <string_view>

#include <isoline/binary/DecodeError.hpp>

namespace isoline::binary
{

namespace
{

    /// The payload bits a LEB128 byte carries, below its continuation bit.
    constexpr auto LebPayloadBits = 7U;
    constexpr auto LebPayloadMask = uint8_t { 0b0111'1111 };
    constexpr auto LebContinuationBit = uint8_t { 0b1000'0000 };
    constexpr auto LebSignBit = uint8_t { 0b0100'0000 };

    /// The most bytes an N-bit LEB128 value may take: ceil(N / 7).
    constexpr unsigned maxLebBytes(unsigned bits) noexcept
    {
        return (bits + LebPayloadBits - 1) / LebPayloadBits;
    }

    /// One row of the RFC 3629 table: a range of lead bytes, the sequence length they start, and the
    /// range the second byte must fall in. Every later byte is a plain continuation byte, 80..BF.
    /// The narrowed second-byte ranges are what exclude overlongs, surrogates and code points above
    /// U+10FFFF.
    struct Utf8LeadRow
    {
        uint8_t leadFirst;
        uint8_t leadLast;
        unsigned length;
        uint8_t secondFirst;
        uint8_t secondLast;
    };

    constexpr auto Utf8LeadTable = std::to_array<Utf8LeadRow>({
        { .leadFirst = 0x00, .leadLast = 0x7F, .length = 1, .secondFirst = 0x80, .secondLast = 0xBF },
        { .leadFirst = 0xC2, .leadLast = 0xDF, .length = 2, .secondFirst = 0x80, .secondLast = 0xBF },
        { .leadFirst = 0xE0, .leadLast = 0xE0, .length = 3, .secondFirst = 0xA0, .secondLast = 0xBF },
        { .leadFirst = 0xE1, .leadLast = 0xEC, .length = 3, .secondFirst = 0x80, .secondLast = 0xBF },
        { .leadFirst = 0xED, .leadLast = 0xED, .length = 3, .secondFirst = 0x80, .secondLast = 0x9F },
        { .leadFirst = 0xEE, .leadLast = 0xEF, .length = 3, .secondFirst = 0x80, .secondLast = 0xBF },
        { .leadFirst = 0xF0, .leadLast = 0xF0, .length = 4, .secondFirst = 0x90, .secondLast = 0xBF },
        { .leadFirst = 0xF1, .leadLast = 0xF3, .length = 4, .secondFirst = 0x80, .secondLast = 0xBF },
        { .leadFirst = 0xF4, .leadLast = 0xF4, .length = 4, .secondFirst = 0x80, .secondLast = 0x8F },
    });

    constexpr auto Utf8ContinuationFirst = uint8_t { 0x80 };
    constexpr auto Utf8ContinuationLast = uint8_t { 0xBF };

} // namespace

bool isValidUtf8(std::span<std::byte const> bytes) noexcept
{
    auto pendingContinuations = 0U;
    auto nextFirst = uint8_t { 0 };
    auto nextLast = uint8_t { 0 };
    for (auto const byte: bytes)
    {
        auto const value = std::to_integer<uint8_t>(byte);
        if (pendingContinuations == 0)
        {
            auto const row = std::ranges::find_if(Utf8LeadTable, [value](Utf8LeadRow const& candidate) {
                return candidate.leadFirst <= value && value <= candidate.leadLast;
            });
            if (row == Utf8LeadTable.end())
                return false;
            pendingContinuations = row->length - 1;
            nextFirst = row->secondFirst;
            nextLast = row->secondLast;
            continue;
        }
        if (value < nextFirst || nextLast < value)
            return false;
        nextFirst = Utf8ContinuationFirst;
        nextLast = Utf8ContinuationLast;
        --pendingContinuations;
    }
    return pendingContinuations == 0;
}

ByteReader::ByteReader(std::span<std::byte const> bytes, std::size_t baseOffset) noexcept:
    _bytes { bytes }, _baseOffset { baseOffset }
{
}

std::size_t ByteReader::offset() const noexcept
{
    return _baseOffset + _position;
}

std::size_t ByteReader::remaining() const noexcept
{
    return _bytes.size() - _position;
}

bool ByteReader::atEnd() const noexcept
{
    return remaining() == 0;
}

DecodeError ByteReader::unexpectedEnd() const noexcept
{
    // The interpreter reports running out at the end of the section or function being read.
    return DecodeError { DecodeErrorCode::UnexpectedEnd, _baseOffset + _bytes.size() };
}

std::expected<std::byte, DecodeError> ByteReader::readByte()
{
    if (atEnd())
        return std::unexpected { unexpectedEnd() };
    return _bytes[_position++];
}

std::expected<std::span<std::byte const>, DecodeError> ByteReader::readBytes(std::size_t count)
{
    if (count > remaining())
        return std::unexpected { unexpectedEnd() };
    auto const bytes = _bytes.subspan(_position, count);
    _position += count;
    return bytes;
}

template <unsigned Bits>
std::expected<uint64_t, DecodeError> ByteReader::readUnsigned()
{
    auto result = uint64_t { 0 };
    for (auto const index: std::views::iota(0U, maxLebBytes(Bits)))
    {
        auto const byteOffset = offset();
        auto const byte = readByte();
        if (!byte)
            return std::unexpected { byte.error() };
        auto const value = std::to_integer<uint8_t>(*byte);
        auto const shift = index * LebPayloadBits;
        auto const bitsLeft = Bits - shift;
        // In the last allowed byte, the payload bits beyond the value's width must be zero.
        auto const payload = static_cast<unsigned>(value & LebPayloadMask);
        if (bitsLeft < LebPayloadBits && payload >= (1U << bitsLeft))
            return std::unexpected { DecodeError { DecodeErrorCode::IntegerTooLarge, byteOffset } };
        result |= static_cast<uint64_t>(payload) << shift;
        if ((value & LebContinuationBit) == 0)
            return result;
    }
    return std::unexpected { DecodeError { DecodeErrorCode::IntegerRepresentationTooLong, offset() } };
}

template <unsigned Bits>
std::expected<int64_t, DecodeError> ByteReader::readSigned()
{
    auto result = uint64_t { 0 };
    for (auto const index: std::views::iota(0U, maxLebBytes(Bits)))
    {
        auto const byteOffset = offset();
        auto const byte = readByte();
        if (!byte)
            return std::unexpected { byte.error() };
        auto const value = std::to_integer<uint8_t>(*byte);
        auto const shift = index * LebPayloadBits;
        auto const bitsLeft = Bits - shift;
        // In the last allowed byte, the payload bits from the sign bit up must all repeat it.
        if (bitsLeft < LebPayloadBits)
        {
            auto const signMask = static_cast<uint8_t>((LebPayloadMask << (bitsLeft - 1)) & LebPayloadMask);
            auto const signBits = static_cast<uint8_t>(value & signMask);
            if (signBits != 0 && signBits != signMask)
                return std::unexpected { DecodeError { DecodeErrorCode::IntegerTooLarge, byteOffset } };
        }
        result |= static_cast<uint64_t>(value & LebPayloadMask) << shift;
        if ((value & LebContinuationBit) == 0)
        {
            auto const width = shift + LebPayloadBits;
            if ((value & LebSignBit) != 0 && width < 64)
                result |= ~uint64_t { 0 } << width;
            return static_cast<int64_t>(result);
        }
    }
    return std::unexpected { DecodeError { DecodeErrorCode::IntegerRepresentationTooLong, offset() } };
}

template <typename Word>
std::expected<Word, DecodeError> ByteReader::readFixed()
{
    return readBytes(sizeof(Word)).transform([](std::span<std::byte const> bytes) {
        auto word = Word { 0 };
        for (auto const byte: bytes | std::views::reverse)
            word = static_cast<Word>((word << 8U) | std::to_integer<Word>(byte));
        return word;
    });
}

std::expected<uint32_t, DecodeError> ByteReader::readU32()
{
    return readUnsigned<32>().transform([](uint64_t value) { return static_cast<uint32_t>(value); });
}

std::expected<uint64_t, DecodeError> ByteReader::readU64()
{
    return readUnsigned<64>();
}

std::expected<int32_t, DecodeError> ByteReader::readS32()
{
    return readSigned<32>().transform([](int64_t value) { return static_cast<int32_t>(value); });
}

std::expected<int64_t, DecodeError> ByteReader::readS33()
{
    return readSigned<33>();
}

std::expected<int64_t, DecodeError> ByteReader::readS64()
{
    return readSigned<64>();
}

std::expected<uint32_t, DecodeError> ByteReader::readFixedU32()
{
    return readFixed<uint32_t>();
}

std::expected<uint64_t, DecodeError> ByteReader::readFixedU64()
{
    return readFixed<uint64_t>();
}

std::expected<uint32_t, DecodeError> ByteReader::readLength()
{
    auto const lengthOffset = offset();
    auto const bound = remaining();
    return readU32().and_then([lengthOffset, bound](uint32_t length) -> std::expected<uint32_t, DecodeError> {
        if (length > bound)
            return std::unexpected { DecodeError { DecodeErrorCode::LengthOutOfBounds, lengthOffset } };
        return length;
    });
}

std::expected<std::string_view, DecodeError> ByteReader::readName()
{
    // The interpreter reports a malformed name at its start, the length prefix.
    auto const nameOffset = offset();
    return readLength().and_then(
        [this, nameOffset](uint32_t length) -> std::expected<std::string_view, DecodeError> {
            auto const bytes = readBytes(length);
            if (!bytes)
                return std::unexpected { bytes.error() };
            if (!isValidUtf8(*bytes))
                return std::unexpected { DecodeError { DecodeErrorCode::MalformedUtf8Encoding, nameOffset } };
            return std::string_view { reinterpret_cast<char const*>(bytes->data()), bytes->size() };
        });
}

} // namespace isoline::binary
