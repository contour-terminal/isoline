// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/component/Features.hpp>
#include <isoline/component/Limits.hpp>
#include <isoline/component/binary/Component.hpp>
#include <isoline/component/binary/Decoder.hpp>
#include <isoline/component/binary/Encoder.hpp>
#include <isoline/component/binary/testing/Builder.hpp>
#include <isoline/component/binary/testing/DecoderFixtures.hpp>

using isoline::binary::testing::ModuleBuilder;
using isoline::component::componentFeatureRow;
using isoline::component::ComponentFeatureSet;
using isoline::component::ComponentFeatureTable;
using isoline::component::ComponentLimits;
using isoline::component::Implementation;
using isoline::component::binary::AliasDefinition;
using isoline::component::binary::Asyncness;
using isoline::component::binary::CanonBuiltinTable;
using isoline::component::binary::CanonDefinition;
using isoline::component::binary::canonicalizeComponent;
using isoline::component::binary::CanonOption;
using isoline::component::binary::CanonOptionKind;
using isoline::component::binary::Component;
using isoline::component::binary::ComponentPool;
using isoline::component::binary::CoreInlineExport;
using isoline::component::binary::CoreModuleDefinition;
using isoline::component::binary::coreModulePositions;
using isoline::component::binary::CoreSort;
using isoline::component::binary::CoreTypeDefinition;
using isoline::component::binary::decodeComponent;
using isoline::component::binary::DecodeError;
using isoline::component::binary::DecodeErrorCode;
using isoline::component::binary::DefinedValueType;
using isoline::component::binary::DefinedValueTypeTable;
using isoline::component::binary::Definition;
using isoline::component::binary::DefinitionPosition;
using isoline::component::binary::encodeComponent;
using isoline::component::binary::ExportDeclaration;
using isoline::component::binary::ExternalId;
using isoline::component::binary::ExternDesc;
using isoline::component::binary::FixedListType;
using isoline::component::binary::FunctionTypeDefinition;
using isoline::component::binary::ICoreModuleCanonicalizer;
using isoline::component::binary::Implements;
using isoline::component::binary::ImportDefinition;
using isoline::component::binary::InstanceTypeDefinition;
using isoline::component::binary::introducedSort;
using isoline::component::binary::LabeledType;
using isoline::component::binary::NameAttributes;
using isoline::component::binary::NestedComponent;
using isoline::component::binary::PrimitiveValueType;
using isoline::component::binary::ScopeKind;
using isoline::component::binary::Sort;
using isoline::component::binary::SortIndex;
using isoline::component::binary::specMessage;
using isoline::component::binary::splitCoreModules;
using isoline::component::binary::TypeBound;
using isoline::component::binary::TypeDefinition;
using isoline::component::binary::TypeIndex;
using isoline::component::binary::VerbatimCoreModules;
using isoline::component::binary::VersionSuffix;
using isoline::component::binary::testing::appendBytes;
using isoline::component::binary::testing::appendName;
using isoline::component::binary::testing::Bytes;
using isoline::component::binary::testing::ComponentBuilder;
using isoline::component::binary::testing::componentOf;
using isoline::component::binary::testing::componentPreamble;
using isoline::component::binary::testing::decoderFixtures;
using isoline::component::binary::testing::modulePreamble;
using isoline::component::binary::testing::nestedComponents;
using isoline::component::binary::testing::plainName;
using isoline::component::binary::testing::section;
using isoline::component::binary::testing::taggedModule;
using isoline::component::binary::testing::TypeScopeBuilder;

namespace
{

/// Bytes from a literal list.
Bytes bytesOf(std::initializer_list<uint8_t> list)
{
    auto bytes = Bytes {};
    appendBytes(bytes, list);
    return bytes;
}

/// Every gated feature on: what decodes then is what P11 decodes at all.
ComponentFeatureSet everyFeature()
{
    auto features = ComponentFeatureSet::defaults();
    for (auto const& row: ComponentFeatureTable)
        features = features.with(row.feature);
    return features;
}

/// The pool of @p bytes, decoded under @p features; the decode must succeed.
ComponentPool decoded(Bytes const& bytes, ComponentFeatureSet features = ComponentFeatureSet::wasi03())
{
    auto pool = decodeComponent(bytes, features, ComponentLimits {});
    REQUIRE(pool.has_value());
    return std::move(*pool);
}

/// The canonical form of @p bytes with verbatim modules; it must succeed.
Bytes canonical(Bytes const& bytes, ComponentFeatureSet features = ComponentFeatureSet::wasi03())
{
    auto modules = VerbatimCoreModules {};
    auto result = canonicalizeComponent(bytes, features, ComponentLimits {}, modules);
    REQUIRE(result.has_value());
    return std::move(*result);
}

/// A pool of one component holding @p definitions, its byte ranges indexing @p bytes.
ComponentPool poolOf(std::vector<Definition> definitions, Bytes bytes = {})
{
    auto pool = ComponentPool {};
    pool.bytes = std::make_shared<std::vector<std::byte> const>(std::move(bytes));
    pool.components.push_back(
        Component { .kind = ScopeKind::Component, .definitions = std::move(definitions) });
    return pool;
}

/// A component of one section @p id holding a vector of one element, @p element.
Bytes oneOf(uint8_t id, Bytes const& element)
{
    auto payload = Bytes { std::byte { 0x01 } };
    appendBytes(payload, element);
    return componentOf({ section(id, payload) });
}

/// Records each module it is given and answers with a module of its own, tagged by ordinal.
class RecordingCanonicalizer final: public ICoreModuleCanonicalizer
{
  public:
    std::expected<std::vector<std::byte>, std::string> canonicalize(
        std::span<std::byte const> module) override
    {
        seen.emplace_back(module.begin(), module.end());
        if (refuseOrdinal == seen.size() - 1)
            return std::unexpected { std::string { "refused" } };
        return taggedModule(std::string(1, static_cast<char>('A' + seen.size() - 1)));
    }

