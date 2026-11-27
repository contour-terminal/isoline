// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/binary/DecodeError.hpp>
#include <isoline/binary/Decoder.hpp>
#include <isoline/binary/NameSection.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>

using isoline::binary::DecodeErrorCode;
using isoline::binary::decodeModule;
using isoline::binary::decodeNameSection;
using isoline::binary::encodeNameSection;
using isoline::binary::IndirectNameMap;
using isoline::binary::NameAssoc;
using isoline::binary::NameKind;
using isoline::binary::NameSection;
using isoline::binary::NameSectionName;
using isoline::binary::NameSubsections;
using isoline::binary::testing::encodeName;
using isoline::binary::testing::encodeU32;

namespace
{

std::vector<std::byte> bytesOf(std::vector<uint8_t> const& bytes)
{
    auto result = std::vector<std::byte> {};
    result.reserve(bytes.size());
    for (auto const byte: bytes)
        result.push_back(std::byte { byte });
    return result;
}

/// A subsection: its id, its size, then @p contents.
std::vector<uint8_t> subsection(uint8_t id, std::vector<uint8_t> const& contents)
{
    auto bytes = std::vector<uint8_t> { id };
    auto const size = encodeU32(static_cast<uint32_t>(contents.size()));
    bytes.insert(bytes.end(), size.begin(), size.end());
    bytes.insert(bytes.end(), contents.begin(), contents.end());
    return bytes;
}

/// A name map's contents, from its entries in the order given.
std::vector<uint8_t> nameMap(std::initializer_list<std::pair<uint32_t, std::string>> entries)
{
    auto bytes = encodeU32(static_cast<uint32_t>(entries.size()));
    for (auto const& [index, name]: entries)
    {
        auto const encodedIndex = encodeU32(index);
        auto const encodedName = encodeName(name);
        bytes.insert(bytes.end(), encodedIndex.begin(), encodedIndex.end());
        bytes.insert(bytes.end(), encodedName.begin(), encodedName.end());
    }
    return bytes;
}

/// The concatenation of @p parts.
std::vector<std::byte> payloadOf(std::initializer_list<std::vector<uint8_t>> parts)
{
    auto bytes = std::vector<uint8_t> {};
    for (auto const& part: parts)
        bytes.insert(bytes.end(), part.begin(), part.end());
    return bytesOf(bytes);
}

/// The name of `NameKind::kind`'s entry with index @p index in @p names, or "?" when there is none.
std::string directName(NameSection const& names, NameKind kind, uint32_t index)
{
    for (auto const& entry: names.direct[static_cast<std::size_t>(kind)])
        if (entry.index == index)
            return entry.name;
    return "?";
}

/// The name payload wasm-tools 1.263.0 writes for
/// (module $m (type $t (func)) (memory $mem 1) (global $g i32 (i32.const 0))
///   (func $f (local $l i32) (block $b (br $b))) (data $d "") (tag $e))
std::vector<std::byte> wasmToolsPayload()
{
    return bytesOf({ 0x00, 0x02, 0x01, 0x6D, 0x01, 0x04, 0x01, 0x00, 0x01, 0x66, 0x02, 0x06, 0x01, 0x00, 0x01,
                     0x00, 0x01, 0x6C, 0x03, 0x06, 0x01, 0x00, 0x01, 0x00, 0x01, 0x62, 0x04, 0x04, 0x01, 0x00,
                     0x01, 0x74, 0x06, 0x06, 0x01, 0x00, 0x03, 0x6D, 0x65, 0x6D, 0x07, 0x04, 0x01, 0x00, 0x01,
                     0x67, 0x09, 0x04, 0x01, 0x00, 0x01, 0x64, 0x0B, 0x04, 0x01, 0x00, 0x01, 0x65 });
}

} // namespace

