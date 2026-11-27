// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <utility>

#include <isoline/component/canonical/LiftLower.hpp>
#include <isoline/component/canonical/Trap.hpp>
#include <isoline/component/canonical/Values.hpp>

namespace isoline::component::canonical
{

/// The most bytes a string lifted from guest memory holds (definitions.py's MAX_STRING_BYTE_LENGTH); a
/// string lowered into one is held to it too, so that every worst-case size below fits a 32-bit
/// `realloc` (REALLOC_I32_MAX > 3 × 2^27 and 2 × 2^28).
inline constexpr auto MaxStringByteLength = (uint32_t { 1 } << 28U) - 1;

/// The most bytes a list lifted from guest memory spans (definitions.py's MAX_LIST_BYTE_LENGTH).
inline constexpr auto MaxListByteLength = (uint32_t { 1 } << 28U) - 1;

/// One row of StringEncodingTable: a `string-encoding` option and how its strings lie in memory.
struct StringEncodingRow
{
    StringEncoding encoding; ///< The option; equal to the row's index.
    uint32_t alignment;      ///< The alignment a string's bytes have (load_string_from_range).
};

/// Every string encoding, one row per StringEncoding enumerator, in enumerator order.
inline constexpr auto StringEncodingTable = std::to_array<StringEncodingRow>({
    { .encoding = StringEncoding::Utf8, .alignment = 1 },
    { .encoding = StringEncoding::Utf16, .alignment = 2 },
    { .encoding = StringEncoding::Latin1Utf16, .alignment = 2 },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: StringEncodingTable)
        {
            if (static_cast<std::size_t>(row.encoding) != index)
                return false;
            ++index;
        }
        return true;
    }(),
    "StringEncodingTable must hold one row per StringEncoding, at the index of its enumerator");

/// How a string is stored into a destination encoding: definitions.py's functions of the same names.
enum class TranscodeFunction : uint8_t
{
    Utf8Copy,                     ///< store_string_copy(…, 1, 1, 'utf-8').
    Utf16Copy,                    ///< store_string_copy(…, 2, 2, 'utf-16-le') of UTF-16.
    Latin1ToUtf16Copy,            ///< store_string_copy(…, 2, 2, 'utf-16-le') of Latin-1.
    Latin1Copy,                   ///< store_string_copy(…, 1, 2, 'latin-1').
    Utf16ToUtf8,                  ///< store_utf16_to_utf8.
    Latin1ToUtf8,                 ///< store_latin1_to_utf8.
    Utf8ToUtf16,                  ///< store_utf8_to_utf16.
    StringToLatin1OrUtf16,        ///< store_string_to_latin1_or_utf16.
    ProbablyUtf16ToLatin1OrUtf16, ///< store_probably_utf16_to_latin1_or_utf16.
};

/// One row of TranscodeTable: a source string's encoding and simple encoding, a destination encoding,
/// and how the string is stored into it.
struct TranscodeRow
{
    StringEncoding from;        ///< The source string's encoding.
    SimpleEncoding fromSimple;  ///< Its simple encoding, the tag read.
    StringEncoding to;          ///< The destination's `string-encoding`.
    TranscodeFunction function; ///< How.
};

