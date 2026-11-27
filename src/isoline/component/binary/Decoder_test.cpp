// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <initializer_list>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <isoline/binary/DecodeError.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/component/Features.hpp>
#include <isoline/component/Limits.hpp>
#include <isoline/component/binary/Component.hpp>
#include <isoline/component/binary/Decoder.hpp>
#include <isoline/component/binary/testing/DecoderFixtures.hpp>

using isoline::component::ComponentFeature;
using isoline::component::ComponentFeatureSet;
using isoline::component::ComponentFeatureTable;
using isoline::component::ComponentLimit;
using isoline::component::ComponentLimits;
using isoline::component::binary::AliasDefinition;
using isoline::component::binary::AliasTarget;
using isoline::component::binary::Asyncness;
using isoline::component::binary::Attribute;
using isoline::component::binary::BorrowType;
using isoline::component::binary::CanonBuiltin;
using isoline::component::binary::CanonBuiltinRow;
using isoline::component::binary::CanonBuiltinTable;
using isoline::component::binary::CanonDefinition;
using isoline::component::binary::CanonOption;
using isoline::component::binary::CanonOptionKind;
using isoline::component::binary::ComponentPool;
using isoline::component::binary::ComponentTypeDefinition;
using isoline::component::binary::CoreExportDeclaration;
using isoline::component::binary::CoreExternType;
using isoline::component::binary::CoreImportDeclaration;
using isoline::component::binary::CoreInlineExport;
using isoline::component::binary::CoreInlineExports;
using isoline::component::binary::CoreInstanceDefinition;
using isoline::component::binary::CoreInstantiate;
using isoline::component::binary::CoreModuleDefinition;
using isoline::component::binary::CoreModuleTypeDefinition;
using isoline::component::binary::CoreOuterAliasDeclaration;
using isoline::component::binary::CoreSort;
using isoline::component::binary::CoreTypeDeclaration;
using isoline::component::binary::CoreTypeDefinition;
using isoline::component::binary::CoreValueKind;
using isoline::component::binary::CustomDefinition;
using isoline::component::binary::decodeComponent;
using isoline::component::binary::DecodeError;
using isoline::component::binary::DecodeErrorCode;
using isoline::component::binary::DecodeErrorMessages;
using isoline::component::binary::DefinedValueType;
using isoline::component::binary::DefinedValueTypeTable;
using isoline::component::binary::Definition;
using isoline::component::binary::EnumType;
using isoline::component::binary::exceededLimit;
using isoline::component::binary::ExportDeclaration;
using isoline::component::binary::ExportDefinition;
using isoline::component::binary::ExternalId;
using isoline::component::binary::ExternDesc;
using isoline::component::binary::FixedListType;
using isoline::component::binary::FlagsType;
using isoline::component::binary::FunctionTypeDefinition;
using isoline::component::binary::FutureType;
using isoline::component::binary::Immediate;
using isoline::component::binary::Implements;
using isoline::component::binary::ImportDefinition;
using isoline::component::binary::InlineExport;
using isoline::component::binary::InlineExports;
using isoline::component::binary::InstanceDefinition;
using isoline::component::binary::InstanceTypeDefinition;
using isoline::component::binary::Instantiate;
using isoline::component::binary::LabeledType;
using isoline::component::binary::LayerKind;
using isoline::component::binary::layerOf;
using isoline::component::binary::ListType;
using isoline::component::binary::MapType;
using isoline::component::binary::NameAttributes;
using isoline::component::binary::NestedComponent;
using isoline::component::binary::OptionType;
using isoline::component::binary::OwnType;
using isoline::component::binary::PrimitiveValueType;
using isoline::component::binary::PrimitiveValueTypeTable;
using isoline::component::binary::RecordType;
using isoline::component::binary::ResourceTypeDefinition;
using isoline::component::binary::ResultType;
using isoline::component::binary::ScopeKind;
using isoline::component::binary::Sort;
using isoline::component::binary::SortIndex;
using isoline::component::binary::specMessage;
using isoline::component::binary::splitCoreModules;
using isoline::component::binary::StreamType;
using isoline::component::binary::TupleType;
using isoline::component::binary::TypeBound;
using isoline::component::binary::TypeDefinition;
using isoline::component::binary::TypeIndex;
using isoline::component::binary::ValueTypeRef;
using isoline::component::binary::VariantCase;
using isoline::component::binary::VariantType;
using isoline::component::binary::VersionSuffix;
using isoline::component::binary::testing::aliasSection;
using isoline::component::binary::testing::appendBytes;
using isoline::component::binary::testing::appendName;
using isoline::component::binary::testing::Bytes;
using isoline::component::binary::testing::componentOf;
using isoline::component::binary::testing::componentPreamble;
using isoline::component::binary::testing::coreInstanceSection;
using isoline::component::binary::testing::DecoderFixture;
using isoline::component::binary::testing::decoderFixtures;
using isoline::component::binary::testing::exportSection;
using isoline::component::binary::testing::importSection;
using isoline::component::binary::testing::instanceSection;
using isoline::component::binary::testing::modulePreamble;
using isoline::component::binary::testing::nestedComponents;
using isoline::component::binary::testing::plainName;
using isoline::component::binary::testing::section;
using isoline::component::binary::testing::taggedModule;

namespace
{

/// Decodes @p bytes under WASI 0.3's features and default limits, unless told otherwise.
std::expected<ComponentPool, DecodeError> decode(Bytes const& bytes,
                                                 ComponentFeatureSet features = ComponentFeatureSet::wasi03(),
                                                 ComponentLimits const& limits = {})
{
    return decodeComponent(bytes, features, limits);
}

/// The error decoding @p bytes reports; the decode must fail.
DecodeError errorOf(Bytes const& bytes,
                    ComponentFeatureSet features = ComponentFeatureSet::wasi03(),
                    ComponentLimits const& limits = {})
{
    auto const result = decode(bytes, features, limits);
    REQUIRE_FALSE(result.has_value());
    return result.error();
}

/// The definitions of the outermost component of @p bytes; the decode must succeed.
std::vector<Definition> definitionsOf(Bytes const& bytes,
                                      ComponentFeatureSet features = ComponentFeatureSet::wasi03())
{
    auto const result = decode(bytes, features);
    REQUIRE(result.has_value());
    return result->components.front().definitions;
}

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

/// A component of one section @p id holding a vector of one element, @p element.
Bytes oneOf(uint8_t id, Bytes const& element)
{
    auto payload = Bytes { std::byte { 0x01 } };
    appendBytes(payload, element);
    return componentOf({ section(id, payload) });
}

/// The one type the type section of @p element defines; the decode must succeed.
TypeDefinition typeOf(Bytes const& element, ComponentFeatureSet features = ComponentFeatureSet::wasi03())
{
    auto const definitions = definitionsOf(oneOf(7, element), features);
    REQUIRE(definitions.size() == 1);
    REQUIRE(std::holds_alternative<TypeDefinition>(definitions.front()));
    return std::get<TypeDefinition>(definitions.front());
}

/// The defined value type @p element defines; the decode must succeed.
DefinedValueType valueTypeOf(Bytes const& element,
                             ComponentFeatureSet features = ComponentFeatureSet::wasi03())
{
    auto const type = typeOf(element, features);
    REQUIRE(std::holds_alternative<DefinedValueType>(type.type));
    return std::get<DefinedValueType>(type.type);
}

/// The one canonical definition @p element encodes; the decode must succeed.
CanonDefinition canonOf(Bytes const& element, ComponentFeatureSet features = ComponentFeatureSet::wasi03())
{
    auto const definitions = definitionsOf(oneOf(8, element), features);
    REQUIRE(definitions.size() == 1);
    REQUIRE(std::holds_alternative<CanonDefinition>(definitions.front()));
    return std::get<CanonDefinition>(definitions.front());
}

/// The message decoding @p bytes fails with.
std::string messageOf(Bytes const& bytes, ComponentFeatureSet features = ComponentFeatureSet::wasi03())
{
    return specMessage(errorOf(bytes, features));
}

/// The smallest bytes of each immediate a canonical built-in takes.
Bytes minimalImmediates(CanonBuiltinRow const& row)
{
    auto bytes = Bytes {};
    for (auto const kind: row.shape)
    {
        switch (kind)
        {
            case Immediate::None: break;
            case Immediate::SortByte:
            case Immediate::Index:
            case Immediate::Options:
            case Immediate::Async:
            case Immediate::Reserved:
            case Immediate::Shared: appendBytes(bytes, { 0x00 }); break;
            case Immediate::ResultList: appendBytes(bytes, { 0x01, 0x00 }); break;
            case Immediate::CoreValueType: appendBytes(bytes, { 0x7F }); break;
        }
    }
    return bytes;
}

/// The number of immediates of @p row's shape that CanonDefinition::immediates stores.
std::size_t storedImmediates(CanonBuiltinRow const& row)
{
    return static_cast<std::size_t>(std::ranges::count_if(row.shape, [](Immediate kind) {
        return kind == Immediate::Index || kind == Immediate::CoreValueType || kind == Immediate::Async
               || kind == Immediate::Shared;
    }));
}

} // namespace

TEST_CASE("Decoder.layerOf")
{
    CHECK(layerOf(modulePreamble()) == LayerKind::CoreModule);
    CHECK(layerOf(componentPreamble()) == LayerKind::Component);
    CHECK(layerOf(bytesOf({ 0x00, 0x61, 0x73, 0x6D, 0x0E, 0x00, 0x01, 0x00 })) == LayerKind::Unknown);
    CHECK(layerOf(bytesOf({ 0x00, 0x61, 0x73, 0x6D, 0x0D, 0x00, 0x01 })) == LayerKind::Unknown);
    CHECK(layerOf(bytesOf({ 0x00, 0x61, 0x73, 0x6E, 0x0D, 0x00, 0x01, 0x00 })) == LayerKind::Unknown);
    CHECK(layerOf(Bytes {}) == LayerKind::Unknown);
    // Only the preamble decides: what follows it does not.
    CHECK(layerOf(componentOf({ section(0x7F, {}) })) == LayerKind::Component);
}

TEST_CASE("Decoder.everyFixtureDecodes")
{
    for (auto const& fixture: decoderFixtures())
    {
        INFO(fixture.name);
        CHECK(decode(fixture.bytes).has_value());
    }
}

TEST_CASE("Decoder.inputIsCopied")
{
    auto input = componentOf({ section(1, taggedModule("a")) });
    auto const pool = decode(input);
    REQUIRE(pool.has_value());
    REQUIRE(pool->components.size() == 1);
    REQUIRE(pool->components.front().definitions.size() == 1);
    auto const& module = std::get<CoreModuleDefinition>(pool->components.front().definitions.front());
    CHECK(module.offset == 10); // the preamble, the section id and its size
    CHECK(module.size == taggedModule("a").size());

    std::ranges::fill(input, std::byte { 0xFF });
    REQUIRE(pool->bytes != nullptr);
    CHECK(pool->bytes->data() != input.data());
    auto const range = std::span { *pool->bytes }.subspan(module.offset, module.size);
    CHECK(std::ranges::equal(range, taggedModule("a")));
}

