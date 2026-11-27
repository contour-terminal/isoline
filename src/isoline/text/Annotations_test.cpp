// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/binary/BranchHints.hpp>
#include <isoline/binary/Decoder.hpp>
#include <isoline/binary/Module.hpp>
#include <isoline/binary/NameSection.hpp>
#include <isoline/binary/Opcode.hpp>
#include <isoline/text/Annotations.hpp>
#include <isoline/text/ModuleReader.hpp>
#include <isoline/text/SExpression.hpp>
#include <isoline/text/TextError.hpp>
#include <isoline/text/TextLimits.hpp>
#include <isoline/text/TopLevel.hpp>
#include <isoline/text/testing/ReadModule.hpp>

using isoline::FeatureSet;
using isoline::binary::BranchHint;
using isoline::binary::BranchLikelihood;
using isoline::binary::CustomErrorCode;
using isoline::binary::IndirectNameMap;
using isoline::binary::Module;
using isoline::binary::NameAssoc;
using isoline::binary::NameKind;
using isoline::binary::NameSection;
using isoline::binary::Opcode;
using isoline::binary::SectionAnchor;
using isoline::text::AnnotationKind;
using isoline::text::compileText;
using isoline::text::CustomPlacement;
using isoline::text::NodeId;
using isoline::text::parseSExpressions;
using isoline::text::PlacementSide;
using isoline::text::readCustomAnnotation;
using isoline::text::specMessage;
using isoline::text::stageOf;
using isoline::text::TextError;
using isoline::text::TextErrorCode;
using isoline::text::TextErrorStage;
using isoline::text::TextLimits;

namespace
{

/// The module @p source reads as, which it must.
Module readModule(std::string_view source, TextLimits limits = {})
{
    auto module = isoline::text::testing::readModuleText(source, limits);
    INFO(source);
    if (!module)
        INFO(specMessage(module.error()));
    REQUIRE(module.has_value());
    return std::move(*module);
}

/// The error @p source is refused with, which it must be.
TextError readError(std::string_view source, TextLimits limits = {})
{
    auto module = isoline::text::testing::readModuleText(source, limits);
    INFO(source);
    REQUIRE_FALSE(module.has_value());
    return std::move(module.error());
}

/// The bytes @p source compiles to, which it must.
std::vector<std::byte> compiled(std::string_view source, TextLimits limits = {})
{
    auto bytes = compileText(source, limits);
    INFO(source);
    if (!bytes)
        INFO(specMessage(bytes.error()));
    REQUIRE(bytes.has_value());
    return std::move(*bytes);
}

/// The module @p source compiles to, decoded again: what the encoding says, not what the reader built.
Module decodedModule(std::string_view source)
{
    auto const bytes = compiled(source);
    auto module = isoline::binary::decodeModule(bytes, FeatureSet::all());
    REQUIRE(module.has_value());
    return std::move(*module);
}

/// The names of @p module's name section.
NameSection namesOf(Module const& module)
{
    auto const section =
        std::ranges::find(module.customs, std::string_view { "name" }, [](auto const& custom) {
            return std::string_view { custom.name };
        });
    REQUIRE(section != module.customs.end());
    auto names = isoline::binary::decodeNameSection(section->bytes);
    REQUIRE(names.has_value());
    return std::move(*names);
}

/// The direct name map of @p kind.
std::vector<NameAssoc> const& directNames(NameSection const& names, NameKind kind)
{
    return names.direct[static_cast<std::size_t>(kind)];
}

/// The indirect name map of @p kind.
std::vector<IndirectNameMap> const& indirectNames(NameSection const& names, NameKind kind)
{
    return names.indirect[static_cast<std::size_t>(kind)];
}

/// The bytes a string holds.
std::vector<std::byte> bytesOf(std::string_view text)
{
    auto bytes = std::vector<std::byte> {};
    std::ranges::transform(text, std::back_inserter(bytes), [](char byte) { return std::byte(byte); });
    return bytes;
}

/// The ids of the sections of a binary module, in order, custom ones included.
std::vector<uint8_t> sectionIds(std::span<std::byte const> bytes)
{
    auto ids = std::vector<uint8_t> {};
    auto position = std::size_t { 8 }; // The magic number and the version.
    while (position < bytes.size())
    {
        ids.push_back(std::to_integer<uint8_t>(bytes[position++]));
        auto size = std::size_t { 0 };
        auto shift = 0U;
        auto byte = uint8_t { 0x80 };
        while ((byte & 0x80U) != 0)
        {
            byte = std::to_integer<uint8_t>(bytes[position++]);
            size |= static_cast<std::size_t>(byte & 0x7FU) << shift;
            shift += 7;
        }
        position += size;
    }
    return ids;
}

/// One expected refusal: a source and the error it is refused with.
struct Refusal
{
    std::string_view source; ///< The text.
    TextErrorCode code;      ///< Its error.
};

/// Checks that each source is refused with its error, at the Custom stage when @p stage says so.
void checkRefusals(std::span<Refusal const> refusals, TextErrorStage stage)
{
    for (auto const& [source, code]: refusals)
    {
        auto const error = readError(source);
        INFO(source);
        INFO(specMessage(error));
        CHECK(error.code == code);
        CHECK(stageOf(error.code) == stage);
    }
}

} // namespace

