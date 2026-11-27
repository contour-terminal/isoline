// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/binary/BranchHints.hpp>
#include <isoline/binary/Decoder.hpp>
#include <isoline/binary/Encoder.hpp>
#include <isoline/binary/Module.hpp>
#include <isoline/binary/Opcode.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/binary/testing/ModuleEquality.hpp>
#include <isoline/binary/testing/OpcodeRowModules.hpp>
#include <isoline/validate/Validator.hpp>

using isoline::FeatureSet;
using isoline::binary::BranchHint;
using isoline::binary::BranchHintSectionName;
using isoline::binary::BranchLikelihood;
using isoline::binary::canonicalize;
using isoline::binary::CustomErrorCode;
using isoline::binary::CustomSection;
using isoline::binary::DataSegment;
using isoline::binary::decodeModule;
using isoline::binary::encodeModule;
using isoline::binary::Export;
using isoline::binary::ExternKind;
using isoline::binary::Function;
using isoline::binary::FuncType;
using isoline::binary::I32;
using isoline::binary::I64;
using isoline::binary::Instruction;
using isoline::binary::Limits;
using isoline::binary::LocalRun;
using isoline::binary::MemoryType;
using isoline::binary::Module;
using isoline::binary::Opcode;
using isoline::binary::SectionAnchor;
using isoline::binary::SegmentMode;
using isoline::binary::testing::ModuleBuilder;
using isoline::binary::testing::opcodeRowModules;
using isoline::binary::testing::sameModule;
using isoline::validate::validateModule;

namespace
{

std::vector<std::byte> bytesOf(std::initializer_list<uint8_t> values)
{
    auto bytes = std::vector<std::byte> {};
    std::ranges::transform(
        values, std::back_inserter(bytes), [](uint8_t value) { return std::byte { value }; });
    return bytes;
}

std::vector<std::byte> readFile(std::filesystem::path const& path)
{
    auto stream = std::ifstream { path, std::ios::binary };
    auto const characters = std::vector<char> { std::istreambuf_iterator<char> { stream }, {} };
    auto bytes = std::vector<std::byte>(characters.size());
    std::ranges::transform(
        characters, bytes.begin(), [](char value) { return static_cast<std::byte>(value); });
    return bytes;
}

/// A module of one type, [params] -> [], and one function of that type with @p body (its end included).
Module oneFunction(std::vector<isoline::binary::ValueType> params, std::vector<Instruction> body)
{
    auto module = Module {};
    module.types.push_back(FuncType { .params = std::move(params), .results = {} });
    module.functions.push_back(0);
    auto function = Function {};
    function.body = std::move(body);
    module.code.push_back(std::move(function));
    return module;
}

Module decoded(std::span<std::byte const> bytes)
{
    auto module = decodeModule(bytes, FeatureSet::all());
    REQUIRE(module.has_value());
    return std::move(*module);
}

/// Whether @p haystack holds @p needle's bytes in a row.
bool contains(std::span<std::byte const> haystack, std::initializer_list<uint8_t> needle)
{
    auto const bytes = bytesOf(needle);
    return !std::ranges::search(haystack, bytes).empty();
}

} // namespace

TEST_CASE("Encoder.encodesEveryOpcodeRowExactly", "[Encoder]")
{
    // Every row's module is canonical by construction, so it comes back byte for byte.
    for (auto const& bytes: opcodeRowModules())
    {
        auto const module = decoded(bytes);
        INFO(isoline::binary::opcodeInfo(module.code.at(1).body.at(0).opcode).text);
        CHECK(encodeModule(module) == bytes);
    }
}

TEST_CASE("Encoder.preservesSpecCorpus", "[Encoder]")
{
    auto const features = FeatureSet::all();
    auto const corpus = std::filesystem::path { ISOLINE_SOURCE_DIR } / "tests" / "spec";
    auto checked = std::size_t { 0 };
    for (auto const& entry: std::filesystem::recursive_directory_iterator { corpus })
    {
        if (!entry.is_regular_file() || entry.path().extension() != ".wasm")
            continue;
        auto const bytes = readFile(entry.path());
        auto const original = decodeModule(bytes, features);
        if (!original)
            continue;
        INFO(entry.path().string());
        ++checked;
        auto const canonical = canonicalize(bytes, features);
        REQUIRE(canonical.has_value());
        auto const reread = decodeModule(*canonical, features);
        REQUIRE(reread.has_value());
        CHECK(sameModule(*reread, *original));
        auto const again = canonicalize(*canonical, features);
        REQUIRE(again.has_value());
        CHECK(*again == *canonical);
        auto const originalValid = validateModule(*original, features);
        auto const rereadValid = validateModule(*reread, features);
        CHECK(rereadValid.has_value() == originalValid.has_value());
        // The encoder alone, on the module as decoded, keeps it too.
        CHECK(sameModule(decoded(encodeModule(*original)), *original));
    }
    CHECK(checked > 1000);
}

