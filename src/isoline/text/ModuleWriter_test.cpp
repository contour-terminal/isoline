// SPDX-License-Identifier: Apache-2.0
#include <core/Sha256.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/binary/BranchHints.hpp>
#include <isoline/binary/Decoder.hpp>
#include <isoline/binary/Encoder.hpp>
#include <isoline/binary/Module.hpp>
#include <isoline/binary/NameSection.hpp>
#include <isoline/binary/Opcode.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/binary/testing/OpcodeRowModules.hpp>
#include <isoline/text/Lexer.hpp>
#include <isoline/text/ModuleWriter.hpp>
#include <isoline/text/SExpression.hpp>
#include <isoline/text/TextError.hpp>
#include <isoline/text/TextLimits.hpp>
#include <isoline/text/TopLevel.hpp>

using isoline::FeatureSet;
using isoline::binary::BranchHintSectionName;
using isoline::binary::canonicalize;
using isoline::binary::CustomSection;
using isoline::binary::DataSegment;
using isoline::binary::decodeModule;
using isoline::binary::decodeNameSection;
using isoline::binary::encodeModule;
using isoline::binary::encodeNameSection;
using isoline::binary::Function;
using isoline::binary::FuncType;
using isoline::binary::I32;
using isoline::binary::Instruction;
using isoline::binary::LocalRun;
using isoline::binary::Module;
using isoline::binary::NameAssoc;
using isoline::binary::NameKind;
using isoline::binary::NameSection;
using isoline::binary::NameSectionName;
using isoline::binary::Opcode;
using isoline::binary::SectionAnchor;
using isoline::binary::SegmentMode;
using isoline::binary::testing::ModuleBuilder;
using isoline::binary::testing::opcodeRowModules;
using isoline::text::compileText;
using isoline::text::CoreForms;
using isoline::text::ModuleWriter;
using isoline::text::NameSectionForm;
using isoline::text::TextLimits;
using isoline::text::WriteError;
using isoline::text::WriterOptions;