TEST_CASE("Annotations.customPlacement")
{
    auto const source =
        R"wat((module (memory 1) (global i32 (i32.const 0)) (@custom "a" (before global) "x")))wat";
    auto const module = readModule(source);
    REQUIRE(module.customs.size() == 1);
    CHECK(module.customs[0].name == "a");
    CHECK(module.customs[0].bytes == bytesOf("x"));
    // The last section the module has before the global section: there is no tag section.
    CHECK(module.customs[0].after == SectionAnchor::Memory);

    // Encoded between the memory section (5) and the global section (6), and decoded so again.
    auto const bytes = compiled(source);
    CHECK(sectionIds(bytes) == std::vector<uint8_t> { 5, 0, 6 });
    auto const decoded = isoline::binary::decodeModule(bytes, FeatureSet::all());
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->customs.size() == 1);
    CHECK(decoded->customs[0].after == SectionAnchor::Memory);

    SECTION("first, last, and the default")
    {
        auto const placed = readModule(R"wat((module
            (@custom "end") (@custom "start" (before first)) (@custom "tail" (after last))
            (func $f)))wat");
        REQUIRE(placed.customs.size() == 4);
        CHECK(placed.customs[0].name == "start");
        CHECK(placed.customs[0].after == SectionAnchor::BeforeFirst);
        // `(after last)` is the default; both keep their source order, and the name section follows.
        CHECK(placed.customs[1].name == "end");
        CHECK(placed.customs[2].name == "tail");
        CHECK(placed.customs[1].after == SectionAnchor::Code);
        CHECK(placed.customs[2].after == SectionAnchor::Code);
        CHECK(placed.customs[3].name == "name");
    }

    SECTION("one slot: after a section before before the next, then source order")
    {
        // Without table, memory and tag sections, `(after func)` and `(before global)` share the slot
        // after the function section; the reference orders After Func before Before Global.
        auto const placed = readModule(R"wat((module
            (@custom "b1" (before global)) (@custom "a1" (after func)) (@custom "b2" (before global))
            (@custom "a2" (after func)) (func) (global i32 (i32.const 0))))wat");
        auto const names =
            placed.customs | std::views::transform([](auto const& custom) { return custom.name; });
        CHECK(std::ranges::equal(names, std::vector<std::string> { "a1", "a2", "b1", "b2" }));
        CHECK(std::ranges::all_of(
            placed.customs, [](auto const& custom) { return custom.after == SectionAnchor::Function; }));
    }

    SECTION("before a section names the one before it, after a section that one")
    {
        auto const placed = readModule(
            R"wat((module (type (func)) (func) (@custom "b" (before func)) (@custom "a" (after type))))wat");
        REQUIRE(placed.customs.size() == 2);
        // After Type sorts before Before Func: they are different sections, Type first.
        CHECK(placed.customs[0].name == "a");
        CHECK(placed.customs[1].name == "b");
        CHECK(placed.customs[0].after == SectionAnchor::Type);
        CHECK(placed.customs[1].after == SectionAnchor::Type);
    }

    SECTION("contents concatenate, and a bare module takes them as fields")
    {
        auto const placed = readModule(R"wat((@custom "my-section4" "" "1" "" "2" "3" "") (func))wat");
        REQUIRE(placed.customs.size() == 1);
        CHECK(placed.customs[0].bytes == bytesOf("123"));
        CHECK(readModule(R"wat((@custom ""))wat").customs.at(0).name.empty());
        CHECK(readModule(R"wat(((@custom "h") module))wat").customs.at(0).name == "h");
    }

    SECTION("readCustomAnnotation")
    {
        auto const tree =
            parseSExpressions(R"wat((@custom "s" (after datacount) "a" "b"))wat", TextLimits {});
        REQUIRE(tree.has_value());
        auto const custom = readCustomAnnotation(*tree, tree->firstTopLevel());
        REQUIRE(custom.has_value());
        CHECK(custom->section.name == "s");
        CHECK(custom->section.bytes == bytesOf("ab"));
        CHECK(custom->placement
              == CustomPlacement { .section = SectionAnchor::DataCount, .side = PlacementSide::After });
    }

    SECTION("bounded by the output limit")
    {
        auto limits = TextLimits {};
        limits.maxModuleBytes = 64;
        auto const big = std::string { R"wat((@custom "c" ")wat" } + std::string(100, 'x') + "\")";
        auto const refused = compileText(big, limits);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == TextErrorCode::OutputTooLarge);
    }
}