TEST_CASE("Decoder.splitCoreModulesOrdinals")
{
    // Module a before the nested component, b in it and c after it: pre-order is a, b, c.
    auto const input = componentOf({ section(1, taggedModule("a")),
                                     section(4, componentOf({ section(1, taggedModule("b")) })),
                                     section(1, taggedModule("c")) });
    auto const modules = splitCoreModules(input, ComponentFeatureSet::wasi03(), {});
    REQUIRE(modules.has_value());
    REQUIRE(modules->size() == 3);
    CHECK(std::ranges::equal((*modules)[0], taggedModule("a")));
    CHECK(std::ranges::equal((*modules)[1], taggedModule("b")));
    CHECK(std::ranges::equal((*modules)[2], taggedModule("c")));
    // Each module is a view into the caller's bytes.
    for (auto const& module: *modules)
    {
        CHECK(module.data() >= input.data());
        CHECK(module.data() + module.size() <= input.data() + input.size());
    }

    auto const pool = decode(input);
    REQUIRE(pool.has_value());
    REQUIRE(pool->components.size() == 2);
    CHECK(pool->components[0].definitions[1] == Definition { NestedComponent { .index = 1 } });

    // A malformed component splits into nothing but its error.
    auto const malformed =
        splitCoreModules(componentOf({ section(13, {}) }), ComponentFeatureSet::wasi03(), {});
    REQUIRE_FALSE(malformed.has_value());
    CHECK(malformed.error().code == DecodeErrorCode::MalformedSectionId);
}