TEST_CASE("Encoder.customPlacement", "[Encoder]")
{
    auto module = oneFunction({}, { Instruction { .opcode = Opcode::End } });
    module.data.push_back(
        DataSegment { .mode = SegmentMode::Passive, .memory = 0, .offset = {}, .bytes = {} });
    module.customs = {
        CustomSection { .name = "c", .bytes = bytesOf({ 0x03 }), .after = SectionAnchor::Data },
        CustomSection { .name = "a", .bytes = bytesOf({ 0x01 }), .after = SectionAnchor::BeforeFirst },
        CustomSection { .name = "b", .bytes = {}, .after = SectionAnchor::Function },
    };

    auto const expected = ModuleBuilder {}
                              .rawSection(0, { 0x01, 'a', 0x01 })
                              .rawSection(1, { 0x01, 0x60, 0x00, 0x00 })
                              .rawSection(3, { 0x01, 0x00 })
                              .rawSection(0, { 0x01, 'b' })
                              .rawSection(10, { 0x01, 0x02, 0x00, 0x0B })
                              .rawSection(11, { 0x01, 0x01, 0x00 })
                              .rawSection(0, { 0x01, 'c', 0x03 })
                              .build();
    auto const bytes = encodeModule(module);
    CHECK(bytes == expected);

    auto const reread = decoded(bytes);
    REQUIRE(reread.customs.size() == 3);
    CHECK(reread.customs[0].name == "a");
    CHECK(reread.customs[0].after == SectionAnchor::BeforeFirst);
    CHECK(reread.customs[1].name == "b");
    CHECK(reread.customs[1].after == SectionAnchor::Function);
    CHECK(reread.customs[2].name == "c");
    CHECK(reread.customs[2].after == SectionAnchor::Data);
}

TEST_CASE("Encoder.branchHintsSynthesized", "[Encoder]")
{
    auto module = oneFunction({ I32 },
                              {
                                  Instruction { .opcode = Opcode::LocalGet },
                                  Instruction { .opcode = Opcode::BrIf },
                                  Instruction { .opcode = Opcode::End },
                              });
    module.branchHints.push_back(
        BranchHint { .function = 0, .instruction = 1, .likelihood = BranchLikelihood::Likely });

    auto const reread = decoded(encodeModule(module));
    CHECK(reread.customErrors.empty());
    REQUIRE(reread.customs.size() == 1);
    CHECK(reread.customs[0].name == BranchHintSectionName);
    // Before the code section, and so after the function section.
    CHECK(reread.customs[0].after == SectionAnchor::Function);
    REQUIRE(reread.code.at(0).branchSites.size() == 1);
    auto const offset = reread.code[0].branchSites[0].offset;
    CHECK(offset == 3); // the locals declaration's byte, then local.get 0
    // One function, 0, with one hint: its offset, the size 1 and the hint byte.
    CHECK(reread.customs[0].bytes == bytesOf({ 0x01, 0x00, 0x01, static_cast<uint8_t>(offset), 0x01, 0x01 }));
    REQUIRE(reread.branchHints.size() == 1);
    CHECK(reread.branchHints[0].instruction == 1);
    CHECK(reread.branchHints[0].likelihood == BranchLikelihood::Likely);

    CHECK(isoline::binary::encodeBranchHints(module) == reread.customs[0].bytes);
    auto unhinted = module;
    unhinted.branchHints.clear();
    CHECK(isoline::binary::encodeBranchHints(unhinted).empty());

    // A raw section of that name is kept verbatim instead, and nothing is synthesized beside it.
    module.customs.push_back(CustomSection { .name = std::string { BranchHintSectionName },
                                             .bytes = bytesOf({ 0x00 }),
                                             .after = SectionAnchor::Function });
    auto const raw = decoded(encodeModule(module));
    REQUIRE(raw.customs.size() == 1);
    CHECK(raw.customs[0].bytes == bytesOf({ 0x00 }));
    CHECK(raw.branchHints.empty());
}

