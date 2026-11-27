// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <ranges>
#include <span>

#include <isoline/component/canonical/Values.hpp>

// Decoding and encoding a string's code points in each simple encoding, without a copy or an
// allocation: what a Values compares and prints strings with, and what lifting validates and lowering
// transcodes them with. Private to isoline_component.

namespace isoline::component::canonical
{

inline constexpr auto HighSurrogateFirst = char32_t { 0xD800 };   ///< The first high (leading) surrogate.
inline constexpr auto LowSurrogateFirst = char32_t { 0xDC00 };    ///< The first low (trailing) surrogate.
inline constexpr auto SurrogateLast = char32_t { 0xDFFF };        ///< The last surrogate.
inline constexpr auto SupplementaryFirst = char32_t { 0x1'0000 }; ///< The first code point past the BMP.
inline constexpr auto MaxCodePoint = char32_t { 0x10'FFFF };      ///< The last code point.
inline constexpr auto Latin1End = char32_t { 0x100 }; ///< The first code point Latin-1 cannot hold.
inline constexpr auto AsciiEnd = char32_t { 0x80 };   ///< The first code point past ASCII.

/// One row of the RFC 3629 table: a range of lead bytes, the sequence length they start, the range the
/// second byte must fall in (which excludes overlongs, surrogates and code points above U+10FFFF), and
/// the payload bits of the lead. Every later byte is a plain continuation.
struct Utf8LeadRow
{
    uint8_t leadFirst;   ///< The first lead byte of the row.
    uint8_t leadLast;    ///< Its last.
    uint8_t length;      ///< The bytes of the sequences it starts.
    uint8_t secondFirst; ///< The least second byte.
    uint8_t secondLast;  ///< The greatest second byte.
    uint8_t leadPayload; ///< The lead's payload bits.
};

/// Well-formed UTF-8, by lead byte (RFC 3629, table 3-7 of the Unicode standard).
inline constexpr auto Utf8LeadTable = std::to_array<Utf8LeadRow>({
    // clang-format off
    { .leadFirst = 0x00, .leadLast = 0x7F, .length = 1, .secondFirst = 0x80, .secondLast = 0xBF, .leadPayload = 0x7F },
    { .leadFirst = 0xC2, .leadLast = 0xDF, .length = 2, .secondFirst = 0x80, .secondLast = 0xBF, .leadPayload = 0x1F },
    { .leadFirst = 0xE0, .leadLast = 0xE0, .length = 3, .secondFirst = 0xA0, .secondLast = 0xBF, .leadPayload = 0x0F },
    { .leadFirst = 0xE1, .leadLast = 0xEC, .length = 3, .secondFirst = 0x80, .secondLast = 0xBF, .leadPayload = 0x0F },
    { .leadFirst = 0xED, .leadLast = 0xED, .length = 3, .secondFirst = 0x80, .secondLast = 0x9F, .leadPayload = 0x0F },
    { .leadFirst = 0xEE, .leadLast = 0xEF, .length = 3, .secondFirst = 0x80, .secondLast = 0xBF, .leadPayload = 0x0F },
    { .leadFirst = 0xF0, .leadLast = 0xF0, .length = 4, .secondFirst = 0x90, .secondLast = 0xBF, .leadPayload = 0x07 },
    { .leadFirst = 0xF1, .leadLast = 0xF3, .length = 4, .secondFirst = 0x80, .secondLast = 0xBF, .leadPayload = 0x07 },
    { .leadFirst = 0xF4, .leadLast = 0xF4, .length = 4, .secondFirst = 0x80, .secondLast = 0x8F, .leadPayload = 0x07 },
    // clang-format on
});

/// The most bytes one code point takes in any encoding.
inline constexpr auto MaxCodePointBytes = std::size_t { 4 };

/// Reads a string's code points one at a time, in its simple encoding.
class CodePointReader
{
  public:
    /// A reader at the first byte of @p bytes.
    /// @param bytes The string's bytes; they must outlive the reader.
    /// @param encoding Their encoding.
    CodePointReader(std::span<std::byte const> bytes, SimpleEncoding encoding) noexcept:
        _bytes { bytes }, _encoding { encoding }
    {
    }

    /// The next code point.
    /// @return It, empty at the end, or why the bytes are not text.
    [[nodiscard]] std::expected<std::optional<char32_t>, StringError> next() noexcept
    {
        if (_position == _bytes.size())
            return std::nullopt;
        switch (_encoding)
        {
            case SimpleEncoding::Utf8: return nextUtf8();
            case SimpleEncoding::Utf16: return nextUtf16();
            case SimpleEncoding::Latin1: return char32_t { byteAt(_position++) };
        }
        return std::nullopt; // uncovered(exhaustive-switch)
    }

    /// The bytes read so far.
    /// @return The offset of the next code point.
    [[nodiscard]] std::size_t position() const noexcept { return _position; }

  private:
    static constexpr auto ContinuationFirst = uint8_t { 0x80 };
    static constexpr auto ContinuationLast = uint8_t { 0xBF };
    static constexpr auto ContinuationPayload = uint8_t { 0x3F };
    static constexpr auto ContinuationBits = 6U;
    static constexpr auto SurrogateBits = 10U;

    [[nodiscard]] uint8_t byteAt(std::size_t index) const noexcept
    {
        return std::to_integer<uint8_t>(_bytes[index]);
    }

    [[nodiscard]] std::expected<std::optional<char32_t>, StringError> nextUtf8() noexcept
    {
        auto const lead = byteAt(_position);
        auto const row = std::ranges::find_if(Utf8LeadTable, [lead](Utf8LeadRow const& candidate) {
            return candidate.leadFirst <= lead && lead <= candidate.leadLast;
        });
        if (row == Utf8LeadTable.end() || _bytes.size() - _position < row->length)
            return std::unexpected { StringError::InvalidUtf8 };
        auto codePoint = static_cast<char32_t>(lead & row->leadPayload);
        auto first = row->secondFirst;
        auto last = row->secondLast;
        for (auto const offset: std::views::iota(std::size_t { 1 }, std::size_t { row->length }))
        {
            auto const byte = byteAt(_position + offset);
            if (byte < first || last < byte)
                return std::unexpected { StringError::InvalidUtf8 };
            codePoint = (codePoint << ContinuationBits) | (byte & ContinuationPayload);
            first = ContinuationFirst;
            last = ContinuationLast;
        }
        _position += row->length;
        return codePoint;
    }

    [[nodiscard]] std::optional<char32_t> unitAt(std::size_t index) const noexcept
    {
        if (_bytes.size() - index < 2)
            return std::nullopt;
        return static_cast<char32_t>(byteAt(index) | (byteAt(index + 1) << 8U));
    }

    [[nodiscard]] std::expected<std::optional<char32_t>, StringError> nextUtf16() noexcept
    {
        auto const unit = unitAt(_position);
        if (!unit || (*unit >= LowSurrogateFirst && *unit <= SurrogateLast))
            return std::unexpected { StringError::InvalidUtf16 };
        if (*unit < HighSurrogateFirst || *unit > SurrogateLast)
        {
            _position += 2;
            return *unit;
        }
        auto const low = unitAt(_position + 2);
        if (!low || *low < LowSurrogateFirst || *low > SurrogateLast)
            return std::unexpected { StringError::InvalidUtf16 };
        _position += 4;
        return SupplementaryFirst + ((*unit - HighSurrogateFirst) << SurrogateBits)
               + (*low - LowSurrogateFirst);
    }

    std::span<std::byte const> _bytes;
    std::size_t _position = 0;
    SimpleEncoding _encoding;
};

/// One code point's bytes in an encoding.
struct EncodedCodePoint
{
    std::array<std::byte, MaxCodePointBytes> bytes {}; ///< The bytes; the first @ref length are used.
    std::size_t length = 0;                            ///< How many.

    /// The bytes used.
    /// @return The first @ref length of @ref bytes.
    [[nodiscard]] std::span<std::byte const> view() const noexcept
    {
        return std::span { bytes }.first(length);
    }
};

/// @p codePoint, a Unicode scalar value, in UTF-8.
/// @param codePoint The code point.
/// @return Its one to four bytes.
[[nodiscard]] constexpr EncodedCodePoint encodeUtf8(char32_t codePoint) noexcept
{
    constexpr auto Payload = 0x3FU;
    auto const byte = [](uint32_t bits) {
        return static_cast<std::byte>(bits & 0xFFU);
    };
    if (codePoint < AsciiEnd)
        return EncodedCodePoint { .bytes = { byte(codePoint) }, .length = 1 };
    if (codePoint < 0x800)
        return EncodedCodePoint {
            .bytes = { byte(0xC0U | (codePoint >> 6U)), byte(0x80U | (codePoint & Payload)) }, .length = 2
        };
    if (codePoint < SupplementaryFirst)
        return EncodedCodePoint { .bytes = { byte(0xE0U | (codePoint >> 12U)),
                                             byte(0x80U | ((codePoint >> 6U) & Payload)),
                                             byte(0x80U | (codePoint & Payload)) },
                                  .length = 3 };
    return EncodedCodePoint { .bytes = { byte(0xF0U | (codePoint >> 18U)),
                                         byte(0x80U | ((codePoint >> 12U) & Payload)),
                                         byte(0x80U | ((codePoint >> 6U) & Payload)),
                                         byte(0x80U | (codePoint & Payload)) },
                              .length = 4 };
}

/// @p codePoint, a Unicode scalar value, in UTF-16, little-endian.
/// @param codePoint The code point.
/// @return Its two or four bytes.
[[nodiscard]] constexpr EncodedCodePoint encodeUtf16(char32_t codePoint) noexcept
{
    auto const byte = [](uint32_t bits) {
        return static_cast<std::byte>(bits & 0xFFU);
    };
    if (codePoint < SupplementaryFirst)
        return EncodedCodePoint { .bytes = { byte(codePoint), byte(codePoint >> 8U) }, .length = 2 };
    auto const offset = codePoint - SupplementaryFirst;
    auto const high = HighSurrogateFirst + (offset >> 10U);
    auto const low = LowSurrogateFirst + (offset & 0x3FFU);
    return EncodedCodePoint { .bytes = { byte(high), byte(high >> 8U), byte(low), byte(low >> 8U) },
                              .length = 4 };
}

} // namespace isoline::component::canonical