TEST_CASE("Annotations.customErrors")
{
    static constexpr auto Refusals = std::to_array<Refusal>({
        { R"wat((@custom))wat", TextErrorCode::CustomMissingName },
        { R"wat((@custom 4))wat", TextErrorCode::CustomMissingName },
        { R"wat((@custom bla))wat", TextErrorCode::CustomMissingName },
        { R"wat((@custom "\df"))wat", TextErrorCode::CustomMalformedUtf8 },
        { R"wat((@custom "bla" here))wat", TextErrorCode::CustomUnexpectedToken },
        { R"wat((@custom "bla" after))wat", TextErrorCode::CustomUnexpectedToken },
        { R"wat((@custom "bla" "x" (after func)))wat", TextErrorCode::CustomUnexpectedToken },
        { R"wat((@custom "bla" (after func) (after func)))wat", TextErrorCode::CustomUnexpectedToken },
        { R"wat((@custom "bla" (@x)))wat", TextErrorCode::CustomUnexpectedToken },
        { R"wat((@custom "bla" (after func x)))wat", TextErrorCode::CustomUnexpectedToken },
        { R"wat((@custom "bla" (after)))wat", TextErrorCode::CustomMalformedSectionKind },
        { R"wat((@custom "bla" (before types)))wat", TextErrorCode::CustomMalformedSectionKind },
        { R"wat((@custom "bla" (before "type")))wat", TextErrorCode::CustomMalformedSectionKind },
        { R"wat((@custom "bla" (before (@global))))wat", TextErrorCode::CustomMalformedSectionKind },
        { R"wat((@custom "bla" ((@before) global)))wat", TextErrorCode::CustomMalformedPlacement },
        { R"wat((@custom "bla" (type)))wat", TextErrorCode::CustomMalformedPlacement },
        { R"wat((@custom "bla" (aft type)))wat", TextErrorCode::CustomMalformedPlacement },
        { R"wat((@custom "bla" ()))wat", TextErrorCode::CustomMalformedPlacement },
        { R"wat((type (@custom "bla") $t (func)))wat", TextErrorCode::MisplacedCustom },
        { R"wat((func (@custom "b")))wat", TextErrorCode::MisplacedCustom },
        { R"wat((func (block (@custom "bla"))))wat", TextErrorCode::MisplacedCustom },
        { R"wat((func (nop (@custom "bla"))))wat", TextErrorCode::MisplacedCustom },
        { R"wat((module (global i32 (i32.const 0) (@custom "bla"))))wat", TextErrorCode::MisplacedCustom },
    });
    checkRefusals(Refusals, TextErrorStage::Custom);

    SECTION("the reference meets them last first")
    {
        // Annot.record prepends: the misplaced one after the one missing its name is reported first.
        CHECK(readError(R"wat((@custom) (func (@custom "b")))wat").code == TextErrorCode::MisplacedCustom);
        CHECK(readError(R"wat((func (@custom "b")) (@custom))wat").code == TextErrorCode::CustomMissingName);
    }

    SECTION("outside the module form, after it reads")
    {
        for (auto const source: { R"wat((module) (@custom "x"))wat",
                                  R"wat((@custom "x") (module))wat",
                                  R"wat((module) (@name "x"))wat",
                                  R"wat((@metadata.code.branch_hint "\01") (module (func nop)))wat" })
        {
            INFO(source);
            auto const read = readError(source);
            CHECK(read.code == TextErrorCode::MisplacedAnnotation);
            CHECK(stageOf(read.code) == TextErrorStage::Custom);
            CHECK(specMessage(read) == "misplaced annotation");
            auto const compiledText = compileText(source, TextLimits {});
            REQUIRE_FALSE(compiledText.has_value());
            CHECK(compiledText.error().code == TextErrorCode::MisplacedAnnotation);
        }
        // The module's own errors come first; unknown annotations outside it mean nothing.
        CHECK(readError(R"wat((@custom "x") (module (func (@custom "y"))))wat").code
              == TextErrorCode::MisplacedCustom);
        CHECK(compiled(R"wat((@x) (module) (@y))wat") == compiled("(module)"));
    }

    SECTION("an annotation's own syntax comes before its placement")
    {
        CHECK(readError(R"wat((func (@custom)))wat").code == TextErrorCode::CustomMissingName);
    }

    SECTION("the module's syntax comes before every annotation")
    {
        auto const error = readError(R"wat((@custom) (func (bogus)))wat");
        CHECK(stageOf(error.code) == TextErrorStage::Syntax);
    }

    SECTION("inside an unknown annotation, nothing is read")
    {
        CHECK(readModule(R"wat((func (@x (@custom))))wat").customs.empty());
    }
}