namespace
{

/// The features every module of these tests decodes under.
FeatureSet const& features()
{
    static auto const all = FeatureSet::all();
    return all;
}

/// The bytes of a file.
std::vector<std::byte> readFile(std::filesystem::path const& path)
{
    auto stream = std::ifstream { path, std::ios::binary };
    auto const text =
        std::string { std::istreambuf_iterator<char> { stream }, std::istreambuf_iterator<char> {} };
    auto bytes = std::vector<std::byte> {};
    bytes.reserve(text.size());
    std::ranges::transform(text, std::back_inserter(bytes), [](char byte) { return std::byte(byte); });
    return bytes;
}

/// The module @p bytes decode to, which they must.
Module decoded(std::span<std::byte const> bytes)
{
    auto module = decodeModule(bytes, features());
    REQUIRE(module.has_value());
    return std::move(*module);
}

/// The text the default writer writes @p module as, which it must.
std::string written(Module const& module)
{
    auto text = ModuleWriter { WriterOptions {} }.write(module);
    REQUIRE(text.has_value());
    return std::move(*text);
}

/// The binary @p text compiles to, which it must.
std::vector<std::byte> compiled(std::string const& text)
{
    auto bytes = compileText(text, TextLimits {}, CoreForms);
    if (!bytes)
        INFO(isoline::text::specMessage(bytes.error())
             << " at " << bytes.error().position.line << ':' << bytes.error().position.column);
    REQUIRE(bytes.has_value());
    return std::move(*bytes);
}

/// Checks that @p bytes, written as text and compiled back, encode to the canonical encoding of @p bytes
/// itself: the reader's output is compared as it is, so that it must already be canonical.
/// @param bytes A binary module, which must decode.
void checkCanonicalRoundTrip(std::span<std::byte const> bytes)
{
    auto const module = decoded(bytes);
    auto const text = written(module);
    auto const again = compiled(text);
    auto const expected = canonicalize(bytes, features());
    REQUIRE(expected.has_value());
    if (again != *expected)
        INFO(text);
    CHECK(again == *expected);
}

/// Checks that @p bytes, written as text and compiled back, mean the same module: both canonicalized.
/// For a branch hint section written raw, which the reader keeps verbatim where canonicalize rewrites
/// one the decoder read whole: the reader's output is then canonical only once canonicalized itself.
/// @param bytes A binary module, which must decode.
void checkCanonicalForms(std::span<std::byte const> bytes)
{
    auto const text = written(decoded(bytes));
    auto const expected = canonicalize(bytes, features());
    auto const actual = canonicalize(compiled(text), features());
    REQUIRE(expected.has_value());
    REQUIRE(actual.has_value());
    if (*expected != *actual)
        INFO(text);
    CHECK(*expected == *actual);
}

/// A defined function of type 0 and no locals, whose body is @p body.
Function functionOf(std::vector<Instruction> body)
{
    auto function = Function {};
    function.body = std::move(body);
    return function;
}

/// A module of one function of type [] -> [] and the body @p body, after which an end follows.
Module oneFunction(std::vector<Instruction> body)
{
    auto module = Module {};
    module.types.push_back(FuncType {});
    module.functions.push_back(0);
    body.push_back(Instruction { .opcode = Opcode::End });
    module.code.push_back(functionOf(std::move(body)));
    return module;
}

/// @p module with @p names appended as its name section, anchored after the data section.
Module named(Module module, NameSection const& names)
{
    module.customs.push_back(CustomSection { .name = std::string { NameSectionName },
                                             .bytes = encodeNameSection(names),
                                             .after = SectionAnchor::Data });
    return module;
}

/// A name section naming functions only.
NameSection functionNames(std::vector<NameAssoc> functions)
{
    auto names = NameSection {};
    names.direct[static_cast<std::size_t>(NameKind::Function)] = std::move(functions);
    return names;
}

/// The bytes of @p values.
std::vector<std::byte> bytesOf(std::initializer_list<uint8_t> values)
{
    auto bytes = std::vector<std::byte> {};
    std::ranges::transform(
        values, std::back_inserter(bytes), [](uint8_t value) { return std::byte { value }; });
    return bytes;
}

/// The name section of a binary module, decoded, which it must have.
NameSection nameSectionOf(std::span<std::byte const> bytes)
{
    auto const module = decoded(bytes);
    auto const section = std::ranges::find(module.customs, NameSectionName, &CustomSection::name);
    REQUIRE(section != module.customs.end());
    auto names = decodeNameSection(section->bytes);
    REQUIRE(names.has_value());
    return std::move(*names);
}

/// The modules `(module binary …)` gives in a script: every list headed `module` whose first atom
/// after an optional identifier (and `definition`) is the keyword `binary`, its strings concatenated.
std::vector<std::vector<std::byte>> binaryModulesOf(std::filesystem::path const& script)
{
    auto const bytes = readFile(script);
    auto const source = std::string { reinterpret_cast<char const*>(bytes.data()), bytes.size() };
    auto const tree = isoline::text::parseSExpressions(source, TextLimits {});
    REQUIRE(tree.has_value());
    auto modules = std::vector<std::vector<std::byte>> {};
    auto const& nodes = tree->nodes();
    auto const isKeyword = [&](isoline::text::NodeId node, std::string_view keyword) {
        return node != isoline::text::NoNode && nodes[node].kind == isoline::text::NodeKind::Atom
               && nodes[node].tokenKind == isoline::text::TokenKind::Keyword && tree->textOf(node) == keyword;
    };
    for (auto const list:
         std::views::iota(isoline::text::NodeId { 0 }, static_cast<isoline::text::NodeId>(nodes.size())))
    {
        if (nodes[list].kind != isoline::text::NodeKind::List || !isKeyword(nodes[list].firstChild, "module"))
            continue;
        auto child = nodes[nodes[list].firstChild].nextSibling;
        while (child != isoline::text::NoNode && nodes[child].kind == isoline::text::NodeKind::Atom
               && (nodes[child].tokenKind == isoline::text::TokenKind::Identifier
                   || isKeyword(child, "definition")))
            child = nodes[child].nextSibling;
        if (!isKeyword(child, "binary"))
            continue;
        auto module = std::vector<std::byte> {};
        for (auto const part: tree->siblingsAfter(child))
        {
            if (nodes[part].tokenKind != isoline::text::TokenKind::String)
                continue;
            auto const string = isoline::text::decodeString(source, tree->tokenOf(part));
            REQUIRE(string.has_value());
            std::ranges::transform(
                *string, std::back_inserter(module), [](char byte) { return std::byte(byte); });
        }
        modules.push_back(std::move(module));
    }
    return modules;
}

/// Whether a decoded module holds what normalize() canonicalizes away and the text cannot say: a data
/// count section no function body needs, or a local run of no locals.
struct Normalizations
{
    std::size_t redundantDataCounts = 0; ///< Modules with a data count no body needs (binary.55).
    std::size_t emptyLocalRuns = 0;      ///< Modules with a local run of no locals (binary.45).
};

void countNormalizations(Module const& module, Normalizations& seen)
{
    if (module.dataCount && !isoline::binary::codeNamesDataSegment(module))
        ++seen.redundantDataCounts;
    auto const hasEmptyRun = std::ranges::any_of(module.code, [](Function const& function) {
        return std::ranges::any_of(function.locals, [](LocalRun const& run) { return run.count == 0; });
    });
    if (hasEmptyRun)
        ++seen.emptyLocalRuns;
}

} // namespace

