// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <isoline/api/Types.hpp>
#include <isoline/component/canonical/GuestMemory.hpp>
#include <isoline/component/canonical/LiftLower.hpp>
#include <isoline/component/canonical/LiftSteps.hpp>
#include <isoline/component/canonical/Strings.hpp>
#include <isoline/component/canonical/Trap.hpp>
#include <isoline/component/canonical/Values.hpp>
#include <isoline/component/canonical/testing/GuestDoubles.hpp>
#include <isoline/component/canonical/testing/Sides.hpp>
#include <isoline/component/types/TypeArena.hpp>

using isoline::api::BulkPollBytes;
using isoline::api::I32;
using isoline::api::Value;
using isoline::component::canonical::equal;
using isoline::component::canonical::liftFlat;
using isoline::component::canonical::LiftSteps;
using isoline::component::canonical::liftString;
using isoline::component::canonical::MaxFlatParams;
using isoline::component::canonical::MaxStringByteLength;
using isoline::component::canonical::OverwritePoison;
using isoline::component::canonical::SimpleEncoding;
using isoline::component::canonical::storeString;
using isoline::component::canonical::StringEncoding;
using isoline::component::canonical::ThisBuildPoisons;
using isoline::component::canonical::transcodeRow;
using isoline::component::canonical::TranscodeTable;
using isoline::component::canonical::TrapCode;
using isoline::component::canonical::trapOf;
using isoline::component::canonical::Utf16Tag;
using isoline::component::canonical::ValueBuilder;
using isoline::component::canonical::Values;
using isoline::component::canonical::testing::bytesOf;
using isoline::component::canonical::testing::FakeGuestMemoryOptions;
using isoline::component::canonical::testing::lifted;
using isoline::component::canonical::testing::Reallocation;
using isoline::component::canonical::testing::Side;
using isoline::component::canonical::testing::trapIn;
using isoline::component::canonical::testing::ZeroGuestMemory;
using isoline::component::types::ValueKind;

namespace
{

/// What definitions.py's store_string_into_range does for one source string and destination encoding.
struct StoreVector
{
    StringEncoding from;                     ///< The source string's encoding.
    uint32_t sourceTaggedCodeUnits;          ///< Its tagged code units.
    std::string_view sourceHex;              ///< Its bytes, in hex.
    StringEncoding to;                       ///< The destination's encoding.
    std::vector<Reallocation> reallocations; ///< The `realloc` calls, in order.
    uint32_t pointer;                        ///< The block returned.
    uint32_t taggedCodeUnits;                ///< The tagged code units returned.
    std::string_view storedHex;              ///< The bytes stored there, in hex.
};

/// The vectors, made by testing/make-transcode-vectors.py from definitions.py at the pin.
[[nodiscard]] std::vector<StoreVector> storeVectors()
{
    return {
#include <isoline/component/canonical/testing/TranscodeVectors.inc>
    };
}

/// The bytes @p hex spells.
[[nodiscard]] std::vector<std::byte> fromHex(std::string_view hex)
{
    auto bytes = std::vector<std::byte> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, hex.size() / 2))
        bytes.push_back(
            static_cast<std::byte>(std::stoul(std::string { hex.substr(2 * index, 2) }, nullptr, 16)));
    return bytes;
}

/// The core values of a string's pointer pair.
[[nodiscard]] std::vector<Value> pairOf(uint32_t address, uint32_t taggedCodeUnits)
{
    return { I32 { address }, I32 { taggedCodeUnits } };
}

/// The string @p bytes lift to, put at @p address of @p side's memory, in @p encoding.
[[nodiscard]] Values liftedString(Side& side,
                                  StringEncoding encoding,
                                  std::span<std::byte const> bytes,
                                  uint32_t taggedCodeUnits,
                                  uint32_t address = 0)
{
    side.memory().put(address, bytes);
    auto context = side.context(encoding);
    return lifted(context, { side.primitive(ValueKind::String) }, pairOf(address, taggedCodeUnits));
}

} // namespace