TEST_CASE("Annotations.nameOverridesId")
{
    auto const names = namesOf(readModule(R"wat((func $lambda (@name "λ")))wat"));
    CHECK(directNames(names, NameKind::Function) == std::vector { NameAssoc { .index = 0, .name = "λ" } });

    SECTION("misplaced and multiple names")
    {
        static constexpr auto Refusals = std::to_array<Refusal>({
            { R"wat((module (func) (@name "M")))wat", TextErrorCode::MisplacedName },
            { R"wat((module (start $f (@name "M")) (func $f)))wat", TextErrorCode::MisplacedName },
            { R"wat((func (export "e") (@name "a")))wat", TextErrorCode::MisplacedName },
            { R"wat((func (nop (@name "a"))))wat", TextErrorCode::MisplacedName },
            { R"wat((func (block (result i32) (@name "b") (i32.const 0))))wat",
              TextErrorCode::MisplacedName },
            { R"wat((func (call_indirect (param (@name "p") i32))))wat", TextErrorCode::MisplacedName },
            { R"wat((func (@name "a") (@name "b")))wat", TextErrorCode::NameMultipleFunctionNames },
            { R"wat((global $g (@name "a") (@x) (@name "b") i32 (i32.const 0)))wat",
              TextErrorCode::NameMultipleFunctionNames },
            { R"wat((module (@name "M1") (@name "M2")))wat", TextErrorCode::NameMultipleModule },
            { R"wat((func (param $x (@name "a") (@name "b") i32)))wat",
              TextErrorCode::NameMultipleLocalNames },
            { R"wat((func (@name)))wat", TextErrorCode::NameStringExpected },
            { R"wat((func (@name x)))wat", TextErrorCode::NameStringExpected },
            { R"wat((func (@name "a" "b")))wat", TextErrorCode::NameUnexpectedToken },
            { R"wat((func (@name "\ff")))wat", TextErrorCode::NameMalformedUtf8 },
            // A descriptor several compact imports share names none of them.
            { R"wat((import "m" (item "a") (item "b") (func (@name "f"))))wat",
              TextErrorCode::MisplacedName },
            // Last first, as the reference meets them: the misplaced name after the empty one.
            { R"wat((func (@name)) (func (nop (@name "x"))))wat", TextErrorCode::MisplacedName },
            // Every name's own errors come before any name is merged.
            { R"wat((func (@name "a") (@name "b")) (func (@name 1)))wat", TextErrorCode::NameStringExpected },
        });
        checkRefusals(Refusals, TextErrorStage::Custom);
        CHECK(specMessage(readError(R"wat((module (@name "M1") (@name "M2")))wat"))
              == "@name annotation: multiple module names");
    }

    SECTION("a named parameter or local is one type")
    {
        CHECK(readError(R"wat((func (param (@name "a") i32 i64)))wat").code
              == TextErrorCode::UnexpectedToken);
        CHECK(readError(R"wat((func (local (@name "a") i32 i64)))wat").code
              == TextErrorCode::UnexpectedToken);
    }

    SECTION("every entity a name may follow, as wasm-tools 1.263.0 reads them")
    {
        auto const module = readModule(R"wat((module $m (@name "M")
            (type $t (@name "T") (func (param $tp (@name "TP") i32)))
            (import "i" "f" (func $if (@name "IF") (param (@name "IP") i32)))
            (func $f (@name "F") (param $p (@name "P") i32) (param i64) (local (@name "L") i32) (local $l2 i32)
              (block $b (@name "B")) block (@name "B2") end (loop (@name "B3")))
            (global (@name "G") i32 (i32.const 0)) (memory (@name "Mem") 1) (table (@name "Tb") 1 funcref)
            (tag (@name "Tg") (param (@name "TgP") i32)) (elem (@name "E") func) (data (@name "D") "")))wat");
        auto const allNames = namesOf(module);
        CHECK(allNames.module == "M");
        CHECK(directNames(allNames, NameKind::Type) == std::vector { NameAssoc { .index = 0, .name = "T" } });
        CHECK(
            directNames(allNames, NameKind::Function)
            == std::vector { NameAssoc { .index = 0, .name = "IF" }, NameAssoc { .index = 1, .name = "F" } });
        CHECK(directNames(allNames, NameKind::Global)
              == std::vector { NameAssoc { .index = 0, .name = "G" } });
        CHECK(directNames(allNames, NameKind::Memory)
              == std::vector { NameAssoc { .index = 0, .name = "Mem" } });
        CHECK(directNames(allNames, NameKind::Table)
              == std::vector { NameAssoc { .index = 0, .name = "Tb" } });
        CHECK(directNames(allNames, NameKind::Tag) == std::vector { NameAssoc { .index = 0, .name = "Tg" } });
        CHECK(directNames(allNames, NameKind::Element)
              == std::vector { NameAssoc { .index = 0, .name = "E" } });
        CHECK(directNames(allNames, NameKind::Data) == std::vector { NameAssoc { .index = 0, .name = "D" } });
        CHECK(indirectNames(allNames, NameKind::Parameter)
              == std::vector {
                  IndirectNameMap { .index = 0, .names = { NameAssoc { .index = 0, .name = "TP" } } } });
        CHECK(indirectNames(allNames, NameKind::TagParameter)
              == std::vector {
                  IndirectNameMap { .index = 0, .names = { NameAssoc { .index = 0, .name = "TgP" } } } });
        CHECK(indirectNames(allNames, NameKind::Local)
              == std::vector {
                  IndirectNameMap { .index = 0, .names = { NameAssoc { .index = 0, .name = "IP" } } },
                  IndirectNameMap { .index = 1,
                                    .names = { NameAssoc { .index = 0, .name = "P" },
                                               NameAssoc { .index = 2, .name = "L" },
                                               NameAssoc { .index = 3, .name = "l2" } } } });
        CHECK(indirectNames(allNames, NameKind::Label)
              == std::vector { IndirectNameMap { .index = 1,
                                                 .names = { NameAssoc { .index = 0, .name = "B" },
                                                            NameAssoc { .index = 1, .name = "B2" },
                                                            NameAssoc { .index = 2, .name = "B3" } } } });
    }

    SECTION("module names, as name_annot.wast writes them")
    {
        CHECK(namesOf(readModule(R"wat((module (@name "Modül")))wat")).module == "Modül");
        CHECK(namesOf(readModule(R"wat((module $moduel (@name "Modül")))wat")).module == "Modül");
        CHECK(namesOf(readModule(R"wat(((@name "H") module (func)))wat")).module == "H");
        // A bare module's fields are the whole text: a name before every field names it.
        CHECK(namesOf(readModule(R"wat((@name "B") (func))wat")).module == "B");
        CHECK(readError(R"wat((func) (@name "B"))wat").code == TextErrorCode::MisplacedName);
    }
}

