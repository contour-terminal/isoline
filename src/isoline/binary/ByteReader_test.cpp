// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <isoline/binary/ByteReader.hpp>
#include <isoline/binary/DecodeError.hpp>

using isoline::binary::ByteReader;
using isoline::binary::DecodeErrorCode;

namespace
{

/// One row of a LEB128 table: the input bytes and either the value read or the error reported.
template <typename T>
struct Row
{
    std::vector<uint8_t> bytes {};
    std::optional<T> value {};
    std::optional<DecodeErrorCode> error {};
};

std::vector<std::byte> toBytes(std::span<uint8_t const> bytes)
{
    auto result = std::vector<std::byte> {};
    result.reserve(bytes.size());
    for (auto const byte: bytes)
        result.push_back(std::byte { byte });
    return result;
}

/// Runs @p read on a reader over each row's bytes and checks the outcome the row names.
template <typename T, typename Read>
void checkRows(std::span<Row<T> const> rows, Read read)
{
    for (auto const& row: rows)
    {
        auto const bytes = toBytes(row.bytes);
        auto reader = ByteReader { bytes };
        auto const result = read(reader);
        INFO("row of " << row.bytes.size() << " bytes");
        if (row.value)
        {
            REQUIRE(result.has_value());
            CHECK(*result == *row.value);
            CHECK(reader.atEnd());
        }
        else
        {
            REQUIRE_FALSE(result.has_value());
            CHECK(result.error().code == *row.error);
        }
    }
}

} // namespace

TEST_CASE("ByteReader.readU32")
{
    auto const rows = std::to_array<Row<uint32_t>>({
        { .bytes = { 0x00 }, .value = 0u, .error = {} },
        { .bytes = { 0xE5, 0x8E, 0x26 }, .value = 624485u, .error = {} },
        { .bytes = { 0xFF, 0xFF, 0xFF, 0xFF, 0x0F }, .value = 0xFFFFFFFFu, .error = {} },
        { .bytes = { 0x80, 0x80, 0x80, 0x80, 0x10 }, .value = {}, .error = DecodeErrorCode::IntegerTooLarge },
        { .bytes = { 0x80, 0x80, 0x80, 0x80, 0x80, 0x00 },
          .value = {},
          .error = DecodeErrorCode::IntegerRepresentationTooLong },
        { .bytes = { 0x80 }, .value = {}, .error = DecodeErrorCode::UnexpectedEnd },
        { .bytes = { 0x83, 0x00 }, .value = 3u, .error = {} }, // a non-minimal encoding is valid
    });
    checkRows<uint32_t>(rows, [](ByteReader& reader) { return reader.readU32(); });
}

TEST_CASE("ByteReader.readU64")
{
    auto const rows = std::to_array<Row<uint64_t>>({
        { .bytes = { 0x7F }, .value = 127u, .error = {} },
        { .bytes = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01 },
          .value = std::numeric_limits<uint64_t>::max(),
          .error = {} },
        { .bytes = { 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x02 },
          .value = {},
          .error = DecodeErrorCode::IntegerTooLarge },
        { .bytes = { 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00 },
          .value = {},
          .error = DecodeErrorCode::IntegerRepresentationTooLong },
    });
    checkRows<uint64_t>(rows, [](ByteReader& reader) { return reader.readU64(); });
}

TEST_CASE("ByteReader.readS32")
{
    auto const rows = std::to_array<Row<int32_t>>({
        { .bytes = { 0x7F }, .value = -1, .error = {} },
        { .bytes = { 0x3F }, .value = 63, .error = {} },
        { .bytes = { 0xFF, 0xFF, 0xFF, 0xFF, 0x07 },
          .value = std::numeric_limits<int32_t>::max(),
          .error = {} },
        { .bytes = { 0x80, 0x80, 0x80, 0x80, 0x78 },
          .value = std::numeric_limits<int32_t>::min(),
          .error = {} },
        { .bytes = { 0xFF, 0xFF, 0xFF, 0xFF, 0x4F }, .value = {}, .error = DecodeErrorCode::IntegerTooLarge },
        { .bytes = { 0xFF, 0xFF, 0xFF, 0xFF, 0x0F }, .value = {}, .error = DecodeErrorCode::IntegerTooLarge },
        { .bytes = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F },
          .value = {},
          .error = DecodeErrorCode::IntegerRepresentationTooLong },
        { .bytes = { 0xFF }, .value = {}, .error = DecodeErrorCode::UnexpectedEnd },
    });
    checkRows<int32_t>(rows, [](ByteReader& reader) { return reader.readS32(); });
}