TEST_CASE("Encoder.branchHintsGroupByFunction", "[Encoder]")
{
    // Two functions after one imported function: the first with two hinted br_ifs, the second with one.
    auto const brIfTwice = std::vector<Instruction> {
        Instruction { .opcode = Opcode::LocalGet }, Instruction { .opcode = Opcode::BrIf },
        Instruction { .opcode = Opcode::LocalGet }, Instruction { .opcode = Opcode::BrIf },
        Instruction { .opcode = Opcode::End },
    };
    auto module = oneFunction({ I32 }, brIfTwice);
    module.importModules.emplace_back("m");
    module.imports.push_back(
        isoline::binary::Import { .moduleIndex = 0, .name = "f", .kind = ExternKind::Func });
    module.functions.push_back(0);
    module.code.push_back(module.code[0]);
    // Out of order: the encoder writes them by function, then by instruction.
    module.branchHints = {
        BranchHint { .function = 2, .instruction = 1, .likelihood = BranchLikelihood::Likely },
        BranchHint { .function = 1, .instruction = 3, .likelihood = BranchLikelihood::Unlikely },
        BranchHint { .function = 1, .instruction = 1, .likelihood = BranchLikelihood::Likely },
    };

    auto const reread = decoded(encodeModule(module));
    CHECK(reread.customErrors.empty());
    REQUIRE(reread.customs.size() == 1);
    // Two functions; function 1 at offsets 3 and 7, function 2 at offset 3.
    CHECK(reread.customs[0].bytes
          == bytesOf({ 0x02, 0x01, 0x02, 0x03, 0x01, 0x01, 0x07, 0x01, 0x00, 0x02, 0x01, 0x03, 0x01, 0x01 }));
    // encodeBranchHints gives the same payload from the hinted bodies alone.
    CHECK(isoline::binary::encodeBranchHints(module) == reread.customs[0].bytes);
    REQUIRE(reread.branchHints.size() == 3);
    CHECK(reread.branchHints[0].function == 1);
    CHECK(reread.branchHints[0].instruction == 1);
    CHECK(reread.branchHints[1].instruction == 3);
    CHECK(reread.branchHints[1].likelihood == BranchLikelihood::Unlikely);
    CHECK(reread.branchHints[2].function == 2);
}

TEST_CASE("Encoder.constantExpressionRunsAreEmpty", "[Encoder]")
{
    // A constant expression has no side tables (the decoder drops them), so a br_table, typed select
    // or try_table in one, which no valid module holds, is written with an empty run.
    auto module = Module {};
    module.globals.push_back(isoline::binary::Global {
        .type = isoline::binary::GlobalType { .type = I32, .mutability = isoline::binary::Mutability::Const },
        .init = {
            Instruction { .opcode = Opcode::BrTable, .a = 4, .b = 3 },
            Instruction { .opcode = Opcode::SelectTyped, .a = 2, .b = 1 },
            Instruction { .opcode = Opcode::TryTable, .b = 2, .c = 5 },
            Instruction { .opcode = Opcode::End },
            Instruction { .opcode = Opcode::End },
        } });
    auto const bytes = encodeModule(module);
    // br_table: no labels, default 0; select: no types; try_table: empty block type, no clauses.
    CHECK(contains(bytes, { 0x0E, 0x00, 0x00, 0x1C, 0x00, 0x1F, 0x40, 0x00, 0x0B, 0x0B }));
    auto const reread = decoded(bytes);
    REQUIRE(reread.globals.size() == 1);
    CHECK(reread.globals[0].init.at(0).b == 1);
    CHECK(reread.globals[0].init.at(1).b == 0);
    CHECK(reread.globals[0].init.at(2).b == 0);
}