TEST_CASE("Annotations.branchHintOffsetMatchesEncoding")
{
    // Sibling placement, as branch_hint.wast and wasm-tools print write it: a hint before the folded if,
    // and one before the folded br_if in its then part. Each names the instruction after it in source
    // order: the if's position is its `(`, before its condition.
    auto const source = R"wat((module (func (param i32)
        (@metadata.code.branch_hint "\01")
        (if (local.get 0) (then (@metadata.code.branch_hint "\00") (br_if 0 (local.get 0)))))))wat";
    auto const expected = std::vector<BranchHint> {
        { .function = 0, .instruction = 1, .likelihood = BranchLikelihood::Likely },
        { .function = 0, .instruction = 3, .likelihood = BranchLikelihood::Unlikely },
    };
    auto const sameHints = [](std::vector<BranchHint> const& hints, std::vector<BranchHint> const& wanted) {
        return std::ranges::equal(hints, wanted, [](BranchHint const& a, BranchHint const& b) {
            return a.function == b.function && a.instruction == b.instruction && a.likelihood == b.likelihood;
        });
    };

    auto const read = readModule(source);
    CHECK(sameHints(read.branchHints, expected));
    REQUIRE(read.code.size() == 1);
    CHECK(read.code[0].body[1].opcode == Opcode::If);
    CHECK(read.code[0].body[3].opcode == Opcode::BrIf);

    // Decoding the encoding finds the hints at the if and the br_if: the offsets are theirs.
    auto const decoded = decodedModule(source);
    CHECK(decoded.customErrors.empty());
    CHECK(sameHints(decoded.branchHints, expected));

    SECTION("flat instructions, and the function index space")
    {
        auto const flat = decodedModule(R"wat((module (import "m" "f" (func)) (func (param i32)
            local.get 0 (@metadata.code.branch_hint "\00") if return end
            local.get 0 (@metadata.code.branch_hint "\01") br_if 0)))wat");
        CHECK(flat.customErrors.empty());
        CHECK(sameHints(flat.branchHints,
                        std::vector<BranchHint> {
                            { .function = 1, .instruction = 1, .likelihood = BranchLikelihood::Unlikely },
                            { .function = 1, .instruction = 5, .likelihood = BranchLikelihood::Likely },
                        }));
    }

    SECTION("a hint before the func keyword is in the function")
    {
        auto const before = readModule(R"wat(((@metadata.code.branch_hint "\01") func (param i32)
            (br_if 0 (local.get 0))))wat");
        CHECK(sameHints(before.branchHints,
                        std::vector<BranchHint> {
                            { .function = 0, .instruction = 1, .likelihood = BranchLikelihood::Likely },
                        }));
    }

    SECTION("a hint before an end or an else names the instruction after it")
    {
        auto const after = readModule(R"wat((func (param i32)
            (block (@metadata.code.branch_hint "\01")) (br_if 0 (local.get 0))))wat");
        CHECK(sameHints(after.branchHints,
                        std::vector<BranchHint> {
                            { .function = 0, .instruction = 3, .likelihood = BranchLikelihood::Likely },
                        }));
    }
}

