// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/component/Features.hpp>
#include <isoline/component/Limits.hpp>
#include <isoline/component/binary/Component.hpp>
#include <isoline/component/binary/Decoder.hpp>
#include <isoline/component/binary/Encoder.hpp>
#include <isoline/component/text/ComponentReader.hpp>
#include <isoline/component/text/ComponentWriter.hpp>
#include <isoline/component/text/Forms.hpp>
#include <isoline/text/Keywords.hpp>
#include <isoline/text/SExpression.hpp>
#include <isoline/text/TextError.hpp>
#include <isoline/text/TextLimits.hpp>
#include <isoline/text/TopLevel.hpp>

using isoline::component::ComponentLimits;
using isoline::component::binary::AliasDefinition;
using isoline::component::binary::AliasTarget;
using isoline::component::binary::CanonBuiltinTable;
using isoline::component::binary::CanonDefinition;
using isoline::component::binary::ComponentPool;
using isoline::component::binary::decodeComponent;
using isoline::component::binary::Definition;
using isoline::component::binary::Immediate;
using isoline::component::text::ComponentReader;
using isoline::component::text::ComponentWriter;
using isoline::component::text::everyComponentFeature;
using isoline::component::text::Forms;
using isoline::text::compileText;
using isoline::text::parseSExpressions;
using isoline::text::TextErrorCode;
using isoline::text::TextLimits;

namespace
{

/// The fixtures whose wasm-tools 1.263.0 encoding is committed beside their text (tests/component/text).
constexpr auto Fixtures = std::to_array<std::string_view>({ "names", "sugar", "types", "canon", "more" });

/// The bytes of a file of the source tree.
std::string contentsOf(std::filesystem::path const& path)
{
    auto file = std::ifstream { path, std::ios::binary };
    REQUIRE(file.is_open());
    return std::string { std::istreambuf_iterator<char> { file }, std::istreambuf_iterator<char> {} };
}

/// The path of a component text fixture.
std::filesystem::path fixture(std::string_view name, std::string_view extension)
{
    return std::filesystem::path { ISOLINE_SOURCE_DIR } / "tests" / "component" / "text"
           / (std::string { name } + std::string { extension });
}

/// The bytes @p source compiles to, which it must.
std::vector<std::byte> compiled(std::string_view source)
{
    auto bytes = compileText(source, TextLimits {}, Forms);
    INFO(source);
    auto const message = bytes ? std::string {}
                               : std::format("{} at {}:{}",
                                             isoline::text::specMessage(bytes.error()),
                                             bytes.error().position.line,
                                             bytes.error().position.column);
    INFO(message);
    REQUIRE(bytes.has_value());
    return std::move(*bytes);
}

/// What @p source is refused with, which it must be.
isoline::text::TextError refused(std::string_view source, TextLimits const& limits = {})
{
    auto bytes = compileText(source, limits, Forms);
    INFO(source);
    REQUIRE_FALSE(bytes.has_value());
    return bytes.error();
}

/// The pool @p source reads to.
ComponentPool pooled(std::string_view source)
{
    auto const bytes = compiled(source);
    auto pool = decodeComponent(bytes, everyComponentFeature(), ComponentLimits {});
    REQUIRE(pool.has_value());
    return std::move(*pool);
}

std::vector<std::byte> bytesOf(std::string const& text)
{
    auto bytes = std::vector<std::byte> {};
    std::ranges::transform(text, std::back_inserter(bytes), [](char c) { return static_cast<std::byte>(c); });
    return bytes;
}

} // namespace

TEST_CASE("ComponentReader.nameSectionMatchesOracle")
{
    // A $-named component: its component-name section, and its core module's name section, are the bytes
    // wasm-tools writes (tests/component/text/names.wasm, its provenance beside it).
    CHECK(compiled(contentsOf(fixture("names", ".wat"))) == bytesOf(contentsOf(fixture("names", ".wasm"))));
}

TEST_CASE("ComponentReader.corpusMatchesWasmTools")
{
    for (auto const name: Fixtures)
    {
        INFO(name);
        CHECK(compiled(contentsOf(fixture(name, ".wat"))) == bytesOf(contentsOf(fixture(name, ".wasm"))));
    }
}

