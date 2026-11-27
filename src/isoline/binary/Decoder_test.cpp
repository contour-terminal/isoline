// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/binary/DecodeError.hpp>
#include <isoline/binary/Decoder.hpp>
#include <isoline/binary/Module.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>

using isoline::Feature;
using isoline::FeatureSet;
using isoline::binary::ConstExpr;
using isoline::binary::DecodeErrorCode;
using isoline::binary::DecodeLimits;
using isoline::binary::decodeModule;
using isoline::binary::ElementSegment;
using isoline::binary::ExternKind;
using isoline::binary::Function;
using isoline::binary::FuncType;
using isoline::binary::importModuleOf;
using isoline::binary::Instruction;
using isoline::binary::labelsOf;
using isoline::binary::Limits;
using isoline::binary::localType;
using isoline::binary::Module;
using isoline::binary::Mutability;
using isoline::binary::Opcode;
using isoline::binary::SegmentMode;
using isoline::binary::selectTypesOf;
using isoline::binary::testing::encodeU32;
using isoline::binary::testing::ModuleBuilder;

namespace
{

/// The module preamble: the magic number and version 1.
constexpr auto Preamble = std::to_array<uint8_t>({ 0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00 });

/// A module of @p sections after the preamble.
std::vector<std::byte> moduleOf(std::initializer_list<uint8_t> sections)
{
    auto bytes = std::vector<std::byte> {};
    for (auto const byte: Preamble)
        bytes.push_back(std::byte { byte });
    for (auto const byte: sections)
        bytes.push_back(std::byte { byte });
    return bytes;
}

/// Raw bytes, with no preamble.
std::vector<std::byte> rawBytes(std::initializer_list<uint8_t> bytes)
{
    auto result = std::vector<std::byte> {};
    for (auto const byte: bytes)
        result.push_back(std::byte { byte });
    return result;
}

/// The error decoding @p bytes reports, or nullopt when it decodes.
std::optional<DecodeErrorCode> errorOf(std::span<std::byte const> bytes,
                                       FeatureSet const& features = FeatureSet::wasm2(),
                                       DecodeLimits const& limits = {})
{
    auto const result = decodeModule(bytes, features, limits);
    if (result)
        return std::nullopt;
    return result.error().code;
}

/// The disabled proposal the decoder names for @p bytes under @p features (DecodeError::disabled).
/// @return It; nothing when the module decodes or the error names none.
std::optional<Feature> disabledOf(std::span<std::byte const> bytes, FeatureSet const& features)
{
    auto const result = decodeModule(bytes, features, DecodeLimits {});
    if (result)
        return std::nullopt;
    return result.error().disabled;
}

/// The module @p bytes decode to; fails the test when they do not.
Module decoded(std::span<std::byte const> bytes, FeatureSet const& features = FeatureSet::wasm2())
{
    auto result = decodeModule(bytes, features);
    if (!result)
        UNSCOPED_INFO("decode error " << static_cast<int>(result.error().code) << " at "
                                      << result.error().offset);
    REQUIRE(result.has_value());
    return std::move(*result);
}

/// The function indices of an element segment that lists them; fails the test when it does not.
std::vector<uint32_t> const& indicesOf(ElementSegment const& segment)
{
    REQUIRE(std::holds_alternative<std::vector<uint32_t>>(segment.items));
    return std::get<std::vector<uint32_t>>(segment.items);
}

/// The expressions of an element segment that lists them; fails the test when it does not.
std::vector<ConstExpr> const& expressionsOf(ElementSegment const& segment)
{
    REQUIRE(std::holds_alternative<std::vector<ConstExpr>>(segment.items));
    return std::get<std::vector<ConstExpr>>(segment.items);
}

/// @p bytes appended to @p out.
void append(std::vector<uint8_t>& out, std::span<uint8_t const> bytes)
{
    out.insert(out.end(), bytes.begin(), bytes.end());
}

} // namespace

TEST_CASE("Decoder.emptyModule")
{
    auto const module = decoded(moduleOf({}));
    CHECK(module.types.empty());
    CHECK(module.imports.empty());
    CHECK(module.functions.empty());
    CHECK(module.tables.empty());
    CHECK(module.memories.empty());
    CHECK(module.globals.empty());
    CHECK(module.exports.empty());
    CHECK_FALSE(module.start.has_value());
    CHECK(module.elements.empty());
    CHECK_FALSE(module.dataCount.has_value());
    CHECK(module.code.empty());
    CHECK(module.data.empty());
    CHECK(module.customs.empty());
}

TEST_CASE("Decoder.badMagic")
{
    auto const bytes = rawBytes({ 0x00, 0x61, 0x73, 0x6E, 0x01, 0x00, 0x00, 0x00 });
    auto const result = decodeModule(bytes, FeatureSet::wasm2());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == DecodeErrorCode::MagicHeaderNotDetected);
    CHECK(result.error().offset == 0);
}

TEST_CASE("Decoder.badVersion")
{
    auto const bytes = rawBytes({ 0x00, 0x61, 0x73, 0x6D, 0x02, 0x00, 0x00, 0x00 });
    auto const result = decodeModule(bytes, FeatureSet::wasm2());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == DecodeErrorCode::UnknownBinaryVersion);
    CHECK(result.error().offset == 4);
}

TEST_CASE("Decoder.truncatedPreamble")
{
    CHECK(errorOf(rawBytes({})) == DecodeErrorCode::UnexpectedEnd);
    CHECK(errorOf(rawBytes({ 0x00, 0x61, 0x73 })) == DecodeErrorCode::UnexpectedEnd);
    CHECK(errorOf(rawBytes({ 0x00, 0x61, 0x73, 0x6D, 0x01 })) == DecodeErrorCode::UnexpectedEnd);
}

TEST_CASE("Decoder.sectionSizeOverrun")
{
    // A size past every byte left is out of bounds before any of the section is read.
    auto const bytes = moduleOf({ 0x01, 0x0A, 0x01, 0x60 });
    auto const result = decodeModule(bytes, FeatureSet::wasm2());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == DecodeErrorCode::LengthOutOfBounds);
    CHECK(result.error().offset == 9);

    // spec binary.wast: 2 types declared, 1 given; the section size 7 exceeds the 4 bytes left.
    CHECK(errorOf(moduleOf({ 0x01, 0x07, 0x02, 0x60, 0x00, 0x00 })) == DecodeErrorCode::LengthOutOfBounds);
}

TEST_CASE("Decoder.sectionSizeMismatch")
{
    // The type section declares 5 bytes, its contents take 4: one function type () -> ().
    auto const bytes = moduleOf({ 0x01, 0x05, 0x01, 0x60, 0x00, 0x00, 0x00, 0x00 });
    auto const result = decodeModule(bytes, FeatureSet::wasm2());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == DecodeErrorCode::SectionSizeMismatch);
    CHECK(result.error().offset == 10);

    // Contents longer than declared run on into the next section, as the interpreter reads them.
    CHECK(errorOf(moduleOf({ 0x01, 0x03, 0x01, 0x60, 0x00, 0x00, 0x03, 0x01, 0x00 }))
          == DecodeErrorCode::SectionSizeMismatch);
}

TEST_CASE("Decoder.sectionOrder")
{
    auto const bytes = ModuleBuilder {}
                           .rawSection(3, { 0x01, 0x00 })             // function section
                           .rawSection(1, { 0x01, 0x60, 0x00, 0x00 }) // type section
                           .build();
    auto const result = decodeModule(bytes, FeatureSet::wasm2());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == DecodeErrorCode::SectionOutOfOrder);
    CHECK(result.error().offset == 12);
    CHECK(isoline::binary::specMessage(result.error().code) == "unexpected content after last section");

    // The data count section sits between the element and the code section.
    CHECK(errorOf(ModuleBuilder {}.rawSection(10, { 0x00 }).rawSection(12, { 0x00 }).build())
          == DecodeErrorCode::SectionOutOfOrder);
    CHECK_FALSE(errorOf(ModuleBuilder {}
                            .rawSection(9, { 0x00 })
                            .rawSection(12, { 0x00 })
                            .rawSection(10, { 0x00 })
                            .build())
                    .has_value());
}

TEST_CASE("Decoder.duplicateSection")
{
    auto const bytes = ModuleBuilder {}.rawSection(1, { 0x00 }).rawSection(1, { 0x00 }).build();
    CHECK(errorOf(bytes) == DecodeErrorCode::DuplicateSection);
    CHECK(isoline::binary::specMessage(DecodeErrorCode::DuplicateSection)
          == "unexpected content after last section");
}

TEST_CASE("Decoder.malformedSectionId")
{
    CHECK(errorOf(moduleOf({ 0x0D, 0x00 })) == DecodeErrorCode::MalformedSectionId);
    CHECK(errorOf(moduleOf({ 0x7F, 0x00 })) == DecodeErrorCode::MalformedSectionId);
    // The data count section belongs to bulk memory.
    CHECK(errorOf(moduleOf({ 0x0C, 0x01, 0x00 }), FeatureSet::none()) == DecodeErrorCode::MalformedSectionId);
}

TEST_CASE("Decoder.customSectionsAnywhere")
{
    auto const bytes = ModuleBuilder {}
                           .rawSection(1, { 0x01, 0x60, 0x00, 0x00 })
                           .rawSection(0, { 0x01, 'a', 0xAB, 0xCD })
                           .rawSection(3, { 0x01, 0x00 })
                           .rawSection(10, { 0x01, 0x02, 0x00, 0x0B })
                           .rawSection(0, { 0x01, 'b' })
                           .build();
    auto const module = decoded(bytes);
    REQUIRE(module.customs.size() == 2);
    CHECK(module.customs[0].name == "a");
    CHECK(module.customs[0].bytes == rawBytes({ 0xAB, 0xCD }));
    CHECK(module.customs[1].name == "b");
    CHECK(module.customs[1].bytes.empty());
    CHECK(module.types.size() == 1);
    CHECK(module.functions.size() == 1);
}

TEST_CASE("Decoder.customSectionMalformed")
{
    // spec custom.wast
    CHECK(errorOf(moduleOf({ 0x00 })) == DecodeErrorCode::UnexpectedEnd);
    CHECK(errorOf(moduleOf({ 0x00, 0x00 })) == DecodeErrorCode::UnexpectedEnd);
    CHECK(errorOf(moduleOf({ 0x00, 0x00, 0x00, 0x05, 0x01, 0x00, 0x07, 0x00, 0x00 }))
          == DecodeErrorCode::UnexpectedEnd);
    // A name longer than its section is out of bounds once it passes the module's end.
    CHECK(errorOf(moduleOf({ 0x00, 0x02, 0x05, 'a' })) == DecodeErrorCode::LengthOutOfBounds);
    // Two concatenated modules: the second's "asm" reads as a custom section of 0x61 bytes.
    CHECK(errorOf(moduleOf({ 0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00 }))
          == DecodeErrorCode::LengthOutOfBounds);
    CHECK(errorOf(moduleOf({ 0x00, 0x02, 0x01, 0xFF })) == DecodeErrorCode::MalformedUtf8Encoding);
}

TEST_CASE("Decoder.functionAndCodeMismatch")
{
    auto const bytes = ModuleBuilder {}.type(FuncType {}).rawSection(3, { 0x01, 0x00 }).build();
    auto const result = decodeModule(bytes, FeatureSet::wasm2());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == DecodeErrorCode::FunctionAndCodeSectionInconsistent);
    CHECK(result.error().offset == bytes.size());

    // Bodies without functions.
    CHECK(errorOf(ModuleBuilder {}.type(FuncType {}).rawSection(10, { 0x01, 0x02, 0x00, 0x0B }).build())
          == DecodeErrorCode::FunctionAndCodeSectionInconsistent);
}

TEST_CASE("Decoder.decodeRejectsHugeVectorCount")
{
    // A count of 0xFFFFFFFF with one byte left is out of bounds before anything is reserved.
    auto const bytes = moduleOf({ 0x01, 0x06, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x60 });
    auto const result = decodeModule(bytes, FeatureSet::wasm2());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == DecodeErrorCode::LengthOutOfBounds);
    CHECK(result.error().offset == 10);

    // The same for every vector: imports, locals, a data segment's bytes.
    CHECK(errorOf(moduleOf({ 0x02, 0x06, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x00 }))
          == DecodeErrorCode::LengthOutOfBounds);
    CHECK(errorOf(moduleOf({ 0x01, 0x04, 0x01, 0x60, 0x00, 0x00, 0x03, 0x02, 0x01, 0x00,
                             0x0A, 0x08, 0x01, 0x06, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0B }))
          == DecodeErrorCode::LengthOutOfBounds);
    CHECK(errorOf(moduleOf({ 0x0B, 0x07, 0x01, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F }))
          == DecodeErrorCode::LengthOutOfBounds);
}

TEST_CASE("Decoder.implementationLimitsAreDistinguishable")
{
    // Over a limit the embedder may raise, or malformed whatever the limits.
    for (auto const& row: isoline::binary::DecodeErrorTable)
    {
        INFO("error: " << row.specMessage);
        CHECK(isImplementationLimit(row.code)
              == (row.code == DecodeErrorCode::ImplementationLimitExceeded
                  || row.code == DecodeErrorCode::TooManyLocals));
    }
    auto const limited =
        ModuleBuilder {}.type(FuncType {}).function(0, std::vector(4, isoline::binary::I32), {}).build();
    auto const result = decodeModule(limited, FeatureSet::wasm2(), DecodeLimits { .maxLocals = 3 });
    REQUIRE_FALSE(result.has_value());
    CHECK(isImplementationLimit(result.error().code));
    CHECK_FALSE(isImplementationLimit(DecodeErrorCode::MalformedSectionId));
}

TEST_CASE("Decoder.tooManyLocals")
{
    // Two runs of 0x80000000 i32 overflow a u32.
    auto const bytes =
        moduleOf({ 0x01, 0x04, 0x01, 0x60, 0x00, 0x00, 0x03, 0x02, 0x01, 0x00, 0x0A, 0x10, 0x01, 0x0E,
                   0x02, 0x80, 0x80, 0x80, 0x80, 0x08, 0x7F, 0x80, 0x80, 0x80, 0x80, 0x08, 0x7F, 0x0B });
    auto const result = decodeModule(bytes, FeatureSet::wasm2());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == DecodeErrorCode::LocalCountOverflow);
    CHECK(result.error().offset == 22);
    // Malformed, not past a limit: the highest maxLocals a u32 holds refuses it alike.
    CHECK_FALSE(isImplementationLimit(result.error().code));
    CHECK(errorOf(
              bytes, FeatureSet::wasm2(), DecodeLimits { .maxLocals = std::numeric_limits<uint32_t>::max() })
          == DecodeErrorCode::LocalCountOverflow);

    // Within a u32 but past DecodeLimits::maxLocals.
    auto const limited =
        ModuleBuilder {}.type(FuncType {}).function(0, std::vector(4, isoline::binary::I32), {}).build();
    CHECK(errorOf(limited, FeatureSet::wasm2(), DecodeLimits { .maxLocals = 3 })
          == DecodeErrorCode::TooManyLocals);
    CHECK_FALSE(errorOf(limited, FeatureSet::wasm2(), DecodeLimits { .maxLocals = 4 }).has_value());
}

TEST_CASE("Decoder.functions")
{
    using isoline::binary::F64;
    using isoline::binary::I32;
    using isoline::binary::I64;
    auto const bytes = ModuleBuilder {}
                           .type(FuncType { .params = { I32, I32 }, .results = { I64 } })
                           .type(FuncType {})
                           .function(1, {}, {})
                           .function(0, { I32, I32, F64 }, { 0x20, 0x00, 0x1A })
                           .memory(Limits { .min = 1, .max = 2 })
                           .exportFunction("run", 1)
                           .build();
    auto const module = decoded(bytes);
    REQUIRE(module.types.size() == 2);
    CHECK(module.types[0] == FuncType { .params = { I32, I32 }, .results = { I64 } });
    CHECK(module.functions == std::vector<uint32_t> { 1, 0 });
    REQUIRE(module.code.size() == 2);
    CHECK(module.code[0].typeIndex == 1);
    CHECK(module.code[0].locals.empty());
    CHECK(module.code[0].localCount == 0);
    CHECK(module.code[1].typeIndex == 0);
    REQUIRE(module.code[1].locals.size() == 2);
    CHECK(module.code[1].locals[0].count == 2);
    CHECK(module.code[1].locals[0].type == I32);
    CHECK(module.code[1].locals[1].count == 1);
    CHECK(module.code[1].locals[1].type == F64);
    CHECK(module.code[1].localCount == 3);
    CHECK(localType(module.code[1], 1) == I32);
    CHECK(localType(module.code[1], 2) == F64);
    CHECK_FALSE(localType(module.code[1], 3).has_value());
    // The second body: its size, two runs (1+2 bytes each), then local.get 0.
    CHECK(static_cast<uint8_t>(bytes[module.code[1].bodyOffset]) == 0x20);
    CHECK(module.code[1].body.size() == 3);
    CHECK(module.code[1].body[0].opcode == Opcode::LocalGet);
    REQUIRE(module.memories.size() == 1);
    CHECK(module.memories[0].limits.min == 1);
    CHECK(module.memories[0].limits.max == 2U);
    REQUIRE(module.exports.size() == 1);
    CHECK(module.exports[0].name == "run");
    CHECK(module.exports[0].kind == ExternKind::Func);
    CHECK(module.exports[0].index == 1);
}

