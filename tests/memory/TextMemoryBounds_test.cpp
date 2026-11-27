// SPDX-License-Identifier: Apache-2.0

// Reading text must never make the host run out of memory (spec §7, §9): what reading a source holds
// is bounded by the source's size, at most a few bytes per byte of it (the bounds below), and what it
// produces by TextLimits::maxModuleBytes. Each test below is a source shaped to amplify one thing the
// reader keeps per token, at a size where a cost per item that grows with another item's size is orders
// of magnitude over the bound.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <expected>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "AllocationCounter.hpp"
#include "AmplifyingModules.hpp"
#include <isoline/Features.hpp>
#include <isoline/api/Engine.hpp>
#include <isoline/binary/BranchHints.hpp>
#include <isoline/binary/Decoder.hpp>
#include <isoline/binary/Encoder.hpp>
#include <isoline/binary/Module.hpp>
#include <isoline/binary/Opcode.hpp>
#include <isoline/text/ModuleWriter.hpp>
#include <isoline/text/SExpression.hpp>
#include <isoline/text/TextError.hpp>
#include <isoline/text/TextLimits.hpp>
#include <isoline/text/TopLevel.hpp>

using isoline::testing::AllocationScope;
using isoline::text::compileText;
using isoline::text::ModuleWriter;
using isoline::text::parseSExpressions;
using isoline::text::TextError;
using isoline::text::TextLimits;
using isoline::text::WriteError;
using isoline::text::WriterOptions;

