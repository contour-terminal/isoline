// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include <isoline/component/Features.hpp>
#include <isoline/component/Limits.hpp>
#include <isoline/component/binary/Component.hpp>
#include <isoline/component/binary/Decoder.hpp>
#include <isoline/component/binary/testing/DecoderFixtures.hpp>

using isoline::component::ComponentFeature;
using isoline::component::ComponentFeatureSet;
using isoline::component::ComponentLimit;
using isoline::component::ComponentLimits;
using isoline::component::binary::AliasDefinition;
using isoline::component::binary::AliasTarget;
using isoline::component::binary::ComponentPool;
using isoline::component::binary::CoreInlineExport;
using isoline::component::binary::CoreInlineExports;
using isoline::component::binary::CoreInstanceDefinition;
using isoline::component::binary::CoreInstantiate;
using isoline::component::binary::CoreModuleDefinition;
using isoline::component::binary::CoreSort;
using isoline::component::binary::CustomDefinition;
using isoline::component::binary::decodeComponent;
using isoline::component::binary::DecodeError;
using isoline::component::binary::DecodeErrorCode;
using isoline::component::binary::DecodeErrorMessages;
using isoline::component::binary::Definition;
using isoline::component::binary::exceededLimit;
using isoline::component::binary::ExportDefinition;
using isoline::component::binary::ExternalId;
using isoline::component::binary::ExternDesc;
using isoline::component::binary::Implements;
using isoline::component::binary::ImportDefinition;
using isoline::component::binary::InlineExport;
using isoline::component::binary::InlineExports;
using isoline::component::binary::InstanceDefinition;
using isoline::component::binary::Instantiate;
using isoline::component::binary::LayerKind;
using isoline::component::binary::layerOf;
using isoline::component::binary::NameAttributes;
using isoline::component::binary::NestedComponent;
using isoline::component::binary::Sort;
using isoline::component::binary::specMessage;
using isoline::component::binary::splitCoreModules;
using isoline::component::binary::TypeBound;
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

    // With external-id alone, the form decodes and implements is refused at its attribute.
    auto const implementsOff =
        errorOf(fixture->bytes, ComponentFeatureSet::defaults().with(ComponentFeature::ExternalId));
    CHECK(
        implementsOff
        == DecodeError { DecodeErrorCode::FeatureDisabled, 15, std::nullopt, ComponentFeature::Implements });
    auto const externalIdOff =
        errorOf(fixture->bytes, ComponentFeatureSet::defaults().with(ComponentFeature::Implements));
    CHECK(externalIdOff.code == DecodeErrorCode::FeatureDisabled);
    CHECK(externalIdOff.disabled == ComponentFeature::ExternalId);

    // A version suffix is 🔗, which P11 refuses whatever the set.
    auto suffixed = bytesOf({ 0x01, 0x02 });
    appendName(suffixed, "x");
    appendBytes(suffixed, { 0x01, 0x01 });
    appendName(suffixed, ".1");
    appendBytes(suffixed, { 0x05, 0x00 });
    auto const suffix =
        errorOf(componentOf({ section(10, suffixed) }),
                ComponentFeatureSet::wasi03().with(ComponentFeature::CanonicalInterfaceNames));
    CHECK(suffix.code == DecodeErrorCode::FeatureDisabled);
    CHECK(suffix.disabled == ComponentFeature::CanonicalInterfaceNames);

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

    // The value sort and the value extern type are 🪙 too.
    auto valueExport = Bytes { std::byte { 0x01 } };
    appendBytes(valueExport, plainName("v"));
    appendBytes(valueExport, { 0x02, 0x00, 0x00 });
    CHECK(errorOf(componentOf({ section(11, valueExport) }))
          == DecodeError { DecodeErrorCode::FeatureDisabled, 14, std::nullopt, ComponentFeature::Values });
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

    // The sections Task 2 decodes.
    for (auto const id: { uint8_t { 3 }, uint8_t { 7 }, uint8_t { 8 } })
    {
        auto const error = errorOf(componentOf({ section(id, bytesOf({ 0x00 })) }));
        CHECK(error == DecodeError { DecodeErrorCode::UnimplementedSection, 10, id });
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