TEST_CASE("Decoder.imports")
{
    auto const bytes = ModuleBuilder {}
                           .type(FuncType {})
                           .rawSection(2,
                                       {
                                           0x03,                             // three imports
                                           0x01, 'm', 0x01, 'f', 0x00, 0x00, // func, type 0
                                           0x01, 'm', 0x01, 't', 0x01, 0x70, 0x01, 0x01, 0x02, // table
                                           0x01, 'm', 0x01, 'g', 0x03, 0x7F, 0x01, // global mut i32
                                       })
                           .build();
    auto const module = decoded(bytes);
    REQUIRE(module.imports.size() == 3);
    CHECK(importModuleOf(module, module.imports[0]) == "m");
    CHECK(module.imports[0].name == "f");
    CHECK(module.imports[0].kind == ExternKind::Func);
    CHECK(module.imports[0].typeIndex == 0);
    CHECK(module.imports[1].name == "t");
    CHECK(module.imports[1].kind == ExternKind::Table);
    CHECK(module.imports[1].table.element == isoline::binary::FuncRef);
    CHECK(module.imports[1].table.limits.min == 1);
    CHECK(module.imports[1].table.limits.max == 2U);
    CHECK(module.imports[2].name == "g");
    CHECK(module.imports[2].kind == ExternKind::Global);
    CHECK(module.imports[2].global.type == isoline::binary::I32);
    CHECK(module.imports[2].global.mutability == Mutability::Var);
    CHECK(module.importedCount(ExternKind::Func) == 1);

    // A memory import, and the malformed kinds.
    auto const memory = decoded(moduleOf({ 0x02, 0x08, 0x01, 0x01, 'm', 0x01, 'm', 0x02, 0x00, 0x03 }));
    REQUIRE(memory.imports.size() == 1);
    CHECK(memory.imports[0].kind == ExternKind::Memory);
    CHECK(memory.imports[0].memory.limits.min == 3);
    CHECK_FALSE(memory.imports[0].memory.limits.max.has_value());
    CHECK(errorOf(moduleOf({ 0x02, 0x06, 0x01, 0x01, 'm', 0x01, 'm', 0x04 }))
          == DecodeErrorCode::MalformedImportKind);
    CHECK(errorOf(moduleOf({ 0x02, 0x08, 0x01, 0x01, 'm', 0x01, 'g', 0x03, 0x7F, 0x02 }))
          == DecodeErrorCode::MalformedMutability);
}

TEST_CASE("Decoder.exports")
{
    auto const module = decoded(
        moduleOf({ 0x07, 0x0D, 0x03, 0x01, 'a', 0x01, 0x00, 0x01, 'b', 0x02, 0x00, 0x01, 'c', 0x03, 0x00 }));
    REQUIRE(module.exports.size() == 3);
    CHECK(module.exports[0].kind == ExternKind::Table);
    CHECK(module.exports[1].kind == ExternKind::Memory);
    CHECK(module.exports[2].kind == ExternKind::Global);
    CHECK(errorOf(moduleOf({ 0x07, 0x05, 0x01, 0x01, 'a', 0x04, 0x00 }))
          == DecodeErrorCode::MalformedExportKind);
    // spec binary.wast: 2 exports declared, 1 given; the second name's length passes the module end.
    CHECK(errorOf(moduleOf({ 0x01, 0x04, 0x01, 0x60, 0x00, 0x00, 0x03, 0x03, 0x02, 0x00,
                             0x00, 0x07, 0x06, 0x02, 0x02, 0x66, 0x31, 0x00, 0x00, 0x0A,
                             0x07, 0x02, 0x02, 0x00, 0x0B, 0x02, 0x00, 0x0B }))
          == DecodeErrorCode::LengthOutOfBounds);
}

TEST_CASE("Decoder.limits")
{
    CHECK(errorOf(moduleOf({ 0x05, 0x03, 0x01, 0x02, 0x00 })) == DecodeErrorCode::LimitsFlagTooLarge);
    CHECK(errorOf(moduleOf({ 0x05, 0x03, 0x01, 0x08, 0x00 })) == DecodeErrorCode::LimitsFlagTooLarge);
    // WebAssembly 2.0 reads the flag as a one-bit LEB128: a continuation byte holding 0 or 1 is too long.
    CHECK(errorOf(moduleOf({ 0x05, 0x04, 0x01, 0x81, 0x00, 0x00 }))
          == DecodeErrorCode::IntegerRepresentationTooLong);
    CHECK(errorOf(moduleOf({ 0x04, 0x06, 0x01, 0x70, 0x80, 0x00, 0x00, 0x00 }))
          == DecodeErrorCode::IntegerRepresentationTooLong);
    CHECK(errorOf(moduleOf({ 0x05, 0x04, 0x01, 0x82, 0x00, 0x00 })) == DecodeErrorCode::LimitsFlagTooLarge);
    CHECK(errorOf(moduleOf({ 0x04, 0x04, 0x01, 0x70, 0x04, 0x00 })) == DecodeErrorCode::LimitsFlagTooLarge);
    CHECK(errorOf(moduleOf({ 0x05, 0x07, 0x01, 0x00, 0x80, 0x80, 0x80, 0x80, 0x10 }))
          == DecodeErrorCode::IntegerTooLargeForLimits);
    auto const module = decoded(moduleOf({ 0x04, 0x05, 0x01, 0x6F, 0x01, 0x00, 0x0A }));
    REQUIRE(module.tables.size() == 1);
    CHECK(module.tables[0].type.element == isoline::binary::ExternRef);
    CHECK(module.tables[0].type.limits.max == 10U);
}

TEST_CASE("Decoder.memory64Limits")
{
    // Memory flags 0x05: a maximum and i64 addresses; min 1, max 2^40 as u64s.
    auto const bytes = moduleOf({ 0x05, 0x09, 0x01, 0x05, 0x01, 0x80, 0x80, 0x80, 0x80, 0x80, 0x20 });
    auto const module = decoded(bytes, FeatureSet::wasm3());
    REQUIRE(module.memories.size() == 1);
    CHECK(module.memories[0].limits.addressType == isoline::binary::AddressType::I64);
    CHECK(module.memories[0].limits.min == 1);
    CHECK(module.memories[0].limits.max == uint64_t { 1 } << 40);
    // Flags 0x04: no maximum. A table64 the same way, after its element type.
    auto const table = decoded(moduleOf({ 0x04, 0x04, 0x01, 0x70, 0x04, 0x02 }), FeatureSet::wasm3());
    REQUIRE(table.tables.size() == 1);
    CHECK(table.tables[0].type.limits.addressType == isoline::binary::AddressType::I64);
    CHECK(table.tables[0].type.limits.min == 2);
    CHECK_FALSE(table.tables[0].type.limits.max.has_value());
    // The 3.0 grammar reads the bounds of an i32 memory as u64s too; the validator bounds them.
    auto const wide =
        decoded(moduleOf({ 0x05, 0x07, 0x01, 0x00, 0x80, 0x80, 0x80, 0x80, 0x10 }), FeatureSet::wasm3());
    CHECK(wide.memories[0].limits.min == uint64_t { 1 } << 32);
    // A bound past u64 is still too large.
    CHECK(errorOf(moduleOf(
                      { 0x05, 0x0D, 0x01, 0x04, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x02 }),
                  FeatureSet::wasm3())
          == DecodeErrorCode::IntegerTooLargeForLimits);
    // Without memory64, 0x04 and 0x05 are flags of no grammar WebAssembly 2.0 knows.
    CHECK(errorOf(bytes) == DecodeErrorCode::LimitsFlagTooLarge);
}

TEST_CASE("Decoder.malformedLimitsFlags")
{
    auto const flags = [](uint8_t flag) {
        return moduleOf({ 0x05, 0x03, 0x01, flag, 0x00 });
    };
    auto const memory64 = FeatureSet::wasm2().with(Feature::Memory64);
    CHECK(errorOf(flags(0x10), memory64) == DecodeErrorCode::MalformedLimitsFlags);
    CHECK(errorOf(flags(0x10)) == DecodeErrorCode::LimitsFlagTooLarge);
    // Shared memories (0x02, 0x03) belong to threads, which Isoline does not decode; 0x08 is
    // custom-page-sizes', and table flags follow the same table.
    for (auto const flag: { 0x02, 0x03, 0x06, 0x07, 0x08 })
        CHECK(errorOf(flags(static_cast<uint8_t>(flag)), memory64) == DecodeErrorCode::MalformedLimitsFlags);
    CHECK(errorOf(moduleOf({ 0x04, 0x04, 0x01, 0x70, 0x08, 0x00 }), memory64)
          == DecodeErrorCode::MalformedLimitsFlags);
    // spec binary.wast: 3.0 reads the flags as a byte, so a LEB128 continuation is malformed.
    CHECK(errorOf(moduleOf({ 0x05, 0x04, 0x01, 0x81, 0x00, 0x00 }), memory64)
          == DecodeErrorCode::MalformedLimitsFlags);
    // The section ends where the flags, the minimum or the maximum should be, in either grammar.
    for (auto const& features: { FeatureSet::wasm2(), memory64 })
    {
        CHECK(errorOf(moduleOf({ 0x05, 0x01, 0x01 }), features) == DecodeErrorCode::UnexpectedEnd);
        CHECK(errorOf(moduleOf({ 0x05, 0x02, 0x01, 0x00 }), features) == DecodeErrorCode::UnexpectedEnd);
        CHECK(errorOf(moduleOf({ 0x05, 0x03, 0x01, 0x01, 0x00 }), features)
              == DecodeErrorCode::UnexpectedEnd);
    }
    CHECK(errorOf(moduleOf({ 0x05, 0x03, 0x01, 0x05, 0x00 }), memory64) == DecodeErrorCode::UnexpectedEnd);
    // A maximum past u64.
    CHECK(
        errorOf(
            moduleOf(
                { 0x05, 0x0E, 0x01, 0x05, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x02 }),
            memory64)
        == DecodeErrorCode::IntegerTooLargeForLimits);
    auto const result = decodeModule(flags(0x10), memory64);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().offset == Preamble.size() + 3);
    CHECK(isoline::binary::specMessage(result.error()) == "malformed limits flags");
}

TEST_CASE("Decoder.pageSizeOne")
{
    // Memory flags 0x08: a custom page size, whose exponent follows the minimum; 2^0 = 1 byte.
    auto const pageSizeOne = moduleOf({ 0x05, 0x04, 0x01, 0x08, 0x01, 0x00 });
    auto const module = decoded(pageSizeOne, FeatureSet::all());
    REQUIRE(module.memories.size() == 1);
    CHECK(module.memories[0].pageSizeLog2 == 0);
    CHECK(module.memories[0].limits.min == 1);
    CHECK(isoline::binary::pageSize(module.memories[0]) == 1);
    // Without the flag, 64 KiB pages.
    auto const plain = decoded(moduleOf({ 0x05, 0x03, 0x01, 0x00, 0x01 }), FeatureSet::all());
    CHECK(plain.memories[0].pageSizeLog2 == isoline::binary::DefaultPageSizeLog2);
}

TEST_CASE("Decoder.customPageSizeFlags")
{
    auto const all = FeatureSet::all();
    // 0x09: a maximum, then the exponent; 0x0D: i64 addresses too, with u64 bounds.
    auto const bounded = decoded(moduleOf({ 0x05, 0x05, 0x01, 0x09, 0x01, 0x02, 0x10 }), all);
    CHECK(bounded.memories[0].limits.max == 2U);
    CHECK(bounded.memories[0].pageSizeLog2 == 16);
    auto const wide = decoded(moduleOf({ 0x05, 0x05, 0x01, 0x0D, 0x01, 0x02, 0x00 }), all);
    CHECK(wide.memories[0].limits.addressType == isoline::binary::AddressType::I64);
    CHECK(wide.memories[0].pageSizeLog2 == 0);
    auto const unbounded64 = decoded(moduleOf({ 0x05, 0x04, 0x01, 0x0C, 0x01, 0x3F }), all);
    CHECK(unbounded64.memories[0].pageSizeLog2 == 63);
    CHECK_FALSE(unbounded64.memories[0].limits.max.has_value());
    // A memory import reads the same memory type.
    auto builder = ModuleBuilder {};
    builder.importMemory("m",
                         "mem",
                         isoline::binary::MemoryType { .limits = Limits { .min = 3, .max = std::nullopt },
                                                       .pageSizeLog2 = 0 });
    auto const imported = decoded(builder.build(), all);
    REQUIRE(imported.imports.size() == 1);
    CHECK(imported.imports[0].memory.pageSizeLog2 == 0);
    CHECK(imported.imports[0].memory.limits.min == 3);

    // An exponent of 64 or more gives no u64 page size; spec custom-page-sizes-invalid.wast uses 65.
    for (auto const exponent: { uint8_t { 0x40 }, uint8_t { 0x41 } })
    {
        auto const result = decodeModule(moduleOf({ 0x05, 0x04, 0x01, 0x08, 0x00, exponent }), all);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == DecodeErrorCode::InvalidCustomPageSize);
        CHECK(result.error().offset == Preamble.size() + 5);
        CHECK(isoline::binary::specMessage(result.error()) == "invalid custom page size");
    }
    // The exponent is a u32: one past it is too large, an end before it unexpected.
    CHECK(errorOf(moduleOf({ 0x05, 0x08, 0x01, 0x08, 0x00, 0x80, 0x80, 0x80, 0x80, 0x10 }), all)
          == DecodeErrorCode::IntegerTooLarge);
    CHECK(errorOf(moduleOf({ 0x05, 0x03, 0x01, 0x08, 0x00 }), all) == DecodeErrorCode::UnexpectedEnd);
    // A table has no page size: every flag with bit 3 is malformed there, defined or imported.
    for (auto const flag: { 0x08, 0x09, 0x0C, 0x0D })
    {
        auto const byte = static_cast<uint8_t>(flag);
        CHECK(errorOf(moduleOf({ 0x04, 0x07, 0x01, 0x70, byte, 0x00, 0x00, 0x00, 0x00 }), all)
              == DecodeErrorCode::MalformedLimitsFlags);
        CHECK(
            errorOf(moduleOf({ 0x02, 0x0B, 0x01, 0x01, 'm', 0x01, 't', 0x01, 0x70, byte, 0x00, 0x00, 0x00 }),
                    all)
            == DecodeErrorCode::MalformedLimitsFlags);
    }
}

TEST_CASE("Decoder.limitsFlagsNeedTheirProposal")
{
    auto const flags = [](uint8_t flag) {
        return moduleOf({ 0x05, 0x05, 0x01, flag, 0x00, 0x00, 0x00 });
    };
    // Either proposal selects WebAssembly 3.0's grammar, in which each flag row needs its own.
    auto const memory64 = FeatureSet::wasm3();
    auto const pageSizes = FeatureSet::wasm2().with(Feature::CustomPageSizes);
    for (auto const flag: { 0x08, 0x09, 0x0C, 0x0D })
        CHECK(errorOf(flags(static_cast<uint8_t>(flag)), memory64) == DecodeErrorCode::MalformedLimitsFlags);
    for (auto const flag: { 0x04, 0x05, 0x0C, 0x0D })
        CHECK(errorOf(flags(static_cast<uint8_t>(flag)), pageSizes) == DecodeErrorCode::MalformedLimitsFlags);
    CHECK(errorOf(flags(0x09), pageSizes) == std::nullopt);
    // custom-page-sizes alone reads 3.0's bytes: the flags a byte, the bounds u64s.
    CHECK(errorOf(moduleOf({ 0x05, 0x04, 0x01, 0x81, 0x00, 0x00 }), pageSizes)
          == DecodeErrorCode::MalformedLimitsFlags);
    auto const wide = decoded(moduleOf({ 0x05, 0x07, 0x01, 0x00, 0x80, 0x80, 0x80, 0x80, 0x10 }), pageSizes);
    CHECK(wide.memories[0].limits.min == uint64_t { 1 } << 32);
    // Neither: WebAssembly 3.0 without memory64 keeps 2.0's grammar, which memory64 widened.
    auto const neither = FeatureSet::wasm3().without(Feature::Memory64);
    CHECK(errorOf(flags(0x04), neither) == DecodeErrorCode::LimitsFlagTooLarge);
    CHECK(errorOf(flags(0x08), neither) == DecodeErrorCode::LimitsFlagTooLarge);
}