    std::vector<Bytes> seen;                  ///< Every module given, in call order.
    std::optional<std::size_t> refuseOrdinal; ///< The ordinal to refuse, if any.
};

/// The index of @p Alternative in Definition.
template <typename Alternative, std::size_t Index = 0>
consteval std::size_t kindOf()
{
    if constexpr (std::is_same_v<std::variant_alternative_t<Index, Definition>, Alternative>)
        return Index;
    else
        return kindOf<Alternative, Index + 1>();
}

/// The options of a CanonCase.
enum class CaseOptions : uint8_t
{
    None,    ///< No options.
    Memory9, ///< `(memory 9)`.
};

/// The result list of a CanonCase.
enum class CaseResult : uint8_t
{
    None, ///< No result: the empty result list.
    U32,  ///< `u32`.
};

/// A canon definition of @p builtin's row with the given immediates, for the hand-built pools.
struct CanonCase
{
    uint8_t opcode;                          ///< The row's opcode.
    std::vector<uint8_t> bytes;              ///< Binary.md's encoding after the opcode, transcribed by hand.
    std::vector<uint32_t> immediates;        ///< What CanonDefinition::immediates stores.
    CaseOptions options = CaseOptions::None; ///< The options it is given.
    CaseResult result = CaseResult::None;    ///< The result list it is given.
};

} // namespace

TEST_CASE("Encoder.roundTripsEveryDecoderFixture")
{
    for (auto const& fixture: decoderFixtures())
    {
        INFO(fixture.name);
        auto const pool = decoded(fixture.bytes);
        CHECK(encodeComponent(pool) == fixture.bytes);
        CHECK(canonical(fixture.bytes) == fixture.bytes);
    }
}

TEST_CASE("Encoder.fixturesHoldEverySectionKind")
{
    // Every Definition alternative a component holds, so every row of the encoder's section table, is
    // round-tripped by some decoder fixture; only the type scopes' export declaration is not one.
    auto seen = std::vector<bool>(std::variant_size_v<Definition>);
    for (auto const& fixture: decoderFixtures())
    {
        for (auto const& scope: decoded(fixture.bytes).components)
        {
            if (scope.kind != ScopeKind::Component)
                continue;
            for (auto const& definition: scope.definitions)
                seen[definition.index()] = true;
        }
    }
    for (auto const index: std::views::iota(std::size_t { 0 }, seen.size()))
    {
        INFO("Definition alternative " << index);
        CHECK(seen[index] == (index != kindOf<ExportDeclaration>()));
    }
}