TEST_CASE("Encoder.normalizeReanchorsCustoms", "[Encoder]")
{
    // A custom section anchored to a section the encoder omits is anchored to the last one it writes
    // before it, where the encoder places it anyway: here after the function section.
    auto module = oneFunction({}, { Instruction { .opcode = Opcode::End } });
    module.customs = {
        CustomSection { .name = "a", .bytes = {}, .after = SectionAnchor::Table },
        CustomSection { .name = "b", .bytes = {}, .after = SectionAnchor::BeforeFirst },
    };
    // The sections are then in the order the encoder writes them: by anchor, module order within one.
    auto const normalized = isoline::binary::normalize(module);
    REQUIRE(normalized.customs.size() == 2);
    CHECK(normalized.customs[0].name == "b");
    CHECK(normalized.customs[0].after == SectionAnchor::BeforeFirst);
    CHECK(normalized.customs[1].name == "a");
    CHECK(normalized.customs[1].after == SectionAnchor::Function);
    auto const reread = decoded(encodeModule(module));
    REQUIRE(reread.customs.size() == 2);
    CHECK(reread.customs[1].after == SectionAnchor::Function);
    CHECK(sameModule(reread, module));
}

namespace
{

/// A module of one function, [i32] -> [], whose code is @p code (its locals declaration on), with
/// @p sections branch hint sections before it that each hint, Likely, the instruction at @p hintOffset.
std::vector<std::byte> hintedModule(std::initializer_list<uint8_t> code, uint8_t hintOffset, int sections = 1)
{
    auto custom = std::vector<uint8_t> { static_cast<uint8_t>(BranchHintSectionName.size()) };
    custom.insert(custom.end(), BranchHintSectionName.begin(), BranchHintSectionName.end());
    custom.insert(custom.end(), { 0x01, 0x00, 0x01, hintOffset, 0x01, 0x01 });
    auto body = std::vector<uint8_t> { 0x01, static_cast<uint8_t>(code.size()) };
    body.insert(body.end(), code.begin(), code.end());
    auto builder = ModuleBuilder {};
    builder.type(FuncType { .params = { I32 }, .results = {} }).rawSection(3, { 0x01, 0x00 });
    for ([[maybe_unused]] auto const section: std::views::iota(0, sections))
        builder.rawSection(0, custom);
    return builder.rawSection(10, std::move(body)).build();
}

} // namespace

TEST_CASE("Encoder.canonicalRewritesBranchHints", "[Encoder]")
{
    // The hint names br_if's offset in the original encoding, which canonicalization changes: a run of
    // no locals is dropped (01 00 7F becomes 00), or a non-minimal local index is shortened (20 80 00).
    // The hint section is written again from Module::branchHints, at br_if's new offset, 3.
    for (auto const& original: { hintedModule({ 0x01, 0x00, 0x7F, 0x20, 0x00, 0x0D, 0x00, 0x0B }, 5),
                                 hintedModule({ 0x00, 0x20, 0x80, 0x00, 0x0D, 0x00, 0x0B }, 4) })
    {
        auto const module = decoded(original);
        REQUIRE(module.branchHints.size() == 1);
        REQUIRE(module.customErrors.empty());

        auto const canonical = canonicalize(original, FeatureSet::all());
        REQUIRE(canonical.has_value());
        auto const reread = decoded(*canonical);
        CHECK(reread.customErrors.empty());
        REQUIRE(reread.branchHints.size() == 1);
        CHECK(reread.branchHints[0].instruction == 1);
        CHECK(reread.branchHints[0].likelihood == BranchLikelihood::Likely);
        REQUIRE(reread.customs.size() == 1);
        CHECK(reread.customs[0].bytes == bytesOf({ 0x01, 0x00, 0x01, 0x03, 0x01, 0x01 }));
        CHECK(sameModule(reread, module));
    }

    // A section the decoder refused (offset 1 is local.get's, no branch) stays as it was.
    auto const refused = hintedModule({ 0x00, 0x20, 0x00, 0x0D, 0x00, 0x0B }, 1);
    auto const module = decoded(refused);
    REQUIRE(module.customErrors.size() == 1);
    auto const canonical = canonicalize(refused, FeatureSet::all());
    REQUIRE(canonical.has_value());
    CHECK(*canonical == refused);

    // So does a valid section beside a duplicate the decoder refused, though its hint was kept.
    auto const duplicated = hintedModule({ 0x01, 0x00, 0x7F, 0x20, 0x00, 0x0D, 0x00, 0x0B }, 5, 2);
    auto const withDuplicate = decoded(duplicated);
    REQUIRE(withDuplicate.branchHints.size() == 1);
    REQUIRE(withDuplicate.customErrors.size() == 1);
    auto const kept = isoline::binary::normalize(withDuplicate);
    CHECK(kept.customs.size() == 2);
}