TEST_CASE("ComponentReader.inlineAliasSugar")
{
    // (func $i "f") is the export alias it abbreviates, inserted before its use; a chain of names aliases
    // an instance at each step, and a core reference's name a core export.
    CHECK(compiled(R"wat((component (import "i" (instance (export "f" (func))))
                        (core func (canon lower (func 0 "f")))))wat")
          == compiled(R"wat((component (import "i" (instance (export "f" (func))))
                        (alias export 0 "f" (func)) (core func (canon lower (func 0)))))wat"));
    CHECK(compiled(R"wat((component (import "i" (instance (export "j" (instance (export "f" (func))))))
                        (core func (canon lower (func 0 "j" "f")))))wat")
          == compiled(R"wat((component (import "i" (instance (export "j" (instance (export "f" (func))))))
                        (alias export 0 "j" (instance)) (alias export 1 "f" (func))
                        (core func (canon lower (func 0)))))wat"));
    CHECK(compiled(R"wat((component (core module) (core instance (instantiate 0))
                        (core instance (export "f" (func 0 "g")))))wat")
          == compiled(R"wat((component (core module) (core instance (instantiate 0))
                        (alias core export 0 "g" (core func)) (core instance (export "f" (func 0)))))wat"));
    // A core instance has no core instance to export: its name is refused.
    CHECK(refused(R"wat((component (core instance (instantiate 0 (with "x" (instance 0 "y"))))))wat").code
          == TextErrorCode::UnexpectedToken);
}

TEST_CASE("ComponentReader.outerIdentifiers")
{
    // (alias outer $c $t) names the scope by its identifier: one scope out, the first type there.
    auto const pool = pooled("(component $c (type $t u8) (component (alias outer $c $t (type))))");
    REQUIRE(pool.components.size() == 2);
    auto const& alias = std::get<AliasDefinition>(pool.components[1].definitions.front());
    CHECK(alias.target == AliasTarget::Outer);
    CHECK(alias.outerCount == 1);
    CHECK(alias.outerIndex == 0);
    // An identifier of an enclosing scope gets an outer alias before its use, as wast inserts it.
    CHECK(compiled("(component (type u16) (type $t u8) (component (type (list $t))))")
          == compiled(
              "(component (type u16) (type $t u8) (component (alias outer 1 1 (type)) (type (list 0))))"));
    // Through a type scope too, counted as a scope.
    CHECK(
        compiled(R"wat((component (type $t u8) (type (instance (export "x" (type (eq $t)))))))wat")
        == compiled(
            R"wat((component (type $t u8) (type (instance (alias outer 1 0 (type)) (export "x" (type (eq 0)))))))wat"));
    // Only a module, a type or a component is aliased outward.
    CHECK(refused(R"wat((component (import "f" (func $f)) (component (export "g" (func $f)))))wat").code
          == TextErrorCode::UnknownIdentifier);
    // Numeric outer aliases are kept as written, for validation to check.
    CHECK(
        std::get<AliasDefinition>(pooled("(component (alias outer 5 7 (type)))").components[0].definitions[0])
            .outerCount
        == 5);
    CHECK(refused("(component (alias outer $missing 0 (type)))").code == TextErrorCode::UnknownIdentifier);
    CHECK(refused("(component (alias outer 3 $t (type)))").code == TextErrorCode::UnknownIdentifier);
    CHECK(refused("(component $c (alias outer $c $t (type)))").code == TextErrorCode::UnknownIdentifier);
}

TEST_CASE("ComponentReader.everyCanonRowHasText")
{
    // Each row validation admits or decodes: written as the writer writes it, read back to that definition.
    for (auto const& row: CanonBuiltinTable)
    {
        // Refused rows too: their text reads as the decoder decodes their bytes.
        INFO(row.text);
        CHECK(isoline::text::isKnownKeyword(row.text));
        auto canon = CanonDefinition {
            .builtin = row.builtin, .immediates = {}, .options = {}, .result = std::nullopt
        };
        auto immediates = uint32_t { 0 };
        for (auto const immediate: row.shape)
        {
            if (immediate == Immediate::Index)
                canon.immediates.push_back(++immediates);
            else if (immediate == Immediate::Async || immediate == Immediate::Shared)
                canon.immediates.push_back(1);
            else if (immediate == Immediate::CoreValueType)
                canon.immediates.push_back(0);
            else if (immediate == Immediate::Options)
                canon.options.push_back(isoline::component::binary::CanonOption {
                    .kind = isoline::component::binary::CanonOptionKind::Memory, .index = 3 });
            else if (immediate == Immediate::ResultList)
                canon.result = isoline::component::binary::PrimitiveValueType::String;
        }
        auto pool = ComponentPool { .bytes = std::make_shared<std::vector<std::byte> const>(),
                                    .components = { isoline::component::binary::Component {
                                        .kind = isoline::component::binary::ScopeKind::Component,
                                        .definitions = { Definition { canon } } } } };
        auto const text = ComponentWriter { {}, isoline::FeatureSet::all() }.write(pool);
        REQUIRE(text.has_value());
        CHECK(text->contains(row.text));
        auto const read = pooled(*text);
        CHECK(std::get<CanonDefinition>(read.components[0].definitions[0]) == canon);
    }
}