TEST_CASE("Decoder.valueTypes")
{
    auto const typeWith = [](uint8_t type) {
        return moduleOf({ 0x01, 0x05, 0x01, 0x60, 0x01, type, 0x00 });
    };
    CHECK_FALSE(errorOf(typeWith(0x7F), FeatureSet::none()).has_value());
    CHECK_FALSE(errorOf(typeWith(0x7C), FeatureSet::none()).has_value());
    CHECK(errorOf(typeWith(0x7B), FeatureSet::none()) == DecodeErrorCode::MalformedReferenceType);
    CHECK_FALSE(errorOf(typeWith(0x7B), FeatureSet::none().with(Feature::Simd)).has_value());
    CHECK(errorOf(typeWith(0x70), FeatureSet::none()) == DecodeErrorCode::MalformedReferenceType);
    CHECK_FALSE(errorOf(typeWith(0x6F)).has_value());
    CHECK(errorOf(typeWith(0x40)) == DecodeErrorCode::MalformedReferenceType);
    // An externref table needs reference types; a funcref table is MVP.
    CHECK(errorOf(moduleOf({ 0x04, 0x04, 0x01, 0x6F, 0x00, 0x00 }), FeatureSet::none())
          == DecodeErrorCode::MalformedReferenceType);
    CHECK_FALSE(errorOf(moduleOf({ 0x04, 0x04, 0x01, 0x70, 0x00, 0x00 }), FeatureSet::none()).has_value());
    // spec binary.wast: a type form as a two-byte signed LEB128 is too long.
    CHECK(errorOf(moduleOf({ 0x01, 0x05, 0x01, 0xE0, 0x7F, 0x00, 0x00 }))
          == DecodeErrorCode::IntegerRepresentationTooLong);
    CHECK(errorOf(moduleOf({ 0x01, 0x04, 0x01, 0x5F, 0x00, 0x00 }))
          == DecodeErrorCode::MalformedFunctionType);
}