namespace
{

/// The bytes parsing a source into its tree may hold per byte of it.
///
/// The tree is the densest thing reading keeps: a 20-byte Node per token but `)`, and a token can be one
/// byte (`,`, `[`, `{`, …), so up to 20 bytes of nodes per source byte. parseSExpressions counts the
/// nodes first and allocates them once, so no vector growth adds to that on any standard library.
/// Measured under Clang/libstdc++ with ASan: the dense punctuation sources below hold exactly 20 bytes
/// per source byte, `(1)1` alternation 15, `()` and `(a a …)` 10. Lists nested to the default cap hold
/// 19.8, half of it the stack of open lists, which the cap bounds.
constexpr auto TreeBytesPerSourceByte = std::size_t { 20 };

/// The bytes compiling a module's text may hold per byte of it: the tree, the binary::Module read from
/// it, and its encoding; for all but a function's fixed cost (bytesPerEmptyFunction) and an encoding
/// that repeats a compact import group's module name, which the module limit bounds.
///
/// The reader reserves the module's functions, imports and each body exactly, so no vector growth adds
/// to that. Measured with Clang/libstdc++ under ASan, and under MSVC's debug runtime: a compact import
/// group of short names holds 15.2 and 15.6 bytes per source byte, the densest case; the implicit types
/// 12.0 and 14.7, v128 literals 10.9 and 10.8, catch clauses 9.2 and 8.7. manyFunctions holds its
/// source's share plus bytesPerEmptyFunction() per function, which is 208 bytes on the one and 368 on
/// the other.
constexpr auto ModuleBytesPerSourceByte = std::size_t { 20 };

/// How many times the module limit compiling may hold at once: the import section being written and
/// the module it is copied into, each a vector grown by doubling, so the bound is three copies. What the
/// cases below hold is a measurement, not the bound: with Clang/libstdc++ under ASan, 2.0 times the limit
/// for an output just within it and 1.8 for one refused at it; under MSVC's debug runtime 2.3 for the
/// output within it.
constexpr auto OutputCopies = std::size_t { 3 };

/// The bytes compiling a module of one dense function body may hold per byte of it, a separate bound
/// from ModuleBytesPerSourceByte: an Instruction is 40 bytes and its source offset 4, so a body
/// reserved by anything but its exact instruction count would exceed it (a doubling body held 24.7
/// bytes per byte of `nop `). Measured under Clang/libstdc++ with ASan: folded blocks nested to the
/// cap hold 22.9 bytes per source byte, their `_blocks` stack grown by doubling (22.1 under MSVC's
/// debug runtime); dense `(block)` 19.2, `(nop)` 16.9.
constexpr auto BodyBytesPerSourceByte = std::size_t { 23 };

/// The bytes writing a module may hold per byte of the text it writes: the text, a string grown by
/// doubling, which holds the old and the new buffer while it grows. Measured under Clang/libstdc++
/// with ASan: 2.6 bytes per text byte, the string just grown.
constexpr auto WriterBytesPerTextByte = std::size_t { 3 };

/// The bytes writing a module may hold per byte of the module, beside its text: the name section,
/// decoded, and each name's decoration, as the name codec holds at most 64 bytes per byte of the section
/// (MemoryBounds_test.cpp's BytesPerNameSectionByte). Measured under Clang/libstdc++ with ASan: manyNames,
/// whose every entry is an empty name, holds 14.8 bytes per module byte beyond its text.
constexpr auto WriterBytesPerModuleByte = std::size_t { 64 };

/// The bytes of text a module may be written as per byte of it, where the text format has a shorthand
/// for everything the module holds. Measured: manyFunctionImports writes 8.2 bytes of text per module
/// byte, the densest; wideTags 8.0, deepTypeChain 6.6, manyFunctions 5.2. A compact import group's module
/// name, which the flat text repeats for every import, is the exception: WriterOptions::maxTextBytes bounds
/// it.
constexpr auto TextBytesPerModuleByte = std::size_t { 16 };

/// What reading may hold whatever the source: the empty source's cost.
constexpr auto FixedBytes = std::size_t { 64 } * 1024;

/// How many nodes each measured source makes: one more than a power of two, so that the tree's vector has
/// just grown to twice what it uses, the worst case of a doubling vector. About 4 MiB of source.
constexpr auto Nodes = (std::size_t { 1 } << 21) + 1;

/// Reads @p source into a tree, and checks that reading held no more than the bound for its size.
/// @param source The text, which must read.
void checkLinear(std::string_view source)
{
    auto const budget = (TreeBytesPerSourceByte * source.size()) + FixedBytes;
    auto const scope = AllocationScope {};
    auto const tree = parseSExpressions(source, TextLimits {});
    auto const treeBytes = scope.peakBytes();
    REQUIRE(tree.has_value());
    CAPTURE(source.size(), budget, treeBytes, tree->nodes().size());
    CHECK(treeBytes <= budget);
}

/// What compiling a source may hold.
struct Budget
{
    std::size_t perSourceByte = 0; ///< The bytes per source byte.
    std::size_t extra = 0;         ///< The bytes beyond those that the source's size does not give.
};

/// Compiles @p source, a module, under @p limits, and checks that compiling held no more than
/// @p budget for its size. The bound is enforced, not only measured: an allocation past it is refused
/// (bad_alloc, which fails the test) before the host commits it.
/// @param source The text.
/// @param budget The bound.
/// @param limits The limits to compile under.
/// @return What compiling made of it.
std::expected<std::vector<std::byte>, TextError> compileWithin(std::string_view source,
                                                               Budget const& budget,
                                                               TextLimits const& limits = {})
{
    auto const bound = (budget.perSourceByte * source.size()) + budget.extra + FixedBytes;
    auto compiled = std::expected<std::vector<std::byte>, TextError> {};
    auto compileBytes = std::size_t { 0 };
    {
        auto const scope = AllocationScope { bound };
        compiled = compileText(source, limits);
        compileBytes = scope.peakBytes();
    }
    CAPTURE(source.size(), bound, compileBytes);
    CHECK(compileBytes <= bound);
    return compiled;
}

/// Compiles @p source, which must compile, within @p budget.
/// @param source The text, a module.
/// @param budget The bound.
void checkCompilesLinear(std::string_view source, Budget const& budget)
{
    auto const compiled = compileWithin(source, budget);
    if (!compiled)
        INFO(isoline::text::specMessage(compiled.error()));
    REQUIRE(compiled.has_value());
}

/// The heap bytes one empty defined function holds in a binary::Module on this standard library: its
/// Function, whose vectors MSVC's debug runtime gives a container proxy each, and its body's end.
/// @return The bytes, measured.
std::size_t bytesPerEmptyFunction()
{
    auto const scope = AllocationScope {};
    auto code = std::vector<isoline::binary::Function>(1);
    code.front().body.push_back(isoline::binary::Instruction { .opcode = isoline::binary::Opcode::End });
    // Unobserved, the vectors' allocations may be elided ([expr.new]), and GCC 16 does: the measure is then
    // zero. Their address, stored to a volatile, is observed.
    auto const* const volatile observed = code.front().body.data();
    static_cast<void>(observed);
    return scope.peakBytes();
}

/// Writes @p bytes, a binary module, as text under @p options, checking that writing held no more than
/// WriterBytesPerTextByte per byte of what it wrote and WriterBytesPerModuleByte per byte of the module.
/// @return The text, or why the writer refused it.
std::expected<std::string, WriteError> writeWithin(std::vector<std::byte> const& bytes,
                                                   WriterOptions const& options = {})
{
    auto const module = isoline::binary::decodeModule(bytes, isoline::FeatureSet::all());
    REQUIRE(module.has_value());
    auto text = std::expected<std::string, WriteError> {};
    auto writeBytes = std::size_t { 0 };
    // The budget is what the largest output the options allow may cost, enforced; the measured peak is
    // then held to the text actually written.
    auto const moduleBytes = WriterBytesPerModuleByte * bytes.size();
    {
        auto const scope =
            AllocationScope { (WriterBytesPerTextByte * options.maxTextBytes) + moduleBytes + FixedBytes };
        text = ModuleWriter { options }.write(*module);
        writeBytes = scope.peakBytes();
    }
    auto const textBytes = text ? text->size() : options.maxTextBytes;
    CAPTURE(bytes.size(), textBytes, writeBytes);
    CHECK(writeBytes <= (WriterBytesPerTextByte * textBytes) + moduleBytes + FixedBytes);
    return text;
}

/// @p count copies of @p unit.
/// @param unit The text to repeat.
/// @param count How often.
/// @return The copies, concatenated.
std::string repeated(std::string_view unit, std::size_t count)
{
    auto text = std::string {};
    text.reserve(unit.size() * count);
    for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, count))
        text += unit;
    return text;
}