TEST_CASE("ComponentReader.deepNestingIsRefused")
{
    // 100 000 nested components: the reader opens scopes from a work list, refusing the one past
    // maxNesting, never recursing (the parser too is iterative, given room for the depth).
    constexpr auto Depth = std::size_t { 100'000 };
    auto source = std::string {};
    for ([[maybe_unused]] auto const each: std::views::iota(std::size_t { 0 }, Depth))
        source += "(component ";
    source += std::string(Depth, ')');
    auto const limits = TextLimits { .maxNesting = 200'000 };
    auto const tree = parseSExpressions(source, limits);
    REQUIRE(tree.has_value());
    auto const reader = ComponentReader { limits, ComponentLimits {}, everyComponentFeature() };
    auto const pool = reader.read(*tree, tree->firstTopLevel());
    REQUIRE_FALSE(pool.has_value());
    CHECK(pool.error().code == TextErrorCode::NestingLimitExceeded);
    // At the scope maxNesting + 1 deep: maxNesting are accepted.
    CHECK(pool.error().position.offset
          == ComponentLimits {}.maxNesting * std::string_view { "(component " }.size());
    auto const shallow = std::string { "(component (component (component)))" };
    auto const shallowTree = parseSExpressions(shallow, limits);
    REQUIRE(shallowTree.has_value());
    CHECK(ComponentReader { limits, ComponentLimits { .maxNesting = 3 }, everyComponentFeature() }
              .read(*shallowTree, shallowTree->firstTopLevel())
              .has_value());
    CHECK_FALSE(ComponentReader { limits, ComponentLimits { .maxNesting = 2 }, everyComponentFeature() }
                    .read(*shallowTree, shallowTree->firstTopLevel())
                    .has_value());
}

TEST_CASE("ComponentReader.forms")
{
    // The script forms: `definition`, binary and quote.
    CHECK(compiled("(component definition $c (type u8))") == compiled("(component $c (type u8))"));
    CHECK(compiled(R"wat((component quote "(type u8)" "(type u16)"))wat")
          == compiled("(component (type u8) (type u16))"));
    auto const empty = compiled("(component)");
    CHECK(compiled(R"wat((component binary "\00asm\0d\00\01\00"))wat") == empty);
    // Only after the head: the keyword, `definition`, an identifier and a name.
    CHECK(compiled(R"wat((component definition $c (@name "c") binary "\00asm\0d\00\01\00"))wat") == empty);
    // Read rather than compiled, a binary form decodes.
    auto const tree = parseSExpressions(R"wat((component binary "\00asm\0d\00\01\00"))wat", TextLimits {});
    REQUIRE(tree.has_value());
    auto const reader = ComponentReader { TextLimits {}, ComponentLimits {}, everyComponentFeature() };
    CHECK(reader.read(*tree, tree->firstTopLevel()).has_value());
    auto const quoted = parseSExpressions(R"wat((component quote "(type u8)"))wat", TextLimits {});
    REQUIRE(quoted.has_value());
    CHECK(reader.read(*quoted, quoted->firstTopLevel()).has_value());
    CHECK(refused(R"wat((component quote "(type"))wat").code == TextErrorCode::UnexpectedToken);
    // The reader takes a `(component …)` list only, and a binary form of strings only.
    for (auto const source: { "(module)", "x", "()" })
    {
        auto const other = parseSExpressions(source, TextLimits {});
        REQUIRE(other.has_value());
        CHECK_FALSE(reader.read(*other, other->firstTopLevel()).has_value());
    }
    auto const notStrings = parseSExpressions("(component binary 1)", TextLimits {});
    REQUIRE(notStrings.has_value());
    CHECK_FALSE(reader.read(*notStrings, notStrings->firstTopLevel()).has_value());
    // Annotations Isoline does not understand are ignored in a binary form's strings.
    CHECK(compiled(R"wat((component binary (@unknown) "\00asm\0d\00\01\00"))wat") == empty);
    CHECK(refused(R"wat((component quote 1))wat").code == TextErrorCode::UnexpectedToken);
    CHECK(refused(R"wat((component binary 1))wat").code == TextErrorCode::UnexpectedToken);
    CHECK(refused(R"wat((component binary "\00asm" (x)))wat").code == TextErrorCode::UnexpectedToken);
}

TEST_CASE("ComponentReader.binaryFormThatDoesNotDecode")
{
    // Read rather than compiled, which keeps a binary form verbatim: the decoder's own message is the
    // subject.
    auto const malformed = parseSExpressions(R"wat((component binary "\00asm"))wat", TextLimits {});
    REQUIRE(malformed.has_value());
    auto const reader = ComponentReader { TextLimits {}, ComponentLimits {}, everyComponentFeature() };
    auto const refusedBinary = reader.read(*malformed, malformed->firstTopLevel());
    REQUIRE_FALSE(refusedBinary.has_value());
    CHECK(refusedBinary.error().code == TextErrorCode::ComponentBinaryMalformed);
    CHECK(refusedBinary.error().position.column == 12);
    CHECK(isoline::text::specMessage(refusedBinary.error())
          == "malformed component binary: unexpected end-of-file");
}

TEST_CASE("ComponentReader.refusals")
{
    struct Row
    {
        std::string_view source;
        TextErrorCode code;
    };
    constexpr auto Rows = std::to_array<Row>({
        { .source = "(component (start 0))", .code = TextErrorCode::UnexpectedToken },
        // `binary` is a binary form's only after the head; anywhere else, nothing it could start.
        { .source = R"wat((component (type u8) binary "\00asm\0d\00\01\00"))wat",
          .code = TextErrorCode::UnexpectedToken },
        { .source = "(component (core rec))", .code = TextErrorCode::UnknownOperator },
        { .source = "(component (bogus))", .code = TextErrorCode::UnknownOperator },
        { .source = "(component x)", .code = TextErrorCode::UnknownOperator },
        { .source = "(component (type $t u8) (type $t u8))", .code = TextErrorCode::DuplicateIdentifier },
        { .source = "(component (type (list $missing)))", .code = TextErrorCode::UnknownIdentifier },
        { .source = R"wat((component (type (variant (case $a "x") (case $a "y")))))wat",
          .code = TextErrorCode::DuplicateIdentifier },
        { .source = "(component (type (record (bad))))", .code = TextErrorCode::UnknownOperator },
        { .source = "(component (type (record x)))", .code = TextErrorCode::UnknownOperator },
        { .source = "(component (type (variant x)))", .code = TextErrorCode::UnknownOperator },
        { .source = "(component (type (list)))", .code = TextErrorCode::UnexpectedToken },
        { .source = "(component (type (list u8 4294967296)))", .code = TextErrorCode::ConstantOutOfRange },
        { .source = "(component (type (own u8)))", .code = TextErrorCode::UnexpectedToken },
        { .source = "(component (type (nonsense)))", .code = TextErrorCode::UnknownOperator },
        { .source = "(component (type (list (nonsense))))", .code = TextErrorCode::UnknownOperator },
        { .source = R"wat((component (type (list "x"))))wat", .code = TextErrorCode::UnexpectedToken },
        { .source = "(component (type (flags 1)))", .code = TextErrorCode::UnexpectedToken },
        { .source = R"wat((component (type (func (param "a")))))wat",
          .code = TextErrorCode::UnexpectedToken },
        { .source = "(component (type (resource (rep v128))))", .code = TextErrorCode::UnexpectedToken },
        { .source = "(component (type (resource (x))))", .code = TextErrorCode::UnexpectedToken },
        { .source = "(component (core type (struct)))", .code = TextErrorCode::UnknownOperator },
        { .source = "(component (core type (func (param i33))))", .code = TextErrorCode::UnknownOperator },
        { .source = "(component (core type (func (param $x))))", .code = TextErrorCode::UnexpectedToken },
        { .source = "(component (core type (func (param (ref bogus)))))",
          .code = TextErrorCode::UnknownOperator },
        { .source = R"wat((component (core type (module (import "a" "b" (thing))))))wat",
          .code = TextErrorCode::UnknownOperator },
        { .source = R"wat((component (core type (module (import "a" "b" (table 1 i32))))))wat",
          .code = TextErrorCode::UnexpectedToken },
        { .source = R"wat((component (core type (module (import "a" "b" (memory 1 (pagesize 3)))))))wat",
          .code = TextErrorCode::InvalidCustomPageSize },
        { .source = "(component (core type (module (type (struct)))))",
          .code = TextErrorCode::UnknownOperator },
        { .source = "(component (core type (module (alias outer 0 0 (func))))))",
          .code = TextErrorCode::UnexpectedToken },
        { .source = R"wat((component (core type (module (export "e" (func (type $nope)))))))wat",
          .code = TextErrorCode::UnknownIdentifier },
        { .source = "(component (canon nonsense (core func)))", .code = TextErrorCode::UnknownOperator },
        { .source = "(component (canon lift (core func 0) (core func)))",
          .code = TextErrorCode::UnexpectedToken },
        { .source = "(component (canon resource.new 0 (func)))", .code = TextErrorCode::UnexpectedToken },
        { .source = "(component (canon context.get v128 0 (core func)))",
          .code = TextErrorCode::UnexpectedToken },
        { .source = "(component (canon lower (core func 0) (core func)))",
          .code = TextErrorCode::UnexpectedToken },
        { .source = "(component (canon waitable-set.wait (table 0) (core func)))",
          .code = TextErrorCode::UnexpectedToken },
        { .source = "(component (canon lift (func 0) (func (type 0))))",
          .code = TextErrorCode::UnexpectedToken },
        { .source = "(component (core func (canon lift (core func 0))))",
          .code = TextErrorCode::UnexpectedToken },
        { .source = "(component (core func (nonsense)))", .code = TextErrorCode::UnexpectedToken },
        { .source = "(component (func (type 0) (nonsense)))", .code = TextErrorCode::UnexpectedToken },
        { .source = R"wat((component (import "a" (value u8))))wat", .code = TextErrorCode::UnexpectedToken },
        { .source = R"wat((component (import "a" (type (lt 0)))))wat",
          .code = TextErrorCode::UnexpectedToken },
        { .source = R"wat((component (import "a" (implements "x") (implements "y") (func))))wat",
          .code = TextErrorCode::DuplicateIdentifier },
        { .source = R"wat((component (export "a" (bogus 0))))wat", .code = TextErrorCode::UnknownOperator },
        { .source = R"wat((component (export "a" (value 0))))wat", .code = TextErrorCode::UnexpectedToken },
        { .source = "(component (alias outer 0 0 (func)))", .code = TextErrorCode::UnexpectedToken },
        { .source = R"wat((component (alias core export 0 "x" (func))))wat",
          .code = TextErrorCode::UnexpectedToken },
        { .source = "(component (alias sideways 0))", .code = TextErrorCode::UnknownOperator },
        { .source = "(component (instance (instantiate 0 (bad))))", .code = TextErrorCode::UnexpectedToken },
        { .source = R"wat((component (core instance (instantiate 0 (with "a" (func 0)))))))wat",
          .code = TextErrorCode::UnexpectedToken },
        { .source = R"wat((component (core instance (export "a" (module 0)))))wat",
          .code = TextErrorCode::UnexpectedToken },
        { .source = R"wat((component (@custom "x" (after last) "y")))wat",
          .code = TextErrorCode::CustomUnexpectedToken },
        { .source = "(component (type $t (@name 1) u8))", .code = TextErrorCode::NameStringExpected },
        { .source = R"wat((component (type $t (@name "a" "b") u8)))wat",
          .code = TextErrorCode::NameUnexpectedToken },
        { .source = R"wat((component (type $t (@name "\ff") u8)))wat",
          .code = TextErrorCode::NameMalformedUtf8 },
        { .source = "(component (type u8 4294967296))", .code = TextErrorCode::UnexpectedToken },
        { .source = "(component (core instance (instantiate 4294967296)))",
          .code = TextErrorCode::ConstantOutOfRange },
        { .source =
              R"wat((component (core type (module (import "a" "b" (memory 18446744073709551616))))))wat",
          .code = TextErrorCode::ConstantOutOfRange },
        { .source = "(module)(component)", .code = TextErrorCode::UnexpectedToken },
        { .source = "(component (type (map)))", .code = TextErrorCode::UnexpectedToken },
        { .source = R"wat((component (core type (module (import "a" "b" (memory))))))wat",
          .code = TextErrorCode::UnexpectedToken },
        { .source =
              R"wat((component (core type (module (import "a" "b" (memory 1 18446744073709551616))))))wat",
          .code = TextErrorCode::ConstantOutOfRange },
        { .source = "(component (core func))", .code = TextErrorCode::UnexpectedToken },
        { .source = "(component (alias export))", .code = TextErrorCode::UnexpectedToken },
        { .source = R"wat((component (component (export "\ff"))))wat", .code = TextErrorCode::MalformedUtf8 },
        { .source = "(component (instance (instantiate)))", .code = TextErrorCode::UnexpectedToken },
        { .source = R"wat((component (instance (instantiate 0 (with "\ff" (func 0))))))wat",
          .code = TextErrorCode::MalformedUtf8 },
        { .source = "(component (alias))", .code = TextErrorCode::UnexpectedToken },
        { .source = "(component (alias outer))", .code = TextErrorCode::UnexpectedToken },
        { .source = "(component (alias outer 0))", .code = TextErrorCode::UnexpectedToken },
        { .source = "(component (core type (module (alias outer))))",
          .code = TextErrorCode::UnexpectedToken },
        { .source = "(component (core type (module (alias outer 0))))",
          .code = TextErrorCode::UnexpectedToken },
        { .source = "(component ())", .code = TextErrorCode::UnexpectedToken },
        { .source = "(component (canon))", .code = TextErrorCode::UnexpectedToken },
        { .source = "(component (core func (canon)))", .code = TextErrorCode::UnexpectedToken },
    });
    for (auto const& row: Rows)
    {
        INFO(row.source);
        CHECK(refused(row.source).code == row.code);
    }
}

TEST_CASE("ComponentReader.outputLimit")
{
    // Core modules together within the output limit; past it, refused.
    auto const source = std::string_view { "(component (core module (func)) (core module (func)))" };
    auto const whole = compiled(source);
    CHECK(refused(source, TextLimits { .maxModuleBytes = whole.size() - 1 }).code
          == TextErrorCode::OutputTooLarge);
    CHECK(
        refused("(component (core module (func)) (core module (func)))", TextLimits { .maxModuleBytes = 20 })
            .code
        == TextErrorCode::OutputTooLarge);
    CHECK(refused(R"wat((component binary "\00asm\0d\00\01\00"))wat", TextLimits { .maxModuleBytes = 4 }).code
          == TextErrorCode::OutputTooLarge);
}

TEST_CASE("ComponentReader.everyNodeMutatedFailsCleanly")
{
    // Every node of every fixture replaced by a hostile token, or deleted: the reader reads the text or
    // refuses it, at a position inside it, and never aborts, whatever the node stood for.
    constexpr auto Replacements = std::to_array<std::string_view>({
        "",
        "bogus",
        "$nope",
        R"("\ff")",
        "4294967296",
        "(bogus)",
        "()",
        "(type 0)",
        "(core func 0)",
        "1",
        "(@name 1)",
        R"($"\ff")",
        "0x",
    });
    // A number longer than TextLimits::maxNumberLength too.
    auto const tooLong = std::string(TextLimits {}.maxNumberLength + 1, '1');
    auto reads = std::size_t { 0 };
    auto refusals = std::size_t { 0 };
    for (auto const name: Fixtures)
    {
        auto const source = contentsOf(fixture(name, ".wat"));
        auto const tree = parseSExpressions(source, TextLimits {});
        REQUIRE(tree.has_value());
        // Each node replaced, then each followed by a token of its own.
        auto const variants = std::views::iota(std::size_t { 0 }, (Replacements.size() * 2) + 1);
        for (auto const& node: tree->nodes())
            for (auto const variant: variants)
            {
                auto mutated = source;
                auto const replacement = variant == Replacements.size() * 2
                                             ? std::string_view { tooLong }
                                             : Replacements[variant % Replacements.size()];
                if (variant < Replacements.size() || variant == Replacements.size() * 2)
                    mutated.replace(node.offset, node.length, replacement);
                else
                    mutated.insert(node.offset + node.length, " " + std::string { replacement });
                auto const bytes = compileText(mutated, TextLimits {}, Forms);
                if (bytes)
                {
                    ++reads;
                    continue;
                }
                ++refusals;
                INFO(mutated);
                CHECK(bytes.error().position.offset <= mutated.size());
            }
    }
    CHECK(reads > 0);
    CHECK(refusals > reads);
}