TEST_CASE("ByteReader.readS33")
{
    auto const rows = std::to_array<Row<int64_t>>({
        { .bytes = { 0x40 }, .value = -64, .error = {} },
        { .bytes = { 0xFF, 0xFF, 0xFF, 0xFF, 0x0F }, .value = int64_t { 0xFFFFFFFF }, .error = {} },
        { .bytes = { 0x80, 0x80, 0x80, 0x80, 0x70 }, .value = -(int64_t { 1 } << 32), .error = {} },
        { .bytes = { 0x80, 0x80, 0x80, 0x80, 0x10 }, .value = {}, .error = DecodeErrorCode::IntegerTooLarge },
        { .bytes = { 0x80, 0x80, 0x80, 0x80, 0x80, 0x00 },
          .value = {},
          .error = DecodeErrorCode::IntegerRepresentationTooLong },
    });
    checkRows<int64_t>(rows, [](ByteReader& reader) { return reader.readS33(); });
}

TEST_CASE("ByteReader.readS64")
{
    auto const rows = std::to_array<Row<int64_t>>({
        { .bytes = { 0x7F }, .value = -1, .error = {} },
        { .bytes = { 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x7F },
          .value = std::numeric_limits<int64_t>::min(),
          .error = {} },
        { .bytes = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 },
          .value = std::numeric_limits<int64_t>::max(),
          .error = {} },
        { .bytes = { 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01 },
          .value = {},
          .error = DecodeErrorCode::IntegerTooLarge },
        { .bytes = { 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00 },
          .value = {},
          .error = DecodeErrorCode::IntegerRepresentationTooLong },
    });
    checkRows<int64_t>(rows, [](ByteReader& reader) { return reader.readS64(); });
}

TEST_CASE("ByteReader.readFixed")
{
    auto const bytes =
        toBytes(std::to_array<uint8_t>({ 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09 }));
    auto reader = ByteReader { bytes };
    CHECK(reader.readFixedU32() == 0x04030201u);
    CHECK(reader.remaining() == 5);
    auto const tooShort = reader.readFixedU64();
    REQUIRE_FALSE(tooShort.has_value());
    CHECK(tooShort.error().code == DecodeErrorCode::UnexpectedEnd);

    auto again = ByteReader { std::span { bytes }.subspan(1) };
    CHECK(again.readFixedU64() == 0x0908070605040302u);
    CHECK(again.atEnd());
}

TEST_CASE("ByteReader.readLength")
{
    // The bound is the bytes left before the length is read, its own LEB128 bytes included.
    struct LengthRow
    {
        std::vector<uint8_t> bytes {};
        std::optional<uint32_t> value {};
    };
    auto const rows = std::to_array<LengthRow>({
        { .bytes = { 0x00 }, .value = 0 },
        { .bytes = { 0x01 }, .value = 1 },        // one byte left before the length: in bounds
        { .bytes = { 0x02, 0xAA }, .value = 2 },  // two left before it
        { .bytes = { 0x03, 0xAA }, .value = {} }, // three > two
        { .bytes = { 0x82, 0x00 }, .value = 2 },  // a two-byte length counts both of its bytes
        { .bytes = { 0xFF, 0xFF, 0xFF, 0xFF, 0x0F }, .value = {} },
    });
    for (auto const& row: rows)
    {
        auto const bytes = toBytes(row.bytes);
        auto reader = ByteReader { bytes, 10 };
        auto const result = reader.readLength();
        INFO("length of " << row.bytes.size() << " bytes");
        if (row.value)
        {
            REQUIRE(result.has_value());
            CHECK(*result == *row.value);
        }
        else
        {
            REQUIRE_FALSE(result.has_value());
            CHECK(result.error().code == DecodeErrorCode::LengthOutOfBounds);
            CHECK(result.error().offset == 10);
        }
    }

    auto const truncated = toBytes(std::to_array<uint8_t>({ 0x80 }));
    auto reader = ByteReader { truncated };
    CHECK(reader.readLength().error().code == DecodeErrorCode::UnexpectedEnd);
}