TEST_CASE("ModuleWriter.roundTripsSpecCorpus")
{
    // Every module of the spec corpus that decodes, its oracle binaries and the modules its scripts give
    // in binary form (binary.wast's, binary.45 and binary.55 among them), written and compiled back,
    // means the same module: canonicalize(x) == canonicalize(compileText(write(decode(x)))).
    auto const corpus = std::filesystem::path { ISOLINE_SOURCE_DIR } / "tests" / "spec";
    auto checked = std::size_t { 0 };
    auto seen = Normalizations {};
    auto const check = [&](std::span<std::byte const> bytes) {
        auto const module = decodeModule(bytes, features());
        if (!module)
            return;
        ++checked;
        countNormalizations(*module, seen);
        checkCanonicalRoundTrip(bytes);
    };
    for (auto const& entry: std::filesystem::recursive_directory_iterator { corpus })
    {
        if (!entry.is_regular_file())
            continue;
        INFO(entry.path().string());
        if (entry.path().extension() == ".wasm")
            check(readFile(entry.path()));
        else if (entry.path().extension() == ".wast")
            for (auto const& bytes: binaryModulesOf(entry.path()))
                check(bytes);
    }
    CHECK(checked > 4000);
    // binary.55's redundant data count and binary.45's zero-count runs are among them: normalize drops
    // what the text cannot say, so the canonical forms agree.
    CHECK(seen.redundantDataCounts > 0);
    CHECK(seen.emptyLocalRuns > 0);
}

TEST_CASE("ModuleWriter.roundTripsEveryOpcodeRow")
{
    for (auto const& bytes: opcodeRowModules())
    {
        auto const module = decoded(bytes);
        INFO(isoline::binary::opcodeInfo(module.code.at(1).body.at(0).opcode).text);
        checkCanonicalRoundTrip(bytes);
    }
}