TEST_CASE("Annotations.hintOnNonBranchIsInvalidTarget")
{
    auto const decoded = decodedModule(R"wat((module (func (param i32) (result i32)
        (@metadata.code.branch_hint "\01") (i32.eq (local.get 0) (local.get 0)))))wat");
    REQUIRE(decoded.customErrors.size() == 1);
    CHECK(decoded.customErrors[0].code == CustomErrorCode::BranchHintInvalidTarget);
    CHECK(isoline::binary::specMessage(decoded.customErrors[0].code)
          == "@metadata.code.branch_hint annotation: invalid target");
    CHECK(decoded.branchHints.empty());
}

TEST_CASE("Annotations.branchHintErrors")
{
    static constexpr auto Refusals = std::to_array<Refusal>({
        { R"wat((func (param i32) (br_if 0 (local.get 0)) (@metadata.code.branch_hint "\01")))wat",
          TextErrorCode::BranchHintInvalidPlacement },
        { R"wat((func (@metadata.code.branch_hint "\01")))wat", TextErrorCode::BranchHintInvalidPlacement },
        { R"wat((func (@metadata.code.branch_hint "\02") nop))wat", TextErrorCode::BranchHintInvalidValue },
        { R"wat((func (@metadata.code.branch_hint) nop))wat", TextErrorCode::BranchHintInvalidValue },
        // The contents are concatenated, as the reference's handler_branch_hint.ml does: "\01x" is no hint.
        { R"wat((func (@metadata.code.branch_hint "\01" "x") nop))wat",
          TextErrorCode::BranchHintInvalidValue },
        { R"wat((func (@metadata.code.branch_hint "\01" x) nop))wat",
          TextErrorCode::BranchHintUnexpectedToken },
        { R"wat((func (@metadata.code.branch_hint (x)) nop))wat", TextErrorCode::BranchHintUnexpectedToken },
        { R"wat((func (@metadata.code.branch_hint "\01") (@metadata.code.branch_hint "\01") nop))wat",
          TextErrorCode::BranchHintDuplicate },
        { R"wat((module (@metadata.code.branch_hint "\01") (func nop)))wat",
          TextErrorCode::BranchHintNotInFunction },
        { R"wat((global i32 (@metadata.code.branch_hint "\01") (i32.const 0)))wat",
          TextErrorCode::BranchHintNotInFunction },
        { R"wat((func (import "m" "f") (@metadata.code.branch_hint "\01")))wat",
          TextErrorCode::BranchHintNotInFunction },
        // An unexpected token comes before the placement, as the reference reads the payload first.
        { R"wat((module (@metadata.code.branch_hint x)))wat", TextErrorCode::BranchHintUnexpectedToken },
    });
    checkRefusals(Refusals, TextErrorStage::Custom);

    SECTION("an empty string and the hint byte together are the hint")
    {
        CHECK(readModule(R"wat((func (@metadata.code.branch_hint "" "\00") nop))wat").branchHints.size()
              == 1);
    }
}