TEST_CASE("Encoder.canonicalizeMergesSections")
{
    SECTION("two adjacent type sections become one")
    {
        auto const split =
            componentOf({ section(7, bytesOf({ 0x01, 0x73 })), section(7, bytesOf({ 0x01, 0x7D })) });
        auto const merged = componentOf({ section(7, bytesOf({ 0x02, 0x73, 0x7D })) });
        CHECK(canonical(split) == merged);
        CHECK(canonical(merged) == merged);
    }
    SECTION("sections apart stay apart, in order")
    {
        auto const apart = componentOf({ section(7, bytesOf({ 0x01, 0x73 })),
                                         section(10,
                                                 [] {
                                                     auto payload = Bytes { std::byte { 0x01 } };
                                                     appendBytes(payload, plainName("r"));
                                                     appendBytes(payload, { 0x03, 0x01 });
                                                     return payload;
                                                 }()),
                                         section(7, bytesOf({ 0x01, 0x7D })) });
        CHECK(canonical(apart) == apart);
    }
    SECTION("single sections are never merged")
    {
        auto const modules = componentOf({ section(1, modulePreamble()), section(1, modulePreamble()) });
        CHECK(canonical(modules) == modules);
        auto custom = Bytes {};
        appendName(custom, "c");
        auto const customs = componentOf({ section(0, custom), section(0, custom) });
        CHECK(canonical(customs) == customs);
    }
    SECTION("a 5-byte LEB 0 becomes one byte")
    {
        // (export "e" (func 0)), its index a 5-byte LEB 0.
        auto padded = plainName("e");
        appendBytes(padded, { 0x01, 0x80, 0x80, 0x80, 0x80, 0x00, 0x00 });
        auto shortest = plainName("e");
        appendBytes(shortest, { 0x01, 0x00, 0x00 });
        CHECK(canonical(oneOf(11, padded)) == oneOf(11, shortest));
        // A vector of none, its count a 5-byte LEB 0, defines nothing: its section is not kept.
        auto const empty = componentOf({ section(7, bytesOf({ 0x80, 0x80, 0x80, 0x80, 0x00 })) });
        CHECK(canonical(empty) == componentPreamble());
        // A padded section size and a padded index, too.
        auto sized = componentPreamble();
        appendBytes(sized, { 0x07, 0x82, 0x80, 0x00, 0x01, 0x73 });
        CHECK(canonical(sized) == componentOf({ section(7, bytesOf({ 0x01, 0x73 })) }));
        auto const index = oneOf(11, [] {
            auto element = plainName("e");
            appendBytes(element, { 0x01, 0x85, 0x80, 0x00, 0x00 });
            return element;
        }());
        CHECK(canonical(index) == oneOf(11, [] {
                  auto element = plainName("e");
                  appendBytes(element, { 0x01, 0x05, 0x00 });
                  return element;
              }()));
    }
    SECTION("idempotent")
    {
        auto const split = componentOf(
            { section(7, bytesOf({ 0x81, 0x80, 0x00, 0x73 })), section(7, bytesOf({ 0x01, 0x7D })) });
        auto const once = canonical(split);
        CHECK(canonical(once) == once);
        for (auto const& fixture: decoderFixtures())
        {
            INFO(fixture.name);
            auto const first = canonical(fixture.bytes);
            CHECK(canonical(first) == first);
        }
    }
    SECTION("the module canonicalizer sees every embedded module once, in ordinal order")
    {
        auto const nested = componentOf({ section(1, taggedModule("a")),
                                          section(4, componentOf({ section(1, taggedModule("b")) })),
                                          section(1, taggedModule("c")) });
        auto modules = RecordingCanonicalizer {};
        auto const result =
            canonicalizeComponent(nested, ComponentFeatureSet::wasi03(), ComponentLimits {}, modules);
        REQUIRE(result.has_value());
        CHECK(modules.seen == std::vector<Bytes> { taggedModule("a"), taggedModule("b"), taggedModule("c") });
        auto const expected = componentOf({ section(1, taggedModule("A")),
                                            section(4, componentOf({ section(1, taggedModule("B")) })),
                                            section(1, taggedModule("C")) });
        CHECK(*result == expected);
        auto const split = splitCoreModules(*result, ComponentFeatureSet::wasi03(), ComponentLimits {});
        REQUIRE(split.has_value());
        REQUIRE(split->size() == 3);
        CHECK(Bytes(split->at(1).begin(), split->at(1).end()) == taggedModule("B"));
    }
    SECTION("a refused module is refused at its offset")
    {
        auto const twoModules = componentOf({ section(1, taggedModule("a")), section(1, taggedModule("b")) });
        auto modules = RecordingCanonicalizer {};
        modules.refuseOrdinal = 1;
        auto const result =
            canonicalizeComponent(twoModules, ComponentFeatureSet::wasi03(), ComponentLimits {}, modules);
        REQUIRE_FALSE(result.has_value());
        // The second module starts after the preamble, the first section's 2 + 12 bytes and its own header.
        CHECK(result.error() == DecodeError { DecodeErrorCode::CoreModuleRefused, 8 + 14 + 2 });
        CHECK(specMessage(result.error()) == "core module refused by its canonicalizer");
        CHECK(modules.seen.size() == 2);
    }
    SECTION("a decode error is the decoder's")
    {
        auto modules = VerbatimCoreModules {};
        auto const result = canonicalizeComponent(
            bytesOf({ 0x00, 0x61, 0x73 }), ComponentFeatureSet::wasi03(), ComponentLimits {}, modules);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == DecodeErrorCode::UnexpectedEnd);
    }
}

TEST_CASE("Encoder.canonicalForms")
{
    SECTION("a name of the legacy form 0x01, or attributed with none, is written plain")
    {
        auto legacy = Bytes { std::byte { 0x01 } };
        appendName(legacy, "a");
        appendBytes(legacy, { 0x01, 0x00 });
        auto attributedEmpty = Bytes { std::byte { 0x02 } };
        appendName(attributedEmpty, "a");
        appendBytes(attributedEmpty, { 0x00, 0x01, 0x00 });
        auto plain = plainName("a");
        appendBytes(plain, { 0x01, 0x00 });
        CHECK(canonical(oneOf(10, legacy)) == oneOf(10, plain));
        CHECK(canonical(oneOf(10, attributedEmpty)) == oneOf(10, plain));
    }
    SECTION("core types in their shortest forms")
    {
        // (module (import "a" "t" (table 1 (ref null func))) (import "a" "m" (memory 2 (pagesize 65536))))
        auto longForm = bytesOf({ 0x50, 0x02, 0x00 });
        appendName(longForm, "a");
        appendName(longForm, "t");
        appendBytes(longForm, { 0x01, 0x63, 0x70, 0x00, 0x01, 0x00 });
        appendName(longForm, "a");
        appendName(longForm, "m");
        appendBytes(longForm, { 0x02, 0x08, 0x02, 0x10 });
        auto shortForm = bytesOf({ 0x50, 0x02, 0x00 });
        appendName(shortForm, "a");
        appendName(shortForm, "t");
        appendBytes(shortForm, { 0x01, 0x70, 0x00, 0x01, 0x00 });
        appendName(shortForm, "a");
        appendName(shortForm, "m");
        appendBytes(shortForm, { 0x02, 0x00, 0x02 });
        CHECK(canonical(oneOf(3, longForm)) == oneOf(3, shortForm));
    }
    SECTION("a valtype's index in its shortest s33")
    {
        // (list 64), its index padded to three bytes: its shortest s33 takes two, the sign bit clear.
        CHECK(canonical(oneOf(7, bytesOf({ 0x70, 0xC0, 0x80, 0x00 })))
              == oneOf(7, bytesOf({ 0x70, 0xC0, 0x00 })));
    }
}