TEST_CASE("ModuleWriter.indentationIsCapped")
{
    // 100'000 nested blocks: indented a level each, the text would hold some 10^10 bytes of spaces.
    constexpr auto Depth = std::size_t { 100'000 };
    auto body = std::vector<uint8_t> {};
    for ([[maybe_unused]] auto const level: std::views::iota(std::size_t { 0 }, Depth))
        body.insert(body.end(), { 0x02, 0x40 });
    body.insert(body.end(), Depth, 0x0B);
    auto builder = ModuleBuilder {};
    builder.type(FuncType {}).function(0, {}, std::move(body));
    auto const bytes = builder.build();
    auto const module = decoded(bytes);
    auto const options = WriterOptions {};
    auto const text = ModuleWriter { options }.write(module);
    REQUIRE(text.has_value());
    // Each line is at most the capped indentation, its instruction (`block`, `end`) and a newline.
    auto const instructions = module.code.front().body.size();
    auto const perInstruction = (std::size_t { options.indentWidth } * options.maxIndentLevel) + 16;
    CAPTURE(text->size(), instructions);
    CHECK(text->size() < perInstruction * instructions);
    CHECK(compiled(*text) == bytes);
}

TEST_CASE("ModuleWriter.awkwardNamesRoundTrip")
{
    // Two functions named "f", a name with a space, the empty name and one with a quote: none is a plain
    // identifier but the first, so the text holds `$f`, `$"a b"`, `$"q\""` and `(@name …)` for the others.
    auto module = Module {};
    module.types.push_back(FuncType {});
    for ([[maybe_unused]] auto const index: std::views::iota(0, 5))
    {
        module.functions.push_back(0);
        module.code.push_back(functionOf({ Instruction { .opcode = Opcode::End } }));
    }
    auto const names = functionNames({
        NameAssoc { .index = 0, .name = "f" },
        NameAssoc { .index = 1, .name = "f" },
        NameAssoc { .index = 2, .name = "a b" },
        NameAssoc { .index = 3, .name = "" },
        NameAssoc { .index = 4, .name = "q\"" },
    });
    auto const text = written(named(module, names));
    INFO(text);
    CHECK(text.contains("(func $f "));
    CHECK(text.contains("(func (@name \"f\") "));
    CHECK(text.contains("(func $\"a b\" "));
    CHECK(text.contains("(func (@name \"\") "));
    CHECK(text.contains("(func $\"q\\\"\" "));
    CHECK_FALSE(text.contains("(@custom"));
    CHECK(nameSectionOf(compiled(text)) == names);
}

TEST_CASE("ModuleWriter.namesEveryKind")
{
    // Every subsection the reader re-synthesizes, from identifiers: the module, types and their
    // parameters, imported and defined functions with their parameters, locals and labels, tables,
    // memories, globals, tags and their parameters, element and data segments.
    auto const source =
        std::string { "(module $m"
                      " (type $t (func (param $p i32) (result i32)))"
                      " (import \"m\" \"f\" (func $imported (type $t) (param $x i32) (result i32)))"
                      " (import \"m\" \"g\" (global $ig i32))"
                      " (table $tab 1 funcref)"
                      " (memory $mem 1)"
                      " (global $g (mut i32) (i32.const 0))"
                      " (tag $e (param $payload i32))"
                      " (func $defined (type $t) (param $a i32) (result i32) (local $l i64) (local i32)"
                      "   block $outer (result i32) local.get $a end)"
                      " (elem $seg func $defined)"
                      " (data $d \"\"))" };
    auto const bytes = compiled(source);
    auto const text = written(decoded(bytes));
    INFO(text);
    CHECK_FALSE(text.contains("(@custom"));
    CHECK(text.contains("(module $m"));
    CHECK(text.contains("block $outer (result i32)"));
    CHECK(compiled(text) == bytes);
}

TEST_CASE("ModuleWriter.annotationsRoundTrip")
{
    // Custom sections anchored after the function and the data section, and branch hints on an if and
    // a br_if: written as `@custom` and branch hint annotations, they compile back to the same bytes.
    auto const source = std::string { "(module"
                                      " (@custom \"after-func\" (after func) \"a\\00b\")"
                                      " (func (param i32)"
                                      "   local.get 0"
                                      "   (@metadata.code.branch_hint \"\\01\") if"
                                      "     local.get 0 (@metadata.code.branch_hint \"\\00\") br_if 0"
                                      "   end)"
                                      " (memory 1)"
                                      " (data (i32.const 0) \"x\")"
                                      " (@custom \"after-data\" \"payload\"))" };
    auto const bytes = compiled(source);
    auto const module = decoded(bytes);
    REQUIRE(module.branchHints.size() == 2);
    auto const text = written(module);
    INFO(text);
    CHECK(text.contains("(@custom \"after-func\" (after func) \"a\\00b\")"));
    CHECK(text.contains("(@custom \"after-data\" (after data) \"payload\")"));
    CHECK(text.contains("(@metadata.code.branch_hint \"\\01\") if"));
    CHECK(text.contains("(@metadata.code.branch_hint \"\\00\") br_if 0"));
    CHECK_FALSE(text.contains("(@custom \"metadata.code.branch_hint\""));
    CHECK(compiled(text) == bytes);
}

TEST_CASE("ModuleWriter.nameBeforeProducersStaysRaw")
{
    // wasm-ld and rustc put the name section before the producers section, both after the data
    // section. Identifiers would re-synthesize it after producers, so it is written raw, and no
    // identifier is printed at all.
    auto module = oneFunction({});
    auto segment = DataSegment {};
    segment.bytes = bytesOf({ 0x2A });
    module.data.push_back(std::move(segment));
    module = named(std::move(module), functionNames({ NameAssoc { .index = 0, .name = "f" } }));
    module.customs.push_back(
        CustomSection { .name = "producers", .bytes = bytesOf({ 0x00 }), .after = SectionAnchor::Data });
    auto const bytes = encodeModule(module);
    auto const text = written(decoded(bytes));
    INFO(text);
    CHECK(text.contains("(@custom \"name\" (after data) \""));
    CHECK(text.find("(@custom \"name\"") < text.find("(@custom \"producers\""));
    CHECK_FALSE(text.contains("$"));
    checkCanonicalRoundTrip(bytes);
    CHECK(compiled(text) == bytes);
}

TEST_CASE("ModuleWriter.nameSectionFormCustomStaysRaw")
{
    // A name section identifiers would re-synthesize is still written raw when asked, as component
    // text asks for a core module that names itself.
    auto module = named(oneFunction({}), functionNames({ NameAssoc { .index = 0, .name = "f" } }));
    auto const bytes = encodeModule(module);
    CHECK(written(decoded(bytes)).contains("$f"));
    auto const text =
        ModuleWriter { WriterOptions { .nameSection = NameSectionForm::Custom } }.write(decoded(bytes));
    REQUIRE(text.has_value());
    INFO(*text);
    CHECK(text->contains("(@custom \"name\""));
    CHECK_FALSE(text->contains("$"));
    CHECK(compiled(*text) == bytes);
}

TEST_CASE("ModuleWriter.unsynthesizableSectionsStayRaw")
{
    auto const checkRaw = [](Module const& module, std::string_view section) {
        auto const bytes = encodeModule(module);
        auto const text = written(decoded(bytes));
        INFO(text);
        CHECK(text.contains("(@custom \"" + std::string { section } + "\""));
        CHECK_FALSE(text.contains("$"));
        CHECK_FALSE(text.contains("(@name"));
        CHECK_FALSE(text.contains("(@metadata.code.branch_hint"));
        if (section == BranchHintSectionName)
            checkCanonicalForms(bytes);
        else
            checkCanonicalRoundTrip(bytes);
    };
    SECTION("a name for a function the module does not have")
    {
        checkRaw(named(oneFunction({}),
                       functionNames(
                           { NameAssoc { .index = 0, .name = "f" }, NameAssoc { .index = 5, .name = "g" } })),
                 NameSectionName);
    }
    SECTION("a local name beyond the function's locals")
    {
        auto names = NameSection {};
        names.indirect[static_cast<std::size_t>(NameKind::Local)].push_back(
            { .index = 0, .names = { NameAssoc { .index = 3, .name = "x" } } });
        checkRaw(named(oneFunction({}), names), NameSectionName);
    }
    SECTION("an empty name section")
    {
        auto module = oneFunction({});
        module.customs.push_back(CustomSection {
            .name = std::string { NameSectionName }, .bytes = {}, .after = SectionAnchor::Data });
        checkRaw(module, NameSectionName);
    }
    // An if on local 0 of a function of one i32 param: the hint section's function 0 at offset 3, the
    // if after the locals declaration (one byte) and its local.get (two).
    auto hinted = Module {};
    hinted.types.push_back(FuncType { .params = { I32 }, .results = {} });
    hinted.functions.push_back(0);
    hinted.code.push_back(functionOf({ Instruction { .opcode = Opcode::LocalGet },
                                       Instruction { .opcode = Opcode::If },
                                       Instruction { .opcode = Opcode::End },
                                       Instruction { .opcode = Opcode::End } }));
    SECTION("a hint section after the code section")
    {
        auto module = hinted;
        module.customs.push_back(CustomSection { .name = std::string { BranchHintSectionName },
                                                 .bytes = bytesOf({ 0x01, 0x00, 0x01, 0x03, 0x01, 0x01 }),
                                                 .after = SectionAnchor::Code });
        REQUIRE(decoded(encodeModule(module)).branchHints.size() == 1);
        checkRaw(module, BranchHintSectionName);
    }
    SECTION("a hint section with a LEB128 longer than it needs")
    {
        auto module = hinted;
        module.customs.push_back(
            CustomSection { .name = std::string { BranchHintSectionName },
                            .bytes = bytesOf({ 0x01, 0x80, 0x00, 0x01, 0x03, 0x01, 0x01 }),
                            .after = SectionAnchor::Function });
        REQUIRE(decoded(encodeModule(module)).branchHints.size() == 1);
        checkRaw(module, BranchHintSectionName);
    }
    SECTION("the same hint section, minimal and in place, is written as an annotation")
    {
        auto module = hinted;
        module.customs.push_back(CustomSection { .name = std::string { BranchHintSectionName },
                                                 .bytes = bytesOf({ 0x01, 0x00, 0x01, 0x03, 0x01, 0x01 }),
                                                 .after = SectionAnchor::Function });
        auto const bytes = encodeModule(module);
        auto const text = written(decoded(bytes));
        INFO(text);
        CHECK(text.contains("(@metadata.code.branch_hint \"\\01\") if"));
        CHECK(compiled(text) == bytes);
    }
}

TEST_CASE("ModuleWriter.invalidNameSectionIsRaw")
{
    // Function names out of index order: the section does not decode, so it is written byte for byte.
    auto module = oneFunction({});
    module.functions.push_back(0);
    module.code.push_back(module.code.front());
    module.data.push_back(DataSegment {});
    // Subsection 1, 7 bytes: two entries, index 1 "b", then index 0 "a".
    module.customs.push_back(
        CustomSection { .name = std::string { NameSectionName },
                        .bytes = bytesOf({ 0x01, 0x07, 0x02, 0x01, 0x01, 'b', 0x00, 0x01, 'a' }),
                        .after = SectionAnchor::Data });
    auto const bytes = encodeModule(module);
    REQUIRE_FALSE(decodeNameSection(decoded(bytes).customs.front().bytes).has_value());
    auto const text = written(decoded(bytes));
    INFO(text);
    CHECK(text.contains("(@custom \"name\" (after data) \"\\01\\07\\02\\01\\01b\\00\\01a\")"));
    CHECK(compiled(text) == bytes);
}

TEST_CASE("ModuleWriter.shadowedLabelsAreNumeric")
{
    // Both blocks are named $l: `br $l` would name the inner one, so the branch to the outer is `br 1`.
    auto const bytes = compiled("(module (func block $l block $l br 1 end end))");
    auto const text = written(decoded(bytes));
    INFO(text);
    CHECK(text.contains("block $l\n"));
    CHECK(text.contains("br 1\n"));
    CHECK(compiled(text) == bytes);
}

TEST_CASE("ModuleWriter.constantsAreExact")
{
    // Floats as exact hex floats, NaN payloads and signs kept, integers signed, vectors as i32x4 lanes.
    auto const bytes =
        compiled("(module (func"
                 " f32.const 3 drop f32.const -0 drop f32.const 0x1p-149 drop"
                 " f32.const -nan:0x200001 drop f32.const inf drop f32.const nan drop"
                 " f64.const 0x1.123456789abcdp+1023 drop f64.const 0x0.0000000000001p-1022 drop"
                 " i32.const -1 drop i64.const 0x8000000000000000 drop"
                 " v128.const i8x16 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 255 drop))");
    auto const text = written(decoded(bytes));
    INFO(text);
    CHECK(text.contains("f32.const 0x1.8p+1\n"));
    CHECK(text.contains("f32.const -0x0p+0\n"));
    CHECK(text.contains("f32.const 0x0.000002p-126\n"));
    CHECK(text.contains("f32.const -nan:0x200001\n"));
    CHECK(text.contains("f32.const inf\n"));
    CHECK(text.contains("f32.const nan:0x400000\n"));
    CHECK(text.contains("f64.const 0x1.123456789abcdp+1023\n"));
    CHECK(text.contains("f64.const 0x0.0000000000001p-1022\n"));
    CHECK(text.contains("i32.const -1\n"));
    CHECK(text.contains("i64.const -9223372036854775808\n"));
    CHECK(text.contains("v128.const i32x4 0x4030201 0x8070605 0xc0b0a09 0xff0f0e0d\n"));
    CHECK(compiled(text) == bytes);
}

TEST_CASE("ModuleWriter.indentLevel")
{
    // The form starts at the level asked for, its fields one deeper: how a component nests a module.
    auto const module = oneFunction({ Instruction { .opcode = Opcode::Nop } });
    auto const text = ModuleWriter { WriterOptions { .indentWidth = 4 } }.write(module, 1);
    REQUIRE(text.has_value());
    INFO(*text);
    CHECK(text->starts_with("    (module\n        (type (func))\n"));
    CHECK(text->contains("\n            nop\n"));
    CHECK(text->ends_with("\n    )\n"));
    CHECK(compiled(*text) == encodeModule(module));
}

TEST_CASE("ModuleWriter.outputIsCapped")
{
    // One run of 50'000 locals is a few bytes of binary and 200 KB of text: within a 64 KiB limit it is
    // refused, having held about the limit.
    auto module = oneFunction({});
    module.code.front().locals.push_back(LocalRun { .count = 50'000, .type = I32 });
    module.code.front().localCount = 50'000;
    auto const limited = WriterOptions { .maxTextBytes = std::size_t { 64 } * 1024 };
    auto const refused = ModuleWriter { limited }.write(module);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == WriteError::TextTooLarge);
    auto const text = ModuleWriter { WriterOptions {} }.write(module);
    REQUIRE(text.has_value());
    CHECK(text->size() > limited.maxTextBytes);
    CHECK(compiled(*text) == encodeModule(module));
}

TEST_CASE("ModuleWriter.golden")
{
    // golden-source.wat, encoded by the pinned wasm-tools (golden.provenance), written exactly as the
    // committed golden.wat, which was reviewed by eye.
    auto const directory = std::filesystem::path { ISOLINE_SOURCE_DIR } / "tests" / "text";
    auto const bytes = readFile(directory / "golden.wasm");
    auto const provenance = readFile(directory / "golden.provenance");
    auto digest = std::string {};
    for (auto const byte: core::Sha256::hash(bytes))
        digest += std::format("{:02x}", static_cast<unsigned>(byte));
    auto const recorded =
        std::string_view { reinterpret_cast<char const*>(provenance.data()), provenance.size() };
    CHECK(recorded.contains("golden.wasm.sha256 = " + digest + "\n"));
    auto const expected = readFile(directory / "golden.wat");
    auto const text = written(decoded(bytes));
    CHECK(text == std::string { reinterpret_cast<char const*>(expected.data()), expected.size() });
    auto const canonical = canonicalize(bytes, features());
    REQUIRE(canonical.has_value());
    CHECK(compiled(text) == *canonical);
}

TEST_CASE("ModuleWriter.pageSizeNotWritable")
{
    // A page size of 2^32 bytes decodes (the decoder admits exponents below 64; only validation refuses
    // it), but the text format's `pagesize` is a u32: the writer refuses it rather than write text that
    // does not read back.
    auto module = oneFunction({});
    module.memories.push_back(isoline::binary::MemoryType { .limits = {}, .pageSizeLog2 = 32 });
    auto const text = ModuleWriter { WriterOptions {} }.write(decoded(encodeModule(module)));
    REQUIRE_FALSE(text.has_value());
    CHECK(text.error() == WriteError::PageSizeNotWritable);
    // 2^31, the largest a u32 holds, is written.
    module.memories.front().pageSizeLog2 = 31;
    auto const largest = encodeModule(module);
    auto const written31 = written(decoded(largest));
    CHECK(written31.contains("(memory 0 (pagesize 2147483648))"));
    CHECK(compiled(written31) == largest);
}

TEST_CASE("ModuleWriter.namesAcrossScopes")
{
    auto const local = [](uint32_t function, std::vector<NameAssoc> names) {
        return isoline::binary::IndirectNameMap { .index = function, .names = std::move(names) };
    };
    SECTION("an empty module name")
    {
        auto names = NameSection {};
        names.module = "";
        auto const text = written(named(oneFunction({}), names));
        INFO(text);
        CHECK(text.starts_with("(module (@name \"\")\n"));
        CHECK(nameSectionOf(compiled(text)) == names);
    }
    SECTION("duplicate local names: a parameter and a local, two locals")
    {
        auto module = oneFunction({});
        module.types.front().params = { I32 };
        module.code.front().locals.push_back(LocalRun { .count = 2, .type = I32 });
        module.code.front().localCount = 2;
        auto names = NameSection {};
        names.indirect[static_cast<std::size_t>(NameKind::Local)].push_back(
            local(0,
                  { NameAssoc { .index = 0, .name = "x" },
                    NameAssoc { .index = 1, .name = "x" },
                    NameAssoc { .index = 2, .name = "x" } }));
        auto const text = written(named(std::move(module), names));
        INFO(text);
        CHECK(text.contains("(param $x i32)"));
        CHECK(text.contains("(local (@name \"x\") i32)\n    (local (@name \"x\") i32)"));
        CHECK(nameSectionOf(compiled(text)) == names);
    }
    SECTION("an imported and a defined function of one name")
    {
        auto module = oneFunction({});
        module.importModules.emplace_back("m");
        auto import = isoline::binary::Import {};
        import.name = "f";
        module.imports.push_back(std::move(import));
        module.functions.push_back(0);
        module.code.push_back(module.code.front());
        auto const names =
            functionNames({ NameAssoc { .index = 0, .name = "f" }, NameAssoc { .index = 1, .name = "f" } });
        auto const text = written(named(std::move(module), names));
        INFO(text);
        CHECK(text.contains("(import \"m\" \"f\" (func $f (type 0)))"));
        CHECK(text.contains("(func (@name \"f\") (type 0)"));
        CHECK(nameSectionOf(compiled(text)) == names);
    }
}

TEST_CASE("ModuleWriter.everyLimitBelowTheTextRefuses")
{
    // A module of two of everything the writer loops over (types, a signature of named parameters,
    // imports, tags, globals, bodies of nested blocks, element segments of indices and of expressions,
    // data, custom sections): under every limit below the size of its text, writing is refused, wherever
    // in the text the limit falls, an indentation included; at its size, it is written whole.
    constexpr auto Source = std::string_view { R"wat((module
  (type (func (param i32 i64) (result i32)))
  (type (func (param f32 f64)))
  (import "m" "f" (func (type 1)))
  (import "m" "g" (func (type 1)))
  (tag (param i32))
  (tag (param i64))
  (global i32 (i32.const 1))
  (global i64 (i64.const 2))
  (table 4 funcref)
  (memory 1)
  (func $f (param $a i32) (param $b i64) (result i32) (block (block (nop) (nop))) (local.get $a))
  (func $g (param $c i32) (param $d i64) (result i32) (block (block (nop) (nop))) (local.get $c))
  (elem (i32.const 0) func $f $g)
  (elem (i32.const 2) funcref (ref.func $f) (ref.func $g))
  (data (i32.const 0) "ab")
  (data (i32.const 2) "cd")
  (@custom "a" (after data) "x")
  (@custom "b" (after data) "y")))wat" };
    auto const module = decoded(compiled(std::string { Source }));
    auto const text = written(module);
    for (auto const limit: std::views::iota(std::size_t { 0 }, text.size()))
    {
        auto const refused = ModuleWriter { WriterOptions { .maxTextBytes = limit } }.write(module);
        INFO(limit);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error() == WriteError::TextTooLarge);
    }
    CHECK(ModuleWriter { WriterOptions { .maxTextBytes = text.size() } }.write(module) == text);
}