TEST_CASE("NameSection.decodesWasmToolsOutput", "[NameSection]")
{
    auto const names = decodeNameSection(wasmToolsPayload());
    REQUIRE(names.has_value());
    CHECK(names->module == "m");
    CHECK(directName(*names, NameKind::Function, 0) == "f");
    CHECK(directName(*names, NameKind::Type, 0) == "t");
    CHECK(directName(*names, NameKind::Memory, 0) == "mem");
    CHECK(directName(*names, NameKind::Global, 0) == "g");
    CHECK(directName(*names, NameKind::Data, 0) == "d");
    CHECK(directName(*names, NameKind::Tag, 0) == "e");
    CHECK(names->direct[static_cast<std::size_t>(NameKind::Table)].empty());
    CHECK(names->direct[static_cast<std::size_t>(NameKind::Element)].empty());
    auto const oneName = [](std::string name) {
        return std::vector<IndirectNameMap> { IndirectNameMap {
            .index = 0, .names = { NameAssoc { .index = 0, .name = std::move(name) } } } };
    };
    CHECK(names->indirect[static_cast<std::size_t>(NameKind::Local)] == oneName("l"));
    CHECK(names->indirect[static_cast<std::size_t>(NameKind::Label)] == oneName("b"));
}

TEST_CASE("NameSection.roundTrip", "[NameSection]")
{
    auto const payload = wasmToolsPayload();
    auto const names = decodeNameSection(payload);
    REQUIRE(names.has_value());
    CHECK(encodeNameSection(*names) == payload);
}

TEST_CASE("NameSection.everySubsectionRoundTrips", "[NameSection]")
{
    // A name for every kind, with indices that take more than one LEB128 byte.
    auto names = NameSection { .module = "", .direct = {}, .indirect = {} };
    for (auto const& row: NameSubsections)
    {
        auto const kind = static_cast<std::size_t>(row.kind);
        switch (row.shape)
        {
            case isoline::binary::NameShape::Single: break;
            case isoline::binary::NameShape::Direct:
                names.direct[kind] = { NameAssoc { .index = 1, .name = "a" },
                                       NameAssoc { .index = 300, .name = "\xC3\xA9" } };
                break;
            case isoline::binary::NameShape::Indirect:
                names.indirect[kind] = { IndirectNameMap { .index = 0, .names = {} },
                                         IndirectNameMap {
                                             .index = 200,
                                             .names = { NameAssoc { .index = 0, .name = "x" },
                                                        NameAssoc { .index = 128, .name = "" } } } };
                break;
        }
    }
    auto const payload = encodeNameSection(names);
    auto const decoded = decodeNameSection(payload);
    REQUIRE(decoded.has_value());
    CHECK(*decoded == names);
}

TEST_CASE("NameSection.encodesCanonically", "[NameSection]")
{
    // Maps are written by ascending index, and subsections with nothing to name are omitted.
    auto names = NameSection {};
    names.direct[static_cast<std::size_t>(NameKind::Function)] = { NameAssoc { .index = 2, .name = "b" },
                                                                   NameAssoc { .index = 0, .name = "a" } };
    names.indirect[static_cast<std::size_t>(NameKind::Local)] = {
        IndirectNameMap {
            .index = 1,
            .names = { NameAssoc { .index = 4, .name = "y" }, NameAssoc { .index = 3, .name = "x" } } },
        IndirectNameMap { .index = 0, .names = {} },
    };
    CHECK(
        encodeNameSection(names)
        == payloadOf({ subsection(1, nameMap({ { 0, "a" }, { 2, "b" } })),
                       subsection(2, { 0x02, 0x00, 0x00, 0x01, 0x02, 0x03, 0x01, 'x', 0x04, 0x01, 'y' }) }));
    CHECK(encodeNameSection(NameSection {}).empty());

    // A non-minimal LEB128 and an empty subsection are what decoding forgets.
    auto const loose = payloadOf({ subsection(1, { 0x01, 0x80, 0x00, 0x01, 'f' }), subsection(4, { 0x00 }) });
    auto const decoded = decodeNameSection(loose);
    REQUIRE(decoded.has_value());
    CHECK(encodeNameSection(*decoded) == payloadOf({ subsection(1, nameMap({ { 0, "f" } })) }));

    auto const empty = decodeNameSection({});
    REQUIRE(empty.has_value());
    CHECK(*empty == NameSection {});
}