TEST_CASE("Decoder.globals")
{
    auto const module = decoded(moduleOf({ 0x06, 0x12, 0x02, 0x7F, 0x00, 0x41, 0x7F, 0x0B, 0x7C, 0x01,
                                           0x44, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x3F, 0x0B }));
    REQUIRE(module.globals.size() == 2);
    REQUIRE(module.globals[0].init.size() == 2);
    CHECK(module.globals[0].init[0].opcode == Opcode::I32Const);
    CHECK(module.globals[0].init[0].c == 0xFFFF'FFFF);
    CHECK(module.globals[0].init[1].opcode == Opcode::End);
    CHECK(module.globals[1].type.mutability == Mutability::Var);
    CHECK(module.globals[1].init[0].c == 0x3FF0'0000'0000'0000);

    // spec binary.wast: an init expression missing its end reads the next section as an opcode, here
    // throw_ref (0x0A), whose proposal WebAssembly 2.0 lacks; the message is "illegal opcode 0a" all the
    // same.
    auto const missingEnd = moduleOf({ 0x01, 0x04, 0x01, 0x60, 0x00, 0x00, 0x03, 0x02, 0x01, 0x00, 0x06, 0x05,
                                       0x01, 0x7F, 0x00, 0x41, 0x00, 0x0A, 0x04, 0x01, 0x02, 0x00, 0x0B });
    CHECK(errorOf(missingEnd) == DecodeErrorCode::IllegalOpcode);
    CHECK(disabledOf(missingEnd, FeatureSet::wasm2()) == Feature::ExceptionHandling);
    // ref.func belongs to reference types.
    auto const refFunc = moduleOf({ 0x06, 0x06, 0x01, 0x70, 0x00, 0xD2, 0x00, 0x0B });
    CHECK_FALSE(errorOf(refFunc).has_value());
    CHECK(errorOf(refFunc, FeatureSet::none()) == DecodeErrorCode::MalformedReferenceType);
}

TEST_CASE("Decoder.refFuncNeedsReferenceTypes")
{
    // Initializers whose type decodes without reference types, so the ref.func gate is what fails.
    auto const elementOffset = moduleOf({ 0x09, 0x07, 0x01, 0x00, 0xD2, 0x00, 0x0B, 0x01, 0x00 });
    CHECK(errorOf(elementOffset, FeatureSet::none()) == DecodeErrorCode::IllegalOpcode);
    CHECK(disabledOf(elementOffset, FeatureSet::none()) == Feature::ReferenceTypes);
    CHECK_FALSE(
        errorOf(elementOffset, FeatureSet::none().with(Feature::ReferenceTypes).with(Feature::BulkMemory))
            .has_value());
    auto const i32Global = moduleOf({ 0x06, 0x06, 0x01, 0x7F, 0x00, 0xD2, 0x00, 0x0B });
    CHECK(errorOf(i32Global, FeatureSet::none()) == DecodeErrorCode::IllegalOpcode);
    CHECK(disabledOf(i32Global, FeatureSet::none()) == Feature::ReferenceTypes);
    CHECK_FALSE(errorOf(i32Global, FeatureSet::none().with(Feature::ReferenceTypes).with(Feature::BulkMemory))
                    .has_value());
}

TEST_CASE("Decoder.elementSegments")
{
    // flags 0: active on table 0, function indices.
    auto const mvp =
        decoded(moduleOf({ 0x09, 0x07, 0x01, 0x00, 0x41, 0x02, 0x0B, 0x01, 0x05 }), FeatureSet::none());
    REQUIRE(mvp.elements.size() == 1);
    CHECK(mvp.elements[0].mode == SegmentMode::Active);
    CHECK(mvp.elements[0].table == 0);
    CHECK(mvp.elements[0].offset[0].c == 2);
    CHECK(indicesOf(mvp.elements[0]) == std::vector<uint32_t> { 5 });

    // flags 1 passive, 2 active with a table, 3 declarative; 5..7 with expressions.
    auto const forms = decoded(moduleOf({ 0x09, 0x21, 0x06, 0x01, 0x00, 0x01, 0x07,       // passive
                                          0x02, 0x01, 0x41, 0x00, 0x0B, 0x00, 0x00,       // table 1
                                          0x03, 0x00, 0x01, 0x03,                         // declarative
                                          0x04, 0x41, 0x00, 0x0B, 0x01, 0xD2, 0x01, 0x0B, // exprs, table 0
                                          0x05, 0x6F, 0x01, 0xD0, 0x6F, 0x0B,             // externref
                                          0x07, 0x70, 0x00 }));
    REQUIRE(forms.elements.size() == 6);
    CHECK(forms.elements[0].mode == SegmentMode::Passive);
    CHECK(indicesOf(forms.elements[0]) == std::vector<uint32_t> { 7 });
    CHECK(forms.elements[1].mode == SegmentMode::Active);
    CHECK(forms.elements[1].table == 1);
    CHECK(indicesOf(forms.elements[1]).empty());
    CHECK(forms.elements[2].mode == SegmentMode::Declarative);
    CHECK(expressionsOf(forms.elements[3]).at(0).at(0).opcode == Opcode::RefFunc);
    CHECK(forms.elements[4].type == isoline::binary::ExternRef);
    CHECK(expressionsOf(forms.elements[4]).at(0).at(0).opcode == Opcode::RefNull);
    CHECK(expressionsOf(forms.elements[4]).at(0).at(0).type == isoline::binary::ExternRef);
    CHECK(forms.elements[5].mode == SegmentMode::Declarative);
    // Function indices are (ref func)s, as WebAssembly 3.0 types them; flag 4's expressions funcrefs.
    CHECK(mvp.elements[0].type == isoline::binary::NonNullFuncRef);
    CHECK(forms.elements[0].type == isoline::binary::NonNullFuncRef);
    CHECK(forms.elements[2].type == isoline::binary::NonNullFuncRef);
    CHECK(forms.elements[3].type == isoline::binary::FuncRef);

    CHECK(errorOf(moduleOf({ 0x09, 0x03, 0x01, 0x08, 0x00 }))
          == DecodeErrorCode::MalformedElementSegmentKind);
    CHECK(errorOf(moduleOf({ 0x09, 0x04, 0x01, 0x01, 0x01, 0x00 })) == DecodeErrorCode::MalformedElementKind);
    // Beyond flags 0, the forms need bulk memory; declarative ones need reference types.
    CHECK(errorOf(moduleOf({ 0x09, 0x04, 0x01, 0x01, 0x00, 0x00 }), FeatureSet::none())
          == DecodeErrorCode::MalformedElementSegmentKind);
    CHECK(errorOf(moduleOf({ 0x09, 0x04, 0x01, 0x03, 0x00, 0x00 }),
                  FeatureSet::none().with(Feature::BulkMemory))
          == DecodeErrorCode::MalformedElementSegmentKind);
    // spec binary.wast: 2 segments declared, 1 given, at the end of the module.
    CHECK(errorOf(moduleOf({ 0x09, 0x07, 0x02, 0x00, 0x41, 0x00, 0x0B, 0x01, 0x00 }))
          == DecodeErrorCode::UnexpectedEnd);
}

TEST_CASE("Decoder.dataSegments")
{
    auto const section = std::vector<uint8_t> {
        0x03,                                     // three segments
        0x00, 0x41, 0x03, 0x0B, 0x02, 0x68, 0x69, // "hi", active on memory 0 at 3
        0x01, 0x00,                               // passive, empty
        0x02, 0x01, 0x41, 0x00, 0x0B, 0x00,       // empty, active on memory 1
    };
    auto const module = decoded(ModuleBuilder {}.rawSection(11, section).build());
    REQUIRE(module.data.size() == 3);
    CHECK(module.data[0].mode == SegmentMode::Active);
    CHECK(module.data[0].offset[0].c == 3);
    CHECK(module.data[0].bytes == rawBytes({ 0x68, 0x69 })); // "hi"
    CHECK(module.data[1].mode == SegmentMode::Passive);
    CHECK(module.data[1].bytes.empty());
    CHECK(module.data[2].memory == 1);

    CHECK(errorOf(moduleOf({ 0x0B, 0x03, 0x01, 0x03, 0x00 })) == DecodeErrorCode::MalformedDataSegmentKind);
    CHECK(errorOf(moduleOf({ 0x0B, 0x03, 0x01, 0x01, 0x00 }), FeatureSet::none())
          == DecodeErrorCode::MalformedDataSegmentKind);
    // spec binary.wast: 7 bytes declared, 6 given; within bounds counting the length, then short.
    CHECK(errorOf(moduleOf({ 0x05,
                             0x03,
                             0x01,
                             0x00,
                             0x01,
                             0x0B,
                             0x0C,
                             0x01,
                             0x00,
                             0x41,
                             0x03,
                             0x0B,
                             0x07,
                             'a',
                             'b',
                             'c',
                             'd',
                             'e',
                             'f' }))
          == DecodeErrorCode::UnexpectedEnd);
    // spec binary.wast: 5 bytes declared, 6 given.
    CHECK(errorOf(moduleOf({ 0x05,
                             0x03,
                             0x01,
                             0x00,
                             0x01,
                             0x0B,
                             0x0C,
                             0x01,
                             0x00,
                             0x41,
                             0x00,
                             0x0B,
                             0x05,
                             'a',
                             'b',
                             'c',
                             'd',
                             'e',
                             'f' }))
          == DecodeErrorCode::SectionSizeMismatch);
}

TEST_CASE("Decoder.dataCount")
{
    auto const module = decoded(moduleOf({ 0x0C, 0x01, 0x01, 0x0B, 0x03, 0x01, 0x01, 0x00 }));
    CHECK(module.dataCount == 1U);
    CHECK(errorOf(moduleOf({ 0x0C, 0x01, 0x02, 0x0B, 0x03, 0x01, 0x01, 0x00 }))
          == DecodeErrorCode::DataCountAndDataSectionInconsistent);
    CHECK(errorOf(moduleOf({ 0x0C, 0x01, 0x01 })) == DecodeErrorCode::DataCountAndDataSectionInconsistent);
}

TEST_CASE("Decoder.startSection")
{
    CHECK(decoded(moduleOf({ 0x08, 0x01, 0x03 })).start == 3U);
}

TEST_CASE("Decoder.implementationLimits")
{
    auto const twoTypes = ModuleBuilder {}.type(FuncType {}).type(FuncType {}).build();
    CHECK(errorOf(twoTypes, FeatureSet::wasm2(), DecodeLimits { .maxTypes = 1 })
          == DecodeErrorCode::ImplementationLimitExceeded);
    auto const twoParams = moduleOf({ 0x01, 0x06, 0x01, 0x60, 0x02, 0x7F, 0x7F, 0x00 });
    CHECK(errorOf(twoParams, FeatureSet::wasm2(), DecodeLimits { .maxParams = 1 })
          == DecodeErrorCode::ImplementationLimitExceeded);
    auto const body = ModuleBuilder {}.type(FuncType {}).function(0, {}, { 0x01, 0x01, 0x01 }).build();
    CHECK(errorOf(body, FeatureSet::wasm2(), DecodeLimits { .maxFunctionBodySize = 4 })
          == DecodeErrorCode::ImplementationLimitExceeded);
    CHECK_FALSE(errorOf(body, FeatureSet::wasm2(), DecodeLimits { .maxFunctionBodySize = 5 }).has_value());
    // Two funcref tables of no minimum size.
    auto const twoTables = moduleOf({ 0x04, 0x07, 0x02, 0x70, 0x00, 0x00, 0x70, 0x00, 0x00 });
    CHECK(errorOf(twoTables, FeatureSet::wasm2(), DecodeLimits { .maxTables = 1 })
          == DecodeErrorCode::ImplementationLimitExceeded);
    CHECK_FALSE(errorOf(twoTables, FeatureSet::wasm2(), DecodeLimits { .maxTables = 2 }).has_value());
    CHECK(DecodeLimits {}.maxTables == 100'000);
}

TEST_CASE("Decoder.localsStayRuns")
{
    // Each body declares 50,000 i32 locals in one run of 6 bytes. Expanded, 20,000 of them would
    // take gigabytes; kept as runs they take memory in proportion to the input.
    constexpr auto FunctionCount = uint32_t { 20'000 };
    constexpr auto Body = std::to_array<uint8_t>({ 0x06, 0x01, 0xD0, 0x86, 0x03, 0x7F, 0x0B });
    auto functions = encodeU32(FunctionCount);
    functions.resize(functions.size() + FunctionCount, 0x00); // all of type 0
    auto code = encodeU32(FunctionCount);
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, FunctionCount))
        append(code, Body);
    auto const bytes = ModuleBuilder {}
                           .rawSection(1, { 0x01, 0x60, 0x00, 0x00 })
                           .rawSection(3, std::move(functions))
                           .rawSection(10, std::move(code))
                           .build();

    auto const module = decoded(bytes);
    REQUIRE(module.code.size() == FunctionCount);
    for (auto const& function: module.code)
    {
        REQUIRE(function.locals.size() == 1);
        REQUIRE(function.localCount == 50'000);
    }
    CHECK(localType(module.code.back(), 49'999) == isoline::binary::I32);
    CHECK_FALSE(localType(module.code.back(), 50'000).has_value());
}

TEST_CASE("Decoder.elementItemsStayIndices")
{
    // flags 0, offset (i32.const 0), then 1,000 function indices of one byte each.
    constexpr auto ItemCount = uint32_t { 1'000 };
    auto section = std::vector<uint8_t> { 0x01, 0x00, 0x41, 0x00, 0x0B };
    append(section, encodeU32(ItemCount));
    section.resize(section.size() + ItemCount, 0x00);
    auto const bytes = ModuleBuilder {}.rawSection(9, std::move(section)).build();

    auto const module = decoded(bytes);
    REQUIRE(module.elements.size() == 1);
    auto const& indices = indicesOf(module.elements[0]);
    CHECK(indices.size() == ItemCount);
    CHECK(std::ranges::all_of(indices, [](uint32_t index) { return index == 0; }));

    CHECK(errorOf(bytes, FeatureSet::wasm2(), DecodeLimits { .maxElementItems = ItemCount - 1 })
          == DecodeErrorCode::ImplementationLimitExceeded);
    CHECK_FALSE(
        errorOf(bytes, FeatureSet::wasm2(), DecodeLimits { .maxElementItems = ItemCount }).has_value());
}

namespace
{

/// A module of one function of type [] -> [] whose instructions are @p bodyWithoutEnd and an end.
std::vector<std::byte> functionWith(std::vector<uint8_t> bodyWithoutEnd)
{
    return ModuleBuilder {}.type(FuncType {}).function(0, {}, std::move(bodyWithoutEnd)).build();
}

/// A module of one function of type [] -> [] whose code section, its id included, is @p codeSection.
std::vector<std::byte> oneFunctionWithCode(std::initializer_list<uint8_t> codeSection)
{
    auto bytes = moduleOf({ 0x01, 0x04, 0x01, 0x60, 0x00, 0x00, 0x03, 0x02, 0x01, 0x00 });
    for (auto const byte: codeSection)
        bytes.push_back(std::byte { byte });
    return bytes;
}

/// The one function @p bytes decode to; fails the test when they do not decode.
Function functionOf(std::span<std::byte const> bytes, FeatureSet const& features = FeatureSet::wasm2())
{
    auto module = decoded(bytes, features);
    REQUIRE(module.code.size() == 1);
    return std::move(module.code[0]);
}

/// The opcodes of @p body, in order.
std::vector<Opcode> opcodesOf(std::span<Instruction const> body)
{
    auto opcodes = std::vector<Opcode> {};
    for (auto const& instruction: body)
        opcodes.push_back(instruction.opcode);
    return opcodes;
}

} // namespace

TEST_CASE("Decoder.blockMatching")
{
    // 0 block, 1 i32.const 1, 2 if, 3 nop, 4 else, 5 nop, 6 end (if), 7 end (block), 8 end (function)
    auto const function =
        functionOf(functionWith({ 0x02, 0x40, 0x41, 0x01, 0x04, 0x40, 0x01, 0x05, 0x01, 0x0B, 0x0B }));
    REQUIRE(opcodesOf(function.body)
            == std::vector { Opcode::Block,
                             Opcode::I32Const,
                             Opcode::If,
                             Opcode::Nop,
                             Opcode::Else,
                             Opcode::Nop,
                             Opcode::End,
                             Opcode::End,
                             Opcode::End });
    CHECK(function.body[0].endIndex == 7);
    CHECK(function.body[2].elseIndex == 4);
    CHECK(function.body[2].endIndex == 6);
    CHECK(function.body[4].endIndex == 6);
    CHECK(function.bodyOffset > 0);

    // A loop resolves as a block does.
    auto const loop = functionOf(functionWith({ 0x03, 0x40, 0x0C, 0x00, 0x0B }));
    CHECK(loop.body[0].endIndex == 2);
}

TEST_CASE("Decoder.ifWithoutElse")
{
    auto const function = functionOf(functionWith({ 0x41, 0x01, 0x04, 0x40, 0x01, 0x0B }));
    REQUIRE(function.body.size() == 5);
    CHECK(function.body[1].endIndex == 3);
    CHECK(function.body[1].elseIndex == function.body[1].endIndex);
}

TEST_CASE("Decoder.elseOutsideIf")
{
    // The spec interpreter ends the open sequence at else, then expects the end that closes it.
    CHECK(errorOf(functionWith({ 0x05 })) == DecodeErrorCode::EndOpcodeExpected);
    CHECK(errorOf(functionWith({ 0x02, 0x40, 0x05, 0x0B })) == DecodeErrorCode::EndOpcodeExpected);
    CHECK(errorOf(functionWith({ 0x41, 0x00, 0x04, 0x40, 0x05, 0x05, 0x0B }))
          == DecodeErrorCode::EndOpcodeExpected);
}

TEST_CASE("Decoder.blockTypes")
{
    auto const body = std::vector<uint8_t> {
        0x02, 0x40, 0x0B,                         // block (empty)
        0x02, 0x7F, 0x00, 0x0B,                   // block (result i32), unreachable
        0x02, 0x00, 0x0B,                         // block (type 0)
        0x02, 0x80, 0x80, 0x80, 0x80, 0x08, 0x0B, // block (type 2^31)
    };
    auto const function = functionOf(functionWith(body));
    using Kind = isoline::binary::BlockType::Kind;
    CHECK(function.body[0].blockType().kind == Kind::Empty);
    CHECK(function.body[2].blockType().kind == Kind::Value);
    CHECK(function.body[2].blockType().value == isoline::binary::I32);
    CHECK(function.body[5].blockType().kind == Kind::TypeIndex);
    CHECK(function.body[5].blockType().typeIndex == 0);
    CHECK(function.body[7].blockType().kind == Kind::TypeIndex);
    CHECK(function.body[7].blockType().typeIndex == 0x8000'0000U);

    // A negative s33 that encodes no value type is reported as the last alternative, a reference
    // type, reports it (spec decode.ml block_type).
    CHECK(errorOf(functionWith({ 0x02, 0x60, 0x0B })) == DecodeErrorCode::MalformedReferenceType);
    // A type index needs multi-value; without it 0x00 is a malformed value type.
    CHECK(errorOf(functionWith({ 0x02, 0x00, 0x0B }), FeatureSet::none())
          == DecodeErrorCode::MalformedReferenceType);
    CHECK_FALSE(errorOf(functionWith({ 0x02, 0x00, 0x0B }), FeatureSet::none().with(Feature::MultiValue))
                    .has_value());
    CHECK(errorOf(functionWith({ 0x02, 0x6F, 0x0B }), FeatureSet::none())
          == DecodeErrorCode::MalformedReferenceType);
}

TEST_CASE("Decoder.brTable")
{
    auto const body =
        std::vector<uint8_t> { 0x41, 0x00, 0x0E, 0x02, 0x00, 0x01, 0x02, 0x41, 0x00, 0x0E, 0x00, 0x05 };
    auto const function = functionOf(functionWith(body));
    REQUIRE(function.body[1].opcode == Opcode::BrTable);
    auto const labels = labelsOf(function, function.body[1]);
    CHECK(std::vector(labels.begin(), labels.end()) == std::vector<uint32_t> { 0, 1, 2 });
    auto const second = labelsOf(function, function.body[3]);
    CHECK(std::vector(second.begin(), second.end()) == std::vector<uint32_t> { 5 });
    CHECK(function.brTableLabels.size() == 4);

    CHECK(errorOf(functionWith(body), FeatureSet::wasm2(), DecodeLimits { .maxBrTableSize = 1 })
          == DecodeErrorCode::ImplementationLimitExceeded);
    CHECK_FALSE(
        errorOf(functionWith(body), FeatureSet::wasm2(), DecodeLimits { .maxBrTableSize = 2 }).has_value());
}

TEST_CASE("Decoder.typedSelect")
{
    auto const function = functionOf(functionWith({ 0x1C, 0x01, 0x7F, 0x1C, 0x01, 0x6F }));
    auto const first = selectTypesOf(function, function.body[0]);
    auto const second = selectTypesOf(function, function.body[1]);
    REQUIRE(first.size() == 1);
    REQUIRE(second.size() == 1);
    CHECK(first[0] == isoline::binary::I32);
    CHECK(second[0] == isoline::binary::ExternRef);
}

TEST_CASE("Decoder.constBits")
{
    auto const function =
        functionOf(functionWith({ 0x43, 0x01, 0x00, 0x80, 0x7F, 0x1A, 0x44, 0x01, 0x00, 0x00, 0x00,
                                  0x00, 0x00, 0xF0, 0x7F, 0x1A, 0x41, 0x7F, 0x1A, 0x42, 0x7F, 0x1A }));
    CHECK(function.body[0].c == 0x7F80'0001);
    CHECK(function.body[2].c == 0x7FF0'0000'0000'0001);
    CHECK(function.body[4].c == 0xFFFF'FFFF);
    CHECK(function.body[6].c == 0xFFFF'FFFF'FFFF'FFFF);
}

TEST_CASE("Decoder.memArg")
{
    auto const function = functionOf(functionWith({ 0x41, 0x00, 0x28, 0x02, 0x08, 0x1A }));
    CHECK(function.body[1].opcode == Opcode::I32Load);
    CHECK(function.body[1].a == 2);
    CHECK(function.body[1].c == 8);
    CHECK(function.body[1].b == 0);
    // WebAssembly 2.0 caps the alignment exponent below 32.
    CHECK(errorOf(functionWith({ 0x41, 0x00, 0x28, 0x20, 0x00, 0x1A }))
          == DecodeErrorCode::MalformedMemopFlags);
    CHECK_FALSE(errorOf(functionWith({ 0x41, 0x00, 0x28, 0x1F, 0x00, 0x1A })).has_value());
}

TEST_CASE("Decoder.immediates")
{
    auto const body = std::vector<uint8_t> {
        0x11, 0x02, 0x01,       // call_indirect type 2, table 1
        0xFC, 0x0C, 0x03, 0x01, // table.init element 3, table 1
        0xFC, 0x0E, 0x04, 0x05, // table.copy to 4, from 5
        0xD0, 0x6F,             // ref.null extern
        0x20, 0x07,             // local.get 7
    };
    auto const function = functionOf(functionWith(body));
    CHECK(function.body[0].opcode == Opcode::CallIndirect);
    CHECK(function.body[0].a == 2);
    CHECK(function.body[0].b == 1);
    CHECK(function.body[1].opcode == Opcode::TableInit);
    CHECK(function.body[1].a == 3);
    CHECK(function.body[1].b == 1);
    CHECK(function.body[2].opcode == Opcode::TableCopy);
    CHECK(function.body[2].a == 4);
    CHECK(function.body[2].b == 5);
    CHECK(function.body[3].opcode == Opcode::RefNull);
    CHECK(function.body[3].type == isoline::binary::ExternRef);
    CHECK(function.body[4].opcode == Opcode::LocalGet);
    CHECK(function.body[4].a == 7);
}

TEST_CASE("Decoder.typeIndexImmediate")
{
    // call_ref's immediate is a u32 type index; br_on_null's and br_on_non_null's a label.
    auto const body = std::vector<uint8_t> {
        0x14, 0x85, 0x01, // call_ref type 133, a two-byte LEB128
        0xD4,             // ref.as_non_null
        0xD5, 0x02,       // br_on_null 2
        0xD6, 0x03,       // br_on_non_null 3
    };
    auto const function = functionOf(functionWith(body), FeatureSet::wasm3());
    REQUIRE(function.body.size() == 5);
    CHECK(function.body[0].opcode == Opcode::CallRef);
    CHECK(function.body[0].a == 133);
    CHECK(function.body[1].opcode == Opcode::RefAsNonNull);
    CHECK(function.body[2].opcode == Opcode::BrOnNull);
    CHECK(function.body[2].a == 2);
    CHECK(function.body[3].opcode == Opcode::BrOnNonNull);
    CHECK(function.body[3].a == 3);

    // A truncated index is an unexpected end, one past 32 bits an integer too large.
    CHECK(errorOf(functionWith({ 0x14, 0x80 }), FeatureSet::wasm3()) == DecodeErrorCode::UnexpectedEnd);
    CHECK(errorOf(functionWith({ 0x14, 0x80, 0x80, 0x80, 0x80, 0x10 }), FeatureSet::wasm3())
          == DecodeErrorCode::IntegerTooLarge);
    // The instructions need typed function references.
    for (auto const opcode: { uint8_t { 0x14 }, uint8_t { 0xD4 }, uint8_t { 0xD5 }, uint8_t { 0xD6 } })
    {
        INFO(int { opcode });
        auto const withoutTypedReferences = FeatureSet::wasm3().without(Feature::TypedFunctionReferences);
        CHECK(errorOf(functionWith({ opcode, 0x00 }), withoutTypedReferences)
              == DecodeErrorCode::IllegalOpcode);
        CHECK(disabledOf(functionWith({ opcode, 0x00 }), withoutTypedReferences)
              == Feature::TypedFunctionReferences);
    }
}

TEST_CASE("Decoder.tailCallsNeedTheirFeatures")
{
    // return_call 1, return_call_indirect type 2 table 3, return_call_ref type 4.
    auto const function =
        functionOf(functionWith({ 0x12, 0x01, 0x13, 0x02, 0x03, 0x15, 0x04 }), FeatureSet::wasm3());
    REQUIRE(function.body.size() == 4);
    CHECK(function.body[0].opcode == Opcode::ReturnCall);
    CHECK(function.body[0].a == 1);
    CHECK(function.body[1].opcode == Opcode::ReturnCallIndirect);
    CHECK(function.body[1].a == 2);
    CHECK(function.body[1].b == 3);
    CHECK(function.body[2].opcode == Opcode::ReturnCallRef);
    CHECK(function.body[2].a == 4);

    // Each needs tail calls; return_call_ref calls through a typed reference, so it needs typed
    // function references too.
    struct Row
    {
        std::vector<uint8_t> body;
        Feature disabled;
    };
    auto const rows = std::to_array<Row>({
        { .body = { 0x12, 0x00 }, .disabled = Feature::TailCall },
        { .body = { 0x13, 0x00, 0x00 }, .disabled = Feature::TailCall },
        { .body = { 0x15, 0x00 }, .disabled = Feature::TailCall },
        { .body = { 0x15, 0x00 }, .disabled = Feature::TypedFunctionReferences },
    });
    for (auto const& row: rows)
    {
        INFO(int { row.body.front() } << " without " << static_cast<int>(row.disabled));
        CHECK(errorOf(functionWith(row.body), FeatureSet::wasm3().without(row.disabled))
              == DecodeErrorCode::IllegalOpcode);
        CHECK(disabledOf(functionWith(row.body), FeatureSet::wasm3().without(row.disabled)) == row.disabled);
    }
    CHECK_FALSE(
        errorOf(functionWith({ 0x12, 0x00 }), FeatureSet::wasm3().without(Feature::TypedFunctionReferences))
            .has_value());
}

TEST_CASE("Decoder.v128ConstGoesToSideTable")
{
    // v128.const 0x00 .. 0x0F, in a global's initializer and again (0x10 .. 0x1F) in a body.
    auto const constant = [](uint8_t first) {
        auto bytes = std::vector<uint8_t> { 0xFD, 0x0C };
        for (auto const index: std::views::iota(0, 16))
            bytes.push_back(static_cast<uint8_t>(first + index));
        return bytes;
    };
    auto body = constant(0x10);
    body.push_back(0x1A); // drop
    auto const bytes = ModuleBuilder {}
                           .type(FuncType {})
                           .global(isoline::binary::GlobalType { .type = isoline::binary::V128,
                                                                 .mutability = Mutability::Const },
                                   constant(0x00))
                           .function(0, {}, std::move(body))
                           .build();
    auto const module = decoded(bytes);
    REQUIRE(module.v128Literals.size() == 2);
    REQUIRE(module.globals.size() == 1);
    REQUIRE(module.code.size() == 1);
    auto const& global = module.globals[0].init[0];
    auto const& local = module.code[0].body[0];
    CHECK(global.opcode == Opcode::V128Const);
    CHECK(global.a == 0);
    CHECK(local.opcode == Opcode::V128Const);
    CHECK(local.a == 1);
    auto const first = isoline::binary::v128LiteralOf(module, global);
    auto const second = isoline::binary::v128LiteralOf(module, local);
    CHECK(std::to_integer<int>(first[0]) == 0x00);
    CHECK(std::to_integer<int>(first[15]) == 0x0F);
    CHECK(std::to_integer<int>(second[0]) == 0x10);
    CHECK(std::to_integer<int>(second[15]) == 0x1F);

    // Fewer than 16 bytes left is an unexpected end.
    CHECK(errorOf(functionWith({ 0xFD, 0x0C, 0x00, 0x01, 0x02 })) == DecodeErrorCode::UnexpectedEnd);
}

TEST_CASE("Decoder.simdImmediates")
{
    auto body = std::vector<uint8_t> {
        0xFD, 0x1B, 0x03,             // i32x4.extract_lane 3
        0xFD, 0x54, 0x00, 0x08, 0x0F, // v128.load8_lane align=0 offset=8 lane 15
        0xFD, 0x0D,                   // i8x16.shuffle, then its 16 lane bytes
    };
    for (auto const lane: std::views::iota(0, 16))
        body.push_back(static_cast<uint8_t>(31 - lane));
    auto const function = functionOf(functionWith(std::move(body)));
    CHECK(function.body[0].opcode == Opcode::I32x4ExtractLane);
    CHECK(function.body[0].lane == 3);
    CHECK(function.body[1].opcode == Opcode::V128Load8Lane);
    CHECK(function.body[1].a == 0);
    CHECK(function.body[1].c == 8);
    CHECK(function.body[1].lane == 15);
    CHECK(function.body[2].opcode == Opcode::I8x16Shuffle);
    CHECK(function.body[2].a == 0);
    // A lane byte is a byte, never a LEB128: 0x80 is lane 128, which validation rejects.
    auto const wide = functionOf(functionWith({ 0xFD, 0x15, 0x80, 0x1A }));
    CHECK(wide.body[0].lane == 0x80);
    CHECK(wide.body[1].opcode == Opcode::Drop);
    // The memory argument of a lane access follows MemArg's rules.
    CHECK(errorOf(functionWith({ 0xFD, 0x54, 0x20, 0x00, 0x00 })) == DecodeErrorCode::MalformedMemopFlags);
}

TEST_CASE("Decoder.memArgOffsetIsU64WithMemory64")
{
    // i64.const 0, i32.load align=2 offset=2^32, drop.
    auto const load = std::vector<uint8_t> { 0x42, 0x00, 0x28, 0x02, 0x80, 0x80, 0x80, 0x80, 0x10, 0x1A };
    auto const function = functionOf(functionWith(load), FeatureSet::wasm3());
    CHECK(function.body[1].c == uint64_t { 1 } << 32);
    CHECK(errorOf(functionWith(load)) == DecodeErrorCode::IntegerTooLarge);
}

TEST_CASE("Decoder.prefixed")
{
    auto const function = functionOf(functionWith({ 0xFC, 0x0A, 0x00, 0x00, 0xFC, 0x0B, 0x00 }));
    CHECK(opcodesOf(function.body) == std::vector { Opcode::MemoryCopy, Opcode::MemoryFill, Opcode::End });
}

TEST_CASE("Decoder.illegalOpcodeNamesItsBytes")
{
    // The interpreter's spelling: illegal for a byte, illegal2 for 0xFC's sub-opcodes, and its vector
    // decoder names 0xFD's by the sub-opcode alone.
    auto const messageOf = [](std::vector<uint8_t> body, FeatureSet const& features = FeatureSet::wasm3()) {
        auto const result = decodeModule(functionWith(std::move(body)), features);
        REQUIRE_FALSE(result.has_value());
        return isoline::binary::specMessage(result.error());
    };
    CHECK(messageOf({ 0xFF }) == "illegal opcode ff");
    CHECK(messageOf({ 0x06 }) == "illegal opcode 06");
    CHECK(messageOf({ 0xFC, 0x12 }) == "illegal opcode fc 12");
    CHECK(messageOf({ 0xFC, 0x80, 0x02 }) == "illegal opcode fc 100");
    CHECK(messageOf({ 0xFD, 0x9A, 0x01 }) == "illegal opcode 9a");
    // An instruction of a disabled proposal is named as an unknown one is, then its feature.
    CHECK(messageOf({ 0xFD, 0x0C }, FeatureSet::none()) == "illegal opcode 0c (simd not enabled)");
}

namespace
{

/// A module a disabled proposal would decode, the spec's error without it, and the proposal named.
struct GatedForm
{
    std::string_view name;           ///< What it uses.
    std::vector<std::byte> bytes;    ///< The module.
    FeatureSet features;             ///< The decoder's set, which lacks the proposal.
    DecodeErrorCode code;            ///< The error a decoder of that set reports, as the spec's.
    std::optional<Feature> disabled; ///< The proposal named; none where no proposal decodes it.
};

} // namespace

TEST_CASE("Decoder.everyGatedFormNamesItsProposal")
{
    auto const wasm3 = FeatureSet::wasm3();
    auto const rows = std::vector<GatedForm> {
        { "tag section",
          moduleOf({ 0x0D, 0x01, 0x00 }),
          FeatureSet::wasm2(),
          DecodeErrorCode::MalformedSectionId,
          Feature::ExceptionHandling },
        { "data count section",
          moduleOf({ 0x0C, 0x01, 0x00 }),
          FeatureSet::none(),
          DecodeErrorCode::MalformedSectionId,
          Feature::BulkMemory },
        { "unknown section",
          moduleOf({ 0x0E, 0x01, 0x00 }),
          wasm3,
          DecodeErrorCode::MalformedSectionId,
          std::nullopt },
        { "v128",
          moduleOf({ 0x01, 0x05, 0x01, 0x60, 0x01, 0x7B, 0x00 }),
          FeatureSet::none(),
          DecodeErrorCode::MalformedReferenceType,
          Feature::Simd },
        { "exnref",
          moduleOf({ 0x01, 0x05, 0x01, 0x60, 0x01, 0x69, 0x00 }),
          FeatureSet::wasm2(),
          DecodeErrorCode::MalformedReferenceType,
          Feature::ExceptionHandling },
        { "(ref null $t)",
          moduleOf({ 0x01, 0x06, 0x01, 0x60, 0x01, 0x63, 0x00, 0x00 }),
          FeatureSet::wasm2(),
          DecodeErrorCode::MalformedReferenceType,
          Feature::TypedFunctionReferences },
        { "unknown value type",
          moduleOf({ 0x01, 0x05, 0x01, 0x60, 0x01, 0x50, 0x00 }),
          wasm3,
          DecodeErrorCode::MalformedReferenceType,
          std::nullopt },
        { "ref.null of a type index",
          functionWith({ 0xD0, 0x00, 0x1A }),
          FeatureSet::wasm2(),
          DecodeErrorCode::MalformedReferenceType,
          Feature::TypedFunctionReferences },
        { "ref.null exn",
          functionWith({ 0xD0, 0x69, 0x1A }),
          FeatureSet::wasm2(),
          DecodeErrorCode::MalformedReferenceType,
          Feature::ExceptionHandling },
        { "tag import",
          moduleOf({ 0x02, 0x08, 0x01, 0x01, 0x6D, 0x01, 0x74, 0x04, 0x00, 0x00 }),
          FeatureSet::wasm2(),
          DecodeErrorCode::MalformedImportKind,
          Feature::ExceptionHandling },
        { "tag export",
          moduleOf({ 0x07, 0x05, 0x01, 0x01, 0x74, 0x04, 0x00 }),
          FeatureSet::wasm2(),
          DecodeErrorCode::MalformedExportKind,
          Feature::ExceptionHandling },
        { "passive element segment",
          moduleOf({ 0x09, 0x02, 0x01, 0x01 }),
          FeatureSet::none(),
          DecodeErrorCode::MalformedElementSegmentKind,
          Feature::BulkMemory },
        { "declarative element segment",
          moduleOf({ 0x09, 0x02, 0x01, 0x03 }),
          FeatureSet::none(),
          DecodeErrorCode::MalformedElementSegmentKind,
          Feature::ReferenceTypes },
        { "element segment flags 8",
          moduleOf({ 0x09, 0x02, 0x01, 0x08 }),
          wasm3,
          DecodeErrorCode::MalformedElementSegmentKind,
          std::nullopt },
        { "passive data segment",
          moduleOf({ 0x0B, 0x02, 0x01, 0x01 }),
          FeatureSet::none(),
          DecodeErrorCode::MalformedDataSegmentKind,
          Feature::BulkMemory },
        { "data segment flags 3",
          moduleOf({ 0x0B, 0x02, 0x01, 0x03 }),
          wasm3,
          DecodeErrorCode::MalformedDataSegmentKind,
          std::nullopt },
        { "i64 memory",
          moduleOf({ 0x05, 0x03, 0x01, 0x04, 0x01 }),
          FeatureSet::wasm2(),
          DecodeErrorCode::LimitsFlagTooLarge,
          Feature::Memory64 },
        { "page size, 2.0 grammar",
          moduleOf({ 0x05, 0x04, 0x01, 0x08, 0x01, 0x00 }),
          FeatureSet::wasm2(),
          DecodeErrorCode::LimitsFlagTooLarge,
          Feature::CustomPageSizes },
        { "page size, 3.0 grammar",
          moduleOf({ 0x05, 0x04, 0x01, 0x08, 0x01, 0x00 }),
          wasm3,
          DecodeErrorCode::MalformedLimitsFlags,
          Feature::CustomPageSizes },
        { "i64 page size, 2.0 grammar",
          moduleOf({ 0x05, 0x04, 0x01, 0x0C, 0x01, 0x00 }),
          FeatureSet::wasm2(),
          DecodeErrorCode::LimitsFlagTooLarge,
          Feature::Memory64 },
        { "table page size",
          moduleOf({ 0x04, 0x05, 0x01, 0x70, 0x08, 0x01, 0x00 }),
          wasm3.with(Feature::CustomPageSizes),
          DecodeErrorCode::MalformedLimitsFlags,
          std::nullopt },
        { "limits flags 2",
          moduleOf({ 0x05, 0x03, 0x01, 0x02, 0x01 }),
          FeatureSet::wasm2(),
          DecodeErrorCode::LimitsFlagTooLarge,
          std::nullopt },
        { "table with initializer",
          moduleOf({ 0x04, 0x02, 0x01, 0x40 }),
          FeatureSet::wasm2(),
          DecodeErrorCode::MalformedReferenceType,
          Feature::TypedFunctionReferences },
        { "block type index",
          functionWith({ 0x02, 0x00, 0x0B }),
          FeatureSet::none(),
          DecodeErrorCode::MalformedReferenceType,
          Feature::MultiValue },
        { "call_indirect table index",
          functionWith({ 0x41, 0x00, 0x11, 0x00, 0x01 }),
          FeatureSet::none(),
          DecodeErrorCode::ZeroByteExpected,
          Feature::ReferenceTypes },
        { "memory.size memory index",
          functionWith({ 0x3F, 0x01, 0x1A }),
          FeatureSet::wasm2(),
          DecodeErrorCode::ZeroByteExpected,
          Feature::MultiMemory },
        { "memarg memory index",
          functionWith({ 0x41, 0x00, 0x28, 0x40, 0x00, 0x00, 0x1A }),
          FeatureSet::wasm2(),
          DecodeErrorCode::MalformedMemopFlags,
          Feature::MultiMemory },
        { "memarg alignment 32",
          functionWith({ 0x41, 0x00, 0x28, 0x20, 0x00, 0x1A }),
          FeatureSet::wasm2(),
          DecodeErrorCode::MalformedMemopFlags,
          std::nullopt },
    };
    for (auto const& row: rows)
    {
        INFO(row.name);
        auto const result = decodeModule(row.bytes, row.features, DecodeLimits {});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == row.code);
        CHECK(result.error().disabled == row.disabled);
        // The spec's message comes first, whatever the proposal named.
        CHECK(
            isoline::binary::specMessage(result.error()).starts_with(isoline::binary::specMessage(row.code)));
    }
}

TEST_CASE("Decoder.disabledFeatureIsNamed")
{
    using isoline::binary::isImplementationLimit;
    // v128.const without SIMD: the feature that would decode it, at the opcode.
    auto const simd = decodeModule(functionWith({ 0xFD, 0x0C }), FeatureSet::wasm2().without(Feature::Simd));
    REQUIRE_FALSE(simd.has_value());
    CHECK(simd.error().code == DecodeErrorCode::IllegalOpcode);
    CHECK(simd.error().disabled == Feature::Simd);
    CHECK(isoline::binary::specMessage(simd.error()) == "illegal opcode 0c (simd not enabled)");
    CHECK_FALSE(isImplementationLimit(simd.error().code));
    // return_call_ref needs typed function references beside tail calls: the first one missing, in
    // FeatureTable order, is named.
    auto const tailCall = decodeModule(functionWith({ 0x15, 0x00 }), FeatureSet::wasm2());
    REQUIRE_FALSE(tailCall.has_value());
    CHECK(tailCall.error().disabled == Feature::TailCall);
    auto const typedRefs =
        decodeModule(functionWith({ 0x15, 0x00 }), FeatureSet::wasm2().with(Feature::TailCall));
    REQUIRE_FALSE(typedRefs.has_value());
    CHECK(typedRefs.error().disabled == Feature::TypedFunctionReferences);
    // An opcode of no proposal names none.
    auto const unknown = decodeModule(functionWith({ 0x06 }), FeatureSet::wasm3());
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().code == DecodeErrorCode::IllegalOpcode);
    CHECK_FALSE(unknown.error().disabled.has_value());
    // An error that names no proposal reads as the spec's alone.
    auto const bare = isoline::binary::DecodeError { DecodeErrorCode::IllegalOpcode, 0 };
    CHECK_FALSE(bare.disabled.has_value());
    CHECK(isoline::binary::specMessage(bare) == "illegal opcode");
}

TEST_CASE("Decoder.subjectOfAnErrorWithoutDetail")
{
    // A row without a detail spells its text alone, whatever subject an error carries.
    using isoline::binary::DecodeError;
    CHECK(isoline::binary::specMessage(DecodeError { DecodeErrorCode::UnexpectedEnd, 0, 7 })
          == "unexpected end of section or function");
    CHECK(isoline::binary::specMessage(DecodeError { DecodeErrorCode::IllegalOpcode, 0 })
          == "illegal opcode");
}

TEST_CASE("Decoder.unknownOpcode")
{
    CHECK(errorOf(functionWith({ 0x06 })) == DecodeErrorCode::IllegalOpcode);
    CHECK(errorOf(functionWith({ 0xFC, 0x12 })) == DecodeErrorCode::IllegalOpcode);
    CHECK(errorOf(functionWith({ 0xFF })) == DecodeErrorCode::IllegalOpcode);
    // The error stands at the opcode, its prefix included.
    auto const bytes = functionWith({ 0x01, 0xFC, 0x12 });
    auto const result = decodeModule(bytes, FeatureSet::wasm2());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().offset == bytes.size() - 3);
}

TEST_CASE("Decoder.missingEnd")
{
    // A last body without its end runs out of module.
    CHECK(errorOf(oneFunctionWithCode({ 0x0A, 0x05, 0x01, 0x03, 0x00, 0x41, 0x01 }))
          == DecodeErrorCode::UnexpectedEnd);
    // A body whose end lies past its declared size.
    CHECK(errorOf(oneFunctionWithCode({ 0x0A, 0x06, 0x01, 0x03, 0x00, 0x41, 0x01, 0x0B }))
          == DecodeErrorCode::SectionSizeMismatch);
    // spec binary.wast "Function with missing end marker (between two functions)": the next body's
    // size byte is read where end is expected. (WebAssembly 2.0's interpreter reports it so.)
    CHECK(errorOf(moduleOf({ 0x01, 0x04, 0x01, 0x60, 0x00, 0x00, 0x03, 0x03, 0x02, 0x00, 0x00, 0x0A, 0x0C,
                             0x02, 0x04, 0x00, 0x41, 0x01, 0x1A, 0x05, 0x00, 0x41, 0x01, 0x1A, 0x0B }))
          == DecodeErrorCode::EndOpcodeExpected);
    // spec binary.wast "Function with missing end marker (at EOF)" and "(at end of code sections)".
    CHECK(errorOf(oneFunctionWithCode({ 0x0A, 0x06, 0x01, 0x04, 0x00, 0x41, 0x01, 0x1A }))
          == DecodeErrorCode::UnexpectedEnd);
    CHECK(errorOf(oneFunctionWithCode(
              { 0x0A, 0x06, 0x01, 0x04, 0x00, 0x41, 0x01, 0x1A, 0x0B, 0x03, 0x01, 0x01, 0x00 }))
          == DecodeErrorCode::SectionSizeMismatch);
}

TEST_CASE("Decoder.memargWithMemoryIndex")
{
    // i32.const 0, i32.load flags=0x42 (alignment 2, a memory index follows) memory 3 offset 8, drop.
    auto const load = std::vector<uint8_t> { 0x41, 0x00, 0x28, 0x42, 0x03, 0x08, 0x1A };
    auto const function = functionOf(functionWith(load), FeatureSet::wasm3());
    CHECK(function.body[1].opcode == Opcode::I32Load);
    CHECK(function.body[1].a == 2);
    CHECK(function.body[1].b == 3);
    CHECK(function.body[1].c == 8);
    // Below bit 6 the memory is 0, and the alignment field takes every value below 64, which the
    // validator bounds by the natural alignment.
    auto const implicit =
        functionOf(functionWith({ 0x41, 0x00, 0x28, 0x3F, 0x08, 0x1A }), FeatureSet::wasm3());
    CHECK(implicit.body[1].a == 63);
    CHECK(implicit.body[1].b == 0);
    // The memory index is a u32 like any other index.
    CHECK(errorOf(functionWith({ 0x41, 0x00, 0x28, 0x42, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0x00, 0x1A }),
                  FeatureSet::wasm3())
          == DecodeErrorCode::IntegerTooLarge);
    // Without multi-memory the flags are 2.0's alignment exponent, below 32.
    CHECK(errorOf(functionWith(load)) == DecodeErrorCode::MalformedMemopFlags);
    CHECK(errorOf(functionWith(load), FeatureSet::wasm3().without(Feature::MultiMemory))
          == DecodeErrorCode::MalformedMemopFlags);
    // A lane access reads its memarg through the same reader: v128.load8_lane memory 2 offset 8 lane 15.
    auto const lane =
        functionOf(functionWith({ 0xFD, 0x54, 0x40, 0x02, 0x08, 0x0F }), FeatureSet::wasm3()).body[0];
    CHECK(lane.opcode == Opcode::V128Load8Lane);
    CHECK(lane.a == 0);
    CHECK(lane.b == 2);
    CHECK(lane.c == 8);
    CHECK(lane.lane == 15);
}

TEST_CASE("Decoder.memoryIndexImmediates")
{
    auto const body = std::vector<uint8_t> {
        0x3F, 0x03,             // memory.size 3
        0x40, 0x04,             // memory.grow 4
        0xFC, 0x0B, 0x05,       // memory.fill 5
        0xFC, 0x0A, 0x06, 0x07, // memory.copy to 6, from 7
        0xFC, 0x08, 0x00, 0x09, // memory.init data 0, memory 9
    };
    auto const bytes =
        ModuleBuilder {}.type(FuncType {}).function(0, {}, body).dataCount(1).data({ 0x01, 0x00 }).build();
    auto const function = functionOf(bytes, FeatureSet::wasm3());
    CHECK(function.body[0].opcode == Opcode::MemorySize);
    CHECK(function.body[0].a == 3);
    CHECK(function.body[1].opcode == Opcode::MemoryGrow);
    CHECK(function.body[1].a == 4);
    CHECK(function.body[2].opcode == Opcode::MemoryFill);
    CHECK(function.body[2].a == 5);
    CHECK(function.body[3].opcode == Opcode::MemoryCopy);
    CHECK(function.body[3].a == 6);
    CHECK(function.body[3].b == 7);
    CHECK(function.body[4].opcode == Opcode::MemoryInit);
    CHECK(function.body[4].a == 0);
    CHECK(function.body[4].b == 9);
    // A memory index is a u32: 0x80 0x00 is an overlong zero, which multi-memory accepts.
    CHECK(functionOf(functionWith({ 0x3F, 0x80, 0x00, 0x1A }), FeatureSet::wasm3()).body[0].a == 0);
}

TEST_CASE("Decoder.memopFlagsTooLarge")
{
    // i32.const 0, i32.load flags=0x80 (as the u32 LEB128 0x80 0x01) offset 0, drop.
    CHECK(errorOf(functionWith({ 0x41, 0x00, 0x28, 0x80, 0x01, 0x00, 0x1A }), FeatureSet::wasm3())
          == DecodeErrorCode::MalformedMemopFlags);
    // The same for a lane access's memarg.
    CHECK(errorOf(functionWith({ 0xFD, 0x54, 0x80, 0x01, 0x00, 0x00 }), FeatureSet::wasm3())
          == DecodeErrorCode::MalformedMemopFlags);

    // Each interpreter reports the flags at its own byte: 3.0 at the first, 2.0 at the last.
    auto const offsetOfFlags = [](std::span<std::byte const> bytes) {
        auto const load = std::to_array({ std::byte { 0x41 }, std::byte { 0x00 }, std::byte { 0x28 } });
        return static_cast<std::size_t>(std::ranges::search(bytes, load).end() - bytes.begin());
    };
    auto const wide = functionWith({ 0x41, 0x00, 0x28, 0x80, 0x01, 0x00, 0x1A });
    CHECK(decodeModule(wide, FeatureSet::wasm3()).error().offset == offsetOfFlags(wide));
    auto const twoBytes = functionWith({ 0x41, 0x00, 0x28, 0xA0, 0x00, 0x00, 0x1A }); // 32
    CHECK(decodeModule(twoBytes, FeatureSet::wasm2()).error().offset == offsetOfFlags(twoBytes) + 1);
}

TEST_CASE("Decoder.tooManyMemories")
{
    auto const importingMemories = [](uint32_t count) {
        auto builder = ModuleBuilder {};
        for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, count))
            builder.importMemory("m", "memory", Limits { .min = 0, .max = std::nullopt });
        return builder;
    };
    CHECK(DecodeLimits {}.maxMemories == 100);
    CHECK(errorOf(importingMemories(101).build(), FeatureSet::wasm3())
          == DecodeErrorCode::ImplementationLimitExceeded);
    CHECK(errorOf(importingMemories(100).build(), FeatureSet::wasm3()) == std::nullopt);
    // The limit counts imports and definitions together.
    CHECK(errorOf(importingMemories(100).memory(Limits { .min = 0, .max = std::nullopt }).build(),
                  FeatureSet::wasm3())
          == DecodeErrorCode::ImplementationLimitExceeded);
    CHECK(errorOf(importingMemories(2).memory(Limits { .min = 0, .max = std::nullopt }).build(),
                  FeatureSet::wasm3(),
                  DecodeLimits { .maxMemories = 3 })
          == std::nullopt);
}

TEST_CASE("Decoder.tableLimitCountsImports")
{
    auto const table = isoline::binary::TableType { .element = isoline::binary::FuncRef,
                                                    .limits = Limits { .min = 0, .max = std::nullopt } };
    auto builder = ModuleBuilder {};
    builder.importTable("m", "t", table).importTable("m", "u", table);
    CHECK(errorOf(builder.build(), FeatureSet::wasm2(), DecodeLimits { .maxTables = 1 })
          == DecodeErrorCode::ImplementationLimitExceeded);
    builder.table(table);
    CHECK(errorOf(builder.build(), FeatureSet::wasm2(), DecodeLimits { .maxTables = 2 })
          == DecodeErrorCode::ImplementationLimitExceeded);
    CHECK(errorOf(builder.build(), FeatureSet::wasm2(), DecodeLimits { .maxTables = 3 }) == std::nullopt);
}

TEST_CASE("Decoder.zeroByteExpected")
{
    CHECK(errorOf(functionWith({ 0x3F, 0x01, 0x1A })) == DecodeErrorCode::ZeroByteExpected);
    CHECK(errorOf(functionWith({ 0x41, 0x00, 0x40, 0x01, 0x1A })) == DecodeErrorCode::ZeroByteExpected);
    CHECK(errorOf(functionWith({ 0xFC, 0x0A, 0x00, 0x01 })) == DecodeErrorCode::ZeroByteExpected);
    CHECK(errorOf(functionWith({ 0xFC, 0x0B, 0x01 })) == DecodeErrorCode::ZeroByteExpected);
    CHECK(errorOf(functionWith({ 0xFC, 0x08, 0x00, 0x01 })) == DecodeErrorCode::ZeroByteExpected);
    // A zero byte, not a zero LEB128: 0x80 0x00 is rejected.
    CHECK(errorOf(functionWith({ 0x3F, 0x80, 0x00, 0x1A })) == DecodeErrorCode::ZeroByteExpected);
    // Before reference types, call_indirect's table index is the same reserved zero byte; with them
    // it is a u32 table index, which validation checks.
    auto const overlongTable = functionWith({ 0x41, 0x00, 0x11, 0x00, 0x80, 0x00 });
    CHECK(errorOf(overlongTable, FeatureSet::none()) == DecodeErrorCode::ZeroByteExpected);
    CHECK(errorOf(overlongTable, FeatureSet::wasm2().without(Feature::ReferenceTypes))
          == DecodeErrorCode::ZeroByteExpected);
    CHECK(errorOf(overlongTable) == std::nullopt);
}

TEST_CASE("Decoder.featureGated")
{
    struct Row
    {
        std::vector<uint8_t> body;
        Feature feature;
    };
    auto const rows = std::to_array<Row>({
        { .body = { 0x41, 0x00, 0xC0, 0x1A }, .feature = Feature::SignExtension },
        { .body = { 0x42, 0x00, 0xC4, 0x1A }, .feature = Feature::SignExtension },
        { .body = { 0x43, 0x00, 0x00, 0x00, 0x00, 0xFC, 0x00, 0x1A },
          .feature = Feature::NonTrappingFloatToInt },
        { .body = { 0xFC, 0x0B, 0x00 }, .feature = Feature::BulkMemory },
        { .body = { 0xFC, 0x0D, 0x00 }, .feature = Feature::BulkMemory },
        { .body = { 0x41, 0x00, 0x25, 0x00, 0x1A }, .feature = Feature::ReferenceTypes },
        { .body = { 0xFC, 0x10, 0x00, 0x1A }, .feature = Feature::ReferenceTypes },
        { .body = { 0xD0, 0x70, 0x1A }, .feature = Feature::ReferenceTypes },
        { .body = { 0x1C, 0x01, 0x7F }, .feature = Feature::ReferenceTypes },
    });
    for (auto const& row: rows)
    {
        auto const bytes = functionWith(row.body);
        INFO("first opcode byte " << int { row.body[0] });
        CHECK(errorOf(bytes, FeatureSet::none()) == DecodeErrorCode::IllegalOpcode);
        CHECK(disabledOf(bytes, FeatureSet::none()) == row.feature);
        CHECK_FALSE(
            errorOf(bytes, FeatureSet::none().with(row.feature).with(Feature::BulkMemory)).has_value());
    }
    // MVP opcodes need nothing.
    CHECK_FALSE(errorOf(functionWith({ 0x41, 0x00, 0x45, 0x1A }), FeatureSet::none()).has_value());
}

TEST_CASE("Decoder.deepNesting")
{
    constexpr auto Depth = std::size_t { 100'000 };
    auto body = std::vector<uint8_t> {};
    body.reserve(3 * Depth);
    for ([[maybe_unused]] auto const _: std::views::iota(std::size_t { 0 }, Depth))
    {
        body.push_back(0x02);
        body.push_back(0x40);
    }
    body.insert(body.end(), Depth, 0x0B);
    auto const function = functionOf(functionWith(std::move(body)));
    REQUIRE(function.body.size() == (2 * Depth) + 1);
    CHECK(function.body[0].endIndex == (2 * Depth) - 1);
    CHECK(function.body[Depth - 1].endIndex == Depth);
}

TEST_CASE("Decoder.dataCountRequired")
{
    // types, functions, [data count 1,] code with @p instructions, data with one passive segment.
    enum class DataCount : uint8_t
    {
        Absent,
        Present,
    };
    auto const withCode = [](DataCount dataCount, std::span<uint8_t const> instructions) {
        auto bytes = std::vector<uint8_t> { 0x01, 0x04, 0x01, 0x60, 0x00, 0x00, 0x03, 0x02, 0x01, 0x00 };
        if (dataCount == DataCount::Present)
            bytes.insert(bytes.end(), { 0x0C, 0x01, 0x01 });
        bytes.insert(bytes.end(),
                     { 0x0A,
                       static_cast<uint8_t>(instructions.size() + 4),
                       0x01,
                       static_cast<uint8_t>(instructions.size() + 2),
                       0x00 });
        bytes.insert(bytes.end(), instructions.begin(), instructions.end());
        bytes.insert(bytes.end(), { 0x0B, 0x0B, 0x03, 0x01, 0x01, 0x00 });
        auto module = rawBytes({ 0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00 });
        for (auto const byte: bytes)
            module.push_back(std::byte { byte });
        return module;
    };
    auto const memoryInit = std::vector<uint8_t> {
        0x41, 0x00, 0x41, 0x00, 0x41, 0x00, // three i32 operands
        0xFC, 0x08, 0x05, 0x00,             // memory.init 5
    };
    auto const dataDrop = std::vector<uint8_t> { 0xFC, 0x09, 0x00 };
    CHECK(errorOf(withCode(DataCount::Absent, memoryInit)) == DecodeErrorCode::DataCountSectionRequired);
    CHECK(errorOf(withCode(DataCount::Absent, dataDrop)) == DecodeErrorCode::DataCountSectionRequired);
    auto const withDataCount = decoded(withCode(DataCount::Present, memoryInit));
    REQUIRE(withDataCount.code.size() == 1);
    CHECK(withDataCount.code[0].body[3].opcode == Opcode::MemoryInit);
    CHECK(withDataCount.code[0].body[3].a == 5);
    CHECK_FALSE(errorOf(withCode(DataCount::Present, dataDrop)).has_value());
    CHECK_FALSE(errorOf(withCode(DataCount::Absent, std::to_array<uint8_t>({ 0x01 }))).has_value());
    // Only function bodies count, as in the spec interpreter: an initializer is not code.
    CHECK_FALSE(errorOf(moduleOf({ 0x06, 0x07, 0x01, 0x7F, 0x00, 0xFC, 0x09, 0x00, 0x0B })).has_value());
}

TEST_CASE("Decoder.constExprsDecodeAnyInstruction")
{
    // Which instructions are constant is the validator's question; the decoder reads them all.
    auto const module = decoded(
        moduleOf({ 0x06, 0x0C, 0x01, 0x7F, 0x00, 0x02, 0x40, 0x0B, 0x41, 0x01, 0x41, 0x02, 0x6A, 0x0B }));
    REQUIRE(module.globals.size() == 1);
    auto const& init = module.globals[0].init;
    CHECK(opcodesOf(init)
          == std::vector {
              Opcode::Block, Opcode::End, Opcode::I32Const, Opcode::I32Const, Opcode::I32Add, Opcode::End });
    CHECK(init[0].endIndex == 1);
    // A br_table's labels are read and dropped.
    CHECK_FALSE(
        errorOf(moduleOf({ 0x06, 0x08, 0x01, 0x7F, 0x00, 0x0E, 0x01, 0x00, 0x00, 0x0B })).has_value());
    // spec binary.wast "Init expression with missing end marker" (WebAssembly 2.0 suite).
    CHECK(errorOf(moduleOf({ 0x06, 0x05, 0x01, 0x7F, 0x00, 0x41, 0x00 })) == DecodeErrorCode::UnexpectedEnd);
}

TEST_CASE("Decoder.sideTablesGrowGeometrically")
{
    // Each typed select and br_table appends to its function's side table. Reserving the table's
    // exact new size on every append copied it whole each time: quadratic, 13 s for this body.
    // Geometric growth leaves spare capacity, which exact reservation never does, so the capacity
    // check catches the regression deterministically (N is no growth step of 2x or 1.5x vectors).
    constexpr auto Count = std::size_t { 300'000 };
    auto selects = std::vector<uint8_t> {};
    auto brTables = std::vector<uint8_t> {};
    for ([[maybe_unused]] auto const _: std::views::iota(std::size_t { 0 }, Count))
    {
        selects.insert(selects.end(), { 0x1C, 0x01, 0x7F });         // select (result i32)
        brTables.insert(brTables.end(), { 0x0E, 0x01, 0x00, 0x00 }); // br_table 0 0
    }

    auto const selectFunction = functionOf(functionWith(std::move(selects)));
    REQUIRE(selectFunction.selectTypes.size() == Count);
    CHECK(selectFunction.selectTypes.capacity() > selectFunction.selectTypes.size());
    CHECK(selectFunction.body[Count - 1].a == Count - 1);
    CHECK(selectFunction.body[Count - 1].b == 1);

    auto const brTableFunction = functionOf(functionWith(std::move(brTables)));
    REQUIRE(brTableFunction.brTableLabels.size() == 2 * Count);
    CHECK(brTableFunction.brTableLabels.capacity() > brTableFunction.brTableLabels.size());
    CHECK(brTableFunction.body[Count - 1].a == (2 * Count) - 2);
    CHECK(brTableFunction.body[Count - 1].b == 2);
}

namespace
{

/// An instruction of @p row with an immediate of its shape, enclosing nothing.
std::vector<uint8_t> instructionOf(isoline::binary::OpcodeInfo const& row)
{
    using isoline::binary::ImmediateKind;
    auto bytes = std::vector<uint8_t> {};
    // A single-byte opcode is that byte; a prefixed one's sub-opcode is a u32 LEB128.
    if (row.prefix == isoline::binary::NoPrefix)
        bytes.push_back(static_cast<uint8_t>(row.code));
    else
    {
        bytes.push_back(row.prefix);
        auto const code = encodeU32(row.code);
        bytes.insert(bytes.end(), code.begin(), code.end());
    }
    switch (row.immediate)
    {
        case ImmediateKind::None: break;
        case ImmediateKind::BlockType: bytes.insert(bytes.end(), { 0x40, 0x0B }); break;      // (empty) end
        case ImmediateKind::TryTable: bytes.insert(bytes.end(), { 0x40, 0x00, 0x0B }); break; // no catches
        case ImmediateKind::LabelIndex:
        case ImmediateKind::FunctionIndex:
        case ImmediateKind::LocalIndex:
        case ImmediateKind::GlobalIndex:
        case ImmediateKind::TableIndex:
        case ImmediateKind::ElemIndex:
        case ImmediateKind::DataIndex:
        case ImmediateKind::TypeIndex:
        case ImmediateKind::TagIndex:
        case ImmediateKind::MemoryIndex:
        case ImmediateKind::I32Const:
        case ImmediateKind::I64Const: bytes.push_back(0x00); break;
        case ImmediateKind::LabelTable:
        case ImmediateKind::CallIndirect:
        case ImmediateKind::TableCopy:
        case ImmediateKind::TableInit:
        case ImmediateKind::MemArg:
        case ImmediateKind::MemoryCopy:
        case ImmediateKind::MemoryInit: bytes.insert(bytes.end(), { 0x00, 0x00 }); break;
        case ImmediateKind::F32Const: bytes.insert(bytes.end(), 4, 0x00); break;
        case ImmediateKind::F64Const: bytes.insert(bytes.end(), 8, 0x00); break;
        case ImmediateKind::HeapType: bytes.push_back(0x70); break;
        case ImmediateKind::SelectTypes: bytes.insert(bytes.end(), { 0x01, 0x7F }); break;
        case ImmediateKind::V128Const:
        case ImmediateKind::Shuffle: bytes.insert(bytes.end(), 16, 0x00); break;
        case ImmediateKind::LaneIndex: bytes.push_back(0x00); break;
        case ImmediateKind::MemArgLane: bytes.insert(bytes.end(), { 0x00, 0x00, 0x00 }); break;
    }
    return bytes;
}

/// A module of one [] -> [] function whose body is @p instruction and an end.
std::vector<std::byte> moduleWithInstruction(std::span<uint8_t const> instruction)
{
    auto body = std::vector<uint8_t> { 0x00 }; // no locals
    body.insert(body.end(), instruction.begin(), instruction.end());
    body.push_back(0x0B);
    auto entries = encodeU32(1);
    auto const bodySize = encodeU32(static_cast<uint32_t>(body.size()));
    entries.insert(entries.end(), bodySize.begin(), bodySize.end());
    entries.insert(entries.end(), body.begin(), body.end());
    auto const sectionSize = encodeU32(static_cast<uint32_t>(entries.size()));

    auto bytes = moduleOf({ 0x01, 0x04, 0x01, 0x60, 0x00, 0x00, 0x03, 0x02, 0x01, 0x00, 0x0A });
    for (auto const byte: sectionSize)
        bytes.push_back(std::byte { byte });
    for (auto const byte: entries)
        bytes.push_back(std::byte { byte });
    return bytes;
}

} // namespace

TEST_CASE("Decoder.refusesAFeatureSetMissingAPrerequisite")
{
    // (func unreachable i8x16.relaxed_swizzle drop): in dead code the operand stack is polymorphic,
    // so without the check this would decode and validate with no v128 ever encoded.
    auto const bytes = functionWith({ 0x00, 0xFD, 0x80, 0x02, 0x1A });
    auto const unclosed = FeatureSet::wasm2().with(Feature::RelaxedSimd).without(Feature::Simd);
    auto const refused = decodeModule(bytes, unclosed);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == DecodeErrorCode::MissingFeaturePrerequisite);
    CHECK(refused.error().offset == 0);
    CHECK(isoline::binary::specMessage(refused.error())
          == "feature enabled without its prerequisite: relaxed-simd requires simd");
    CHECK_FALSE(errorOf(bytes, FeatureSet::wasm2().with(Feature::RelaxedSimd)).has_value());
}

TEST_CASE("Decoder.everyOpcodeGatedByItsProposal")
{
    // Driven by the opcode table, so a row added with the wrong proposal, or none, fails here, and so
    // does one whose OpcodeCrossRequirements the decoder ignores.
    using isoline::binary::ImmediateKind;
    for (auto const& row: isoline::binary::OpcodeTable)
    {
        // else and end only stand inside the structures the reader resolves.
        if (row.opcode == Opcode::Else || row.opcode == Opcode::End)
            continue;
        INFO(row.text);
        auto const bytes = moduleWithInstruction(instructionOf(row));
        if (!row.proposal)
        {
            CHECK_FALSE(errorOf(bytes, FeatureSet::none()).has_value());
            continue;
        }
        CHECK(errorOf(bytes, FeatureSet::none()) == DecodeErrorCode::IllegalOpcode);
        auto const named = disabledOf(bytes, FeatureSet::none());
        REQUIRE(named.has_value());
        CHECK(row.required.has(*named));
        // Accepted with its proposal: code naming a data segment then gets as far as wanting the
        // data count section, which this module lacks.
        auto const namesDataSegment =
            row.immediate == ImmediateKind::DataIndex || row.immediate == ImmediateKind::MemoryInit;
        auto const accepted = namesDataSegment ? std::optional { DecodeErrorCode::DataCountSectionRequired }
                                               : std::optional<DecodeErrorCode> {};
        auto const enabled = row.required.withPrerequisites();
        CHECK(errorOf(bytes, enabled) == accepted);
        // Every feature it requires, its proposal's and any other's, is needed: without any one of
        // them it is unknown.
        for (auto const& feature: isoline::FeatureTable)
        {
            if (!row.required.has(feature.feature))
                continue;
            INFO("without " << feature.name);
            CHECK(errorOf(bytes, enabled.without(feature.feature)) == DecodeErrorCode::IllegalOpcode);
            CHECK(disabledOf(bytes, enabled.without(feature.feature)) == feature.feature);
        }
    }
}

namespace
{

/// Checks that @p module, which must decode, fails to decode once any one of its sections is cut
/// short: each section in turn ends the module, holding only its first `kept` bytes (its size is
/// that count), for every `kept` below its size. Every section before it is intact, so the cut is
/// the module's only defect the decoder meets first: it must report running out, at an offset
/// inside the cut section — not a later consistency check, which would hide a cut it accepted.
void checkEveryTruncationFails(std::span<std::byte const> module)
{
    using isoline::binary::DecodeError;
    REQUIRE(errorOf(module) == std::nullopt);
    auto sectionStart = Preamble.size();
    while (sectionStart < module.size())
    {
        // Every size the builders write here fits one LEB128 byte.
        auto const id = std::to_integer<uint8_t>(module[sectionStart]);
        auto const size = std::to_integer<uint8_t>(module[sectionStart + 1]);
        REQUIRE(size < 0x80);
        auto const contents = module.subspan(sectionStart + 2, size);
        auto const sectionEnd = sectionStart + 2 + size;
        CAPTURE(id);
        // Uncut, the prefix decodes; only a function section still lacks its code section.
        auto const prefixError = errorOf(module.first(sectionEnd));
        CHECK((prefixError == std::nullopt
               || prefixError == DecodeErrorCode::FunctionAndCodeSectionInconsistent));
        for (auto const kept: std::views::iota(uint8_t { 0 }, size))
        {
            auto truncated = std::vector<std::byte>(
                module.begin(), module.begin() + static_cast<std::ptrdiff_t>(sectionStart));
            truncated.push_back(std::byte { id });
            truncated.push_back(std::byte { kept });
            // Copied, not inserted: GCC 14's release build reports the insert of an empty range as an
            // overlapping memcpy.
            std::ranges::copy(contents.first(kept), std::back_inserter(truncated));
            CAPTURE(kept);
            auto const result = decodeModule(truncated, FeatureSet::wasm2());
            REQUIRE_FALSE(result.has_value());
            auto const& error = result.error();
            CHECK((error.code == DecodeErrorCode::UnexpectedEnd
                   || error.code == DecodeErrorCode::LengthOutOfBounds
                   || error.code == DecodeErrorCode::SectionSizeMismatch));
            CHECK(error.offset >= sectionStart);
            CHECK(error.offset <= truncated.size());
        }
        sectionStart = sectionEnd;
    }
}

} // namespace

TEST_CASE("Decoder.everyTruncatedSectionFails")
{
    using isoline::binary::AddressType;
    auto const one = Limits { .min = 1, .max = std::nullopt, .addressType = AddressType::I32 };

    SECTION("module sections")
    {
        // Every field a section can end inside of: a global import's mutability, a table index and
        // element kind (element flags 2), a memory index and offset (data flags 2). No function is
        // defined, so no section's absence makes an earlier prefix inconsistent.
        auto builder = ModuleBuilder {};
        builder.type(FuncType {})
            .importFunction("m", "f", 0)
            .importGlobal("m", "g", { .type = isoline::binary::I32, .mutability = Mutability::Var })
            .table({ .element = isoline::binary::FuncRef, .limits = one })
            .memory(one)
            .global({ .type = isoline::binary::I32, .mutability = Mutability::Const }, { 0x41, 0x00 })
            .exportEntity("g", ExternKind::Global, 1)
            .element({ 0x02, 0x00, 0x41, 0x00, 0x0B, 0x00, 0x01, 0x00 })
            .data({ 0x02, 0x00, 0x41, 0x00, 0x0B, 0x01, 0x2A });
        checkEveryTruncationFails(builder.build());
    }

    SECTION("code section")
    {
        // The code section last, so a cut in it leaves the function section's count matched.
        auto builder = ModuleBuilder {};
        builder.type(FuncType {}).function(0, { isoline::binary::I32 }, { 0x41, 0x05, 0x21, 0x00, 0x01 });
        checkEveryTruncationFails(builder.build());
    }
}

TEST_CASE("Decoder.typedReferenceTypes")
{
    using isoline::binary::AbstractHeapType;
    using isoline::binary::concreteRef;
    using isoline::binary::Nullable;
    auto const wasm3 = FeatureSet::wasm3();
    // (type (func (param (ref 0) (ref null 1) (ref 128)) (result (ref func) (ref null func))))
    auto const typeSection = moduleOf({ 0x01,
                                        0x0F,
                                        0x01,
                                        0x60,
                                        0x03,
                                        0x64,
                                        0x00,
                                        0x63,
                                        0x01,
                                        0x64,
                                        0x80,
                                        0x01,
                                        0x02,
                                        0x64,
                                        0x70,
                                        0x63,
                                        0x70 });
    auto const module = decoded(typeSection, wasm3);
    REQUIRE(module.types.size() == 1);
    auto const& type = module.types[0];
    CHECK(type.params
          == std::vector {
              concreteRef(0, Nullable::No), concreteRef(1, Nullable::Yes), concreteRef(128, Nullable::No) });
    auto const nonNullFunc = isoline::binary::ValueType {
        .kind = isoline::binary::ValueKind::Ref,
        .nullable = Nullable::No,
        .heap = { .kind = AbstractHeapType::Func },
    };
    CHECK(type.results == std::vector { nonNullFunc, isoline::binary::FuncRef });

    // Without typed function references, 0x63 and 0x64 are no type, at the type's byte.
    auto const refused = decodeModule(typeSection, FeatureSet::wasm2());
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == DecodeErrorCode::MalformedReferenceType);
    CHECK(refused.error().offset == Preamble.size() + 5);
    // A heap type cut short is no s33, and so read as an abstract one: a byte with a continuation
    // bit is too long a representation of the one byte that encodes those.
    CHECK(errorOf(moduleOf({ 0x01, 0x05, 0x01, 0x60, 0x01, 0x64, 0x80 }), wasm3)
          == DecodeErrorCode::IntegerRepresentationTooLong);

    // Locals, globals and block types take them too.
    auto builder = ModuleBuilder {};
    builder.type(FuncType {})
        .global(isoline::binary::GlobalType { .type = concreteRef(0, Nullable::Yes),
                                              .mutability = Mutability::Var },
                { 0xD0, 0x00 })
        .function(0, { concreteRef(0, Nullable::Yes) }, { 0x02, 0x64, 0x00, 0x00, 0x0B });
    auto const typed = decoded(builder.build(), wasm3);
    CHECK(typed.globals.at(0).type.type == concreteRef(0, Nullable::Yes));
    CHECK(localType(typed.code.at(0), 0) == concreteRef(0, Nullable::Yes));
    CHECK(typed.code.at(0).body.at(0).blockType().value == concreteRef(0, Nullable::No));
}

TEST_CASE("Decoder.rejectsGcHeapTypes")
{
    // The GC heaps are no heap type without GC, after 0x63 or 0x64 or as a shorthand: any, eq, i31,
    // struct, array, none, nofunc, noextern.
    for (auto const code: { 0x6E, 0x6D, 0x6C, 0x6B, 0x6A, 0x71, 0x73, 0x72 })
    {
        auto const heap = static_cast<uint8_t>(code);
        auto const nullable =
            decodeModule(moduleOf({ 0x01, 0x06, 0x01, 0x60, 0x01, 0x63, heap, 0x00 }), FeatureSet::wasm3());
        REQUIRE_FALSE(nullable.has_value());
        CHECK(nullable.error().code == DecodeErrorCode::MalformedReferenceType);
        CHECK(nullable.error().offset == Preamble.size() + 6); // the heap type
        CHECK(errorOf(moduleOf({ 0x01, 0x06, 0x01, 0x60, 0x01, 0x64, heap, 0x00 }), FeatureSet::wasm3())
              == DecodeErrorCode::MalformedReferenceType);
        CHECK(errorOf(moduleOf({ 0x01, 0x05, 0x01, 0x60, 0x01, heap, 0x00 }), FeatureSet::all())
              == DecodeErrorCode::MalformedReferenceType);
    }
    // rec, sub and the struct and array forms are GC's definitions: no function type.
    for (auto const form: { 0x4E, 0x50, 0x4F, 0x5F, 0x5E })
        CHECK(errorOf(moduleOf({ 0x01, 0x04, 0x01, static_cast<uint8_t>(form), 0x00, 0x00 }),
                      FeatureSet::wasm3())
              == DecodeErrorCode::MalformedFunctionType);
}

TEST_CASE("Decoder.exceptionReferenceShorthands")
{
    using isoline::binary::ExnRef;
    using isoline::binary::NullExnRef;
    // (func (param exnref nullexnref (ref null exn))), under exception handling only.
    auto const bytes = moduleOf({ 0x01, 0x08, 0x01, 0x60, 0x03, 0x69, 0x74, 0x63, 0x69, 0x00 });
    auto const module = decoded(bytes, FeatureSet::wasm3());
    CHECK(module.types.at(0).params == std::vector { ExnRef, NullExnRef, ExnRef });
    CHECK(errorOf(bytes, FeatureSet::wasm3().without(Feature::ExceptionHandling))
          == DecodeErrorCode::MalformedReferenceType);
}

TEST_CASE("Decoder.refNullTakesAHeapType")
{
    using isoline::binary::concreteRef;
    using isoline::binary::Nullable;
    auto const refNull = [](uint8_t heap) {
        auto builder = ModuleBuilder {};
        builder.type(FuncType {}).function(0, {}, { 0xD0, heap, 0x1A });
        return builder.build();
    };
    CHECK(decoded(refNull(0x00), FeatureSet::wasm3()).code.at(0).body.at(0).type
          == concreteRef(0, Nullable::Yes));
    CHECK(decoded(refNull(0x70), FeatureSet::wasm3()).code.at(0).body.at(0).type == isoline::binary::FuncRef);
    CHECK(decoded(refNull(0x69), FeatureSet::wasm3()).code.at(0).body.at(0).type == isoline::binary::ExnRef);
    CHECK(decoded(refNull(0x6F), FeatureSet::wasm2()).code.at(0).body.at(0).type
          == isoline::binary::ExternRef);
    // A type index needs typed function references; exn exception handling; nofunc is GC's.
    CHECK(errorOf(refNull(0x00), FeatureSet::wasm2()) == DecodeErrorCode::MalformedReferenceType);
    CHECK(errorOf(refNull(0x69), FeatureSet::wasm3().without(Feature::ExceptionHandling))
          == DecodeErrorCode::MalformedReferenceType);
    CHECK(errorOf(refNull(0x73), FeatureSet::wasm3()) == DecodeErrorCode::MalformedReferenceType);
}

TEST_CASE("Decoder.tableInitializer")
{
    using isoline::binary::AbstractHeapType;
    using isoline::binary::Nullable;
    auto const nonNullFunc = isoline::binary::ValueType {
        .kind = isoline::binary::ValueKind::Ref,
        .nullable = Nullable::No,
        .heap = { .kind = AbstractHeapType::Func },
    };
    auto const one = Limits { .min = 1, .max = std::nullopt };
    auto builder = ModuleBuilder {};
    builder.type(FuncType {})
        .function(0, {}, {})
        .table(isoline::binary::TableType { .element = nonNullFunc, .limits = one }, { 0xD2, 0x00 })
        .table(isoline::binary::TableType { .element = isoline::binary::FuncRef, .limits = one });
    auto const module = decoded(builder.build(), FeatureSet::wasm3());
    REQUIRE(module.tables.size() == 2);
    CHECK(module.tables[0].type.element == nonNullFunc);
    REQUIRE(module.tables[0].init.size() == 2);
    CHECK(module.tables[0].init[0].opcode == Opcode::RefFunc);
    CHECK(module.tables[1].init.empty()); // its slots start as null

    // 0x40 0x00 introduces the initializer form under typed function references only.
    auto const withInit = moduleOf({ 0x04, 0x09, 0x01, 0x40, 0x00, 0x70, 0x00, 0x01, 0xD0, 0x70, 0x0B });
    CHECK(errorOf(withInit, FeatureSet::wasm3()) == std::nullopt);
    auto const refused = decodeModule(withInit, FeatureSet::wasm2());
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == DecodeErrorCode::MalformedReferenceType);
    CHECK(refused.error().offset == Preamble.size() + 3);
    // As in the interpreter, which tries the plain form when the initializer form fails, any error
    // after 0x40 is a malformed reference type at 0x40: here the zero byte is missing.
    auto const noZero = decodeModule(moduleOf({ 0x04, 0x08, 0x01, 0x40, 0x70, 0x00, 0x01, 0xD0, 0x70, 0x0B }),
                                     FeatureSet::wasm3());
    REQUIRE_FALSE(noZero.has_value());
    CHECK(noZero.error().code == DecodeErrorCode::MalformedReferenceType);
    CHECK(noZero.error().offset == Preamble.size() + 3);
}

TEST_CASE("Decoder.functionReferencesNeedReferenceTypes")
{
    // FeatureRow::prerequisites: typed function references build on reference types (3.0's
    // function-references Overview), which build on bulk memory.
    auto const unclosed =
        FeatureSet::wasm2().with(Feature::TypedFunctionReferences).without(Feature::ReferenceTypes);
    auto const refused = decodeModule(moduleOf({}), unclosed);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == DecodeErrorCode::MissingFeaturePrerequisite);
    CHECK(isoline::binary::specMessage(refused.error())
          == "feature enabled without its prerequisite: function-references requires reference-types");
    CHECK(errorOf(moduleOf({}), FeatureSet::wasm2().with(Feature::TypedFunctionReferences)) == std::nullopt);
}

TEST_CASE("Decoder.tagSectionOrder")
{
    // Type [] -> [], a memory, a tag of type 0, an i32 global: the tag section (13) sits between
    // the memory (5) and the global (6) sections.
    auto const features = FeatureSet::wasm3();
    auto const ordered = moduleOf({ 0x01, 0x04, 0x01, 0x60, 0x00, 0x00, // type
                                    0x05, 0x03, 0x01, 0x00, 0x01,       // memory
                                    0x0D, 0x03, 0x01, 0x00, 0x00,       // tag
                                    0x06, 0x06, 0x01, 0x7F, 0x00, 0x41, 0x00, 0x0B });
    auto const module = decoded(ordered, features);
    REQUIRE(module.tags.size() == 1);
    CHECK(module.tags[0].typeIndex == 0);

    auto const afterGlobal = moduleOf({ 0x01,
                                        0x04,
                                        0x01,
                                        0x60,
                                        0x00,
                                        0x00,
                                        0x06,
                                        0x06,
                                        0x01,
                                        0x7F,
                                        0x00,
                                        0x41,
                                        0x00,
                                        0x0B,
                                        0x0D,
                                        0x03,
                                        0x01,
                                        0x00,
                                        0x00 });
    CHECK(errorOf(afterGlobal, features) == DecodeErrorCode::SectionOutOfOrder);
    auto const beforeMemory = moduleOf(
        { 0x01, 0x04, 0x01, 0x60, 0x00, 0x00, 0x0D, 0x03, 0x01, 0x00, 0x00, 0x05, 0x03, 0x01, 0x00, 0x01 });
    CHECK(errorOf(beforeMemory, features) == DecodeErrorCode::SectionOutOfOrder);

    // Without exception handling the section id is unknown.
    auto const tagOnly = moduleOf({ 0x01, 0x04, 0x01, 0x60, 0x00, 0x00, 0x0D, 0x03, 0x01, 0x00, 0x00 });
    CHECK(errorOf(tagOnly, features.without(Feature::ExceptionHandling))
          == DecodeErrorCode::MalformedSectionId);
    // A tag's attribute byte must be 0, an exception.
    auto const attribute = moduleOf({ 0x01, 0x04, 0x01, 0x60, 0x00, 0x00, 0x0D, 0x03, 0x01, 0x01, 0x00 });
    CHECK(errorOf(attribute, features) == DecodeErrorCode::ZeroByteExpected);
    // The tag section's entries are bounded.
    CHECK(errorOf(tagOnly, features, DecodeLimits { .maxTags = 0 })
          == DecodeErrorCode::ImplementationLimitExceeded);
}

TEST_CASE("Decoder.tagImportsAndExports")
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = { isoline::binary::I32 }, .results = {} })
        .importTag("m", "t", 0)
        .tag(0)
        .exportEntity("t", ExternKind::Tag, 1);
    auto const bytes = builder.build();
    auto const module = decoded(bytes, FeatureSet::wasm3());
    REQUIRE(module.imports.size() == 1);
    CHECK(module.imports[0].kind == ExternKind::Tag);
    CHECK(module.imports[0].typeIndex == 0);
    CHECK(module.importedCount(ExternKind::Tag) == 1);
    REQUIRE(module.exports.size() == 1);
    CHECK(module.exports[0].kind == ExternKind::Tag);
    CHECK(module.exports[0].index == 1);
    // Kind byte 4 is a tag only with exception handling.
    auto const without = FeatureSet::wasm3().without(Feature::ExceptionHandling);
    auto imported = ModuleBuilder {};
    imported.type(FuncType {}).importTag("m", "t", 0);
    CHECK(errorOf(imported.build(), without) == DecodeErrorCode::MalformedImportKind);
    auto exported = ModuleBuilder {};
    exported.exportEntity("t", ExternKind::Tag, 0);
    CHECK(errorOf(exported.build(), without) == DecodeErrorCode::MalformedExportKind);
}

TEST_CASE("Decoder.tryTableClauses")
{
    using isoline::binary::CatchClause;
    using isoline::binary::catchClausesOf;
    using isoline::binary::CatchKind;
    auto const features = FeatureSet::wasm3();
    // try_table (result i32) (catch 0 0) (catch_ref 1 1) (catch_all 2) (catch_all_ref 3) throw 0 end,
    // then a second try_table without clauses, whose run is empty.
    auto builder = ModuleBuilder {};
    builder.type(FuncType {}).function(0, {}, { 0x1F, 0x7F, 0x04, 0x00, 0x00, 0x00, 0x01,
                                                0x01, 0x01, 0x02, 0x02, 0x03, 0x03, 0x08,
                                                0x00, 0x0B, 0x1A, 0x1F, 0x40, 0x00, 0x0B });
    auto const bytes = builder.build();
    auto const module = decoded(bytes, features);
    auto const& function = module.code.at(0);
    REQUIRE(function.body.size() >= 6);
    auto const& tryTable = function.body[0];
    CHECK(tryTable.opcode == Opcode::TryTable);
    CHECK(tryTable.blockType().kind == isoline::binary::BlockType::Kind::Value);
    CHECK(tryTable.endIndex == 2);
    CHECK(function.body[1].opcode == Opcode::Throw);
    CHECK(function.body[1].a == 0);
    CHECK(std::ranges::equal(catchClausesOf(function, tryTable),
                             std::to_array<CatchClause>({
                                 { .kind = CatchKind::Catch, .tag = 0, .label = 0 },
                                 { .kind = CatchKind::CatchRef, .tag = 1, .label = 1 },
                                 { .kind = CatchKind::CatchAll, .tag = 0, .label = 2 },
                                 { .kind = CatchKind::CatchAllRef, .tag = 0, .label = 3 },
                             })));
    CHECK(function.body[4].opcode == Opcode::TryTable);
    CHECK(function.body[4].endIndex == 5);
    CHECK(catchClausesOf(function, function.body[4]).empty());

    // The clauses of one try_table are bounded.
    auto twoClauses = ModuleBuilder {};
    twoClauses.type(FuncType {}).function(0, {}, { 0x1F, 0x40, 0x02, 0x02, 0x00, 0x02, 0x00, 0x0B });
    auto const twoClausesBytes = twoClauses.build();
    CHECK(errorOf(twoClausesBytes, features, DecodeLimits { .maxCatchClauses = 2 }) == std::nullopt);
    CHECK(errorOf(twoClausesBytes, features, DecodeLimits { .maxCatchClauses = 1 })
          == DecodeErrorCode::ImplementationLimitExceeded);
    CHECK(DecodeLimits {}.maxCatchClauses == 65'520);

    // A body that ends inside a clause runs out.
    auto const cut = moduleOf({ 0x01,
                                0x04,
                                0x01,
                                0x60,
                                0x00,
                                0x00, // type
                                0x03,
                                0x02,
                                0x01,
                                0x00, // function
                                0x0A,
                                0x06,
                                0x01,
                                0x04,
                                0x00,
                                0x1F,
                                0x40,
                                0x01 });
    CHECK(errorOf(cut, features) == DecodeErrorCode::UnexpectedEnd);

    // A clause kind above 3 is malformed, at its byte.
    auto malformed = ModuleBuilder {};
    malformed.type(FuncType {}).function(0, {}, { 0x1F, 0x40, 0x01, 0x04, 0x00, 0x0B });
    auto const malformedBytes = malformed.build();
    auto const result = decodeModule(malformedBytes, features);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == DecodeErrorCode::MalformedCatchClause);
    CHECK(isoline::binary::specMessage(result.error()) == "malformed catch clause");
}

namespace
{

/// The features the compact import tests decode with: 3.0 and the compact import section.
FeatureSet compactImportFeatures()
{
    return FeatureSet::wasm3().with(Feature::CompactImports);
}

/// A module whose only section is an import section of @p payload, after one type `[] -> []`.
std::vector<std::byte> importingModule(std::vector<uint8_t> payload)
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType {}).rawSection(2, std::move(payload));
    return builder.build();
}

} // namespace

TEST_CASE("Decoder.compactImportsByItem")
{
    // "env" "" 0x7F [("a", func 0), ("b", global i32)]
    auto const bytes = importingModule(
        { 0x01, 0x03, 'e', 'n', 'v', 0x00, 0x7F, 0x02, 0x01, 'a', 0x00, 0x00, 0x01, 'b', 0x03, 0x7F, 0x00 });
    auto const module = decoded(bytes, compactImportFeatures());
    REQUIRE(module.imports.size() == 2);
    CHECK(importModuleOf(module, module.imports[0]) == "env");
    CHECK(module.imports[0].name == "a");
    CHECK(module.imports[0].kind == ExternKind::Func);
    CHECK(module.imports[0].typeIndex == 0);
    CHECK(importModuleOf(module, module.imports[1]) == "env");
    CHECK(module.imports[1].name == "b");
    CHECK(module.imports[1].kind == ExternKind::Global);
    CHECK(module.imports[1].global.type == isoline::binary::I32);
    CHECK(module.imports[1].global.mutability == Mutability::Const);

    // Without the proposal the discriminator is no import kind.
    CHECK(errorOf(bytes, FeatureSet::wasm3()) == DecodeErrorCode::MalformedImportKind);
}

TEST_CASE("Decoder.compactImportsByType")
{
    // "env" "" 0x7E (global i32) ["x", "y", "z"]
    auto const bytes = importingModule(
        { 0x01, 0x03, 'e', 'n', 'v', 0x00, 0x7E, 0x03, 0x7F, 0x00, 0x03, 0x01, 'x', 0x01, 'y', 0x01, 'z' });
    auto const module = decoded(bytes, compactImportFeatures());
    REQUIRE(module.imports.size() == 3);
    CHECK(module.importModules.size() == 1);
    auto const names = std::to_array<std::string_view>({ "x", "y", "z" });
    for (auto const index: std::views::iota(std::size_t { 0 }, names.size()))
    {
        auto const& import = module.imports[index];
        // Every item names the entry's one module name; none holds a copy.
        CHECK(import.moduleIndex == 0);
        CHECK(importModuleOf(module, import) == "env");
        CHECK(import.name == names[index]);
        CHECK(import.kind == ExternKind::Global);
        CHECK(import.global.type == isoline::binary::I32);
    }
    CHECK(errorOf(bytes, FeatureSet::wasm3()) == DecodeErrorCode::MalformedImportKind);

    // The discriminator only follows an empty item name; after another it is malformed.
    CHECK(errorOf(importingModule({ 0x01, 0x01, 'a', 0x01, 'b', 0x7E, 0x03, 0x7F, 0x00, 0x01, 0x01, 'x' }),
                  compactImportFeatures())
          == DecodeErrorCode::MalformedImportKind);
    // A group's extern types are read as a single import's are.
    CHECK(errorOf(importingModule({ 0x01, 0x01, 'a', 0x00, 0x7E, 0x05, 0x01, 0x01, 'x' }),
                  compactImportFeatures())
          == DecodeErrorCode::MalformedImportKind);
    CHECK(errorOf(importingModule({ 0x01, 0x01, 'a', 0x00, 0x7F, 0x01, 0x01, 'x', 0x05 }),
                  compactImportFeatures())
          == DecodeErrorCode::MalformedImportKind);
    // A section that ends after the empty item name ends unexpectedly, as a single import does.
    CHECK(errorOf(importingModule({ 0x01, 0x01, 'a', 0x00 }), compactImportFeatures())
          == DecodeErrorCode::UnexpectedEnd);
    CHECK(errorOf(importingModule({ 0x01, 0x01, 'a', 0x00 }), FeatureSet::wasm3())
          == DecodeErrorCode::UnexpectedEnd);
    // An empty item name without a discriminator is a single import.
    auto const single = decoded(importingModule({ 0x01, 0x00, 0x00, 0x00, 0x00 }), compactImportFeatures());
    REQUIRE(single.imports.size() == 1);
    CHECK(single.imports[0].kind == ExternKind::Func);
}

TEST_CASE("Decoder.compactImportsCounted")
{
    // 0x7E with maxImports + 1 empty names: refused at the item count, before any is read.
    auto const limit = DecodeLimits {}.maxImports;
    CHECK(limit == 100'000);
    auto payload = std::vector<uint8_t> { 0x01, 0x03, 'e', 'n', 'v', 0x00, 0x7E, 0x03, 0x7F, 0x00 };
    auto const countOffset = payload.size();
    append(payload, encodeU32(limit + 1));
    payload.insert(payload.end(), limit + 1, 0x00);
    auto const bytes = importingModule(payload);
    auto const result = decodeModule(bytes, compactImportFeatures());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == DecodeErrorCode::ImplementationLimitExceeded);
    // The module's preamble, type section (2 + 4 bytes) and import section header (1 + 3 bytes).
    CHECK(result.error().offset == 8 + 6 + 4 + countOffset);

    // The limit counts the imports of every entry: two groups of two exceed three.
    auto const twoGroups =
        importingModule({ 0x02, 0x01, 'a',  0x00, 0x7E, 0x00, 0x00, 0x02, 0x01, 'b', 0x01, 'c', 0x01,
                          'a',  0x00, 0x7F, 0x02, 0x01, 'd',  0x00, 0x00, 0x01, 'e', 0x00, 0x00 });
    CHECK(errorOf(twoGroups, compactImportFeatures(), DecodeLimits { .maxImports = 3 })
          == DecodeErrorCode::ImplementationLimitExceeded);
    CHECK(decoded(twoGroups, compactImportFeatures()).imports.size() == 4);

    // A single import after a group counts towards the same limit, at its entry.
    auto const groupThenSingle = importingModule({ 0x02,
                                                   0x01,
                                                   'a',
                                                   0x00,
                                                   0x7E,
                                                   0x00,
                                                   0x00,
                                                   0x02,
                                                   0x01,
                                                   'b',
                                                   0x01,
                                                   'c',
                                                   0x01,
                                                   'a',
                                                   0x01,
                                                   'd',
                                                   0x00,
                                                   0x00 });
    CHECK(errorOf(groupThenSingle, compactImportFeatures(), DecodeLimits { .maxImports = 2 })
          == DecodeErrorCode::ImplementationLimitExceeded);
    CHECK(decoded(groupThenSingle, compactImportFeatures()).imports.size() == 3);

    // Each memory a 0x7E group expands to counts towards maxMemories.
    auto const memories = importingModule(
        { 0x01, 0x01, 'm', 0x00, 0x7E, 0x02, 0x00, 0x00, 0x03, 0x01, 'x', 0x01, 'y', 0x01, 'z' });
    auto const refused = decodeModule(memories, compactImportFeatures(), DecodeLimits { .maxMemories = 2 });
    REQUIRE(!refused);
    CHECK(refused.error().code == DecodeErrorCode::ImplementationLimitExceeded);
    CHECK(refused.error().offset == memories.size() - 2); // the third item, 0x01 'z'
    CHECK(errorOf(memories, compactImportFeatures(), DecodeLimits { .maxMemories = 3 }) == std::nullopt);
}