/// A module of one compact import group: @p items items of a module named by @p nameLength bytes.
/// @param nameLength The module name's length.
/// @param items How many items.
/// @return Its text.
std::string compactImportGroup(std::size_t nameLength, std::size_t items)
{
    return "(module (import \"" + std::string(nameLength, 'm') + "\" "
           + repeated("(item \"\" (global i32))", items) + "))";
}

} // namespace

TEST_CASE("TextMemoryBounds.denseParentheses")
{
    SECTION("empty lists: one node per two bytes")
    {
        checkLinear(repeated("()", Nodes));
    }
    SECTION("one list of atoms: one node per two bytes")
    {
        checkLinear("(" + repeated("a ", Nodes - 1) + ")");
    }
    SECTION("punctuation at the top level: one node per byte")
    {
        checkLinear(repeated(",", Nodes));
    }
    SECTION("one list of punctuation: one node per byte")
    {
        checkLinear("(" + repeated(",", Nodes - 2) + ")");
    }
    SECTION("brackets and braces: one node per byte")
    {
        checkLinear(repeated("{}[]", Nodes / 4));
    }
    SECTION("atoms and lists alternating: three nodes per four bytes")
    {
        checkLinear(repeated("(1)1", Nodes / 3));
    }
    SECTION("lists nested to the cap: the stack of open lists is bounded by the cap")
    {
        auto const depth = std::size_t { TextLimits {}.maxNesting };
        checkLinear(std::string(depth, '(') + std::string(depth, ')'));
    }
}