TEST_CASE("NameSection.unknownSubsectionSkipped", "[NameSection]")
{
    // Id 10 (GC field names) and id 14 (unknown) are skipped unread, though their contents are junk.
    auto const payload = payloadOf({ subsection(1, nameMap({ { 0, "f" } })),
                                     subsection(10, { 0xFF, 0xFF }),
                                     subsection(11, nameMap({ { 0, "e" } })),
                                     subsection(14, { 0x01 }) });
    auto const names = decodeNameSection(payload);
    REQUIRE(names.has_value());
    CHECK(directName(*names, NameKind::Function, 0) == "f");
    CHECK(directName(*names, NameKind::Tag, 0) == "e");
    CHECK(encodeNameSection(*names)
          == payloadOf({ subsection(1, nameMap({ { 0, "f" } })), subsection(11, nameMap({ { 0, "e" } })) }));
}

TEST_CASE("NameSection.outOfOrderAndRepeatedSkipped", "[NameSection]")
{
    // Types (4) then functions (1): the functions come out of order. A second function map repeats one.
    auto const outOfOrder =
        decodeNameSection(payloadOf({ subsection(4, nameMap({ { 0, "t" } })), subsection(1, { 0xFF }) }));
    REQUIRE(outOfOrder.has_value());
    CHECK(directName(*outOfOrder, NameKind::Type, 0) == "t");
    CHECK(outOfOrder->direct[static_cast<std::size_t>(NameKind::Function)].empty());

    auto const repeated = decodeNameSection(
        payloadOf({ subsection(1, nameMap({ { 0, "f" } })), subsection(1, nameMap({ { 0, "g" } })) }));
    REQUIRE(repeated.has_value());
    CHECK(directName(*repeated, NameKind::Function, 0) == "f");

    // An unknown id counts for the order too.
    auto const afterUnknown =
        decodeNameSection(payloadOf({ subsection(14, {}), subsection(0, encodeName("m")) }));
    REQUIRE(afterUnknown.has_value());
    CHECK(!afterUnknown->module.has_value());
}

TEST_CASE("NameSection.unsortedIsInvalid", "[NameSection]")
{
    auto const unsorted =
        decodeNameSection(payloadOf({ subsection(1, nameMap({ { 1, "a" }, { 0, "b" } })) }));
    REQUIRE(!unsorted.has_value());
    CHECK(unsorted.error().code == DecodeErrorCode::NameIndexOutOfOrder);
    CHECK(unsorted.error().offset == 6); // the second entry: id, size, count, then index 1 and "a"

    auto const duplicate =
        decodeNameSection(payloadOf({ subsection(7, nameMap({ { 3, "a" }, { 3, "b" } })) }));
    REQUIRE(!duplicate.has_value());
    CHECK(duplicate.error().code == DecodeErrorCode::NameIndexOutOfOrder);

    // Functions of an indirect map out of order, and the locals within one.
    auto const outer = decodeNameSection(payloadOf({ subsection(2, { 0x02, 0x01, 0x00, 0x00, 0x00 }) }));
    REQUIRE(!outer.has_value());
    CHECK(outer.error().code == DecodeErrorCode::NameIndexOutOfOrder);
    auto const inner =
        decodeNameSection(payloadOf({ subsection(3, { 0x01, 0x00, 0x02, 0x01, 0x00, 0x00, 0x00 }) }));
    REQUIRE(!inner.has_value());
    CHECK(inner.error().code == DecodeErrorCode::NameIndexOutOfOrder);
}