TEST_CASE("ModuleWriter.unnameableEntitiesKeepTheNameSectionRaw")
{
    // Names of entities the reader cannot name, which it could not re-synthesize: the section is written
    // raw, and no identifier.
    auto const indirect = [](NameKind kind, uint32_t outer) {
        auto names = NameSection {};
        names.indirect[static_cast<std::size_t>(kind)].push_back(isoline::binary::IndirectNameMap {
            .index = outer, .names = { NameAssoc { .index = 0, .name = "x" } } });
        return names;
    };
    auto const withImportedFunction = [] {
        auto module = oneFunction({});
        module.importModules.emplace_back("m");
        auto import = isoline::binary::Import {};
        import.name = "f";
        module.imports.push_back(std::move(import));
        return module;
    };
    auto withImportedTag = oneFunction({});
    withImportedTag.importModules.emplace_back("m");
    auto tag = isoline::binary::Import {};
    tag.name = "t";
    tag.kind = isoline::binary::ExternKind::Tag;
    withImportedTag.imports.push_back(std::move(tag));
    auto typeOutOfRange = oneFunction({});
    typeOutOfRange.functions.front() = 5;
    struct Case
    {
        std::string_view what; ///< What the name section names.
        Module module;         ///< The module, named.
    };
    auto const cases = std::to_array<Case>({
        { "a local of a function of no type", named(typeOutOfRange, indirect(NameKind::Local, 0)) },
        { "a local of no function", named(oneFunction({}), indirect(NameKind::Local, 3)) },
        { "a label of an imported function", named(withImportedFunction(), indirect(NameKind::Label, 0)) },
        { "a label of no function", named(oneFunction({}), indirect(NameKind::Label, 3)) },
        { "a parameter of no type", named(oneFunction({}), indirect(NameKind::Parameter, 3)) },
        { "a parameter of no tag", named(oneFunction({}), indirect(NameKind::TagParameter, 0)) },
        { "a parameter of an imported tag", named(withImportedTag, indirect(NameKind::TagParameter, 0)) },
    });
    for (auto const& entry: cases)
    {
        INFO(entry.what);
        auto const text = written(entry.module);
        INFO(text);
        CHECK(text.contains("(@custom \"name\""));
        CHECK_FALSE(text.contains('$'));
    }
}