TEST_CASE("Encoder.canonicalMovesHintsBeforeCode", "[Encoder]")
{
    // A raw hint section after the code section, between customs "a" (before code) and "b" (after it).
    // The synthesized section goes where SynthesizedSections places it: before code, after "a".
    auto hint = std::vector<uint8_t> { static_cast<uint8_t>(BranchHintSectionName.size()) };
    hint.insert(hint.end(), BranchHintSectionName.begin(), BranchHintSectionName.end());
    hint.insert(hint.end(), { 0x01, 0x00, 0x01, 0x05, 0x01, 0x01 });
    auto const original = ModuleBuilder {}
                              .type(FuncType { .params = { I32 }, .results = {} })
                              .rawSection(3, { 0x01, 0x00 })
                              .rawSection(0, { 0x01, 'a' })
                              .rawSection(10, { 0x01, 0x08, 0x01, 0x00, 0x7F, 0x20, 0x00, 0x0D, 0x00, 0x0B })
                              .rawSection(0, std::move(hint))
                              .rawSection(0, { 0x01, 'b' })
                              .build();
    auto const module = decoded(original);
    REQUIRE(module.branchHints.size() == 1);
    REQUIRE(module.customErrors.empty());
    REQUIRE(module.customs.size() == 3);
    CHECK(module.customs[1].after == SectionAnchor::Code);

    auto const canonical = canonicalize(original, FeatureSet::all());
    REQUIRE(canonical.has_value());
    auto const reread = decoded(*canonical);
    REQUIRE(reread.customs.size() == 3);
    CHECK(reread.customs[0].name == "a");
    CHECK(reread.customs[0].after == SectionAnchor::Function);
    CHECK(reread.customs[1].name == BranchHintSectionName);
    CHECK(reread.customs[1].after == SectionAnchor::Function);
    CHECK(reread.customs[2].name == "b");
    CHECK(reread.customs[2].after == SectionAnchor::Code);
    CHECK(reread.branchHints.size() == 1);
    CHECK(reread.customErrors.empty());
}

TEST_CASE("Encoder.canonicalReencodesNameSection", "[Encoder]")
{
    // A function map with a non-minimal index (80 00), an empty type map, and GC field names (id 10).
    auto const nameSection = [](std::vector<uint8_t> const& payload) {
        auto custom = std::vector<uint8_t> { 0x04, 'n', 'a', 'm', 'e' };
        custom.insert(custom.end(), payload.begin(), payload.end());
        return ModuleBuilder {}.type(FuncType {}).rawSection(0, std::move(custom)).build();
    };
    auto const canonical = canonicalize(
        nameSection({ 0x01, 0x05, 0x01, 0x80, 0x00, 0x01, 'f', 0x04, 0x01, 0x00, 0x0A, 0x01, 0x00 }),
        FeatureSet::all());
    REQUIRE(canonical.has_value());
    CHECK(*canonical == nameSection({ 0x01, 0x04, 0x01, 0x00, 0x01, 'f' }));

    // One the name section codec refuses (indices 1, 0) stays verbatim: names are advisory.
    auto const unsorted = nameSection({ 0x01, 0x07, 0x02, 0x01, 0x01, 'a', 0x00, 0x01, 'b' });
    auto const kept = canonicalize(unsorted, FeatureSet::all());
    REQUIRE(kept.has_value());
    CHECK(*kept == unsorted);
}

TEST_CASE("Encoder.hintOnAnyInstructionEncodes", "[Encoder]")
{
    auto module = oneFunction({ I32 },
                              {
                                  Instruction { .opcode = Opcode::LocalGet },
                                  Instruction { .opcode = Opcode::LocalGet },
                                  Instruction { .opcode = Opcode::I32Eq },
                                  Instruction { .opcode = Opcode::Drop },
                                  Instruction { .opcode = Opcode::End },
                              });
    module.branchHints.push_back(
        BranchHint { .function = 0, .instruction = 2, .likelihood = BranchLikelihood::Unlikely });
    auto const reread = decoded(encodeModule(module));
    CHECK(reread.branchHints.empty());
    REQUIRE(reread.customErrors.size() == 1);
    CHECK(reread.customErrors[0].code == CustomErrorCode::BranchHintInvalidTarget);
}