TEST_CASE("Annotations.unknownIgnoredEverywhere")
{
    // Every token of a small module, separated by spaces; (@x) is put between every two of them.
    auto const tokens = std::string_view {
        R"wat(( module $m ( type $t ( func ( param i32 ) ) ) ( import "a" "b" ( func $i ( type $t ) ) ) )wat"
        R"wat(( func $f ( export "f" ) ( param $p i32 ) ( result i32 ) ( local i64 ) )wat"
        R"wat(( block $b ( result i32 ) local.get $p br_if $b ( i32.const 1 ) ) ) )wat"
        R"wat(( memory 1 ) ( global $g ( mut i32 ) ( i32.const 0 ) ) ( data ( i32.const 0 ) "x" ) ))wat"
    };
    auto annotated = std::string {};
    for (auto const token: tokens | std::views::split(' '))
    {
        annotated += std::string_view { token };
        annotated += " (@x) ";
    }
    CHECK(compiled(annotated) == compiled(tokens));

    SECTION("annotations nest arbitrarily deep, and are skipped without recursion")
    {
        auto limits = TextLimits {};
        limits.maxNesting = 200'000;
        constexpr auto Depth = std::size_t { 100'000 };
        auto deep = std::string { "(module (func " };
        for ([[maybe_unused]] auto const level: std::views::iota(std::size_t { 0 }, Depth))
            deep += "(@x ";
        deep += std::string(Depth, ')');
        deep += " nop))";
        CHECK(compiled(deep, limits) == compiled("(module (func nop))"));
    }

    SECTION("known annotations are what the tree tells apart")
    {
        auto const tree = parseSExpressions(
            R"wat(((@custom) (@name) (@metadata.code.branch_hint) (@"name") (@x)))wat", TextLimits {});
        REQUIRE(tree.has_value());
        auto kinds = std::vector<std::optional<AnnotationKind>> {};
        for (auto const child: tree->children(tree->firstTopLevel()))
            kinds.push_back(tree->annotationKindOf(child));
        CHECK(kinds
              == std::vector<std::optional<AnnotationKind>> { AnnotationKind::Custom,
                                                              AnnotationKind::Name,
                                                              AnnotationKind::BranchHint,
                                                              AnnotationKind::Name,
                                                              std::nullopt });
    }
}