TEST_CASE("Decoder.importsExportsAliasesInstances")
{
    auto const definitions = definitionsOf(componentOf(
        { coreInstanceSection(), instanceSection(), aliasSection(), importSection(), exportSection() }));
    REQUIRE(definitions.size() == 11);
    CHECK(definitions[0]
          == Definition { CoreInstanceDefinition {
              .expression =
                  CoreInstantiate { .module = 0, .arguments = { { .name = "m", .instance = 0 } } } } });
    CHECK(
        definitions[1]
        == Definition { CoreInstanceDefinition {
            .expression = CoreInlineExports {
                .exports = { CoreInlineExport { .name = "g", .sort = CoreSort::Global, .index = 2 } } } } });
    CHECK(definitions[2]
          == Definition { InstanceDefinition {
              .expression = Instantiate {
                  .component = 0,
                  .arguments = { { .name = "a", .item = { .sort = Sort::Func, .index = 0 } } } } } });
    CHECK(definitions[3]
          == Definition { InstanceDefinition {
              .expression = InlineExports {
                  .exports = { InlineExport { .name = NameAttributes { .name = "x", .attributes = {} },
                                              .item = { .sort = Sort::CoreModule, .index = 3 } } } } } });
    CHECK(definitions[4]
          == Definition { AliasDefinition {
              .sort = Sort::Func, .target = AliasTarget::Export, .instance = 0, .name = "f" } });
    CHECK(definitions[5]
          == Definition { AliasDefinition {
              .sort = Sort::CoreTable, .target = AliasTarget::CoreExport, .instance = 4, .name = "t" } });
    CHECK(definitions[6]
          == Definition { AliasDefinition { .sort = Sort::Type,
                                            .target = AliasTarget::Outer,
                                            .instance = 0,
                                            .name = {},
                                            .outerCount = 1,
                                            .outerIndex = 7 } });
    CHECK(definitions[7]
          == Definition { ImportDefinition { .name = NameAttributes { .name = "a", .attributes = {} },
                                             .type = ExternDesc { .sort = Sort::Func, .index = 0 } } });
    CHECK(definitions[8]
          == Definition { ImportDefinition {
              .name = NameAttributes { .name = "r", .attributes = {} },
              .type = ExternDesc { .sort = Sort::Type, .index = 0, .bound = TypeBound::SubResource } } });
    CHECK(definitions[9]
          == Definition { ExportDefinition { .name = NameAttributes { .name = "b", .attributes = {} },
                                             .item = { .sort = Sort::Func, .index = 0 },
                                             .type = std::nullopt } });
    CHECK(definitions[10]
          == Definition { ExportDefinition {
              .name = NameAttributes { .name = "c", .attributes = {} },
              .item = { .sort = Sort::Instance, .index = 1 },
              .type = ExternDesc { .sort = Sort::Type, .index = 2, .bound = TypeBound::Equal } } });

    // Indices are not checked here: a range error is validation's.
    auto far = Bytes { std::byte { 0x01 } };
    appendBytes(far, plainName("z"));
    appendBytes(far, { 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F });
    CHECK(definitionsOf(componentOf({ section(10, far) })).front()
          == Definition {
              ImportDefinition { .name = NameAttributes { .name = "z", .attributes = {} },
                                 .type = ExternDesc { .sort = Sort::Func, .index = 0xFFFF'FFFF } } });

    // The other extern type kinds, and the legacy plain-name byte 0x01, which wasm-tools reads as 0x00.
    auto kinds = Bytes { std::byte { 0x03 } };
    appendBytes(kinds, { 0x01 });
    appendName(kinds, "m");
    appendBytes(kinds, { 0x00, 0x11, 0x04 }); // (core module (type 4))
    appendBytes(kinds, plainName("c"));
    appendBytes(kinds, { 0x04, 0x05 }); // (component (type 5))
    appendBytes(kinds, plainName("i"));
    appendBytes(kinds, { 0x05, 0x06 }); // (instance (type 6))
    auto const imports = definitionsOf(componentOf({ section(10, kinds) }));
    REQUIRE(imports.size() == 3);
    CHECK(std::get<ImportDefinition>(imports[0]).name.name == "m");
    CHECK(std::get<ImportDefinition>(imports[0]).type == ExternDesc { .sort = Sort::CoreModule, .index = 4 });
    CHECK(std::get<ImportDefinition>(imports[1]).type == ExternDesc { .sort = Sort::Component, .index = 5 });
    CHECK(std::get<ImportDefinition>(imports[2]).type == ExternDesc { .sort = Sort::Instance, .index = 6 });

    // Every core extern kind, and every outer alias kind.
    auto coreExports = Bytes {};
    appendBytes(coreExports, { 0x01, 0x01, 0x05 }); // 1 instance of 5 inline exports
    for (auto const kind: { 0x00, 0x01, 0x02, 0x03, 0x04 })
    {
        appendName(coreExports, "e");
        appendBytes(coreExports, { static_cast<uint8_t>(kind), 0x00 });
    }
    auto const coreInstances = definitionsOf(componentOf({ section(2, coreExports) }));
    auto const& exports =
        std::get<CoreInlineExports>(std::get<CoreInstanceDefinition>(coreInstances.front()).expression);
    REQUIRE(exports.exports.size() == 5);
    CHECK(exports.exports[0].sort == CoreSort::Func);
    CHECK(exports.exports[1].sort == CoreSort::Table);
    CHECK(exports.exports[2].sort == CoreSort::Memory);
    CHECK(exports.exports[3].sort == CoreSort::Global);
    CHECK(exports.exports[4].sort == CoreSort::Tag);

    auto outers = bytesOf({ 0x04,
                            0x00,
                            0x10,
                            0x02,
                            0x00,
                            0x00,
                            0x00,
                            0x11,
                            0x02,
                            0x00,
                            0x00,
                            0x03,
                            0x02,
                            0x00,
                            0x00,
                            0x04,
                            0x02,
                            0x00,
                            0x00 });
    auto const aliases = definitionsOf(componentOf({ section(6, outers) }));
    REQUIRE(aliases.size() == 4);
    CHECK(std::get<AliasDefinition>(aliases[0]).sort == Sort::CoreType);
    CHECK(std::get<AliasDefinition>(aliases[1]).sort == Sort::CoreModule);
    CHECK(std::get<AliasDefinition>(aliases[2]).sort == Sort::Type);
    CHECK(std::get<AliasDefinition>(aliases[3]).sort == Sort::Component);

    // A custom section keeps its name and the range of its payload.
    auto custom = Bytes {};
    appendName(custom, "hi");
    appendBytes(custom, { 0x01, 0x02, 0x03 });
    CHECK(definitionsOf(componentOf({ section(0, custom) })).front()
          == Definition { CustomDefinition { .name = "hi", .offset = 13, .size = 3 } });
}

TEST_CASE("Decoder.nameAttributes")
{
    auto const& fixtures = decoderFixtures();
    auto const fixture =
        std::ranges::find(fixtures, std::string_view { "nameAttributes" }, &DecoderFixture::name);
    REQUIRE(fixture != fixtures.end());
    CHECK(definitionsOf(fixture->bytes).front()
          == Definition { ImportDefinition {
              .name = NameAttributes { .name = "x",
                                       .attributes = { Implements { .interfaceName = "ns:p/i@1.0.0" },
                                                       ExternalId { .id = "x" } } },
              .type = ExternDesc { .sort = Sort::Instance, .index = 0 } } });

    // Without the feature, the attributed form is refused at its byte, in wasm-tools' words.
    auto const refused = errorOf(fixture->bytes, ComponentFeatureSet::defaults());
    CHECK(
        refused
        == DecodeError { DecodeErrorCode::FeatureDisabled, 11, std::nullopt, ComponentFeature::Implements });
    CHECK(specMessage(refused) == "the `cm-implements` feature is not active (🏷️)");

    // With any attribute's feature, the form decodes, every attribute in it: each attribute's own
    // gate is validation's, as in wasmparser (Validator.externDeclarations).
    for (auto const feature: { ComponentFeature::Implements, ComponentFeature::CanonicalInterfaceNames })
    {
        INFO(static_cast<int>(feature));
        auto const decoded = definitionsOf(fixture->bytes, ComponentFeatureSet::defaults().with(feature));
        CHECK(std::get<ImportDefinition>(decoded.front()).name.attributes.size() == 2);
    }

    // A version suffix (🔗) decodes too.
    auto suffixed = bytesOf({ 0x01, 0x02 });
    appendName(suffixed, "x");
    appendBytes(suffixed, { 0x01, 0x01 });
    appendName(suffixed, ".1");
    appendBytes(suffixed, { 0x05, 0x00 });
    CHECK(std::get<ImportDefinition>(definitionsOf(componentOf({ section(10, suffixed) })).front())
              .name.attributes
          == std::vector<Attribute> { VersionSuffix { .suffix = ".1" } });

    // Each kind at most once.
    auto twice = bytesOf({ 0x01, 0x02 });
    appendName(twice, "x");
    appendBytes(twice, { 0x02, 0x00 });
    appendName(twice, "a");
    appendBytes(twice, { 0x00 });
    appendName(twice, "b");
    appendBytes(twice, { 0x05, 0x00 });
    auto const duplicate = errorOf(componentOf({ section(10, twice) }));
    CHECK(duplicate == DecodeError { DecodeErrorCode::DuplicateNameAttribute, 18, 0 });
    CHECK(specMessage(duplicate) == "duplicate 'implements' option in name");

    // Unknown name forms and attribute bytes.
    auto badForm = bytesOf({ 0x01, 0x03 });
    appendName(badForm, "x");
    appendBytes(badForm, { 0x00 });
    CHECK(specMessage(errorOf(componentOf({ section(10, badForm) })))
          == "invalid leading byte (0x3) for component name");
    auto badAttribute = bytesOf({ 0x01, 0x02 });
    appendName(badAttribute, "x");
    appendBytes(badAttribute, { 0x01, 0x03, 0x00 });
    auto const attribute = errorOf(componentOf({ section(10, badAttribute) }));
    CHECK(attribute.offset == 15);
    CHECK(specMessage(attribute) == "invalid leading byte (0x3) for name option");
}

TEST_CASE("Decoder.startAndValuesRefused")
{
    for (auto const id: { uint8_t { 9 }, uint8_t { 12 } })
    {
        INFO(static_cast<int>(id));
        auto const component = componentOf({ section(id, bytesOf({ 0x00, 0x00, 0x00 })) });
        auto const error = errorOf(component);
        // At the section's contents, as wasm-tools reports a start function.
        CHECK(
            error
            == DecodeError { DecodeErrorCode::FeatureDisabled, 10, std::nullopt, ComponentFeature::Values });
        CHECK(specMessage(error) == "support for component model `value`s is not enabled (🪙)");
        // P11 refuses 🪙 even when the set enables it.
        CHECK(errorOf(component, ComponentFeatureSet::wasi03().with(ComponentFeature::Values)) == error);
    }

    // An item of the value sort decodes, as wasmparser reads it, and validation refuses it; a value
    // extern type, whose value bound the AST has no shape for, is refused here.
    auto valueExport = Bytes { std::byte { 0x01 } };
    appendBytes(valueExport, plainName("v"));
    appendBytes(valueExport, { 0x02, 0x00, 0x00 });
    CHECK(std::get<ExportDefinition>(definitionsOf(componentOf({ section(11, valueExport) })).front()).item
          == SortIndex { .sort = Sort::Value, .index = 0 });
    auto valueImport = Bytes { std::byte { 0x01 } };
    appendBytes(valueImport, plainName("v"));
    appendBytes(valueImport, { 0x02, 0x01, 0x79 });
    CHECK(errorOf(componentOf({ section(10, valueImport) }))
          == DecodeError { DecodeErrorCode::FeatureDisabled, 14, std::nullopt, ComponentFeature::Values });
}

TEST_CASE("Decoder.deepNestingIsRefused")
{
    // Built iteratively and decoded on the decoder's own stack of frames: no stack overflow.
    auto const deep = errorOf(nestedComponents(100'000));
    CHECK(deep.code == DecodeErrorCode::LimitExceeded);
    CHECK(exceededLimit(deep) == ComponentLimit::Nesting);
    CHECK(specMessage(deep) == "nesting exceeds the limit of 100 levels");

    // The limit counts the outermost component: 100 levels decode, 101 do not.
    auto const atLimit = decode(nestedComponents(100));
    REQUIRE(atLimit.has_value());
    CHECK(atLimit->components.size() == 100);
    CHECK(exceededLimit(errorOf(nestedComponents(101))) == ComponentLimit::Nesting);

    // Reported where the component past the limit starts: components of 28, 18 and 8 bytes nest at
    // offsets 0, 10 and 20.
    auto limits = ComponentLimits {};
    limits.maxNesting = 2;
    CHECK(decode(nestedComponents(2), ComponentFeatureSet::wasi03(), limits).has_value());
    auto const third = errorOf(nestedComponents(3), ComponentFeatureSet::wasi03(), limits);
    CHECK(exceededLimit(third) == ComponentLimit::Nesting);
    CHECK(third.offset == 20);

    // A limit below one refuses even the outermost component.
    limits.maxNesting = 0;
    auto const none = errorOf(componentPreamble(), ComponentFeatureSet::wasi03(), limits);
    CHECK(none.offset == 0);
    CHECK(exceededLimit(none) == ComponentLimit::Nesting);
}

TEST_CASE("Decoder.lyingCounts")
{
    // A count of 2^32 - 1 with 3 bytes left fails before anything is read or reserved for it.
    auto const error =
        errorOf(componentOf({ section(10, bytesOf({ 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x00, 0x01, 0x61 })) }));
    CHECK(error.code == DecodeErrorCode::UnexpectedEnd);
    CHECK(error.offset == 10);
    CHECK(specMessage(error) == "unexpected end-of-file");

    // A count no larger than the bytes left reads elements until they run out.
    auto const honest =
        errorOf(componentOf({ section(10, bytesOf({ 0x02, 0x00, 0x01, 0x61, 0x01, 0x00 })) }));
    CHECK(honest.code == DecodeErrorCode::UnexpectedEnd);
    CHECK(honest.offset == 16);

    // The same for a name, and for every vector inside a definition.
    CHECK(errorOf(componentOf({ section(0, bytesOf({ 0x05, 0x61, 0x62 })) })).code
          == DecodeErrorCode::UnexpectedEnd);
    CHECK(errorOf(componentOf({ section(2, bytesOf({ 0x01, 0x00, 0x00, 0xFF, 0xFF, 0x03 })) })).code
          == DecodeErrorCode::UnexpectedEnd);
    CHECK(errorOf(componentOf({ section(5, bytesOf({ 0x01, 0x01, 0xFF, 0xFF, 0x03 })) })).code
          == DecodeErrorCode::UnexpectedEnd);
}

TEST_CASE("Decoder.componentBytesCap")
{
    auto limits = ComponentLimits {};
    limits.maxComponentBytes = 16;
    auto name = Bytes {};
    appendName(name, "abcde");
    auto const atLimit = componentOf({ section(0, name) });
    REQUIRE(atLimit.size() == 16);
    CHECK(decode(atLimit, ComponentFeatureSet::wasi03(), limits).has_value());

    appendBytes(name, { 0x00 });
    auto const overLimit = componentOf({ section(0, name) });
    REQUIRE(overLimit.size() == 17);
    auto const error = errorOf(overLimit, ComponentFeatureSet::wasi03(), limits);
    CHECK(exceededLimit(error) == ComponentLimit::ComponentBytes);
    CHECK(error.offset == 0);
    CHECK(specMessage(error) == "component size exceeds the limit of 16 bytes");
}

TEST_CASE("Decoder.definitionsAndNameCaps")
{
    auto custom = Bytes {};
    appendName(custom, "abcd");
    auto limits = ComponentLimits {};
    limits.maxDefinitions = 2;
    limits.maxNameBytes = 4;

    // Definitions count per component, of every section; the nested component has its own count.
    CHECK(
        decode(componentOf({ section(0, custom), section(0, custom) }), ComponentFeatureSet::wasi03(), limits)
            .has_value());
    auto const three = errorOf(componentOf({ section(0, custom), section(0, custom), section(0, custom) }),
                               ComponentFeatureSet::wasi03(),
                               limits);
    CHECK(exceededLimit(three) == ComponentLimit::Definitions);
    CHECK(three.offset == 24);
    CHECK(specMessage(three) == "definitions count exceeds limit of 2");
    CHECK(decode(componentOf({ section(0, custom),
                               section(4, componentOf({ section(0, custom), section(0, custom) })) }),
                 ComponentFeatureSet::wasi03(),
                 limits)
              .has_value());

    // A name longer than the limit is refused before it is copied.
    auto longName = Bytes {};
    appendName(longName, "abcde");
    auto const name = errorOf(componentOf({ section(0, longName) }), ComponentFeatureSet::wasi03(), limits);
    CHECK(exceededLimit(name) == ComponentLimit::NameBytes);
    CHECK(name.offset == 10);
    CHECK(specMessage(name) == "string size out of bounds");
    auto longImport = Bytes { std::byte { 0x01 } };
    appendBytes(longImport, plainName("abcde"));
    appendBytes(longImport, { 0x01, 0x00 });
    CHECK(exceededLimit(
              errorOf(componentOf({ section(10, longImport) }), ComponentFeatureSet::wasi03(), limits))
          == ComponentLimit::NameBytes);
}

TEST_CASE("Decoder.decodedItemsCap")
{
    auto custom = Bytes {};
    appendName(custom, "");
    auto limits = ComponentLimits {};
    limits.maxDecodedItems = 3;

    // Counted across the whole pool: a sibling costs two items, its nested component and its custom
    // section, so two of them hold four, past three, though no component holds more than two
    // definitions.
    auto const sibling = section(4, componentOf({ section(0, custom) }));
    CHECK(decode(componentOf({ sibling }), ComponentFeatureSet::wasi03(), limits).has_value());
    auto const siblings = errorOf(componentOf({ sibling, sibling }), ComponentFeatureSet::wasi03(), limits);
    CHECK(exceededLimit(siblings) == ComponentLimit::DecodedItems);
    CHECK(specMessage(siblings) == "decoded items count exceeds limit of 3");

    // The elements of a definition's vectors count too: one instance of three inline exports is four.
    auto exports = Bytes {};
    appendBytes(exports, { 0x01, 0x01, 0x03 });
    for ([[maybe_unused]] auto const index: { 0, 1, 2 })
    {
        appendName(exports, "");
        appendBytes(exports, { 0x00, 0x00 });
    }
    auto const inner = errorOf(componentOf({ section(2, exports) }), ComponentFeatureSet::wasi03(), limits);
    CHECK(exceededLimit(inner) == ComponentLimit::DecodedItems);
    CHECK(inner.offset == 19); // the third export
    limits.maxDecodedItems = 4;
    CHECK(decode(componentOf({ section(2, exports) }), ComponentFeatureSet::wasi03(), limits).has_value());
}

TEST_CASE("Decoder.preambles")
{
    struct Case
    {
        Bytes bytes;
        DecodeErrorCode code;
    };
    auto const cases = std::vector<Case> {
        { .bytes = Bytes {}, .code = DecodeErrorCode::UnexpectedEnd },
        { .bytes = bytesOf({ 0x00, 0x61, 0x73 }), .code = DecodeErrorCode::UnexpectedEnd },
        { .bytes = bytesOf({ 0x00, 0x61, 0x73, 0x6D, 0x0D, 0x00, 0x01 }),
          .code = DecodeErrorCode::UnexpectedEnd },
        { .bytes = bytesOf({ 0x61, 0x73, 0x6D, 0x00, 0x0D, 0x00, 0x01, 0x00 }),
          .code = DecodeErrorCode::MagicHeaderNotDetected },
        { .bytes = bytesOf({ 0x00, 0x61, 0x73, 0x6D, 0x0C, 0x00, 0x01, 0x00 }),
          .code = DecodeErrorCode::UnknownBinaryVersion },
        { .bytes = bytesOf({ 0x00, 0x61, 0x73, 0x6D, 0x0D, 0x00, 0x02, 0x00 }),
          .code = DecodeErrorCode::UnknownBinaryVersion },
        { .bytes = modulePreamble(), .code = DecodeErrorCode::ExpectedComponentHeader },
    };
    for (auto const& entry: cases)
        CHECK(errorOf(entry.bytes).code == entry.code);

    auto const version = errorOf(bytesOf({ 0x00, 0x61, 0x73, 0x6D, 0x0E, 0x00, 0x01, 0x00 }));
    CHECK(version.offset == 4);
    CHECK(specMessage(version) == "unknown binary version:    0x1000e");

    // A module section must hold a module, and a component section a component.
    auto const module = errorOf(componentOf({ section(1, componentPreamble()) }));
    CHECK(module == DecodeError { DecodeErrorCode::ExpectedModuleHeader, 10 });
    CHECK(specMessage(module) == "expected a version header for a module");
    CHECK(errorOf(componentOf({ section(4, modulePreamble()) }))
          == DecodeError { DecodeErrorCode::ExpectedComponentHeader, 10 });
    CHECK(errorOf(componentOf({ section(1, bytesOf({ 0x00, 0x61, 0x73, 0x6D })) })).code
          == DecodeErrorCode::UnexpectedEnd);
}

TEST_CASE("Decoder.sectionFraming")
{
    // Unknown ids, with the id named while it is a byte below 0x80.
    auto const thirteen = errorOf(componentOf({ section(13, {}) }));
    CHECK(thirteen == DecodeError { DecodeErrorCode::MalformedSectionId, 10, 13 });
    CHECK(specMessage(thirteen) == "malformed section id: 13");
    auto const high = errorOf(componentOf({ bytesOf({ 0xFF }) }));
    CHECK(high == DecodeError { DecodeErrorCode::MalformedSectionId, 8 });
    CHECK(specMessage(high) == "malformed section id");

    // A section larger than the component runs out at its end; larger than its nested component's
    // section, it is too large.
    CHECK(errorOf(componentOf({ bytesOf({ 0x0A, 0x03, 0x00 }) })).code == DecodeErrorCode::UnexpectedEnd);
    CHECK(errorOf(componentOf({ bytesOf({ 0x00 }) })).code == DecodeErrorCode::UnexpectedEnd);
    auto nested = componentPreamble();
    appendBytes(nested, { 0x00, 0x05, 0x00 });
    auto const tooLarge = errorOf(componentOf({ section(4, nested) }));
    CHECK(tooLarge == DecodeError { DecodeErrorCode::SectionTooLarge, 19 });
    CHECK(specMessage(tooLarge) == "section too large");

    // Contents left over after a section's vector.
    auto const mismatch = errorOf(componentOf({ section(10, bytesOf({ 0x00, 0x00 })) }));
    CHECK(mismatch == DecodeError { DecodeErrorCode::SectionSizeMismatch, 11 });
    CHECK(specMessage(mismatch) == "section size mismatch: unexpected data at the end of the section");

    // The core type, type and canonical sections: an empty vector holds nothing, and what is left
    // after it is a size mismatch.
    for (auto const id: { uint8_t { 3 }, uint8_t { 7 }, uint8_t { 8 } })
    {
        INFO(static_cast<int>(id));
        CHECK(definitionsOf(componentOf({ section(id, bytesOf({ 0x00 })) })).empty());
        CHECK(errorOf(componentOf({ section(id, bytesOf({ 0x00, 0x00 })) }))
              == DecodeError { DecodeErrorCode::SectionSizeMismatch, 11 });
    }

    // Every section may repeat, in any order.
    CHECK(
        definitionsOf(componentOf({ importSection(), section(0, bytesOf({ 0x00 })), importSection() })).size()
        == 5);
}

TEST_CASE("Decoder.leadingBytes")
{
    struct Case
    {
        uint8_t id;
        Bytes payload;
        std::string message;
        uint64_t offset;
    };
    auto withName = [](std::initializer_list<uint8_t> before, std::initializer_list<uint8_t> after) {
        auto bytes = bytesOf(before);
        appendName(bytes, "f");
        appendBytes(bytes, after);
        return bytes;
    };
    auto const cases = std::vector<Case> {
        { .id = 2,
          .payload = bytesOf({ 0x01, 0x02 }),
          .message = "invalid leading byte (0x2) for core instance",
          .offset = 11 },
        { .id = 2,
          .payload = withName({ 0x01, 0x00, 0x00, 0x01 }, { 0x00, 0x00 }),
          .message = "invalid leading byte (0x0) for instantiation arg kind",
          .offset = 16 },
        { .id = 2,
          .payload = withName({ 0x01, 0x01, 0x01 }, { 0x10, 0x00 }),
          .message = "invalid leading byte (0x10) for external kind",
          .offset = 15 },
        { .id = 5,
          .payload = bytesOf({ 0x01, 0x02 }),
          .message = "invalid leading byte (0x2) for instance",
          .offset = 11 },
        { .id = 5,
          .payload = withName({ 0x01, 0x00, 0x00, 0x01 }, { 0x06, 0x00 }),
          .message = "invalid leading byte (0x6) for component external kind",
          .offset = 16 },
        { .id = 5,
          .payload = withName({ 0x01, 0x00, 0x00, 0x01 }, { 0x00, 0x10, 0x00 }),
          .message = "invalid leading byte (0x10) for component external kind",
          .offset = 17 },
        { .id = 6,
          .payload = bytesOf({ 0x01, 0x01, 0x03 }),
          .message = "invalid leading byte (0x3) for alias",
          .offset = 12 },
        { .id = 6,
          .payload = withName({ 0x01, 0x01, 0x01, 0x00 }, {}),
          .message = "invalid leading byte (0x1) for core instance export kind",
          .offset = 11 },
        { .id = 6,
          .payload = withName({ 0x01, 0x00, 0x05, 0x01, 0x00 }, {}),
          .message = "invalid leading byte (0x5) for external kind",
          .offset = 11 },
        { .id = 6,
          .payload = bytesOf({ 0x01, 0x05, 0x02, 0x00, 0x00 }),
          .message = "invalid leading byte (0x5) for component outer alias kind",
          .offset = 11 },
        { .id = 6,
          .payload = bytesOf({ 0x01, 0x00, 0x01, 0x02, 0x00, 0x00 }),
          .message = "invalid leading byte (0x1) for component outer alias kind",
          .offset = 12 },
        { .id = 10,
          .payload = withName({ 0x01, 0x00 }, { 0x03, 0x02 }),
          .message = "invalid leading byte (0x2) for type bound",
          .offset = 15 },
        { .id = 11,
          .payload = withName({ 0x01, 0x00 }, { 0x01, 0x00, 0x02 }),
          .message = "invalid leading byte (0x2) for optional component export type",
          .offset = 16 },
        { .id = 10,
          .payload = withName({ 0x01, 0x00 }, { 0x06, 0x00 }),
          .message = "invalid leading byte (0x6) for component external kind",
          .offset = 14 },
    };
    for (auto const& entry: cases)
    {
        INFO(entry.message);
        auto const error = errorOf(componentOf({ section(entry.id, entry.payload) }));
        CHECK(specMessage(error) == entry.message);
        CHECK(error.offset == entry.offset);
    }
}

TEST_CASE("Decoder.coreReadErrors")
{
    // ByteReader's errors, in wasm-tools' words.
    auto const tooLarge = errorOf(componentOf({ bytesOf({ 0x0A, 0x81, 0x80, 0x80, 0x80, 0x70, 0x00 }) }));
    CHECK(tooLarge.code == DecodeErrorCode::IntegerTooLarge);
    CHECK(specMessage(tooLarge) == "invalid var_u32: integer too large");
    auto const tooLong = errorOf(componentOf({ bytesOf({ 0x0A, 0x81, 0x80, 0x80, 0x80, 0x80, 0x00 }) }));
    CHECK(tooLong.code == DecodeErrorCode::IntegerRepresentationTooLong);
    CHECK(specMessage(tooLong) == "invalid var_u32: integer representation too long");
    auto const utf8 = errorOf(componentOf({ section(0, bytesOf({ 0x02, 0xFF, 0xFE })) }));
    CHECK(utf8.code == DecodeErrorCode::MalformedUtf8Encoding);
    CHECK(specMessage(utf8) == "malformed UTF-8 encoding");
    // A padded but in-range LEB128 is fine.
    CHECK(decode(componentOf({ bytesOf({ 0x0A, 0x81, 0x80, 0x80, 0x80, 0x00, 0x00 }) })).has_value());

    // Every row has a text.
    for (auto const& row: DecodeErrorMessages)
        CHECK_FALSE(row.text.empty());
}

TEST_CASE("Decoder.everyCanonRowDecodes")
{
    // The smallest bytes after each opcode, and how many immediates they store, transcribed from
    // Binary.md's canon production at the pin rather than derived from a row's shape.
    struct Minimal
    {
        uint8_t opcode;
        std::vector<uint8_t> bytes;
        std::size_t stored;
    };
    auto const minimal = std::vector<Minimal> {
        { .opcode = 0x00, .bytes = { 0x00, 0x00, 0x00, 0x00 }, .stored = 2 }, // lift: 0x00 f opts ft
        { .opcode = 0x01, .bytes = { 0x00, 0x00, 0x00 }, .stored = 1 },       // lower: 0x00 f opts
        { .opcode = 0x02, .bytes = { 0x00 }, .stored = 1 },                   // resource.new rt
        { .opcode = 0x03, .bytes = { 0x00 }, .stored = 1 },                   // resource.drop rt
        { .opcode = 0x04, .bytes = { 0x00 }, .stored = 1 },                   // resource.rep rt
        { .opcode = 0x05, .bytes = {}, .stored = 0 },                         // task.cancel
        { .opcode = 0x06, .bytes = { 0x00 }, .stored = 1 },                   // subtask.cancel async?
        { .opcode = 0x09, .bytes = { 0x01, 0x00, 0x00 }, .stored = 0 },       // task.return rs opts
        { .opcode = 0x0A, .bytes = { 0x7F, 0x00 }, .stored = 2 },             // context.get t i
        { .opcode = 0x0B, .bytes = { 0x7F, 0x00 }, .stored = 2 },             // context.set t i
        { .opcode = 0x0C, .bytes = { 0x00 }, .stored = 0 },                   // thread.yield 0x00
        { .opcode = 0x0D, .bytes = {}, .stored = 0 },                         // subtask.drop
        { .opcode = 0x0E, .bytes = { 0x00 }, .stored = 1 },                   // stream.new t
        { .opcode = 0x0F, .bytes = { 0x00, 0x00 }, .stored = 1 },             // stream.read t opts
        { .opcode = 0x10, .bytes = { 0x00, 0x00 }, .stored = 1 },             // stream.write t opts
        { .opcode = 0x11, .bytes = { 0x00, 0x00 }, .stored = 2 },             // stream.cancel-read t async?
        { .opcode = 0x12, .bytes = { 0x00, 0x00 }, .stored = 2 },             // stream.cancel-write t async?
        { .opcode = 0x13, .bytes = { 0x00 }, .stored = 1 },                   // stream.drop-readable t
        { .opcode = 0x14, .bytes = { 0x00 }, .stored = 1 },                   // stream.drop-writable t
        { .opcode = 0x15, .bytes = { 0x00 }, .stored = 1 },                   // future.new t
        { .opcode = 0x16, .bytes = { 0x00, 0x00 }, .stored = 1 },             // future.read t opts
        { .opcode = 0x17, .bytes = { 0x00, 0x00 }, .stored = 1 },             // future.write t opts
        { .opcode = 0x18, .bytes = { 0x00, 0x00 }, .stored = 2 },             // future.cancel-read t async?
        { .opcode = 0x19, .bytes = { 0x00, 0x00 }, .stored = 2 },             // future.cancel-write t async?
        { .opcode = 0x1A, .bytes = { 0x00 }, .stored = 1 },                   // future.drop-readable t
        { .opcode = 0x1B, .bytes = { 0x00 }, .stored = 1 },                   // future.drop-writable t
        { .opcode = 0x1C, .bytes = { 0x00 }, .stored = 0 },                   // error-context.new opts
        { .opcode = 0x1D, .bytes = { 0x00 }, .stored = 0 },             // error-context.debug-message opts
        { .opcode = 0x1E, .bytes = {}, .stored = 0 },                   // error-context.drop
        { .opcode = 0x1F, .bytes = {}, .stored = 0 },                   // waitable-set.new
        { .opcode = 0x20, .bytes = { 0x00, 0x00 }, .stored = 1 },       // waitable-set.wait 0x00 m
        { .opcode = 0x21, .bytes = { 0x00, 0x00 }, .stored = 1 },       // waitable-set.poll 0x00 m
        { .opcode = 0x22, .bytes = {}, .stored = 0 },                   // waitable-set.drop
        { .opcode = 0x23, .bytes = {}, .stored = 0 },                   // waitable.join
        { .opcode = 0x24, .bytes = {}, .stored = 0 },                   // backpressure.inc
        { .opcode = 0x25, .bytes = {}, .stored = 0 },                   // backpressure.dec
        { .opcode = 0x26, .bytes = {}, .stored = 0 },                   // thread.index
        { .opcode = 0x27, .bytes = { 0x00, 0x00 }, .stored = 2 },       // thread.new-indirect ft t
        { .opcode = 0x28, .bytes = {}, .stored = 0 },                   // thread.resume-later
        { .opcode = 0x29, .bytes = { 0x00 }, .stored = 0 },             // thread.suspend 0x00
        { .opcode = 0x2A, .bytes = { 0x00 }, .stored = 0 },             // thread.suspend-then-resume 0x00
        { .opcode = 0x2B, .bytes = { 0x00 }, .stored = 0 },             // thread.yield-then-resume 0x00
        { .opcode = 0x2C, .bytes = { 0x00 }, .stored = 0 },             // thread.suspend-then-promote 0x00
        { .opcode = 0x2D, .bytes = { 0x00 }, .stored = 0 },             // thread.yield-then-promote 0x00
        { .opcode = 0x2E, .bytes = { 0x00 }, .stored = 1 },             // stream.forward t
        { .opcode = 0x2F, .bytes = { 0x00 }, .stored = 1 },             // future.forward t
        { .opcode = 0x40, .bytes = { 0x00, 0x00 }, .stored = 2 },       // thread.spawn-ref sh? ft
        { .opcode = 0x41, .bytes = { 0x00, 0x00, 0x00 }, .stored = 3 }, // thread.spawn-indirect sh? ft t
        { .opcode = 0x42, .bytes = { 0x00 }, .stored = 1 },             // thread.available-parallelism sh?
    };
    REQUIRE(minimal.size() == CanonBuiltinTable.size());
    for (auto const index: std::views::iota(std::size_t { 0 }, minimal.size()))
    {
        auto const& row = CanonBuiltinTable[index];
        auto const& expected = minimal[index];
        INFO(row.text);
        REQUIRE(row.opcode == expected.opcode);
        auto literal = Bytes {};
        appendBytes(literal, expected.bytes);
        // The shape's smallest encoding is Binary.md's, for the rows P11 refuses too.
        CHECK(minimalImmediates(row) == literal);
        CHECK(storedImmediates(row) == expected.stored);
        auto element = Bytes { std::byte { row.opcode } };
        appendBytes(element, literal);
        // Every row decodes whatever the set, as wasmparser's reader reads it: its gate is
        // validation's (Validator.deferredDefinitions).
        for (auto const features: { ComponentFeatureSet::defaults(), everyFeature() })
        {
            auto const canon = canonOf(element, features);
            CHECK(canon.builtin == row.builtin);
            CHECK(canon.immediates.size() == expected.stored);
            CHECK(canon.options.empty());
        }
    }
}

TEST_CASE("Decoder.canonTableSortedAndComplete")
{
    // Binary.md's canon production at the pin, opcode for opcode.
    auto const expected = std::vector<uint8_t> {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12,
        0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23,
        0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F, 0x40, 0x41, 0x42,
    };
    auto opcodes = std::vector<uint8_t> {};
    for (auto const& row: CanonBuiltinTable)
        opcodes.push_back(row.opcode);
    CHECK(opcodes == expected);
    CHECK(std::ranges::is_sorted(opcodes));

    // And gate for gate: lift, lower and the resource built-ins are ungated.
    constexpr auto Async = ComponentFeature::Async;
    constexpr auto ErrorContext = ComponentFeature::ErrorContext;
    constexpr auto Threading = ComponentFeature::Threading;
    constexpr auto Forward = ComponentFeature::Forward;
    constexpr auto Shared = ComponentFeature::SharedThreading;
    auto const gates = std::vector<std::optional<ComponentFeature>> {
        std::nullopt, std::nullopt, std::nullopt, std::nullopt,
        std::nullopt, // lift … resource.rep
        Async,        Async,        Async,        Async,
        Async, // task.cancel … context.set
        Async,        Async,        Async,        Async,
        Async, // thread.yield … stream.write
        Async,        Async,        Async,        Async,
        Async, // stream.cancel-read … future.new
        Async,        Async,        Async,        Async,
        Async, // future.read … future.drop-readable
        Async,        ErrorContext, ErrorContext, ErrorContext,
        Async, // future.drop-writable … waitable-set.new
        Async,        Async,        Async,        Async,
        Async, // waitable-set.wait … backpressure.inc
        Async,        Threading,    Threading,    Threading,
        Threading, // backpressure.dec … thread.suspend
        Threading,    Threading,    Threading,    Threading,
        Forward,                                          // … stream.forward
        Forward,      Shared,       Shared,       Shared, // future.forward … available-parallelism
    };
    auto tableGates = std::vector<std::optional<ComponentFeature>> {};
    for (auto const& row: CanonBuiltinTable)
        tableGates.push_back(row.gate);
    CHECK(tableGates == gates);

    // The opcodes between are no built-in; nor are the sort bytes but func's.
    CHECK(messageOf(oneOf(8, bytesOf({ 0x07 }))) == "invalid leading byte (0x7) for canonical function");
    CHECK(messageOf(oneOf(8, bytesOf({ 0x43 }))) == "invalid leading byte (0x43) for canonical function");
    CHECK(messageOf(oneOf(8, bytesOf({ 0x30 }))) == "invalid leading byte (0x30) for canonical function");
    auto const lift = errorOf(oneOf(8, bytesOf({ 0x00, 0x01, 0x00, 0x00, 0x00 })));
    CHECK(lift.offset == 12);
    CHECK(specMessage(lift) == "invalid leading byte (0x1) for canonical function lift");
    CHECK(messageOf(oneOf(8, bytesOf({ 0x01, 0x01, 0x00, 0x00 })))
          == "invalid leading byte (0x1) for canonical function lower");
    CHECK(messageOf(oneOf(8, bytesOf({ 0x01, 0x00, 0x00, 0x01, 0x0A })))
          == "invalid leading byte (0xa) for canonical option");

    // The reserved byte and the booleans, in binary.wast's words.
    CHECK(messageOf(oneOf(8, bytesOf({ 0x0C, 0x01 }))) == "non-zero reserved byte");
    CHECK(messageOf(oneOf(8, bytesOf({ 0x0C, 0x02 }))) == "invalid boolean value");
    CHECK(messageOf(oneOf(8, bytesOf({ 0x20, 0x01, 0x00 }))) == "non-zero reserved byte");
    CHECK(messageOf(oneOf(8, bytesOf({ 0x20, 0x02, 0x00 }))) == "invalid boolean value");
    CHECK(messageOf(oneOf(8, bytesOf({ 0x06, 0x02 }))) == "invalid boolean value");
    CHECK(canonOf(bytesOf({ 0x06, 0x01 })).immediates == std::vector<uint32_t> { 1 });
    CHECK(canonOf(bytesOf({ 0x0A, 0x7F, 0x05 })).immediates
          == std::vector<uint32_t> { static_cast<uint32_t>(CoreValueKind::I32), 5 });
    CHECK(messageOf(oneOf(8, bytesOf({ 0x0A, 0x7B, 0x00 })))
          == "invalid leading byte (0x7b) for core value type");
}

TEST_CASE("Decoder.everyDefinedValueType")
{
    struct Case
    {
        Bytes bytes;
        DefinedValueType type;
    };
    auto withNames = [](std::initializer_list<uint8_t> head, std::initializer_list<std::string_view> names) {
        auto bytes = bytesOf(head);
        for (auto const name: names)
            appendName(bytes, name);
        return bytes;
    };
    auto variant = withNames({ 0x71, 0x02 }, { "x" });
    appendBytes(variant, { 0x01, 0x7E, 0x00 });
    appendName(variant, "y");
    appendBytes(variant, { 0x00, 0x00 });
    auto record = withNames({ 0x72, 0x01 }, { "a" });
    appendBytes(record, { 0x7F });
    auto const cases = std::vector<Case> {
        { .bytes = record,
          .type =
              RecordType { .fields = { LabeledType { .label = "a", .type = PrimitiveValueType::Bool } } } },
        { .bytes = variant,
          .type = VariantType { .cases = { VariantCase { .label = "x", .type = PrimitiveValueType::S8 },
                                           VariantCase { .label = "y", .type = std::nullopt } } } },
        { .bytes = bytesOf({ 0x70, 0x7B }), .type = ListType { .element = PrimitiveValueType::U16 } },
        { .bytes = bytesOf({ 0x70, 0x02 }), .type = ListType { .element = TypeIndex { .index = 2 } } },
        { .bytes = bytesOf({ 0x67, 0x7D, 0x03 }),
          .type = FixedListType { .element = PrimitiveValueType::U8, .length = 3 } },
        { .bytes = bytesOf({ 0x6F, 0x02, 0x7C, 0x79 }),
          .type = TupleType { .elements = { PrimitiveValueType::S16, PrimitiveValueType::U32 } } },
        { .bytes = withNames({ 0x6E, 0x02 }, { "f1", "f2" }),
          .type = FlagsType { .labels = { "f1", "f2" } } },
        { .bytes = withNames({ 0x6D, 0x02 }, { "e1", "e2" }), .type = EnumType { .labels = { "e1", "e2" } } },
        { .bytes = bytesOf({ 0x6B, 0x7A }), .type = OptionType { .type = PrimitiveValueType::S32 } },
        { .bytes = bytesOf({ 0x6A, 0x01, 0x76, 0x01, 0x75 }),
          .type = ResultType { .ok = PrimitiveValueType::F32, .error = PrimitiveValueType::F64 } },
        { .bytes = bytesOf({ 0x6A, 0x00, 0x01, 0x78 }),
          .type = ResultType { .ok = std::nullopt, .error = PrimitiveValueType::S64 } },
        { .bytes = bytesOf({ 0x69, 0x00 }), .type = OwnType { .resource = 0 } },
        { .bytes = bytesOf({ 0x68, 0x03 }), .type = BorrowType { .resource = 3 } },
        { .bytes = bytesOf({ 0x66, 0x01, 0x7D }), .type = StreamType { .element = PrimitiveValueType::U8 } },
        { .bytes = bytesOf({ 0x65, 0x00 }), .type = FutureType { .element = std::nullopt } },
        { .bytes = bytesOf({ 0x63, 0x73, 0x79 }),
          .type = MapType { .key = PrimitiveValueType::String, .value = PrimitiveValueType::U32 } },
    };
    for (auto const& entry: cases)
    {
        INFO(static_cast<int>(entry.bytes.front()));
        CHECK(valueTypeOf(entry.bytes) == entry.type);
        // Gated or not, each decodes without its feature: the gates are validation's.
        CHECK(valueTypeOf(entry.bytes, ComponentFeatureSet::defaults()) == entry.type);
    }
    // Every row of the table is among the cases.
    for (auto const& row: DefinedValueTypeTable)
    {
        INFO(row.keyword);
        CHECK(std::ranges::any_of(
            cases, [&](Case const& entry) { return entry.bytes.front() == std::byte { row.byte }; }));
    }

    // Every primitive, as a defvaltype and as a valtype; error-context's 📝 is validation's.
    for (auto const& row: PrimitiveValueTypeTable)
    {
        INFO(row.text);
        CHECK(valueTypeOf(bytesOf({ row.byte })) == DefinedValueType { row.type });
        CHECK(valueTypeOf(bytesOf({ 0x6B, row.byte }))
              == DefinedValueType { OptionType { .type = row.type } });
    }

    // Binary.md's grammar conditions on counts are decoded as encoded: wasm-tools refuses an empty
    // record, variant, tuple, flags or enum and more than 32 flags in its validator, and the suites
    // hold them as assert_invalid, so T4's validator refuses them, in wasm-tools' words.
    CHECK(valueTypeOf(bytesOf({ 0x72, 0x00 })) == DefinedValueType { RecordType {} });
    CHECK(valueTypeOf(bytesOf({ 0x71, 0x00 })) == DefinedValueType { VariantType {} });
    CHECK(valueTypeOf(bytesOf({ 0x6F, 0x00 })) == DefinedValueType { TupleType {} });
    CHECK(valueTypeOf(bytesOf({ 0x6E, 0x00 })) == DefinedValueType { FlagsType {} });
    CHECK(valueTypeOf(bytesOf({ 0x6D, 0x00 })) == DefinedValueType { EnumType {} });
    auto flags33 = bytesOf({ 0x6E, 0x21 });
    for (auto const index: std::views::iota(0, 33))
        appendName(flags33, "f" + std::to_string(index));
    CHECK(std::get<FlagsType>(valueTypeOf(flags33)).labels.size() == 33);

    // Leading bytes, as binary.wast spells them.
    for (auto const byte: { uint8_t { 0x62 }, uint8_t { 0x44 }, uint8_t { 0x3E } })
        CHECK(messageOf(oneOf(7, bytesOf({ byte })))
              == std::format("invalid leading byte (0x{:x}) for component defined type", byte));
    auto nonzero = bytesOf({ 0x71, 0x01 });
    appendName(nonzero, "c");
    appendBytes(nonzero, { 0x00, 0x01 });
    CHECK(messageOf(oneOf(7, nonzero)) == "invalid leading byte (0x1) for zero byte required");
    CHECK(messageOf(oneOf(7, bytesOf({ 0x70, 0x40 })))
          == "invalid leading byte (0x40) for component value type");
    CHECK(messageOf(oneOf(7, bytesOf({ 0x6A, 0x02 })))
          == "invalid leading byte (0x2) for optional component value type");

    // Resource types.
    CHECK(typeOf(bytesOf({ 0x3F, 0x7F, 0x00 }))
          == TypeDefinition {
              .type = ResourceTypeDefinition { .rep = CoreValueKind::I32, .destructor = std::nullopt } });
    CHECK(
        typeOf(bytesOf({ 0x3F, 0x7F, 0x01, 0x04 }))
        == TypeDefinition { .type = ResourceTypeDefinition { .rep = CoreValueKind::I32, .destructor = 4 } });
    CHECK(messageOf(oneOf(7, bytesOf({ 0x3F, 0x7F, 0x02 })))
          == "invalid leading byte (0x2) for resource destructor");
}

TEST_CASE("Decoder.asyncFunctionTypeDecoded")
{
    auto withParam = bytesOf({ 0x40, 0x01 });
    appendName(withParam, "p");
    appendBytes(withParam, { 0x7F, 0x00, 0x79 });
    CHECK(
        typeOf(withParam)
        == TypeDefinition { .type = FunctionTypeDefinition {
                                .asyncness = Asyncness::Sync,
                                .params = { LabeledType { .label = "p", .type = PrimitiveValueType::Bool } },
                                .result = PrimitiveValueType::U32 } });

    auto const async = bytesOf({ 0x43, 0x00, 0x01, 0x00 });
    auto const type = typeOf(async, ComponentFeatureSet::wasi03());
    REQUIRE(std::holds_alternative<FunctionTypeDefinition>(type.type));
    CHECK(std::get<FunctionTypeDefinition>(type.type).asyncness == Asyncness::Async);
    // Without 🔀 too: the gate is validation's (Validator.asyncRefusedUntilP12).
    CHECK(typeOf(async, ComponentFeatureSet::defaults()) == type);
}

TEST_CASE("Decoder.mapDecodesWithoutItsGate")
{
    // 🗺️ is a validation rule in wasmparser ("Maps require the component model map feature"), so the
    // map decodes whatever the set (Validator.mapKeyTypes refuses it without the feature).
    auto const map = bytesOf({ 0x63, 0x73, 0x79 });
    for (auto const features: { ComponentFeatureSet::defaults(), ComponentFeatureSet::wasi03() })
        CHECK(valueTypeOf(map, features)
              == DefinedValueType {
                  MapType { .key = PrimitiveValueType::String, .value = PrimitiveValueType::U32 } });
}

TEST_CASE("Decoder.nestedInstanceTypes")
{
    // (type (instance (type (instance (type (instance (alias outer 1 0 (type)) (export "t" (type (eq
    // 0)))))))))
    auto element =
        bytesOf({ 0x42, 0x01, 0x01, 0x42, 0x01, 0x01, 0x42, 0x02, 0x02, 0x03, 0x02, 0x01, 0x00, 0x04 });
    appendBytes(element, plainName("t"));
    appendBytes(element, { 0x03, 0x00, 0x00 });
    auto const pool = decode(oneOf(7, element));
    REQUIRE(pool.has_value());
    REQUIRE(pool->components.size() == 4);
    CHECK(pool->components[0].kind == ScopeKind::Component);
    for (auto const scope: { 1U, 2U })
    {
        CHECK(pool->components[scope - 1].definitions
              == std::vector<Definition> {
                  TypeDefinition { .type = InstanceTypeDefinition { .scope = scope } } });
        CHECK(pool->components[scope].kind == ScopeKind::InstanceType);
    }
    CHECK(pool->components[2].definitions
          == std::vector<Definition> { TypeDefinition { .type = InstanceTypeDefinition { .scope = 3 } } });
    CHECK(pool->components[3].kind == ScopeKind::InstanceType);
    CHECK(pool->components[3].definitions
          == std::vector<Definition> {
              AliasDefinition { .sort = Sort::Type,
                                .target = AliasTarget::Outer,
                                .instance = 0,
                                .name = {},
                                .outerCount = 1,
                                .outerIndex = 0 },
              ExportDeclaration {
                  .name = NameAttributes { .name = "t", .attributes = {} },
                  .type = ExternDesc { .sort = Sort::Type, .index = 0, .bound = TypeBound::Equal } },
          });

    // A scope closes where its declarations end: the next type is the outer scope's.
    auto siblings = bytesOf({ 0x03, 0x42, 0x01, 0x01, 0x73, 0x7F, 0x41, 0x01, 0x03 });
    appendBytes(siblings, plainName("i"));
    appendBytes(siblings, { 0x01, 0x00 });
    auto const closed = decode(componentOf({ section(7, siblings) }));
    REQUIRE(closed.has_value());
    REQUIRE(closed->components.size() == 3);
    CHECK(
        closed->components[0].definitions
        == std::vector<Definition> { TypeDefinition { .type = InstanceTypeDefinition { .scope = 1 } },
                                     TypeDefinition { .type = DefinedValueType { PrimitiveValueType::Bool } },
                                     TypeDefinition { .type = ComponentTypeDefinition { .scope = 2 } } });
    CHECK(closed->components[1].definitions
          == std::vector<Definition> {
              TypeDefinition { .type = DefinedValueType { PrimitiveValueType::String } } });
    CHECK(closed->components[2].kind == ScopeKind::ComponentType);
    CHECK(closed->components[2].definitions
          == std::vector<Definition> {
              ImportDefinition { .name = NameAttributes { .name = "i", .attributes = {} },
                                 .type = ExternDesc { .sort = Sort::Func, .index = 0 } } });

    // Declarators: an import only in a component type; 0x05 nowhere.
    auto import = bytesOf({ 0x42, 0x01, 0x03 });
    appendBytes(import, plainName("a"));
    appendBytes(import, { 0x03, 0x01 });
    CHECK(messageOf(oneOf(7, import))
          == "invalid leading byte (0x3) for component or instance type declaration");
    CHECK(messageOf(oneOf(7, bytesOf({ 0x41, 0x01, 0x05, 0x73 })))
          == "invalid leading byte (0x5) for component or instance type declaration");

    // Type scopes nest on the decoder's own stack, counted with the components around them: 99 levels
    // inside the outermost component decode, 100 do not, nor do 100'000.
    auto nested = [](uint32_t levels) {
        auto bytes = bytesOf({ 0x01 });
        for ([[maybe_unused]] auto const level: std::views::iota(uint32_t { 1 }, levels))
            appendBytes(bytes, { 0x42, 0x01, 0x01 });
        appendBytes(bytes, { 0x42, 0x00 });
        return componentOf({ section(7, bytes) });
    };
    auto const atLimit = decode(nested(99));
    REQUIRE(atLimit.has_value());
    CHECK(atLimit->components.size() == 100);
    auto const past = errorOf(nested(100));
    CHECK(exceededLimit(past) == ComponentLimit::Nesting);
    CHECK(past.offset == 11 + (99 * 3) + 1); // the 100th instance type's byte, past the section's size
    CHECK(exceededLimit(errorOf(nested(100'000))) == ComponentLimit::Nesting);
}

TEST_CASE("Decoder.resultListForms")
{
    auto const some = typeOf(bytesOf({ 0x40, 0x00, 0x00, 0x79 }));
    REQUIRE(std::holds_alternative<FunctionTypeDefinition>(some.type));
    CHECK(std::get<FunctionTypeDefinition>(some.type).result == ValueTypeRef { PrimitiveValueType::U32 });
    auto const none = typeOf(bytesOf({ 0x40, 0x00, 0x01, 0x00 }));
    REQUIRE(std::holds_alternative<FunctionTypeDefinition>(none.type));
    CHECK_FALSE(std::get<FunctionTypeDefinition>(none.type).result.has_value());
    CHECK(messageOf(oneOf(7, bytesOf({ 0x40, 0x00, 0x01, 0x01 })))
          == "invalid leading byte (0x1) for number of results");
    CHECK(messageOf(oneOf(7, bytesOf({ 0x40, 0x00, 0x02, 0x00 })))
          == "invalid leading byte (0x2) for component function results");

    // task.return reads the same result list.
    CHECK(canonOf(bytesOf({ 0x09, 0x00, 0x79, 0x00 })).result == ValueTypeRef { PrimitiveValueType::U32 });
    CHECK_FALSE(canonOf(bytesOf({ 0x09, 0x01, 0x00, 0x00 })).result.has_value());
}

TEST_CASE("Decoder.optionsDecoded")
{
    // (canon lift (core func 0) (memory 0) (realloc 1) (post-return 2) string-encoding=latin1+utf16 (type 3))
    auto const lift =
        canonOf(bytesOf({ 0x00, 0x00, 0x00, 0x04, 0x03, 0x00, 0x04, 0x01, 0x05, 0x02, 0x02, 0x03 }));
    CHECK(lift.builtin == CanonBuiltin::Lift);
    CHECK(lift.immediates == std::vector<uint32_t> { 0, 3 });
    CHECK(lift.options
          == std::vector<CanonOption> { { .kind = CanonOptionKind::Memory, .index = 0 },
                                        { .kind = CanonOptionKind::Realloc, .index = 1 },
                                        { .kind = CanonOptionKind::PostReturn, .index = 2 },
                                        { .kind = CanonOptionKind::Latin1Utf16, .index = 0 } });

    // async and callback are 🔀.
    auto const asyncLift = bytesOf({ 0x00, 0x00, 0x00, 0x02, 0x06, 0x07, 0x01, 0x00 });
    CHECK(canonOf(asyncLift).options
          == std::vector<CanonOption> { { .kind = CanonOptionKind::Async, .index = 0 },
                                        { .kind = CanonOptionKind::Callback, .index = 1 } });
    CHECK(canonOf(asyncLift, ComponentFeatureSet::defaults()).options.size() == 2);
}

TEST_CASE("Decoder.coreTypes")
{
    auto coreTypeOf = [](Bytes const& element) {
        auto const definitions = definitionsOf(oneOf(3, element));
        REQUIRE(definitions.size() == 1);
        REQUIRE(std::holds_alternative<CoreTypeDefinition>(definitions.front()));
        return std::get<CoreTypeDefinition>(definitions.front());
    };
    using isoline::binary::FuncType;
    CHECK(coreTypeOf(bytesOf({ 0x60, 0x01, 0x7F, 0x00 }))
          == CoreTypeDefinition { .type = FuncType { .params = { isoline::binary::I32 }, .results = {} } });

    // A module type of every declarator, as binary.wast has it, and a memory import.
    auto module = bytesOf({ 0x50, 0x05, 0x01, 0x60, 0x00, 0x00, 0x00 });
    appendName(module, "a");
    appendName(module, "b");
    appendBytes(module, { 0x00, 0x00, 0x00 });
    appendName(module, "a");
    appendName(module, "m");
    appendBytes(module, { 0x02, 0x00, 0x01, 0x02, 0x10, 0x01, 0x01, 0x00, 0x03 });
    appendName(module, "e");
    appendBytes(module, { 0x00, 0x00 });
    auto memory = CoreExternType { .kind = isoline::binary::ExternKind::Memory };
    memory.memory.limits.min = 1;
    CHECK(coreTypeOf(module)
          == CoreTypeDefinition {
              .type = CoreModuleTypeDefinition {
                  .declarations = {
                      CoreTypeDeclaration { .type = FuncType {} },
                      CoreImportDeclaration { .module = "a", .name = "b", .type = CoreExternType {} },
                      CoreImportDeclaration { .module = "a", .name = "m", .type = memory },
                      CoreOuterAliasDeclaration { .outerCount = 1, .outerIndex = 0 },
                      CoreExportDeclaration { .name = "e", .type = CoreExternType {} },
                  } } });

    // Leading bytes, as binary.wast spells them.
    CHECK(messageOf(oneOf(3, bytesOf({ 0x50, 0x01, 0x04, 0x60, 0x00, 0x00 })))
          == "invalid leading byte (0x4) for type definition");
    CHECK(messageOf(oneOf(3, bytesOf({ 0x50, 0x01, 0x02, 0x00, 0x01, 0x01, 0x00 })))
          == "invalid leading byte (0x0) for outer alias kind");
    CHECK(messageOf(oneOf(3, bytesOf({ 0x50, 0x01, 0x02, 0x10, 0x00, 0x01, 0x00 })))
          == "invalid leading byte (0x0) for outer alias target");
    CHECK(messageOf(oneOf(3, bytesOf({ 0x00, 0x60, 0x00, 0x00 })))
          == "invalid leading byte (0x60) for non-final sub type");

    // Core GC's subtypes and rec groups do not decode (Isoline's core has no GC), in the core
    // decoder's words, at their form byte.
    auto const sub = errorOf(oneOf(3, bytesOf({ 0x00, 0x50, 0x00, 0x60, 0x00, 0x00 })));
    CHECK(sub
          == DecodeError { DecodeErrorCode::MalformedCoreType,
                           12,
                           static_cast<uint64_t>(isoline::binary::DecodeErrorCode::MalformedFunctionType) });
    CHECK(specMessage(sub) == "malformed function type");
    CHECK(errorOf(oneOf(3, bytesOf({ 0x4E, 0x01, 0x60, 0x00, 0x00 }))).code
          == DecodeErrorCode::MalformedCoreType);
    CHECK(errorOf(oneOf(3, bytesOf({ 0x50, 0x01, 0x01, 0x50, 0x00, 0x60, 0x00, 0x00 }))).code
          == DecodeErrorCode::MalformedCoreType);
    // A byte that begins no core type, at the top or as a subtype's composite type, is wasmparser's
    // invalid leading byte for type (binary.wast's two module type vectors), not a GC refusal.
    auto const notRec = errorOf(oneOf(3, bytesOf({ 0x50, 0x01, 0x01, 0x00, 0x50, 0x00, 0x60, 0x00, 0x00 })));
    CHECK(notRec == DecodeError { DecodeErrorCode::InvalidCoreTypeForm, 14, 0x00 });
    CHECK(specMessage(notRec) == "invalid leading byte (0x0) for type");
    auto const notComposite =
        errorOf(oneOf(3, bytesOf({ 0x50, 0x02, 0x01, 0x50, 0x00, 0x01, 0x60, 0x00, 0x00 })));
    CHECK(notComposite == DecodeError { DecodeErrorCode::InvalidCoreTypeForm, 16, 0x01 });
    CHECK(specMessage(notComposite) == "invalid leading byte (0x1) for type");
    CHECK(messageOf(oneOf(3, bytesOf({ 0x01, 0x60, 0x00, 0x00 }))) == "invalid leading byte (0x1) for type");
    // Every GC form is refused, a subtype once its supertypes are read, at its first byte.
    for (auto const form: { 0x4E, 0x5E, 0x5F, 0x5D, 0x65, 0x4C, 0x4D })
        CHECK(errorOf(oneOf(3, bytesOf({ static_cast<uint8_t>(form), 0x00 }))).code
              == DecodeErrorCode::MalformedCoreType);
    CHECK(errorOf(oneOf(3, bytesOf({ 0x00, 0x50, 0x01, 0x07, 0x5F, 0x00 }))).offset == 12);
    CHECK(errorOf(oneOf(3, bytesOf({ 0x00, 0x4F, 0x00, 0x60, 0x00, 0x00 }))).code
          == DecodeErrorCode::InvalidNonFinalSubType);

    // A core read error is reported as everywhere else.
    CHECK(errorOf(oneOf(3, bytesOf({ 0x60, 0x05 }))).code == DecodeErrorCode::UnexpectedEnd);
}

TEST_CASE("Decoder.typeCaps")
{
    // maxTypes counts the types and core types each scope defines.
    auto limits = ComponentLimits {};
    limits.maxTypes = 2;
    auto const three = componentOf(
        { section(7, bytesOf({ 0x02, 0x73, 0x73 })), section(3, bytesOf({ 0x01, 0x60, 0x00, 0x00 })) });
    auto const refused = errorOf(three, ComponentFeatureSet::wasi03(), limits);
    CHECK(exceededLimit(refused) == ComponentLimit::Types);
    CHECK(refused.offset == 16);
    CHECK(specMessage(refused) == "types count exceeds limit of 2");
    // A type scope's own types are its own: two in it, two around it.
    CHECK(decode(componentOf({ section(7, bytesOf({ 0x02, 0x42, 0x02, 0x01, 0x73, 0x01, 0x73, 0x73 })) }),
                 ComponentFeatureSet::wasi03(),
                 limits)
              .has_value());
    CHECK(exceededLimit(errorOf(
              componentOf({ section(
                  7, bytesOf({ 0x01, 0x42, 0x03, 0x01, 0x73, 0x01, 0x73, 0x00, 0x60, 0x00, 0x00 })) }),
              ComponentFeatureSet::wasi03(),
              limits))
          == ComponentLimit::Types);

    // Every element is a decoded item: a record's fields, and a core function type's parameters once read.
    limits = ComponentLimits {};
    limits.maxDecodedItems = 3;
    auto record = bytesOf({ 0x72, 0x03 });
    for (auto const label: { "a", "b", "c" })
    {
        appendName(record, label);
        appendBytes(record, { 0x7F });
    }
    CHECK(exceededLimit(errorOf(oneOf(7, record), ComponentFeatureSet::wasi03(), limits))
          == ComponentLimit::DecodedItems);
    limits.maxDecodedItems = 4;
    CHECK(decode(oneOf(7, record), ComponentFeatureSet::wasi03(), limits).has_value());
    CHECK(decode(oneOf(3, bytesOf({ 0x60, 0x02, 0x7F, 0x7F, 0x01, 0x7F })),
                 ComponentFeatureSet::wasi03(),
                 limits)
              .has_value());
    CHECK(exceededLimit(errorOf(oneOf(3, bytesOf({ 0x60, 0x02, 0x7F, 0x7F, 0x02, 0x7F, 0x7F })),
                                ComponentFeatureSet::wasi03(),
                                limits))
          == ComponentLimit::DecodedItems);
}

TEST_CASE("Decoder.truncatedSectionsFail")
{
    // Every vector section of every fixture, cut short at each of its bytes and its size adjusted:
    // each read that runs out reports the end of the file, wherever in a definition it stands.
    auto const readLeb = [](Bytes const& bytes, std::size_t& at) {
        auto value = uint64_t { 0 };
        auto shift = 0U;
        while (true)
        {
            auto const byte = std::to_integer<uint8_t>(bytes[at++]);
            value |= uint64_t { byte & 0x7FU } << shift;
            shift += 7U;
            if ((byte & 0x80U) == 0)
                return value;
        }
    };
    constexpr auto PreambleSize = std::size_t { 8 };
    auto cuts = 0;
    for (auto const& fixture: decoderFixtures())
    {
        auto at = PreambleSize;
        while (at < fixture.bytes.size())
        {
            auto const id = std::to_integer<uint8_t>(fixture.bytes[at++]);
            auto const size = readLeb(fixture.bytes, at);
            auto const payload = Bytes(fixture.bytes.begin() + static_cast<std::ptrdiff_t>(at),
                                       fixture.bytes.begin() + static_cast<std::ptrdiff_t>(at + size));
            at += size;
            // A custom section's payload, a core module and a nested component are no vectors.
            if (id == 0 || id == 1 || id == 4)
                continue;
            for (auto const length: std::views::iota(std::size_t { 0 }, payload.size()))
            {
                INFO(fixture.name << ": section " << int { id } << " cut to " << length << " bytes");
                auto const cut =
                    Bytes(payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(length));
                CHECK(errorOf(componentOf({ section(id, cut) })).code == DecodeErrorCode::UnexpectedEnd);
                ++cuts;
            }
        }
    }
    CHECK(cuts > 250);
    // A type index cut inside its LEB128, and one too long.
    CHECK(errorOf(oneOf(7, bytesOf({ 0x70, 0x80 }))).code == DecodeErrorCode::UnexpectedEnd);
    CHECK(errorOf(oneOf(7, bytesOf({ 0x70, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00 }))).code
          == DecodeErrorCode::IntegerRepresentationTooLong);
}

TEST_CASE("Decoder.capsInEverySection")
{
    auto limits = ComponentLimits {};
    limits.maxDefinitions = 1;
    // Refused before the second element is read, in a vector section and in the type section.
    auto imports = Bytes { std::byte { 0x02 } };
    for ([[maybe_unused]] auto const index: { 0, 1 })
    {
        appendBytes(imports, plainName("a"));
        appendBytes(imports, { 0x01, 0x00 });
    }
    CHECK(exceededLimit(errorOf(componentOf({ section(10, imports) }), ComponentFeatureSet::wasi03(), limits))
          == ComponentLimit::Definitions);
    CHECK(exceededLimit(errorOf(componentOf({ section(7, bytesOf({ 0x02, 0x73, 0x73 })) }),
                                ComponentFeatureSet::wasi03(),
                                limits))
          == ComponentLimit::Definitions);

    // A custom section and a nested component each cost an item.
    limits = ComponentLimits {};
    limits.maxDecodedItems = 0;
    auto custom = Bytes {};
    appendName(custom, "c");
    CHECK(exceededLimit(errorOf(componentOf({ section(0, custom) }), ComponentFeatureSet::wasi03(), limits))
          == ComponentLimit::DecodedItems);
    CHECK(exceededLimit(errorOf(
              componentOf({ section(4, componentPreamble()) }), ComponentFeatureSet::wasi03(), limits))
          == ComponentLimit::DecodedItems);

    // A value type and a scope type each count against maxTypes.
    limits = ComponentLimits {};
    limits.maxTypes = 0;
    CHECK(exceededLimit(errorOf(oneOf(7, bytesOf({ 0x73 })), ComponentFeatureSet::wasi03(), limits))
          == ComponentLimit::Types);
    CHECK(exceededLimit(errorOf(oneOf(7, bytesOf({ 0x42, 0x00 })), ComponentFeatureSet::wasi03(), limits))
          == ComponentLimit::Types);
}

TEST_CASE("Decoder.specMessageWithoutSubject")
{
    // An error built without the subject its detail reads falls back to its row's text.
    CHECK(specMessage(DecodeError { DecodeErrorCode::MalformedSectionId, 0 }) == "malformed section id");
    CHECK(specMessage(DecodeError { DecodeErrorCode::UnknownBinaryVersion, 0 }) == "unknown binary version");
    CHECK(specMessage(DecodeError { DecodeErrorCode::DuplicateNameAttribute, 0 }) == "option in name");
    CHECK(specMessage(DecodeError { DecodeErrorCode::DuplicateNameAttribute, 0, 99 }) == "option in name");
    CHECK(specMessage(DecodeError { DecodeErrorCode::FeatureDisabled, 0 }) == "feature not enabled");
    CHECK(specMessage(DecodeError { DecodeErrorCode::LimitExceeded, 0 }) == "limit exceeded");
    CHECK(specMessage(DecodeError { DecodeErrorCode::MalformedCoreType, 0 }) == "malformed core type");
    CHECK(specMessage(DecodeError { DecodeErrorCode::MalformedCoreType, 0, 0xFFFF })
          == "malformed core type");
    CHECK(specMessage(DecodeError { DecodeErrorCode::InvalidCanonSort, 0, 0x0700 }) == "canonical function");
    CHECK_FALSE(exceededLimit(DecodeError { DecodeErrorCode::UnexpectedEnd, 0 }).has_value());
    CHECK_FALSE(exceededLimit(DecodeError { DecodeErrorCode::LimitExceeded, 0 }).has_value());
}

TEST_CASE("Decoder.coreGrammarErrors")
{
    // A core function type and a core extern type the core decoder refuses, in its words.
    auto const function = errorOf(oneOf(3, bytesOf({ 0x60, 0x01, 0x55, 0x00 })));
    CHECK(function.code == DecodeErrorCode::MalformedCoreType);
    CHECK(function.subject
          == static_cast<uint64_t>(isoline::binary::DecodeErrorCode::MalformedReferenceType));
    auto module = bytesOf({ 0x50, 0x01, 0x00 });
    appendName(module, "a");
    appendName(module, "b");
    appendBytes(module, { 0x09 });
    CHECK(errorOf(oneOf(3, module)).code == DecodeErrorCode::MalformedCoreType);

    // An export alias of a sort no component names; 🪙's value sort decodes, validation refuses it.
    auto alias = bytesOf({ 0x06, 0x00, 0x00 });
    appendName(alias, "f");
    CHECK(errorOf(oneOf(6, alias)) == DecodeError { DecodeErrorCode::InvalidExternalKind, 11, 0x06 });
    auto value = bytesOf({ 0x02, 0x00, 0x00 });
    appendName(value, "v");
    CHECK(std::get<AliasDefinition>(definitionsOf(oneOf(6, value), ComponentFeatureSet::defaults()).front())
              .sort
          == Sort::Value);
}

TEST_CASE("Decoder.truncatedForms")
{
    // Forms cut short where no fixture's truncation reaches: each read reports the end of the file.
    auto const cases = std::vector<std::pair<uint8_t, std::vector<uint8_t>>> {
        { 8, { 0x06 } },             // subtask.cancel, its async? missing
        { 7, { 0x72, 0x01 } },       // a record field's label
        { 7, { 0x3F } },             // a resource's representation
        { 7, { 0x3F, 0x7F } },       // a resource's destructor
        { 3, { 0x00 } },             // a non-final subtype's form
        { 3, { 0x4F } },             // a subtype's supertypes
        { 3, { 0x4F, 0x00 } },       // a subtype's composite type
        { 3, { 0x50, 0x01, 0x01 } }, // a module type's type declaration
    };
    for (auto const& [id, element]: cases)
    {
        INFO("section " << int { id } << ", " << element.size() << " bytes");
        auto bytes = Bytes {};
        appendBytes(bytes, element);
        CHECK(errorOf(oneOf(id, bytes)).code == DecodeErrorCode::UnexpectedEnd);
    }

    // A core module and a nested component cost an item, and a nested component a definition.
    auto limits = ComponentLimits {};
    limits.maxDecodedItems = 0;
    CHECK(exceededLimit(
              errorOf(componentOf({ section(1, modulePreamble()) }), ComponentFeatureSet::wasi03(), limits))
          == ComponentLimit::DecodedItems);
    limits = ComponentLimits {};
    limits.maxDefinitions = 0;
    CHECK(exceededLimit(errorOf(
              componentOf({ section(4, componentPreamble()) }), ComponentFeatureSet::wasi03(), limits))
          == ComponentLimit::Definitions);
}

TEST_CASE("Decoder.truncatedFixedList")
{
    // (list t len) cut off before its element and before its length: the section ends early.
    CHECK(errorOf(oneOf(7, bytesOf({ 0x67 }))).code == DecodeErrorCode::UnexpectedEnd);
    CHECK(errorOf(oneOf(7, bytesOf({ 0x67, 0x7D }))).code == DecodeErrorCode::UnexpectedEnd);
}