TEST_CASE("Strings.transcodeTable")
{
    // Every vector: the source lifts, stores with definitions.py's `realloc` calls and bytes, and the
    // stored string lifts back equal to it. Together the vectors reach every row of TranscodeTable.
    auto rowsReached = std::set<std::size_t> {};
    for (auto const& vector: storeVectors())
    {
        CAPTURE(vector.sourceHex, static_cast<int>(vector.from), static_cast<int>(vector.to));
        auto source = Side {};
        auto const values =
            liftedString(source, vector.from, fromHex(vector.sourceHex), vector.sourceTaggedCodeUnits);
        auto const text = values.root(0).string();
        auto const& row = transcodeRow(text.encoding, text.simpleEncoding(), vector.to);
        rowsReached.insert(static_cast<std::size_t>(&row - TranscodeTable.data()));

        auto destination = Side {};
        auto context = destination.context(vector.to);
        auto const stored = storeString(context, text);
        REQUIRE(stored.has_value());
        CHECK(destination.memory().reallocations() == vector.reallocations);
        CHECK(*stored == std::pair { vector.pointer, vector.taggedCodeUnits });
        auto const expected = fromHex(vector.storedHex);
        CHECK(std::ranges::equal(destination.memory().bytes().subspan(vector.pointer, expected.size()),
                                 expected));
        auto relifted = destination.context(vector.to);
        auto const back = lifted(
            relifted, { destination.primitive(ValueKind::String) }, pairOf(stored->first, stored->second));
        CHECK(equal(back.root(0), values.root(0)));
    }
    CHECK(rowsReached.size() == TranscodeTable.size());
}

TEST_CASE("Strings.latin1Utf16Tag")
{
    auto const ab = bytesOf({ 'A', 0, 'B', 0 });

    // The tag bit selects UTF-16: two code units, four bytes.
    auto side = Side {};
    auto const tagged = liftedString(side, StringEncoding::Latin1Utf16, ab, 2 | Utf16Tag);
    CHECK(tagged.root(0).string().simpleEncoding() == SimpleEncoding::Utf16);
    CHECK(tagged.root(0).string().bytes.size() == 4);
    CHECK(tagged.root(0).string().utf8() == "AB");

    // Without it, Latin-1: two bytes, "A\0".
    auto const latin1 = liftedString(side, StringEncoding::Latin1Utf16, ab, 2);
    CHECK(latin1.root(0).string().simpleEncoding() == SimpleEncoding::Latin1);
    CHECK(latin1.root(0).string().utf8() == std::string { "A\0", 2 });

    // UTF-16 that Latin-1 holds is stored probably-UTF-16: copied, then shrunk to Latin-1 in place.
    auto destination = Side {};
    auto context = destination.context(StringEncoding::Latin1Utf16);
    auto const stored = storeString(context, tagged.root(0).string());
    REQUIRE(stored.has_value());
    CHECK(stored->second == 2);
    CHECK(destination.memory().reallocations()
          == std::vector {
              Reallocation { .old = 0, .oldSize = 0, .alignment = 2, .newSize = 4, .result = 1024 },
              Reallocation { .old = 1024, .oldSize = 4, .alignment = 2, .newSize = 2, .result = 1028 } });
    CHECK(std::ranges::equal(destination.memory().bytes().subspan(stored->first, 2), bytesOf({ 'A', 'B' })));
}