TEST_CASE("TextMemoryBounds.openListsPastTheNestingLimit")
{
    // 200 MB of `(`: refused with "nesting limit exceeded" at the first list past the cap, having reserved
    // only for the lists before it. Counting every `(` first would reserve 4 GB of nodes for a tree the
    // reading pass refuses at its 10'001st list; the 1 GiB budget refuses that, small.
    constexpr auto Budget = std::size_t { 1 } << 30;
    auto const source = std::string(std::size_t { 200 } * 1000 * 1000, '(');
    auto refusal = std::optional<TextError> {};
    auto peak = std::size_t { 0 };
    {
        auto const scope = AllocationScope { Budget };
        auto const tree = parseSExpressions(source, TextLimits {});
        peak = scope.peakBytes();
        REQUIRE_FALSE(tree.has_value());
        refusal = tree.error();
    }
    CAPTURE(peak);
    CHECK(refusal->code == isoline::text::TextErrorCode::NestingLimitExceeded);
    CHECK(refusal->position.offset == TextLimits {}.maxNesting);
    // What the lists before it hold, nodes and the stack of open lists: as the cap's lists nested and
    // closed do, measured above, per byte of their 2 * maxNesting bytes.
    CHECK(peak <= (TreeBytesPerSourceByte * 2 * TextLimits {}.maxNesting) + FixedBytes);
}

TEST_CASE("TextMemoryBounds.defaultModuleLimit")
{
    // The cases below refuse at a small limit; the default is the engine's module limit, 64 MiB, which
    // they bound the same way, OutputCopies times over.
    STATIC_CHECK(TextLimits {}.maxModuleBytes == isoline::api::LoadLimits {}.maxModuleBytes);
    STATIC_CHECK(TextLimits {}.maxModuleBytes == std::size_t { 64 } * 1024 * 1024);
}