TEST_CASE("ByteReader.readName")
{
    struct NameRow
    {
        std::vector<uint8_t> bytes {};
        std::optional<std::string_view> value {};
        std::optional<DecodeErrorCode> error {};
    };
    auto const rows = std::to_array<NameRow>({
        { .bytes = { 0x02, 'h', 'i' }, .value = "hi", .error = {} },
        { .bytes = { 0x00 }, .value = "", .error = {} },
        { .bytes = { 0x02, 0xC3, 0xA4 }, .value = "\xC3\xA4", .error = {} },                     // U+00E4
        { .bytes = { 0x04, 0xF4, 0x8F, 0xBF, 0xBF }, .value = "\xF4\x8F\xBF\xBF", .error = {} }, // U+10FFFF
        { .bytes = { 0x01, 0xFF }, .value = {}, .error = DecodeErrorCode::MalformedUtf8Encoding },
        { .bytes = { 0x03, 0xED, 0xA0, 0x80 }, .value = {}, .error = DecodeErrorCode::MalformedUtf8Encoding },
        { .bytes = { 0x02, 0xC0, 0x80 },
          .value = {},
          .error = DecodeErrorCode::MalformedUtf8Encoding }, // overlong
        { .bytes = { 0x03, 0xE0, 0x9F, 0xBF }, .value = {}, .error = DecodeErrorCode::MalformedUtf8Encoding },
        { .bytes = { 0x04, 0xF4, 0x90, 0x80, 0x80 },
          .value = {},
          .error = DecodeErrorCode::MalformedUtf8Encoding },
        { .bytes = { 0x01, 0x80 },
          .value = {},
          .error = DecodeErrorCode::MalformedUtf8Encoding }, // lone tail
        { .bytes = { 0x01, 0xC3 },
          .value = {},
          .error = DecodeErrorCode::MalformedUtf8Encoding }, // truncated
        // Past every byte left, the length prefix's own included: out of bounds.
        { .bytes = { 0x05, 'a' }, .value = {}, .error = DecodeErrorCode::LengthOutOfBounds },
        // Within the bytes left only by counting the prefix: in bounds, then the bytes run out.
        { .bytes = { 0x02, 'a' }, .value = {}, .error = DecodeErrorCode::UnexpectedEnd },
    });
    for (auto const& row: rows)
    {
        auto const bytes = toBytes(row.bytes);
        auto reader = ByteReader { bytes };
        auto const result = reader.readName();
        INFO("name of " << row.bytes.size() << " bytes");
        if (row.value)
        {
            REQUIRE(result.has_value());
            CHECK(*result == *row.value);
        }
        else
        {
            REQUIRE_FALSE(result.has_value());
            CHECK(result.error().code == *row.error);
        }
    }
}

TEST_CASE("ByteReader.offsetIsAbsolute")
{
    auto const bytes = toBytes(std::to_array<uint8_t>({ 0x2A }));
    auto reader = ByteReader { bytes, 100 };
    CHECK(reader.offset() == 100);
    CHECK(reader.readByte() == std::byte { 0x2A });
    CHECK(reader.offset() == 101);
    auto const pastEnd = reader.readByte();
    REQUIRE_FALSE(pastEnd.has_value());
    CHECK(pastEnd.error().code == DecodeErrorCode::UnexpectedEnd);
    CHECK(pastEnd.error().offset == 101);
}

TEST_CASE("ByteReader.errorOffsetsFollowTheSpecInterpreter")
{
    // integer too large: at the offending byte; integer representation too long: at the byte past the limit.
    auto const tooLarge = toBytes(std::to_array<uint8_t>({ 0x80, 0x80, 0x80, 0x80, 0x10 }));
    auto largeReader = ByteReader { tooLarge, 10 };
    CHECK(largeReader.readU32().error().offset == 14);

    auto const tooLong = toBytes(std::to_array<uint8_t>({ 0x80, 0x80, 0x80, 0x80, 0x80, 0x00 }));
    auto longReader = ByteReader { tooLong, 10 };
    CHECK(longReader.readU32().error().offset == 15);

    // A signed value too large for its width is reported at its offending byte too.
    auto const signedTooLarge = toBytes(std::to_array<uint8_t>({ 0xFF, 0xFF, 0xFF, 0xFF, 0x4F }));
    auto signedReader = ByteReader { signedTooLarge, 20 };
    auto const signedResult = signedReader.readS32();
    REQUIRE_FALSE(signedResult.has_value());
    CHECK(signedResult.error().code == DecodeErrorCode::IntegerTooLarge);
    CHECK(signedResult.error().offset == 24);

    // A malformed name is reported at its length prefix, not at its first byte of text.
    auto const badName = toBytes(std::to_array<uint8_t>({ 0x01, 0xFF }));
    auto nameReader = ByteReader { badName, 30 };
    auto const nameResult = nameReader.readName();
    REQUIRE_FALSE(nameResult.has_value());
    CHECK(nameResult.error().code == DecodeErrorCode::MalformedUtf8Encoding);
    CHECK(nameResult.error().offset == 30);
}