TEST_CASE("Strings.invalidEncodingTraps")
{
    struct Case
    {
        StringEncoding encoding;
        std::vector<std::byte> bytes;
        uint32_t taggedCodeUnits;
        char const* name;
    };
    auto const invalid = GENERATE(values<Case>({
        { StringEncoding::Utf8, bytesOf({ 0xc3, 0x28 }), 2, "utf-8" },       // a lead without continuation
        { StringEncoding::Utf8, bytesOf({ 'a', 0xc3 }), 2, "utf-8" },        // truncated at the end
        { StringEncoding::Utf8, bytesOf({ 0xff }), 1, "utf-8" },             // never valid
        { StringEncoding::Utf8, bytesOf({ 0xed, 0xa0, 0x80 }), 3, "utf-8" }, // an encoded surrogate
        { StringEncoding::Utf8, bytesOf({ 0xe0, 0x80, 0x80 }), 3, "utf-8" }, // an overlong 3-byte form
        { StringEncoding::Utf8, bytesOf({ 0xf0, 0x80, 0x80, 0x80 }), 4, "utf-8" }, // an overlong 4-byte form
        { StringEncoding::Utf8, bytesOf({ 0xf5, 0x80, 0x80, 0x80 }), 4, "utf-8" }, // past U+10FFFF
        { StringEncoding::Utf16, bytesOf({ 0x00, 0xd8 }), 1, "utf-16" },           // a lone high surrogate
        { StringEncoding::Utf16, bytesOf({ 0x00, 0xdc, 'a', 0 }), 2, "utf-16" },   // a lone low surrogate
        { StringEncoding::Latin1Utf16, bytesOf({ 'a', 0, 0x00, 0xd8 }), 2 | Utf16Tag, "utf-16" },
    }));
    auto side = Side {};
    side.memory().put(0, invalid.bytes);
    auto context = side.context(invalid.encoding);
    auto const trap = trapIn(liftFlat(context,
                                      std::vector { side.primitive(ValueKind::String) },
                                      pairOf(0, invalid.taggedCodeUnits),
                                      MaxFlatParams));
    CHECK(trap == trapOf(TrapCode::InvalidEncoding, invalid.name));
    CHECK(trap.message() == std::string { "invalid " } + invalid.name);
    CHECK(side.budget().live() == 0);
}

TEST_CASE("Strings.overLongTraps")
{
    // A memory of a gibibyte, so only the length check stands between a lying length and the host: the
    // budget of 4 KiB would refuse what passed it (LiveValueBytes), and nothing is read either way.
    auto memory = ZeroGuestMemory { uint64_t { 1 } << 30U, 0 };
    auto side = Side { FakeGuestMemoryOptions {}, 4096 };
    auto const string = std::vector { side.primitive(ValueKind::String) };
    struct Case
    {
        StringEncoding encoding;
        uint32_t taggedCodeUnits;
        TrapCode code;
    };
    auto const length = GENERATE(values<Case>({
        { StringEncoding::Utf8, uint32_t { 1 } << 28U, TrapCode::OutOfBounds },
        { StringEncoding::Utf8, MaxStringByteLength, TrapCode::LiveValueBytes },
        { StringEncoding::Utf16, (uint32_t { 1 } << 27U) + 1, TrapCode::OutOfBounds },
        { StringEncoding::Utf16, (uint32_t { 1 } << 27U) - 1, TrapCode::LiveValueBytes },
        { StringEncoding::Utf16, ~uint32_t { 0 }, TrapCode::OutOfBounds },
        { StringEncoding::Latin1Utf16, ~uint32_t { 0 }, TrapCode::OutOfBounds },
        { StringEncoding::Latin1Utf16, (uint32_t { 1 } << 27U) | Utf16Tag, TrapCode::OutOfBounds },
        { StringEncoding::Latin1Utf16, uint32_t { 1 } << 28U, TrapCode::OutOfBounds },
    }));
    auto context = side.contextOver(memory, length.encoding);
    auto const trap = trapIn(liftFlat(context, string, pairOf(0, length.taggedCodeUnits), MaxFlatParams));
    CHECK(trap.code == length.code);
    CHECK(memory.readBytes() == 0);
    CHECK(side.budget().live() == 0);
}