TEST_CASE("NameSection.malformedIsInvalid", "[NameSection]")
{
    auto const notUtf8 = decodeNameSection(payloadOf({ subsection(0, { 0x01, 0xFF }) }));
    REQUIRE(!notUtf8.has_value());
    CHECK(notUtf8.error().code == DecodeErrorCode::MalformedUtf8Encoding);

    // The module name leaves a byte of its subsection unread.
    auto const tooLarge = decodeNameSection(payloadOf({ subsection(0, { 0x01, 'm', 0x00 }) }));
    REQUIRE(!tooLarge.has_value());
    CHECK(tooLarge.error().code == DecodeErrorCode::NameSubsectionSizeMismatch);
    CHECK(tooLarge.error().offset == 4);

    // The module name runs past its subsection, into the next one.
    auto const tooSmall = decodeNameSection(bytesOf({ 0x00, 0x01, 0x01, 'm', 0x01, 0x00 }));
    REQUIRE(!tooSmall.has_value());
    CHECK(tooSmall.error().code == DecodeErrorCode::UnexpectedEnd);

    // A subsection longer than the payload, and one cut off before its size.
    auto const truncated = decodeNameSection(bytesOf({ 0x01, 0x05, 0x00 }));
    REQUIRE(!truncated.has_value());
    CHECK(truncated.error().code == DecodeErrorCode::LengthOutOfBounds);
    auto const noSize = decodeNameSection(bytesOf({ 0x01 }));
    REQUIRE(!noSize.has_value());
    CHECK(noSize.error().code == DecodeErrorCode::UnexpectedEnd);
    auto const noContents = decodeNameSection(bytesOf({ 0x01, 0x01 }));
    REQUIRE(!noContents.has_value());
    CHECK(noContents.error().code == DecodeErrorCode::UnexpectedEnd);

    // Subsections cut off before a name map's or an indirect map's count, and before either's first index.
    for (auto const& cut:
         { subsection(1, {}), subsection(2, {}), subsection(1, { 0x01 }), subsection(2, { 0x01 }) })
    {
        auto const names = decodeNameSection(bytesOf(cut));
        REQUIRE(!names.has_value());
        CHECK(names.error().code == DecodeErrorCode::UnexpectedEnd);
    }
}

TEST_CASE("NameSection.parameterNames", "[NameSection]")
{
    // What wasm-tools writes for (type (func (param $x i32))) as type 3: subsection 12, type 3, param 0.
    auto const names = decodeNameSection(bytesOf({ 0x0C, 0x06, 0x01, 0x03, 0x01, 0x00, 0x01, 'x' }));
    REQUIRE(names.has_value());
    CHECK(names->indirect[static_cast<std::size_t>(NameKind::Parameter)]
          == std::vector<IndirectNameMap> {
              IndirectNameMap { .index = 3, .names = { NameAssoc { .index = 0, .name = "x" } } } });
}

TEST_CASE("NameSection.corpusRoundTrips", "[NameSection]")
{
    // Every name section of the spec corpus decodes, and encodes back to its very bytes: the corpus
    // holds wasm-tools' output, whose subsections are all rows of NameSubsections.
    auto const corpus = std::filesystem::path { ISOLINE_SOURCE_DIR } / "tests" / "spec";
    auto checked = std::size_t { 0 };
    for (auto const& entry: std::filesystem::recursive_directory_iterator { corpus })
    {
        if (!entry.is_regular_file() || entry.path().extension() != ".wasm")
            continue;
        auto file = std::ifstream { entry.path(), std::ios::binary };
        auto const raw = std::vector<char> { std::istreambuf_iterator<char> { file }, {} };
        auto const bytes = std::as_bytes(std::span { raw });
        auto const module = decodeModule(bytes, isoline::FeatureSet::all());
        if (!module)
            continue;
        for (auto const& custom: module->customs)
        {
            if (custom.name != NameSectionName)
                continue;
            INFO(entry.path().string());
            ++checked;
            auto const names = decodeNameSection(custom.bytes);
            REQUIRE(names.has_value());
            CHECK(encodeNameSection(*names) == custom.bytes);
        }
    }
    CHECK(checked > 2000);
}