TEST_CASE("TextMemoryBounds.reader")
{
    // The text twins of the binary amplifiers (MemoryBounds_test.cpp), and the instruction shapes that
    // make the most of the reader's per-instruction state per source byte.
    constexpr auto Items = std::size_t { 200'000 };
    SECTION("manyFunctions: one function per six bytes")
    {
        checkCompilesLinear(
            "(module " + repeated("(func)", Items) + ")",
            Budget { .perSourceByte = ModuleBytesPerSourceByte, .extra = bytesPerEmptyFunction() * Items });
    }
    SECTION("compactImportGroup: a short module name, many items")
    {
        // One item more than a power of two: an imports vector grown by doubling would just have grown to
        // twice what it uses, which the bound does not admit; the reader reserves it exactly.
        checkCompilesLinear(compactImportGroup(16, (std::size_t { 1 } << 16) + 1),
                            Budget { .perSourceByte = ModuleBytesPerSourceByte });
    }
    SECTION("compactImportGroup: one 16 KiB module name, as many items as the module limit takes")
    {
        // The binary repeats the name for every item, so its output is about OutputLimit, not linear in the
        // source; it is bounded by the limit, held about twice while the section is copied into the module.
        constexpr auto OutputLimit = std::size_t { 8 } * 1024 * 1024;
        constexpr auto NameLength = std::size_t { 16 } * 1024;
        auto const compiled = compileWithin(
            compactImportGroup(NameLength, (OutputLimit / NameLength) - 1),
            Budget { .perSourceByte = ModuleBytesPerSourceByte, .extra = OutputCopies * OutputLimit },
            TextLimits { .maxModuleBytes = OutputLimit });
        REQUIRE(compiled.has_value());
        CHECK(compiled->size() > OutputLimit / 2);
    }
    SECTION("compactImportGroup: a 16 KiB module name for more items than the module limit takes")
    {
        // 8'192 items of a 16 KiB name ask for 128 MiB of binary from 128 KiB of source: refused as the
        // import section reaches the limit, having held no more than it. The default limit is 64 MiB
        // (TextMemoryBounds.defaultModuleLimit checks it); a limit of 8 MiB refuses the same way in a
        // fraction of the time.
        constexpr auto OutputLimit = std::size_t { 8 } * 1024 * 1024;
        auto const compiled = compileWithin(
            compactImportGroup(std::size_t { 16 } * 1024, 8'192),
            Budget { .perSourceByte = ModuleBytesPerSourceByte, .extra = OutputCopies * OutputLimit },
            TextLimits { .maxModuleBytes = OutputLimit });
        REQUIRE_FALSE(compiled.has_value());
        CHECK(compiled.error().code == isoline::text::TextErrorCode::OutputTooLarge);
    }
    SECTION("manyV128Literals: one literal per 26 bytes")
    {
        checkCompilesLinear("(module (func " + repeated("v128.const i64x2 0 0 drop ", Items) + "))",
                            Budget { .perSourceByte = ModuleBytesPerSourceByte });
    }
    SECTION("manyCatchClauses: one clause per 12 bytes")
    {
        checkCompilesLinear("(module (tag) (func (try_table " + repeated("(catch 0 0) ", Items) + ")))",
                            Budget { .perSourceByte = ModuleBytesPerSourceByte });
    }
    SECTION("100'000 implicit types: distinct parameter lists")
    {
        constexpr auto FunctionCount = 100'000U;
        constexpr auto ParamCount = 17U;
        auto source = std::string { "(module" };
        for (auto const ordinal: std::views::iota(0U, FunctionCount))
        {
            source += "(func (param";
            for (auto const bit: std::views::iota(0U, ParamCount))
                source += ((ordinal >> bit) & 1U) != 0 ? " i64" : " i32";
            source += "))";
        }
        checkCompilesLinear(source + ")", Budget { .perSourceByte = ModuleBytesPerSourceByte });
    }
    SECTION("dense flat nop: one instruction per four bytes")
    {
        checkCompilesLinear("(module (func " + repeated("nop ", Items * 4) + "))",
                            Budget { .perSourceByte = BodyBytesPerSourceByte });
    }
    SECTION("dense folded nop: one instruction per five bytes")
    {
        checkCompilesLinear("(module (func " + repeated("(nop)", Items * 4) + "))",
                            Budget { .perSourceByte = BodyBytesPerSourceByte });
    }
    SECTION("dense folded blocks: two instructions per seven bytes")
    {
        checkCompilesLinear("(module (func " + repeated("(block)", Items * 4) + "))",
                            Budget { .perSourceByte = BodyBytesPerSourceByte });
    }
    SECTION("folded operators nested to the cap: the work stack is bounded by the nesting")
    {
        auto const depth = std::size_t { TextLimits {}.maxNesting } - 2;
        checkCompilesLinear("(module (func " + repeated("(nop ", depth) + std::string(depth + 2, ')'),
                            Budget { .perSourceByte = BodyBytesPerSourceByte });
    }
    SECTION("folded blocks nested to the cap: each level a block and its end")
    {
        auto const depth = std::size_t { TextLimits {}.maxNesting } - 2;
        checkCompilesLinear("(module (func " + repeated("(block ", depth) + std::string(depth + 2, ')'),
                            Budget { .perSourceByte = BodyBytesPerSourceByte });
    }
    SECTION("flat blocks nested deep: each level a block and its end")
    {
        checkCompilesLinear("(module (func " + repeated("block ", Items) + repeated("end ", Items) + "))",
                            Budget { .perSourceByte = BodyBytesPerSourceByte });
    }
    SECTION("folded ifs nested to the cap: two levels per if")
    {
        auto const depth = (std::size_t { TextLimits {}.maxNesting } - 2) / 2;
        checkCompilesLinear("(module (func " + repeated("(if (then ", depth)
                                + std::string((2 * depth) + 2, ')'),
                            Budget { .perSourceByte = BodyBytesPerSourceByte });
    }
}

TEST_CASE("TextMemoryBounds.writtenAmplifyingModules")
{
    // The binary amplifiers (AmplifyingModules.hpp), written as text and compiled back: the text is
    // bounded as WriterOptions::maxTextBytes documents it, writing holds a few bytes per byte of it, and
    // compiling the text holds what TextMemoryBounds.reader bounds compiling any text by. Every case is
    // written under a small limit, so that a writer that amplified more would be refused small.
    struct Case
    {
        std::string_view name;        ///< What the module amplifies.
        std::vector<std::byte> bytes; ///< The module.
        std::size_t textBound = 0;    ///< The most text it may be written as, FixedBytes aside.
        std::size_t functions = 0;    ///< Its defined functions, each holding bytesPerEmptyFunction().
        /// What compiling its text may hold per byte: one dense body is bounded apart.
        std::size_t compileBytesPerSourceByte = ModuleBytesPerSourceByte;
    };
    constexpr auto Items = uint32_t { 50'000 };
    auto const linear = [](std::vector<std::byte> bytes) {
        auto const bound = TextBytesPerModuleByte * bytes.size();
        return std::pair { std::move(bytes), bound };
    };
    auto const make = [&](std::string_view name,
                          std::pair<std::vector<std::byte>, std::size_t> module,
                          std::size_t functions = 0) {
        return Case {
            .name = name, .bytes = std::move(module.first), .textBound = module.second, .functions = functions
        };
    };
    // The four amplifiers WriterOptions::maxTextBytes names, each bounded by its documented cost.
    constexpr auto NameBytes = std::size_t { 16 } * 1024;
    constexpr auto Locals = uint32_t { 50'000 };
    constexpr auto Params = std::size_t { 1000 };
    constexpr auto Depth = uint32_t { 50'000 };
    auto cases = std::vector<Case> {};
    // `(import "mmmmmmmmmmmmmmmm" "" (global i32))`: the name and 30 bytes per one-byte item.
    cases.push_back(
        make("compactImportGroup: a short module name",
             { isoline::testing::compactImportGroup(16, Items), (16 + 32) * std::size_t { Items } }));
    // The same for a long name, and the hint annotation the writer re-synthesizes beside it.
    cases.push_back(make("hintedImportGroup: a long module name and a branch hint",
                         { isoline::testing::hintedImportGroup(NameBytes, 128), (NameBytes + 32) * 128 },
                         1));
    // ` i32` per local.
    cases.push_back(make("wideLocalRuns",
                         { isoline::testing::wideLocalRuns(20, Locals), 4 * std::size_t { 20 } * Locals },
                         20));
    // ` i32` per parameter of every function's inline signature.
    cases.push_back(
        make("namedParameterFunctions",
             { isoline::testing::namedParameterFunctions(Params, 1000), ((4 * Params) + 64) * 1000 },
             1000));
    // A line of capped indentation per instruction, about 47 bytes per module byte.
    auto nested = isoline::testing::nestedBlocks(Depth);
    auto const nestedBound = 48 * nested.size();
    cases.push_back(make("nestedBlocks", { std::move(nested), nestedBound }, 1));
    cases.back().compileBytesPerSourceByte = BodyBytesPerSourceByte;
    // Everything else is linear.
    cases.push_back(make("wideTags", linear(isoline::testing::wideTags(1000, 4000))));
    cases.push_back(make("manyFunctions", linear(isoline::testing::manyFunctions(Items)), Items));
    cases.push_back(make("manyV128Literals", linear(isoline::testing::manyV128Literals(Items)), 1));
    cases.push_back(make("manyCatchClauses", linear(isoline::testing::manyCatchClauses(Items)), 1));
    cases.push_back(make("deepTypeChain", linear(isoline::testing::deepTypeChain(5000))));
    cases.push_back(make("manyFunctionImports", linear(isoline::testing::manyFunctionImports(1000, Items))));
    cases.push_back(make("multiValueCalls", linear(isoline::testing::multiValueCalls(4000)), 2));
    cases.push_back(make("manyNames", linear(isoline::testing::manyNames(20'000))));
    cases.push_back(make("sparseLongNames", linear(isoline::testing::sparseLongNames(2000, 256))));
    cases.push_back(make("lyingNameCount", linear(isoline::testing::lyingNameCount(1024 * 1024))));
    for (auto const& entry: cases)
    {
        INFO(entry.name);
        // Room for the bound and no more, so that a writer amplifying past it fails at the limit.
        auto const options = WriterOptions { .maxTextBytes = entry.textBound + FixedBytes };
        auto const text = writeWithin(entry.bytes, options);
        REQUIRE(text.has_value());
        CAPTURE(entry.bytes.size(), text->size(), entry.textBound);
        checkCompilesLinear(*text,
                            Budget { .perSourceByte = entry.compileBytesPerSourceByte,
                                     .extra = bytesPerEmptyFunction() * entry.functions });
    }
}

TEST_CASE("TextMemoryBounds.writtenHintSectionIsNotReencoded")
{
    // A compact import group of a 16 KiB module name and a branch hint, whose encoding repeats the name for
    // every item, as its text does. Deciding whether the hint is re-synthesized encodes the hinted body
    // alone (binary::encodeBranchHints), never the module: what writing it allocates in all, freed or not,
    // exceeds what writing the same module without its hint allocates by less than one encoding of the
    // module. A whole-module encoding allocates at least that much, so one is refused, and so are the two
    // the writer once compared. Each write is also held to writeWithin's peak budget, refusing past it.
    auto const bytes = isoline::testing::hintedImportGroup(std::size_t { 16 } * 1024, 128);
    auto const options = WriterOptions { .maxTextBytes = std::size_t { 4 } * 1024 * 1024 };
    auto const hinted = isoline::binary::decodeModule(bytes, isoline::FeatureSet::all());
    REQUIRE(hinted.has_value());
    REQUIRE(hinted->branchHints.size() == 1);
    auto stripped = *hinted;
    stripped.branchHints.clear();
    std::erase_if(stripped.customs, [](isoline::binary::CustomSection const& custom) {
        return custom.name == isoline::binary::BranchHintSectionName;
    });
    auto const encodedBytes = isoline::binary::encodeModule(*hinted).size();
    auto const budget = (WriterBytesPerTextByte * options.maxTextBytes)
                        + (WriterBytesPerModuleByte * bytes.size()) + FixedBytes;
    auto const allocatedWriting = [&](isoline::binary::Module const& module) {
        auto const scope = AllocationScope { budget };
        auto const text = ModuleWriter { options }.write(module);
        auto const allocated = scope.allocatedBytes();
        REQUIRE(text.has_value());
        return std::pair { allocated, *text };
    };
    auto const [hintedBytes, hintedText] = allocatedWriting(*hinted);
    auto const [strippedBytes, strippedText] = allocatedWriting(stripped);
    CAPTURE(encodedBytes, hintedBytes, strippedBytes, hintedText.size(), strippedText.size());
    CHECK(hintedText.contains("(@metadata.code.branch_hint \"\\01\") if"));
    CHECK_FALSE(strippedText.contains("branch_hint"));
    REQUIRE(hintedBytes >= strippedBytes);
    CHECK(hintedBytes - strippedBytes < encodedBytes);
}

TEST_CASE("TextMemoryBounds.writtenTextLimit")
{
    // 8'192 imports of one 16 KiB module name: 128 KiB of binary asks for 128 MiB of text, every import
    // repeating the name. Under an 8 MiB limit the writer refuses it, having held about the limit.
    constexpr auto TextLimit = std::size_t { 8 } * 1024 * 1024;
    auto const text = writeWithin(isoline::testing::compactImportGroup(std::size_t { 16 } * 1024, 8'192),
                                  WriterOptions { .maxTextBytes = TextLimit });
    REQUIRE_FALSE(text.has_value());
    CHECK(text.error() == WriteError::TextTooLarge);
}