TEST_CASE("Encoder.memargMemoryIndex", "[Encoder]")
{
    auto module = oneFunction({},
                              {
                                  Instruction { .opcode = Opcode::I32Const },
                                  Instruction { .opcode = Opcode::I32Load, .a = 2, .b = 0, .c = 0 },
                                  Instruction { .opcode = Opcode::Drop },
                                  Instruction { .opcode = Opcode::I32Const },
                                  Instruction { .opcode = Opcode::I32Load, .a = 2, .b = 1, .c = 4 },
                                  Instruction { .opcode = Opcode::Drop },
                                  Instruction { .opcode = Opcode::End },
                              });
    module.memories = { MemoryType {}, MemoryType {} };
    auto const bytes = encodeModule(module);
    // i32.load, then the flags, the memory index if any, and the offset.
    CHECK(contains(bytes, { 0x41, 0x00, 0x28, 0x02, 0x00, 0x1A }));
    CHECK(contains(bytes, { 0x41, 0x00, 0x28, 0x42, 0x01, 0x04, 0x1A }));
    auto const reread = decoded(bytes);
    CHECK(sameModule(reread, module));
}

TEST_CASE("Encoder.importsArePlain", "[Encoder]")
{
    // One compact entry (0x7E: one extern type, func 0, for every item) from module "m": a, b and c.
    auto const compact =
        ModuleBuilder {}
            .type(FuncType {})
            .rawSection(2, { 0x01, 0x01, 'm', 0x00, 0x7E, 0x00, 0x00, 0x03, 0x01, 'a', 0x01, 'b', 0x01, 'c' })
            .build();
    auto const module = decoded(compact);
    REQUIRE(module.imports.size() == 3);
    CHECK(module.importModules.size() == 1);

    auto const plain = ModuleBuilder {}
                           .type(FuncType {})
                           .importFunction("m", "a", 0)
                           .importFunction("m", "b", 0)
                           .importFunction("m", "c", 0)
                           .build();
    CHECK(encodeModule(module) == plain);
    CHECK(sameModule(decoded(plain), module));
}

TEST_CASE("Encoder.canonicalDropsRedundantDataCount", "[Encoder]")
{
    // A data count of 0 with no data and no memory.init, as binary.55.wasm has it.
    auto const redundant = ModuleBuilder {}.dataCount(0).build();
    CHECK(canonicalize(redundant, FeatureSet::all()) == ModuleBuilder {}.build());

    // A count that differs from the data segments' is no encoding choice: the decoder refuses it.
    auto mismatched = Module {};
    mismatched.dataCount = 2;
    CHECK(isoline::binary::normalize(mismatched).dataCount == 2);

    // The same count is kept once code names a data segment.
    for (auto const instruction:
         { std::initializer_list<uint8_t> { 0x41, 0x00, 0x41, 0x00, 0x41, 0x00, 0xFC, 0x08, 0x00, 0x00 },
           std::initializer_list<uint8_t> { 0xFC, 0x09, 0x00 } })
    {
        auto const needed =
            ModuleBuilder {}
                .type(FuncType {})
                .function(0, {}, std::vector<uint8_t>(instruction))
                .memory(Limits {
                    .min = 1, .max = std::nullopt, .addressType = isoline::binary::AddressType::I32 })
                .dataCount(1)
                .data({ 0x01, 0x00 })
                .build();
        auto const canonical = canonicalize(needed, FeatureSet::all());
        REQUIRE(canonical.has_value());
        CHECK(*canonical == needed);
    }
}