TEST_CASE("ModuleWriter.hintsBesideSplitLocalRunsStayRaw")
{
    // A hinted function whose locals are two runs of one type: the text merges them, so the body would
    // encode at other offsets than the section names. The section is written raw, and means the same.
    auto module = decoded(compiled(R"wat((module (func (param i32) (local i32)
        (@metadata.code.branch_hint "\01") (if (local.get 0) (then)))))wat"));
    REQUIRE(module.branchHints.size() == 1);
    auto& function = module.code.front();
    function.locals = { LocalRun { .count = 1, .type = I32 }, LocalRun { .count = 1, .type = I32 } };
    function.localCount = 2;
    auto const text = written(module);
    INFO(text);
    CHECK(text.contains(std::format("(@custom \"{}\"", BranchHintSectionName)));
    CHECK_FALSE(text.contains("(@metadata.code.branch_hint"));
}

TEST_CASE("ModuleWriter.constantExpressionsKeepNoSideTables")
{
    // A constant expression keeps no catch clauses nor branch labels (binary::ConstExpr), which only an
    // invalid module holds there: a try_table is written without clauses, and a br_table with the one
    // label it needs to read as one.
    auto module = oneFunction({});
    module.globals.push_back(isoline::binary::Global { .type = isoline::binary::GlobalType { .type = I32 },
                                                       .init = { Instruction { .opcode = Opcode::TryTable },
                                                                 Instruction { .opcode = Opcode::BrTable },
                                                                 Instruction { .opcode = Opcode::End } } });
    auto const text = written(module);
    INFO(text);
    CHECK(text.contains("(global i32 try_table br_table 0)"));
}