TEST_CASE("Strings.bounds")
{
    // test/values/strings.wast: an empty string may sit at the memory's end, not past it; a string one
    // byte past the end traps; latin1+utf16 is 2-aligned even for Latin-1.
    auto side = Side { FakeGuestMemoryOptions { .size = 64 } };
    auto const string = std::vector { side.primitive(ValueKind::String) };
    auto context = side.context();
    CHECK(lifted(context, string, pairOf(64, 0)).root(0).string().bytes.empty());
    CHECK(trapIn(liftFlat(context, string, pairOf(65, 0), MaxFlatParams)).code == TrapCode::OutOfBounds);
    CHECK(trapIn(liftFlat(context, string, pairOf(63, 2), MaxFlatParams)).code == TrapCode::OutOfBounds);
    auto latin1 = side.context(StringEncoding::Latin1Utf16);
    CHECK(trapIn(liftFlat(latin1, string, pairOf(1, 1), MaxFlatParams))
          == trapOf(TrapCode::UnalignedPointer, 1, 2));
    auto utf16 = side.context(StringEncoding::Utf16);
    CHECK(trapIn(liftFlat(utf16, string, pairOf(3, 1), MaxFlatParams))
          == trapOf(TrapCode::UnalignedPointer, 3, 2));
    CHECK(side.memory().reads().empty());
}