TEST_CASE("Encoder.canonicalMergesLocalRuns", "[Encoder]")
{
    // Runs of 0 i32, 2 i64 and 1 i64, as binary.45.wasm's zero-count runs: one run of 3 i64.
    auto const runs = ModuleBuilder {}
                          .type(FuncType {})
                          .rawSection(3, { 0x01, 0x00 })
                          .rawSection(10, { 0x01, 0x08, 0x03, 0x00, 0x7F, 0x02, 0x7E, 0x01, 0x7E, 0x0B })
                          .build();
    auto const canonical = canonicalize(runs, FeatureSet::all());
    REQUIRE(canonical.has_value());
    CHECK(*canonical == ModuleBuilder {}.type(FuncType {}).function(0, { I64, I64, I64 }, {}).build());
    auto const reread = decoded(*canonical);
    REQUIRE(reread.code.at(0).locals.size() == 1);
    CHECK(reread.code[0].locals[0].count == 3);
    CHECK(reread.code[0].locals[0].type == I64);

    // The encoder itself writes the runs it is given.
    auto const module = decoded(runs);
    CHECK(encodeModule(module) == runs);
    CHECK(sameModule(module, reread));

    // Runs are merged only as far as a count holds: a run that would overflow stays a run of its own.
    auto full = module;
    full.code[0].locals = { LocalRun { .count = 0xFFFF'FFFF, .type = I32 },
                            LocalRun { .count = 1, .type = I32 } };
    CHECK(isoline::binary::normalize(full).code[0].locals.size() == 2);
}

TEST_CASE("ModuleEquality.detectsDroppedFields", "[ModuleEquality]")
{
    auto module = oneFunction({},
                              {
                                  Instruction { .opcode = Opcode::I32Const },
                                  Instruction { .opcode = Opcode::I32Load, .a = 2, .b = 1, .c = 0 },
                                  Instruction { .opcode = Opcode::Drop },
                                  Instruction { .opcode = Opcode::End },
                              });
    module.memories = { MemoryType {}, MemoryType {} };
    module.exports.push_back(Export { .name = "f", .kind = ExternKind::Func, .index = 0 });
    module.start = 0;
    CHECK(sameModule(module, module));

    auto memoryZero = module;
    memoryZero.code[0].body[1].b = 0;
    CHECK_FALSE(sameModule(module, memoryZero));

    auto noExport = module;
    noExport.exports.clear();
    CHECK_FALSE(sameModule(module, noExport));

    auto noStart = module;
    noStart.start.reset();
    CHECK_FALSE(sameModule(module, noStart));

    // Only a redundant run grouping differs: the same module.
    auto regrouped = module;
    regrouped.code[0].locals = { LocalRun { .count = 1, .type = I32 }, LocalRun { .count = 1, .type = I32 } };
    regrouped.code[0].localCount = 2;
    auto grouped = module;
    grouped.code[0].locals = { LocalRun { .count = 2, .type = I32 } };
    grouped.code[0].localCount = 2;
    CHECK(sameModule(regrouped, grouped));
}

TEST_CASE("Encoder.maxBytes", "[Encoder]")
{
    // A module of one compact import group: the encoding repeats its module name per import.
    auto module = Module {};
    module.importModules.emplace_back(100, 'm');
    for ([[maybe_unused]] auto const index: std::views::iota(0, 10))
        module.imports.push_back(isoline::binary::Import { .moduleIndex = 0,
                                                           .name = "",
                                                           .kind = ExternKind::Global,
                                                           .typeIndex = 0,
                                                           .table = {},
                                                           .memory = {},
                                                           .global = { .type = I32 } });
    auto const whole = encodeModule(module);
    REQUIRE(whole.size() > 1'000);

    // Exactly its size encodes, to the byte; one byte less is refused.
    auto const fitting = encodeModule(module, whole.size());
    REQUIRE(fitting.has_value());
    CHECK(*fitting == whole);
    auto const refused = encodeModule(module, whole.size() - 1);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == isoline::binary::EncodeError::ModuleTooLarge);

    // So is a code section past the limit, and custom sections.
    auto const code = oneFunction({}, std::vector<Instruction>(100, Instruction { .opcode = Opcode::Nop }));
    auto const codeBytes = encodeModule(code);
    CHECK(encodeModule(code, codeBytes.size()).has_value());
    CHECK_FALSE(encodeModule(code, codeBytes.size() - 1).has_value());
    CHECK_FALSE(encodeModule(code, 50).has_value());
    auto custom = Module {};
    custom.customs.push_back(
        CustomSection { .name = "c", .bytes = std::vector<std::byte>(100), .after = SectionAnchor::Data });
    auto const customBytes = encodeModule(custom);
    CHECK(encodeModule(custom, customBytes.size()).has_value());
    CHECK_FALSE(encodeModule(custom, customBytes.size() - 1).has_value());
}