TEST_CASE("ByteReader.readBytes")
{
    auto const bytes = toBytes(std::to_array<uint8_t>({ 1, 2, 3 }));
    auto reader = ByteReader { bytes, 7 };
    auto const head = reader.readBytes(2);
    REQUIRE(head.has_value());
    CHECK(head->size() == 2);
    CHECK((*head)[1] == std::byte { 2 });
    CHECK(reader.offset() == 9);
    CHECK(reader.remaining() == 1);

    auto const tooMany = reader.readBytes(2);
    REQUIRE_FALSE(tooMany.has_value());
    CHECK(tooMany.error().code == DecodeErrorCode::UnexpectedEnd);
    CHECK(tooMany.error().offset == 10);
    CHECK(reader.remaining() == 1);
}

TEST_CASE("DecodeError.specMessages")
{
    using isoline::binary::specMessage;

    struct MessageRow
    {
        DecodeErrorCode code;
        std::string_view message;
    };
    // The messages the spec suite expects, as fixed by the decoder's brief.
    auto const rows = std::to_array<MessageRow>({
        // The suite expects both this and its prefix "unexpected end".
        { .code = DecodeErrorCode::UnexpectedEnd, .message = "unexpected end of section or function" },
        { .code = DecodeErrorCode::IntegerTooLarge, .message = "integer too large" },
        { .code = DecodeErrorCode::IntegerRepresentationTooLong,
          .message = "integer representation too long" },
        { .code = DecodeErrorCode::MalformedUtf8Encoding, .message = "malformed UTF-8 encoding" },
        { .code = DecodeErrorCode::MagicHeaderNotDetected, .message = "magic header not detected" },
        { .code = DecodeErrorCode::UnknownBinaryVersion, .message = "unknown binary version" },
        { .code = DecodeErrorCode::MalformedSectionId, .message = "malformed section id" },
        { .code = DecodeErrorCode::SectionSizeMismatch, .message = "section size mismatch" },
        { .code = DecodeErrorCode::JunkAfterLastSection, .message = "junk after last section" },
        { .code = DecodeErrorCode::TooManyLocals, .message = "too many locals" },
        { .code = DecodeErrorCode::LocalCountOverflow, .message = "too many locals" },
        { .code = DecodeErrorCode::IllegalOpcode, .message = "illegal opcode" },
        { .code = DecodeErrorCode::MalformedReferenceType, .message = "malformed reference type" },
        { .code = DecodeErrorCode::MalformedImportKind, .message = "malformed import kind" },
        { .code = DecodeErrorCode::MalformedMutability, .message = "malformed mutability" },
        // WebAssembly 2.0 reads a limits flag as a one-bit LEB128, so a flag above 1 is too large.
        { .code = DecodeErrorCode::LimitsFlagTooLarge, .message = "integer too large" },
        { .code = DecodeErrorCode::FunctionAndCodeSectionInconsistent,
          .message = "function and code section have inconsistent lengths" },
        { .code = DecodeErrorCode::DataCountAndDataSectionInconsistent,
          .message = "data count and data section have inconsistent lengths" },
        { .code = DecodeErrorCode::DataCountSectionRequired, .message = "data count section required" },
        { .code = DecodeErrorCode::UnexpectedContentAfterEnd,
          .message = "unexpected content after last section" },
        { .code = DecodeErrorCode::ZeroByteExpected, .message = "zero byte expected" },
        { .code = DecodeErrorCode::LengthOutOfBounds, .message = "length out of bounds" },
        { .code = DecodeErrorCode::SectionOutOfOrder, .message = "unexpected content after last section" },
        { .code = DecodeErrorCode::DuplicateSection, .message = "unexpected content after last section" },
        { .code = DecodeErrorCode::MalformedExportKind, .message = "malformed export kind" },
        { .code = DecodeErrorCode::MalformedElementKind, .message = "malformed element kind" },
        { .code = DecodeErrorCode::MalformedDataSegmentKind, .message = "malformed data segment kind" },
        { .code = DecodeErrorCode::MalformedFunctionType, .message = "malformed function type" },
        { .code = DecodeErrorCode::EndOpcodeExpected, .message = "END opcode expected" },
        { .code = DecodeErrorCode::MalformedMemopFlags, .message = "malformed memop flags" },
    });
    for (auto const& row: rows)
    {
        INFO("expected message: " << row.message);
        CHECK(specMessage(row.code) == row.message);
    }
    for (auto const& row: isoline::binary::DecodeErrorTable)
        CHECK_FALSE(row.specMessage.empty());
}