TEST_CASE("Encoder.deepNestingIsIterative")
{
    SECTION("100 nested components, the cap, round-trip")
    {
        auto const deep = nestedComponents(100);
        auto const pool = decoded(deep);
        CHECK(pool.components.size() == 100);
        CHECK(encodeComponent(pool) == deep);
    }
    SECTION("100'000 nested components built by hand encode without recursion")
    {
        constexpr auto Depth = uint32_t { 100'000 };
        auto pool = ComponentPool {};
        pool.bytes = std::make_shared<std::vector<std::byte> const>();
        for (auto const level: std::views::iota(uint32_t { 0 }, Depth))
        {
            auto component = Component { .kind = ScopeKind::Component, .definitions = {} };
            if (level + 1 < Depth)
                component.definitions.emplace_back(NestedComponent { .index = level + 1 });
            pool.components.push_back(std::move(component));
        }
        CHECK(encodeComponent(pool) == nestedComponents(Depth));
    }
    SECTION("100'000 nested instance types built by hand encode without recursion")
    {
        constexpr auto Depth = uint32_t { 100'000 };
        auto pool = poolOf({ TypeDefinition { .type = InstanceTypeDefinition { .scope = 1 } } });
        // Each instance type declares the next as its one type: (type (instance (type (instance ...)))).
        auto declarations = bytesOf({ 0x01, 0x42 });
        for (auto const level: std::views::iota(uint32_t { 1 }, Depth + 1))
        {
            auto scope = Component { .kind = ScopeKind::InstanceType, .definitions = {} };
            if (level < Depth)
            {
                scope.definitions.emplace_back(
                    TypeDefinition { .type = InstanceTypeDefinition { .scope = level + 1 } });
                appendBytes(declarations, { 0x01, 0x01, 0x42 });
            }
            pool.components.push_back(std::move(scope));
        }
        appendBytes(declarations, { 0x00 });
        CHECK(encodeComponent(pool) == componentOf({ section(7, declarations) }));
    }
}

TEST_CASE("Encoder.everyDefinedValueType")
{
    // Binary.md's encoding of each compound defvaltype at the pin, in DefinedValueTypeTable's order,
    // transcribed by hand: a type of each, its payload naming every part it has.
    struct Case
    {
        DefinedValueType type;
        std::vector<uint8_t> bytes;
    };
    auto const cases = std::vector<Case> {
        { .type = PrimitiveValueType::ErrorContext, .bytes = { 0x64 } },
        { .type =
              isoline::component::binary::RecordType {
                  .fields = { { .label = "a", .type = PrimitiveValueType::Bool } } },
          .bytes = { 0x72, 0x01, 0x01, 'a', 0x7F } },
        { .type =
              isoline::component::binary::VariantType {
                  .cases = { { .label = "a", .type = TypeIndex { .index = 3 } },
                             { .label = "b", .type = std::nullopt } } },
          .bytes = { 0x71, 0x02, 0x01, 'a', 0x01, 0x03, 0x00, 0x01, 'b', 0x00, 0x00 } },
        { .type = isoline::component::binary::ListType { .element = PrimitiveValueType::U8 },
          .bytes = { 0x70, 0x7D } },
        { .type = FixedListType { .element = PrimitiveValueType::U8, .length = 4 },
          .bytes = { 0x67, 0x7D, 0x04 } },
        { .type = isoline::component::binary::TupleType { .elements = { PrimitiveValueType::S8,
                                                                        TypeIndex { .index = 1 } } },
          .bytes = { 0x6F, 0x02, 0x7E, 0x01 } },
        { .type = isoline::component::binary::FlagsType { .labels = { "x", "y" } },
          .bytes = { 0x6E, 0x02, 0x01, 'x', 0x01, 'y' } },
        { .type = isoline::component::binary::EnumType { .labels = { "z" } },
          .bytes = { 0x6D, 0x01, 0x01, 'z' } },
        { .type = isoline::component::binary::OptionType { .type = PrimitiveValueType::Char },
          .bytes = { 0x6B, 0x74 } },
        { .type = isoline::component::binary::ResultType { .ok = std::nullopt,
                                                           .error = PrimitiveValueType::String },
          .bytes = { 0x6A, 0x00, 0x01, 0x73 } },
        { .type = isoline::component::binary::OwnType { .resource = 2 }, .bytes = { 0x69, 0x02 } },
        { .type = isoline::component::binary::BorrowType { .resource = 3 }, .bytes = { 0x68, 0x03 } },
        { .type = isoline::component::binary::StreamType { .element = PrimitiveValueType::U32 },
          .bytes = { 0x66, 0x01, 0x79 } },
        { .type = isoline::component::binary::FutureType { .element = std::nullopt },
          .bytes = { 0x65, 0x00 } },
        { .type = isoline::component::binary::MapType { .key = PrimitiveValueType::String,
                                                        .value = PrimitiveValueType::U64 },
          .bytes = { 0x63, 0x73, 0x77 } },
    };
    REQUIRE(cases.size() == DefinedValueTypeTable.size() + 1);
    for (auto const& [type, bytes]: cases)
    {
        // A primitive's gate is its PrimitiveValueTypeTable row's, a compound's its DefinedValueTypeTable
        // row's.
        auto literal = Bytes {};
        appendBytes(literal, bytes);
        auto const expected = oneOf(7, literal);
        INFO(bytes.front());
        auto const built = poolOf({ TypeDefinition { .type = type } });
        CHECK(encodeComponent(built) == expected);
        CHECK(built.components.front().definitions.front() == Definition { TypeDefinition { .type = type } });
        // The round trip holds for every one, gated or not: the gates are validation's.
        auto const pool = decodeComponent(expected, ComponentFeatureSet::defaults(), ComponentLimits {});
        REQUIRE(pool.has_value());
        CHECK(encodeComponent(*pool) == expected);
    }
}

TEST_CASE("Encoder.everyCanonRow")
{
    // Binary.md's canon production at the pin, opcode for opcode, transcribed by hand with distinct
    // immediates (indices 5, 6, async and shared 1, core type i64, options (memory 9), result u32), so
    // a wrong opcode or a wrong immediate order changes the bytes.
    auto const cases = std::vector<CanonCase> {
        { .opcode = 0x00,
          .bytes = { 0x00, 0x05, 0x01, 0x03, 0x09, 0x06 },
          .immediates = { 5, 6 },
          .options = CaseOptions::Memory9 },
        { .opcode = 0x01,
          .bytes = { 0x00, 0x05, 0x01, 0x03, 0x09 },
          .immediates = { 5 },
          .options = CaseOptions::Memory9 },
        { .opcode = 0x02, .bytes = { 0x05 }, .immediates = { 5 } },
        { .opcode = 0x03, .bytes = { 0x05 }, .immediates = { 5 } },
        { .opcode = 0x04, .bytes = { 0x05 }, .immediates = { 5 } },
        { .opcode = 0x05, .bytes = {}, .immediates = {} },
        { .opcode = 0x06, .bytes = { 0x01 }, .immediates = { 1 } },
        { .opcode = 0x09,
          .bytes = { 0x00, 0x79, 0x01, 0x03, 0x09 },
          .immediates = {},
          .options = CaseOptions::Memory9,
          .result = CaseResult::U32 },
        { .opcode = 0x0A, .bytes = { 0x7E, 0x05 }, .immediates = { 1, 5 } },
        { .opcode = 0x0B, .bytes = { 0x7E, 0x05 }, .immediates = { 1, 5 } },
        { .opcode = 0x0C, .bytes = { 0x00 }, .immediates = {} },
        { .opcode = 0x0D, .bytes = {}, .immediates = {} },
        { .opcode = 0x0E, .bytes = { 0x05 }, .immediates = { 5 } },
        { .opcode = 0x0F,
          .bytes = { 0x05, 0x01, 0x03, 0x09 },
          .immediates = { 5 },
          .options = CaseOptions::Memory9 },
        { .opcode = 0x10,
          .bytes = { 0x05, 0x01, 0x03, 0x09 },
          .immediates = { 5 },
          .options = CaseOptions::Memory9 },
        { .opcode = 0x11, .bytes = { 0x05, 0x01 }, .immediates = { 5, 1 } },
        { .opcode = 0x12, .bytes = { 0x05, 0x01 }, .immediates = { 5, 1 } },
        { .opcode = 0x13, .bytes = { 0x05 }, .immediates = { 5 } },
        { .opcode = 0x14, .bytes = { 0x05 }, .immediates = { 5 } },
        { .opcode = 0x15, .bytes = { 0x05 }, .immediates = { 5 } },
        { .opcode = 0x16,
          .bytes = { 0x05, 0x01, 0x03, 0x09 },
          .immediates = { 5 },
          .options = CaseOptions::Memory9 },
        { .opcode = 0x17,
          .bytes = { 0x05, 0x01, 0x03, 0x09 },
          .immediates = { 5 },
          .options = CaseOptions::Memory9 },
        { .opcode = 0x18, .bytes = { 0x05, 0x01 }, .immediates = { 5, 1 } },
        { .opcode = 0x19, .bytes = { 0x05, 0x01 }, .immediates = { 5, 1 } },
        { .opcode = 0x1A, .bytes = { 0x05 }, .immediates = { 5 } },
        { .opcode = 0x1B, .bytes = { 0x05 }, .immediates = { 5 } },
        { .opcode = 0x1C, .bytes = { 0x01, 0x03, 0x09 }, .immediates = {}, .options = CaseOptions::Memory9 },
        { .opcode = 0x1D, .bytes = { 0x01, 0x03, 0x09 }, .immediates = {}, .options = CaseOptions::Memory9 },
        { .opcode = 0x1E, .bytes = {}, .immediates = {} },
        { .opcode = 0x1F, .bytes = {}, .immediates = {} },
        { .opcode = 0x20, .bytes = { 0x00, 0x05 }, .immediates = { 5 } },
        { .opcode = 0x21, .bytes = { 0x00, 0x05 }, .immediates = { 5 } },
        { .opcode = 0x22, .bytes = {}, .immediates = {} },
        { .opcode = 0x23, .bytes = {}, .immediates = {} },
        { .opcode = 0x24, .bytes = {}, .immediates = {} },
        { .opcode = 0x25, .bytes = {}, .immediates = {} },
        { .opcode = 0x26, .bytes = {}, .immediates = {} },
        { .opcode = 0x27, .bytes = { 0x05, 0x06 }, .immediates = { 5, 6 } },
        { .opcode = 0x28, .bytes = {}, .immediates = {} },
        { .opcode = 0x29, .bytes = { 0x00 }, .immediates = {} },
        { .opcode = 0x2A, .bytes = { 0x00 }, .immediates = {} },
        { .opcode = 0x2B, .bytes = { 0x00 }, .immediates = {} },
        { .opcode = 0x2C, .bytes = { 0x00 }, .immediates = {} },
        { .opcode = 0x2D, .bytes = { 0x00 }, .immediates = {} },
        { .opcode = 0x2E, .bytes = { 0x05 }, .immediates = { 5 } },
        { .opcode = 0x2F, .bytes = { 0x05 }, .immediates = { 5 } },
        { .opcode = 0x40, .bytes = { 0x01, 0x05 }, .immediates = { 1, 5 } },
        { .opcode = 0x41, .bytes = { 0x01, 0x05, 0x06 }, .immediates = { 1, 5, 6 } },
        { .opcode = 0x42, .bytes = { 0x01 }, .immediates = { 1 } },
    };
    REQUIRE(cases.size() == CanonBuiltinTable.size());
    for (auto const index: std::views::iota(std::size_t { 0 }, cases.size()))
    {
        auto const& row = CanonBuiltinTable[index];
        auto const& expected = cases[index];
        INFO(row.text);
        REQUIRE(row.opcode == expected.opcode);
        auto const definition = CanonDefinition {
            .builtin = row.builtin,
            .immediates = expected.immediates,
            .options = expected.options == CaseOptions::Memory9
                           ? std::vector<CanonOption> { { .kind = CanonOptionKind::Memory, .index = 9 } }
                           : std::vector<CanonOption> {},
            .result =
                expected.result == CaseResult::U32
                    ? std::optional { isoline::component::binary::ValueTypeRef { PrimitiveValueType::U32 } }
                    : std::nullopt,
        };
        auto element = Bytes { std::byte { row.opcode } };
        appendBytes(element, expected.bytes);
        auto const component = oneOf(8, element);
        CHECK(encodeComponent(poolOf({ definition })) == component);
        // The decoder reads the same definition back where P11 decodes the row.
        if (row.gate && componentFeatureRow(*row.gate).p11 == Implementation::Refused)
            continue;
        auto const pool = decoded(component, everyFeature());
        CHECK(pool.components.front().definitions == std::vector<Definition> { definition });
    }
}

TEST_CASE("Encoder.everyCanonOption")
{
    // Each option once, the options with an index naming 7: Binary.md's canonopt bytes.
    auto options = std::vector<CanonOption> {};
    for (auto const kind: { CanonOptionKind::Utf8,
                            CanonOptionKind::Utf16,
                            CanonOptionKind::Latin1Utf16,
                            CanonOptionKind::Memory,
                            CanonOptionKind::Realloc,
                            CanonOptionKind::PostReturn,
                            CanonOptionKind::Async,
                            CanonOptionKind::Callback })
        options.push_back(
            { .kind = kind,
              .index = kind >= CanonOptionKind::Memory && kind != CanonOptionKind::Async ? 7U : 0U });
    auto const definition = CanonDefinition { .builtin = isoline::component::binary::CanonBuiltin::Lower,
                                              .immediates = { 0 },
                                              .options = options,
                                              .result = std::nullopt };
    auto const component = oneOf(8,
                                 bytesOf({ 0x01,
                                           0x00,
                                           0x00,
                                           0x08,
                                           0x00,
                                           0x01,
                                           0x02,
                                           0x03,
                                           0x07,
                                           0x04,
                                           0x07,
                                           0x05,
                                           0x07,
                                           0x06,
                                           0x07,
                                           0x07 }));
    CHECK(encodeComponent(poolOf({ definition })) == component);
    CHECK(encodeComponent(decoded(component, everyFeature())) == component);
}

TEST_CASE("Encoder.coreTypesRoundTrip")
{
    // A module type of every core extern type, in their shortest forms.
    auto type = bytesOf({ 0x50, 0x07 });
    auto const import = [&](std::string_view name, std::initializer_list<uint8_t> externType) {
        appendBytes(type, { 0x00 });
        appendName(type, "a");
        appendName(type, name);
        appendBytes(type, externType);
    };
    appendBytes(type, { 0x01, 0x60, 0x02, 0x7B, 0x69, 0x01, 0x74 }); // (type (func (param v128 exnref)
                                                                     // (result nullexnref)))
    import("f", { 0x00, 0x00 });                                     // (func (type 0))
    import("t", { 0x01, 0x6F, 0x05, 0x01, 0x02 });                   // (table i64 1 2 externref)
    import("m", { 0x02, 0x09, 0x01, 0x02, 0x00 });                   // (memory 1 2 (pagesize 1))
    import("g", { 0x03, 0x64, 0x00, 0x01 });                         // (global (mut (ref 0)))
    import("h", { 0x03, 0x63, 0x6F, 0x00 }); // (global (ref null extern)) as (ref null extern)
    appendBytes(type, { 0x03 });             // (export "x" (tag (type 0)))
    appendName(type, "x");
    appendBytes(type, { 0x04, 0x00, 0x00 });
    // (ref null extern) is externref's long form: the canonical form is its shorthand.
    auto expected = type;
    auto const longRef = bytesOf({ 0x03, 0x63, 0x6F, 0x00 });
    auto const at = std::ranges::search(expected, longRef);
    REQUIRE_FALSE(at.empty());
    auto const offset = at.begin() - expected.begin();
    expected.erase(expected.begin() + offset + 1, expected.begin() + offset + 2);
    auto const component = oneOf(3, type);
    CHECK(canonical(component) == oneOf(3, expected));
    // A core function type of its own.
    auto const function = oneOf(3, bytesOf({ 0x60, 0x01, 0x7C, 0x00 }));
    CHECK(canonical(function) == function);
}

TEST_CASE("Encoder.namesAndAttributes")
{
    // Every attribute, in NameAttributeTable's order; 🔗 is refused at decode, so the pool is built.
    auto const name = NameAttributes { .name = "x",
                                       .attributes = { Implements { .interfaceName = "i" },
                                                       VersionSuffix { .suffix = "s" },
                                                       ExternalId { .id = "e" } } };
    auto const pool = poolOf(
        { ImportDefinition { .name = name, .type = ExternDesc { .sort = Sort::Instance, .index = 0 } } });
    auto element = bytesOf({ 0x02 });
    appendName(element, "x");
    appendBytes(element, { 0x03, 0x00 });
    appendName(element, "i");
    appendBytes(element, { 0x01 });
    appendName(element, "s");
    appendBytes(element, { 0x02 });
    appendName(element, "e");
    appendBytes(element, { 0x05, 0x00 });
    CHECK(encodeComponent(pool) == oneOf(10, element));
    // The pool holds the name it was given: a 🔗 attribute compares by its suffix.
    auto const& held = std::get<ImportDefinition>(pool.components.front().definitions.front()).name;
    CHECK(held == name);
    CHECK_FALSE(held.attributes.at(1)
                == isoline::component::binary::Attribute { VersionSuffix { .suffix = "t" } });
}

TEST_CASE("Encoder.resourcesAndTypeDeclarations")
{
    // A resource with a destructor, and a component type of every declaration kind.
    auto builder = ComponentBuilder {};
    CHECK(builder.resource(3) == 0);
    auto declarations = TypeScopeBuilder {};
    CHECK(declarations.coreType(CoreTypeDefinition { .type = isoline::binary::FuncType {} }) == 0);
    CHECK(declarations.type(TypeDefinition { .type = DefinedValueType { PrimitiveValueType::U8 } }) == 0);
    CHECK(declarations.aliasOuter(Sort::Type, 1, 0) == 1);
    CHECK(declarations.import("i", ExternDesc { .sort = Sort::Func, .index = 0 }) == 0);
    CHECK(declarations.exportItem(
              "r", ExternDesc { .sort = Sort::Type, .index = 0, .bound = TypeBound::SubResource })
          == 2);
    auto inner = TypeScopeBuilder {};
    CHECK(inner.exportItem("e", ExternDesc { .sort = Sort::Type, .index = 0, .bound = TypeBound::Equal })
          == 0);
    auto const innerIndex = declarations.instanceType(std::move(inner));
    CHECK(innerIndex == 3);
    CHECK(declarations.componentType(TypeScopeBuilder {}) == 4);
    auto const declarationsIndex = builder.componentType(std::move(declarations));
    CHECK(declarationsIndex == 1);
    CHECK(builder.instanceType(TypeScopeBuilder {}) == 2);

    auto expected = bytesOf({ 0x03, 0x3F, 0x7F, 0x01, 0x03, 0x41, 0x07 });
    appendBytes(expected, { 0x00, 0x60, 0x00, 0x00 });       // (core type (func))
    appendBytes(expected, { 0x01, 0x7D });                   // (type u8)
    appendBytes(expected, { 0x02, 0x03, 0x02, 0x01, 0x00 }); // (alias outer 1 0 (type))
    appendBytes(expected, { 0x03 });                         // (import "i" (func (type 0)))
    appendBytes(expected, plainName("i"));
    appendBytes(expected, { 0x01, 0x00 });
    appendBytes(expected, { 0x04 }); // (export "r" (type (sub resource)))
    appendBytes(expected, plainName("r"));
    appendBytes(expected, { 0x03, 0x01 });
    appendBytes(expected, { 0x01, 0x42, 0x01, 0x04 }); // (type (instance (export "e" (type (eq 0)))))
    appendBytes(expected, plainName("e"));
    appendBytes(expected, { 0x03, 0x00, 0x00 });
    appendBytes(expected, { 0x01, 0x41, 0x00 }); // (type (component))
    appendBytes(expected, { 0x42, 0x00 });       // (instance)
    auto const bytes = builder.build();
    CHECK(bytes == componentOf({ section(7, expected) }));
    CHECK(encodeComponent(decoded(bytes)) == bytes);
}

TEST_CASE("Builder.liftedIdentity")
{
    auto const module = ModuleBuilder {}
                            .type(isoline::binary::FuncType { .params = { isoline::binary::I32 },
                                                              .results = { isoline::binary::I32 } })
                            .function(0, {}, { 0x20, 0x00 })
                            .exportFunction("id", 0)
                            .build();
    auto builder = ComponentBuilder {};
    CHECK(builder.coreModule(module) == 0);
    CHECK(builder.coreInstantiate(0, {}) == 0);
    CHECK(builder.aliasCoreExport(0, "id", CoreSort::Func) == 0);
    CHECK(builder.type(TypeDefinition {
              .type = FunctionTypeDefinition { .asyncness = Asyncness::Sync,
                                               .params = { LabeledType { .label = "x",
                                                                         .type = PrimitiveValueType::U32 } },
                                               .result = PrimitiveValueType::U32 } })
          == 0);
    CHECK(builder.canonLift(0, {}, 0) == 0);
    CHECK(builder.exportItem("id", Sort::Func, 0) == 1);

    auto const bytes = builder.build();
    auto const pool = decoded(bytes);
    REQUIRE(pool.components.size() == 1);
    auto const& definitions = pool.components.front().definitions;
    auto kinds = std::vector<std::size_t> {};
    for (auto const& definition: definitions)
        kinds.push_back(definition.index());
    CHECK(kinds
          == std::vector<std::size_t> { kindOf<CoreModuleDefinition>(),
                                        kindOf<isoline::component::binary::CoreInstanceDefinition>(),
                                        kindOf<AliasDefinition>(),
                                        kindOf<TypeDefinition>(),
                                        kindOf<CanonDefinition>(),
                                        kindOf<isoline::component::binary::ExportDefinition>() });
    // The module is embedded verbatim, and the pool equals the builder's but for its byte ranges.
    auto const split = splitCoreModules(bytes, ComponentFeatureSet::wasi03(), ComponentLimits {});
    REQUIRE(split.has_value());
    REQUIRE(split->size() == 1);
    CHECK(Bytes(split->front().begin(), split->front().end()) == module);
    // After the preamble, the section's id and its one-byte size.
    REQUIRE(module.size() < 0x80);
    CHECK(definitions.front() == Definition { CoreModuleDefinition { .offset = 10, .size = module.size() } });
    CHECK(definitions.at(4) == builder.pool().components.front().definitions.at(4));
}

TEST_CASE("Builder.nestingAndIndexSpaces")
{
    auto inner = ComponentBuilder {};
    CHECK(inner.coreModule(taggedModule("b")) == 0);
    auto innermost = ComponentBuilder {};
    CHECK(innermost.coreModule(taggedModule("c")) == 0);
    auto const innermostIndex = inner.nested(std::move(innermost));
    CHECK(innermostIndex == 0);
    CHECK(inner.instanceType(TypeScopeBuilder {}) == 0);

    auto outer = ComponentBuilder {};
    auto const payload = bytesOf({ 0xAB });
    outer.custom("note", payload);
    CHECK(outer.coreModule(taggedModule("a")) == 0);
    auto const innerIndex = outer.nested(std::move(inner));
    CHECK(innerIndex == 0);
    CHECK(outer.coreModule(taggedModule("d")) == 1);
    CHECK(outer.coreInlineExports({ CoreInlineExport { .name = "g", .sort = CoreSort::Global, .index = 0 } })
          == 0);
    CHECK(outer.instantiate(0, { { "a", SortIndex { .sort = Sort::Func, .index = 0 } } }) == 0);
    CHECK(outer.inlineInstance({ { "x", SortIndex { .sort = Sort::Component, .index = 0 } } }) == 1);
    CHECK(outer.aliasExport(0, "f", Sort::Func) == 0);
    CHECK(outer.aliasOuter(Sort::Type, 0, 0) == 0);
    CHECK(outer.import("i", ExternDesc { .sort = Sort::Func, .index = 0 }) == 1);
    CHECK(outer.canonLower(0, { CanonOption { .kind = CanonOptionKind::Utf16, .index = 0 } }) == 0);
    CHECK(outer.canon(CanonDefinition { .builtin = isoline::component::binary::CanonBuiltin::ResourceNew,
                                        .immediates = { 0 },
                                        .options = {},
                                        .result = std::nullopt })
          == 1);
    CHECK(outer.coreType(CoreTypeDefinition { .type = isoline::binary::FuncType {} }) == 0);
    CHECK(outer.definition(TypeDefinition { .type = DefinedValueType { PrimitiveValueType::Bool } }) == 1);
    CHECK(outer.exportItem("c", Sort::Instance, 1, ExternDesc { .sort = Sort::Instance, .index = 0 }) == 2);
    CHECK(outer.count(Sort::CoreModule) == 2);
    CHECK(outer.count(Sort::Component) == 1);
    CHECK(outer.count(Sort::Value) == 0);
    CHECK_FALSE(introducedSort(isoline::component::binary::CustomDefinition {}).has_value());
    CHECK(introducedSort(ExportDeclaration { .name = {}, .type = ExternDesc { .sort = Sort::Component } })
          == Sort::Component);

    auto const bytes = outer.build();
    auto const pool = decoded(bytes);
    // The modules come out in ordinal order, pre-order through the adopted scopes.
    auto const split = splitCoreModules(bytes, ComponentFeatureSet::wasi03(), ComponentLimits {});
    REQUIRE(split.has_value());
    auto modules = std::vector<Bytes> {};
    for (auto const& module: *split)
        modules.emplace_back(module.begin(), module.end());
    CHECK(
        modules
        == std::vector<Bytes> { taggedModule("a"), taggedModule("b"), taggedModule("c"), taggedModule("d") });
    CHECK(coreModulePositions(pool)
          == std::vector<DefinitionPosition> { { .scope = 0, .definition = 1 },
                                               { .scope = 1, .definition = 0 },
                                               { .scope = 2, .definition = 0 },
                                               { .scope = 0, .definition = 3 } });
    // The built pool encodes as the decoded one: same scopes and kinds, ranges aside.
    CHECK(encodeComponent(outer.pool()) == bytes);
    CHECK(encodeComponent(pool) == bytes);
    auto const& custom =
        std::get<isoline::component::binary::CustomDefinition>(pool.components.front().definitions.front());
    CHECK(custom.name == "note");
    CHECK(custom.size == 1);
    CHECK(pool.components.at(3).kind == ScopeKind::InstanceType);
}