/// store_string_into_range's cases, one row per (source encoding, simple encoding, destination) a
/// string can have. definitions.py matches a utf8 or utf16 destination on the simple encoding alone, so
/// its utf16-to-utf8 and utf16-to-utf16 cases are reached from a utf16 and from a tagged latin1+utf16
/// source: two rows each, and twelve rows for the ten cases, each destination having four.
inline constexpr auto TranscodeTable = std::to_array<TranscodeRow>({
    // clang-format off
    { .from = StringEncoding::Utf8, .fromSimple = SimpleEncoding::Utf8, .to = StringEncoding::Utf8, .function = TranscodeFunction::Utf8Copy },
    { .from = StringEncoding::Utf16, .fromSimple = SimpleEncoding::Utf16, .to = StringEncoding::Utf8, .function = TranscodeFunction::Utf16ToUtf8 },
    { .from = StringEncoding::Latin1Utf16, .fromSimple = SimpleEncoding::Utf16, .to = StringEncoding::Utf8, .function = TranscodeFunction::Utf16ToUtf8 },
    { .from = StringEncoding::Latin1Utf16, .fromSimple = SimpleEncoding::Latin1, .to = StringEncoding::Utf8, .function = TranscodeFunction::Latin1ToUtf8 },
    { .from = StringEncoding::Utf8, .fromSimple = SimpleEncoding::Utf8, .to = StringEncoding::Utf16, .function = TranscodeFunction::Utf8ToUtf16 },
    { .from = StringEncoding::Utf16, .fromSimple = SimpleEncoding::Utf16, .to = StringEncoding::Utf16, .function = TranscodeFunction::Utf16Copy },
    { .from = StringEncoding::Latin1Utf16, .fromSimple = SimpleEncoding::Utf16, .to = StringEncoding::Utf16, .function = TranscodeFunction::Utf16Copy },
    { .from = StringEncoding::Latin1Utf16, .fromSimple = SimpleEncoding::Latin1, .to = StringEncoding::Utf16, .function = TranscodeFunction::Latin1ToUtf16Copy },
    { .from = StringEncoding::Utf8, .fromSimple = SimpleEncoding::Utf8, .to = StringEncoding::Latin1Utf16, .function = TranscodeFunction::StringToLatin1OrUtf16 },
    { .from = StringEncoding::Utf16, .fromSimple = SimpleEncoding::Utf16, .to = StringEncoding::Latin1Utf16, .function = TranscodeFunction::StringToLatin1OrUtf16 },
    { .from = StringEncoding::Latin1Utf16, .fromSimple = SimpleEncoding::Latin1, .to = StringEncoding::Latin1Utf16, .function = TranscodeFunction::Latin1Copy },
    { .from = StringEncoding::Latin1Utf16, .fromSimple = SimpleEncoding::Utf16, .to = StringEncoding::Latin1Utf16, .function = TranscodeFunction::ProbablyUtf16ToLatin1OrUtf16 },
    // clang-format on
});

/// The TranscodeTable row of a source string and a destination encoding.
/// @param from The source string's encoding.
/// @param fromSimple Its simple encoding: one its encoding admits (checked).
/// @param to The destination's encoding.
/// @return The row.
[[nodiscard]] TranscodeRow const& transcodeRow(StringEncoding from,
                                               SimpleEncoding fromSimple,
                                               StringEncoding to);

/// Lifts the string of @p taggedCodeUnits at @p address, in the context's encoding (definitions.py's
/// load_string_from_range), as the next value of @p builder: its bytes are copied once, straight into
/// the values' storage, and kept in their encoding (D4).
///
/// Its length is checked first, then its alignment and its bounds in 64-bit arithmetic; then its bytes
/// are charged to the budget before they are allocated, copied in api::BulkPollBytes granules, polling
/// after each, and validated as they arrive.
/// @param context The lifting side; it has a memory (checked).
/// @param builder The builder whose next value is the string.
/// @param address The string's first byte.
/// @param taggedCodeUnits Its tagged code units.
/// @return Nothing; or OutOfBounds past MaxStringByteLength or the memory's end, UnalignedPointer,
///         LiveValueBytes, InvalidEncoding, or the trap of a read or a poll.
/// @throws std::bad_alloc When the host cannot allocate what the budget allowed.
[[nodiscard]] std::expected<void, Trap> liftString(LiftContext& context,
                                                   ValueBuilder& builder,
                                                   uint32_t address,
                                                   uint32_t taggedCodeUnits);

/// Stores @p value in the context's encoding into a block its `realloc` allocates (definitions.py's
/// store_string_into_range): TranscodeTable's function for the pair, with the reference's `realloc`
/// calls, each result checked. Its bytes are written in order, never read back except where the
/// reference reads them (the Latin-1 prefix store_string_to_latin1_or_utf16 widens in place), and every
/// api::BulkPollBytes of bytes written or transcoded polls.
/// @param context The lowering side; it has a memory (checked).
/// @param value The string.
/// @return The block's address and the string's tagged code units in the destination; or OutOfBounds
///         for a string of more than MaxStringByteLength bytes, the trap of a `realloc`, a write or a
///         poll, or InvalidEncoding for bytes that are not text (which only a broken caller of
///         ValueBuilder::encodedString() makes).
/// @throws std::bad_alloc When a trap's message cannot be stored.
[[nodiscard]] std::expected<std::pair<uint32_t, uint32_t>, Trap> storeString(LiftContext& context,
                                                                             StringValue const& value);

} // namespace isoline::component::canonical