TEST_CASE("Strings.reallocMisbehaviour")
{
    auto source = Side {};
    auto const snowman = liftedString(source, StringEncoding::Utf8, bytesOf({ 0xe2, 0x98, 0x83 }), 3);
    auto const ascii = std::vector<std::byte>(0x20, std::byte { 'x' });
    auto const ascii32 = liftedString(source, StringEncoding::Utf8, ascii, 0x20, 64);
    struct Case
    {
        std::vector<uint32_t> forced;
        StringEncoding to;
        bool snowmanSource;
        TrapCode code;
    };
    auto const misbehaving = GENERATE(values<Case>({
        // An odd block for a 2-aligned UTF-16 destination.
        { { 3 }, StringEncoding::Utf16, true, TrapCode::UnalignedPointer },
        // A block whose last byte passes the memory's end.
        { { 4096 - 1 }, StringEncoding::Utf8, false, TrapCode::OutOfBounds },
        // A block whose end wraps past 2^32 in 32-bit arithmetic.
        { { 0xffff'fff0 }, StringEncoding::Utf8, false, TrapCode::OutOfBounds },
        // A good first block, then a misaligned shrink (store_utf8_to_utf16's second call).
        { { 1024, 1025 }, StringEncoding::Utf16, true, TrapCode::UnalignedPointer },
        // A good first block, then an out-of-bounds widen (store_string_to_latin1_or_utf16's second call).
        { { 1024, 0xffff'fff0 }, StringEncoding::Latin1Utf16, true, TrapCode::OutOfBounds },
    }));
    auto destination = Side { FakeGuestMemoryOptions { .forcedResults = misbehaving.forced } };
    auto context = destination.context(misbehaving.to);
    auto const& value = misbehaving.snowmanSource ? snowman : ascii32;
    auto const trap = trapIn(storeString(context, value.root(0).string()));
    CHECK(trap.code == misbehaving.code);
    CHECK(destination.memory().reallocations().size() == misbehaving.forced.size());
    if (misbehaving.forced.size() == 1)
        CHECK(destination.memory().writes().empty());
}

TEST_CASE("Strings.transcodePollsInterrupt")
{
    // 1 MiB of UTF-16 lowered into UTF-8 polls at least once per BulkPollBytes of the source it decodes,
    // however few bytes it writes: ASCII (512 KiB written, the block never grows) as U+00E9 (1 MiB). An
    // interrupt at the third poll ends the store there.
    constexpr auto Units = uint32_t { 1 } << 19U;
    auto const unit = GENERATE(std::byte { 'a' }, std::byte { 0xe9 });
    auto const ascii = unit == std::byte { 'a' };
    auto const outputBytes = uint64_t { Units } * (ascii ? 1 : 2);
    auto utf16 = std::vector<std::byte> {};
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, Units))
        utf16.insert(utf16.end(), { unit, std::byte { 0 } });
    auto source = Side { FakeGuestMemoryOptions { .size = utf16.size() } };
    auto const values = liftedString(source, StringEncoding::Utf16, utf16, Units);

    auto const interruptAt = GENERATE(std::optional<uint32_t> {}, std::optional<uint32_t> { 3 });
    auto destination = Side { FakeGuestMemoryOptions { .interruptAtPoll = interruptAt } };
    auto context = destination.context(StringEncoding::Utf8);
    auto const stored = storeString(context, values.root(0).string());
    auto written = uint64_t { 0 };
    for (auto const& write: destination.memory().writes())
        written += write.length;
    if (!interruptAt)
    {
        REQUIRE(stored.has_value());
        CHECK(stored->second == outputBytes);
        CHECK(destination.memory().polls() >= (uint64_t { 2 } * Units) / BulkPollBytes);
        CHECK(written == outputBytes);
        return;
    }
    CHECK(trapIn(stored) == trapOf(TrapCode::Interrupted));
    CHECK(destination.memory().polls() == 3);
    // Three granules of work at most were done: nothing was written after the third poll, and the
    // final shrink never ran.
    CHECK(written <= 3 * BulkPollBytes);
    CHECK(destination.memory().reallocations().size() == (ascii ? 1 : 2));
}

TEST_CASE("Strings.tooLongToStoreTraps")
{
    // A host string past MaxStringByteLength traps before any `realloc`: every worst-case size of a
    // shorter one fits a 32-bit realloc.
    auto side = Side { FakeGuestMemoryOptions {}, uint64_t { 1 } << 30U };
    auto builder = ValueBuilder { side.arena(),
                                  std::vector { side.primitive(ValueKind::String) },
                                  side.budgetPointer() };
    // Zero bytes: Latin-1 NULs, text.
    CHECK(builder.encodedString(StringEncoding::Latin1Utf16, MaxStringByteLength + 1).size()
          == MaxStringByteLength + 1);
    auto values = builder.finish();
    REQUIRE(values.has_value());
    auto context = side.context(StringEncoding::Utf8);
    CHECK(trapIn(storeString(context, values->root(0).string())).code == TrapCode::OutOfBounds);
    CHECK(side.memory().reallocations().empty());
}

TEST_CASE("Strings.unvalidatedBytesTrapOnStore")
{
    // encodedString() leaves validation to its caller: a host string that is not text traps InvalidEncoding
    // when stored, in every row that reads it, the copies included, so no guest receives one.
    struct Case
    {
        StringEncoding from;
        uint32_t taggedCodeUnits;
        std::vector<std::byte> bytes;
        StringEncoding to;
        char const* name;
    };
    auto const loneSurrogate = bytesOf({ 'a', 0, 0x00, 0xd8 });
    auto const notUtf8 = bytesOf({ 0xc3, 0x28 });
    auto const encodedSurrogate = bytesOf({ 0xed, 0xa0, 0x80 });
    auto const invalid = GENERATE_COPY(values<Case>({
        { StringEncoding::Utf8, 2, notUtf8, StringEncoding::Utf8, "utf-8" },          // Utf8Copy
        { StringEncoding::Utf8, 3, encodedSurrogate, StringEncoding::Utf8, "utf-8" }, // Utf8Copy
        { StringEncoding::Utf8, 2, notUtf8, StringEncoding::Utf16, "utf-8" },         // Utf8ToUtf16
        { StringEncoding::Utf8, 2, notUtf8, StringEncoding::Latin1Utf16, "utf-8" },   // StringToLatin1OrUtf16
        { StringEncoding::Utf16, 2, loneSurrogate, StringEncoding::Utf8, "utf-16" },  // Utf16ToUtf8
        { StringEncoding::Utf16, 2, loneSurrogate, StringEncoding::Utf16, "utf-16" }, // Utf16Copy
        { StringEncoding::Latin1Utf16,
          2 | Utf16Tag,
          loneSurrogate,
          StringEncoding::Utf16,
          "utf-16" }, // Utf16Copy
        { StringEncoding::Latin1Utf16, 2 | Utf16Tag, loneSurrogate, StringEncoding::Latin1Utf16, "utf-16" },
    }));
    auto const store = [](Case const& string, std::vector<std::byte> const& bytes) {
        auto side = Side {};
        auto builder = ValueBuilder { side.arena(),
                                      std::vector { side.primitive(ValueKind::String) },
                                      side.budgetPointer() };
        auto const stored = builder.encodedString(string.from, string.taggedCodeUnits);
        REQUIRE(stored.size() == bytes.size());
        std::ranges::copy(bytes, stored.begin());
        auto values = builder.finish();
        REQUIRE(values.has_value());
        auto context = side.context(string.to);
        return trapIn(storeString(context, values->root(0).string()));
    };
    CHECK(store(invalid, invalid.bytes) == trapOf(TrapCode::InvalidEncoding, invalid.name));

    // The fault past a granule of valid text: the copy decodes every granule before it writes it.
    auto const unitBytes = invalid.from == StringEncoding::Utf8 ? std::size_t { 1 } : std::size_t { 2 };
    auto const prefix = (2 * BulkPollBytes) / unitBytes;
    auto longer = std::vector<std::byte> {};
    for ([[maybe_unused]] auto const unit: std::views::iota(std::size_t { 0 }, prefix))
        longer.insert(longer.end(), { std::byte { 'a' }, std::byte {} });
    longer.resize(prefix * unitBytes);
    longer.insert(longer.end(), invalid.bytes.begin(), invalid.bytes.end());
    auto longerString = invalid;
    longerString.taggedCodeUnits += static_cast<uint32_t>(prefix);
    CHECK(store(longerString, longer) == trapOf(TrapCode::InvalidEncoding, invalid.name));
}

TEST_CASE("Strings.validationAcrossGranules")
{
    // Strings are copied and validated a BulkPollBytes granule at a time: a code point split by a
    // granule's end is whole once the next arrives, and a fault in the last granule still traps.
    auto text = std::vector<std::byte>(BulkPollBytes - 1, std::byte { 'a' });
    auto const tail = bytesOf({ 0xc3, 0xa9, 0xf0, 0x9f, 0x98, 0x80 }); // "é😀", é split by the granule
    text.insert(text.end(), tail.begin(), tail.end());
    auto side = Side { FakeGuestMemoryOptions { .size = 2 * BulkPollBytes } };
    auto const values = liftedString(side, StringEncoding::Utf8, text, static_cast<uint32_t>(text.size()));
    CHECK(values.root(0).string().bytes.size() == text.size());
    CHECK(side.memory().reads().size() == 2);
    CHECK(side.memory().polls() == 1);

    text.back() = std::byte { 'x' };
    side.memory().put(0, text);
    auto context = side.context();
    CHECK(trapIn(liftFlat(context,
                          std::vector { side.primitive(ValueKind::String) },
                          pairOf(0, static_cast<uint32_t>(text.size())),
                          MaxFlatParams))
          == trapOf(TrapCode::InvalidEncoding, "utf-8"));
}

TEST_CASE("Strings.everyFailurePointTraps")
{
    // Every TranscodeTable row, on a string long enough to poll and to take its growth path: an interrupt
    // at any of its polls, a failing read or write, or a misbehaving `realloc` at any of its calls, ends
    // the store in that trap.
    struct Source
    {
        StringEncoding encoding;
        std::vector<std::byte> bytes;
        uint32_t taggedCodeUnits;
    };
    static constexpr auto Repeats = uint32_t { 40'000 };
    auto const followedBy = [](std::vector<std::byte> bytes, std::vector<std::byte> const& tail) {
        bytes.insert(bytes.end(), tail.begin(), tail.end());
        return bytes;
    };
    auto const repeated = [](std::vector<std::byte> const& unit) {
        auto bytes = std::vector<std::byte> {};
        for ([[maybe_unused]] auto const repeat: std::views::iota(uint32_t { 0 }, Repeats))
            bytes.insert(bytes.end(), unit.begin(), unit.end());
        return bytes;
    };
    // "aé☃" in each source form that holds it, "aé" in Latin-1: past ASCII, and but for Latin-1 past it.
    auto const sources = std::vector<Source> {
        { StringEncoding::Utf8, repeated(bytesOf({ 'a', 0xc3, 0xa9, 0xe2, 0x98, 0x83 })), 6 * Repeats },
        { StringEncoding::Utf16, repeated(bytesOf({ 'a', 0, 0xe9, 0, 0x03, 0x26 })), 3 * Repeats },
        { StringEncoding::Latin1Utf16,
          repeated(bytesOf({ 'a', 0, 0xe9, 0, 0x03, 0x26 })),
          (3 * Repeats) | Utf16Tag },
        { StringEncoding::Latin1Utf16, repeated(bytesOf({ 'a', 0xe9 })), 2 * Repeats },
        { StringEncoding::Latin1Utf16, repeated(bytesOf({ 'a', 0, 0xe9, 0 })), (2 * Repeats) | Utf16Tag },
        // Latin-1 throughout, then for UTF-8 and UTF-16 one code point past it: a long prefix widened in
        // place, read back from guest memory.
        { StringEncoding::Utf8, repeated(bytesOf({ 0xc3, 0xa9 })), 2 * Repeats },
        { StringEncoding::Utf8,
          followedBy(repeated(bytesOf({ 0xc3, 0xa9 })), bytesOf({ 0xe2, 0x98, 0x83 })),
          (2 * Repeats) + 3 },
        { StringEncoding::Utf16,
          followedBy(repeated(bytesOf({ 0xe9, 0 })), bytesOf({ 0x03, 0x26 })),
          Repeats + 1 },
    };
    for (auto const& source: sources)
    {
        auto side = Side { FakeGuestMemoryOptions { .size = source.bytes.size() } };
        auto const values = liftedString(side, source.encoding, source.bytes, source.taggedCodeUnits);
        for (auto const to: { StringEncoding::Utf8, StringEncoding::Utf16, StringEncoding::Latin1Utf16 })
        {
            CAPTURE(static_cast<int>(source.encoding), source.taggedCodeUnits, static_cast<int>(to));
            auto clean = Side {};
            auto cleanContext = clean.context(to);
            REQUIRE(storeString(cleanContext, values.root(0).string()).has_value());
            for (auto const poll: std::views::iota(uint32_t { 1 }, clean.memory().polls() + 1))
            {
                auto interrupted = Side { FakeGuestMemoryOptions { .interruptAtPoll = poll } };
                auto context = interrupted.context(to);
                CHECK(trapIn(storeString(context, values.root(0).string())) == trapOf(TrapCode::Interrupted));
            }
            auto const writes = static_cast<uint32_t>(clean.memory().writes().size());
            for (auto const write: std::views::iota(uint32_t { 1 }, writes + 1))
            {
                auto failing = Side { FakeGuestMemoryOptions { .failAtWrite = write } };
                auto context = failing.context(to);
                CHECK(trapIn(storeString(context, values.root(0).string())).code == TrapCode::OutOfBounds);
            }
            auto const reads = static_cast<uint32_t>(clean.memory().reads().size());
            for (auto const read: std::views::iota(uint32_t { 1 }, reads + 1))
            {
                auto failing = Side { FakeGuestMemoryOptions { .failAtRead = read } };
                auto context = failing.context(to);
                CHECK(trapIn(storeString(context, values.root(0).string())).code == TrapCode::OutOfBounds);
            }
            auto const calls = static_cast<uint32_t>(clean.memory().reallocations().size());
            for (auto const call: std::views::iota(uint32_t { 1 }, calls + 1))
            {
                auto misbehaving = Side { FakeGuestMemoryOptions { .misbehaveAtReallocation = call } };
                auto context = misbehaving.context(to);
                CHECK(trapIn(storeString(context, values.root(0).string())).code == TrapCode::OutOfBounds);
            }
        }
    }
}

TEST_CASE("Strings.liftStringReportsTheBudget")
{
    // liftString() itself reports a refused charge, before it reads anything.
    auto side = Side { FakeGuestMemoryOptions {}, 64 };
    auto builder = ValueBuilder { side.arena(),
                                  std::vector { side.primitive(ValueKind::String) },
                                  side.budgetPointer() };
    auto context = side.context();
    CHECK(trapIn(liftString(context, builder, 0, 1024)) == trapOf(TrapCode::LiveValueBytes, 64));
    CHECK(side.memory().reads().empty());
}

TEST_CASE("Strings.liftFailurePointsTrap")
{
    // A string of three granules: an interrupt at any of its polls, or a failing read, ends the lift and
    // leaves nothing charged.
    constexpr auto Bytes = 3 * BulkPollBytes;
    auto const text = std::vector<std::byte>(Bytes, std::byte { 'z' });
    auto const liftTrap = [&](FakeGuestMemoryOptions options) {
        options.size = Bytes;
        auto side = Side { std::move(options) };
        side.memory().put(0, text);
        auto context = side.context();
        auto const trap = trapIn(liftFlat(
            context, std::vector { side.primitive(ValueKind::String) }, pairOf(0, Bytes), MaxFlatParams));
        CHECK(side.budget().live() == 0);
        return trap;
    };
    for (auto const at: { 1U, 2U, 3U })
    {
        CHECK(liftTrap(FakeGuestMemoryOptions { .interruptAtPoll = at }) == trapOf(TrapCode::Interrupted));
        CHECK(liftTrap(FakeGuestMemoryOptions { .failAtRead = at }).code == TrapCode::OutOfBounds);
    }
}

TEST_CASE("Strings.hostBytesLeftUnwrittenAreZero")
{
    // A host that writes only part of an encodedString() hands the guest zeros there, never stale memory.
    auto side = Side {};
    auto builder = ValueBuilder { side.arena(),
                                  std::vector { side.primitive(ValueKind::String) },
                                  side.budgetPointer() };
    auto const bytes = builder.encodedString(StringEncoding::Utf8, 4);
    REQUIRE(bytes.size() == 4);
    std::ranges::copy(bytesOf({ 'a', 'b' }), bytes.begin());
    auto values = builder.finish();
    REQUIRE(values.has_value());
    auto context = side.context();
    auto const stored = storeString(context, values->root(0).string());
    REQUIRE(stored.has_value());
    CHECK(std::ranges::equal(side.memory().bytes().subspan(stored->first, 4), bytesOf({ 'a', 'b', 0, 0 })));
}

TEST_CASE("Strings.liftStepsPoisonInDebug")
{
    // The lift's own steps are not zeroed: a debug build fills them with 0xFF, so a lift that skipped a
    // byte would hand on text that is not UTF-8, which lowering refuses.
    if constexpr (ThisBuildPoisons == OverwritePoison::Off)
        SKIP("a release build does not poison");
    auto side = Side {};
    auto builder = ValueBuilder { side.arena(),
                                  std::vector { side.primitive(ValueKind::String) },
                                  side.budgetPointer() };
    auto const bytes = LiftSteps::encodedString(builder, StringEncoding::Utf8, 3);
    REQUIRE(bytes.size() == 3);
    bytes[0] = std::byte { 'a' };
    auto values = builder.finish();
    REQUIRE(values.has_value());
    CHECK(std::ranges::equal(values->root(0).string().bytes, bytesOf({ 'a', 0xff, 0xff })));
    auto context = side.context();
    CHECK(trapIn(storeString(context, values->root(0).string()))
          == trapOf(TrapCode::InvalidEncoding, "utf-8"));
}
