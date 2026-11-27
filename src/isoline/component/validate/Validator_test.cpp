// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/component/Features.hpp>
#include <isoline/component/Limits.hpp>
#include <isoline/component/binary/Component.hpp>
#include <isoline/component/binary/Decoder.hpp>
#include <isoline/component/binary/testing/Builder.hpp>
#include <isoline/component/binary/testing/DecoderFixtures.hpp>
#include <isoline/component/types/TypeArena.hpp>
#include <isoline/component/validate/Validator.hpp>
#include <isoline/component/validate/testing/CoreModuleValidator.hpp>

using isoline::component::ComponentFeature;
using isoline::component::ComponentFeatureSet;
using isoline::component::ComponentLimit;
using isoline::component::ComponentLimits;
using isoline::component::binary::Asyncness;
using isoline::component::binary::BorrowType;
using isoline::component::binary::CanonBuiltin;
using isoline::component::binary::CanonDefinition;
using isoline::component::binary::CanonOption;
using isoline::component::binary::CanonOptionKind;
using isoline::component::binary::ComponentPool;
using isoline::component::binary::CoreValueKind;
using isoline::component::binary::decodeComponent;
using isoline::component::binary::DefinedValueType;
using isoline::component::binary::DefinitionPosition;
using isoline::component::binary::EnumType;
using isoline::component::binary::ExternDesc;
using isoline::component::binary::FixedListType;
using isoline::component::binary::FlagsType;
using isoline::component::binary::FunctionTypeDefinition;
using isoline::component::binary::FutureType;
using isoline::component::binary::Implements;
using isoline::component::binary::LabeledType;
using isoline::component::binary::ListType;
using isoline::component::binary::MapType;
using isoline::component::binary::NameAttributes;
using isoline::component::binary::OptionType;
using isoline::component::binary::OwnType;
using isoline::component::binary::PrimitiveValueType;
using isoline::component::binary::RecordType;
using isoline::component::binary::ResourceTypeDefinition;
using isoline::component::binary::ResultType;
using isoline::component::binary::Sort;
using isoline::component::binary::SortIndex;
using isoline::component::binary::StreamType;
using isoline::component::binary::TupleType;
using isoline::component::binary::TypeBound;
using isoline::component::binary::TypeDefinition;
using isoline::component::binary::TypeIndex;
using isoline::component::binary::ValueTypeRef;
using isoline::component::binary::VariantCase;
using isoline::component::binary::VariantType;
using isoline::component::binary::testing::appendBytes;
using isoline::component::binary::testing::appendName;
using isoline::component::binary::testing::Bytes;
using isoline::component::binary::testing::ComponentBuilder;
using isoline::component::binary::testing::componentOf;
using isoline::component::binary::testing::modulePreamble;
using isoline::component::binary::testing::plainName;
using isoline::component::binary::testing::section;
using isoline::component::binary::testing::TypeScopeBuilder;
using isoline::component::types::TypeExtern;
using isoline::component::validate::exceededLimit;
using isoline::component::validate::specMessage;
using isoline::component::validate::validateComponent;
using isoline::component::validate::ValidatedComponent;
using isoline::component::validate::ValidationError;
using isoline::component::validate::ValidationErrorCode;
using isoline::component::validate::testing::CoreModuleValidator;

namespace
{
using Validated = std::expected<std::shared_ptr<ValidatedComponent const>, ValidationError>;

[[nodiscard]] Validated validated(ComponentPool pool,
                                  ComponentFeatureSet features = ComponentFeatureSet::defaults())
{
    auto coreModules = CoreModuleValidator {};
    return validateComponent(std::move(pool), features, ComponentLimits {}, coreModules);
}

/// The error validating @p pool reports; the validation must fail.
[[nodiscard]] ValidationError refusal(ComponentPool pool,
                                      ComponentFeatureSet features = ComponentFeatureSet::defaults())
{
    auto result = validated(std::move(pool), features);
    REQUIRE_FALSE(result.has_value());
    return result.error();
}

/// The message validating @p pool refuses it with.
[[nodiscard]] std::string refusalText(ComponentPool pool,
                                      ComponentFeatureSet features = ComponentFeatureSet::defaults())
{
    return specMessage(refusal(std::move(pool), features));
}

/// Whether @p pool validates.
[[nodiscard]] bool accepted(ComponentPool pool,
                            ComponentFeatureSet features = ComponentFeatureSet::defaults())
{
    auto const result = validated(std::move(pool), features);
    if (!result)
        UNSCOPED_INFO(specMessage(result.error()));
    return result.has_value();
}

/// A component of one type definition.
[[nodiscard]] ComponentPool oneType(DefinedValueType type)
{
    auto builder = ComponentBuilder {};
    std::ignore = builder.type(TypeDefinition { .type = std::move(type) });
    return builder.pool();
}

[[nodiscard]] ValueTypeRef at(uint32_t index)
{
    return TypeIndex { .index = index };
}

[[nodiscard]] TypeDefinition defined(DefinedValueType type)
{
    return TypeDefinition { .type = std::move(type) };
}

[[nodiscard]] RecordType recordOf(std::vector<std::string> labels,
                                  ValueTypeRef type = PrimitiveValueType::U32)
{
    auto record = RecordType {};
    for (auto& label: labels)
        record.fields.push_back(LabeledType { .label = std::move(label), .type = type });
    return record;
}

[[nodiscard]] FunctionTypeDefinition functionOf(std::vector<LabeledType> params,
                                                std::optional<ValueTypeRef> result = std::nullopt,
                                                Asyncness asyncness = Asyncness::Sync)
{
    return FunctionTypeDefinition { .asyncness = asyncness, .params = std::move(params), .result = result };
}

/// The labels "f1" … "f<count>".
[[nodiscard]] std::vector<std::string> numbered(std::string_view prefix, uint32_t count)
{
    auto labels = std::vector<std::string> {};
    for (auto const index: std::views::iota(uint32_t { 1 }, count + 1))
        labels.push_back(std::format("{}{}", prefix, index));
    return labels;
}

/// Decodes @p bytes, which must decode, then validates them.
[[nodiscard]] Validated validatedBytes(Bytes const& bytes)
{
    auto pool = decodeComponent(bytes, ComponentFeatureSet::defaults(), ComponentLimits {});
    REQUIRE(pool.has_value());
    return validated(std::move(*pool));
}

/// A type section of @p count types, @p body their encodings.
[[nodiscard]] Bytes typeSection(uint8_t count, std::initializer_list<uint8_t> body)
{
    auto payload = Bytes { std::byte { count } };
    appendBytes(payload, body);
    return componentOf({ section(7, payload) });
}
} // namespace

TEST_CASE("Validator.wellFormedDefinedTypes")
{
    // test/validation/defined-types.wast:8-26 at the pin, transcribed.
    auto builder = ComponentBuilder {};
    for (auto const primitive: { PrimitiveValueType::Bool,
                                 PrimitiveValueType::U8,
                                 PrimitiveValueType::S8,
                                 PrimitiveValueType::U16,
                                 PrimitiveValueType::S16,
                                 PrimitiveValueType::U32,
                                 PrimitiveValueType::S32,
                                 PrimitiveValueType::U64,
                                 PrimitiveValueType::S64,
                                 PrimitiveValueType::F32,
                                 PrimitiveValueType::F64,
                                 PrimitiveValueType::Char,
                                 PrimitiveValueType::String })
        std::ignore = builder.type(defined(primitive));
    auto const emptyResult = builder.type(defined(ResultType {}));
    auto const record = builder.type(
        defined(RecordType { .fields = {
                                 LabeledType { .label = "x", .type = at(emptyResult) },
                                 LabeledType { .label = "y", .type = PrimitiveValueType::String },
                             } }));
    auto const variant = builder.type(defined(
        VariantType { .cases = { VariantCase { .label = "r", .type = at(record) },
                                 VariantCase { .label = "s", .type = PrimitiveValueType::String } } }));
    auto const errorResult =
        builder.type(defined(ResultType { .ok = at(variant), .error = PrimitiveValueType::U32 }));
    std::ignore = builder.type(defined(ResultType { .ok = at(variant), .error = std::nullopt }));
    std::ignore = builder.type(defined(ResultType { .ok = std::nullopt, .error = at(variant) }));
    auto const option = builder.type(defined(OptionType { .type = at(record) }));
    std::ignore = builder.type(TypeDefinition {
        .type = functionOf({ LabeledType { .label = "a", .type = at(errorResult) } }, at(option)) });
    auto const errorNumber = builder.type(defined(EnumType { .labels = { "a", "b", "e" } }));
    std::ignore = builder.type(defined(ListType { .element = at(errorNumber) }));
    auto const oflags = builder.type(defined(FlagsType { .labels = { "read", "write", "exclusive" } }));
    std::ignore =
        builder.type(defined(TupleType { .elements = { at(oflags), at(errorNumber), at(record) } }));

    auto const result = validated(builder.pool());
    REQUIRE(result.has_value());
    auto const& component = **result;
    CHECK(component.type.imports.empty());
    // The primitives defined above are the arena's own: no type beyond the compounds was added.
    CHECK(component.types.valueTypeCount() == 14 + 11);
}

TEST_CASE("Validator.countRulesSuiteVectors")
{
    // test/binary/binary.wast:637-722 at the pin: each decodes, and validation refuses it.
    struct Vector
    {
        Bytes bytes;
        std::string_view text;
    };
    auto thirtyThree = Bytes { std::byte { 0x01 }, std::byte { 0x6E }, std::byte { 0x21 } };
    for (auto const& label: numbered("f", 33))
        appendName(thirtyThree, std::format("f{:02}", std::stoi(label.substr(1)) - 1));
    auto const vectors = std::vector<Vector> {
        { .bytes = typeSection(1, { 0x72, 0x00 }), .text = "record type must have at least one field" },
        { .bytes = typeSection(1, { 0x71, 0x00 }), .text = "variant type must have at least one case" },
        { .bytes = typeSection(1, { 0x6F, 0x00 }), .text = "tuple type must have at least one type" },
        { .bytes = typeSection(1, { 0x6E, 0x00 }), .text = "flags must have at least one entry" },
        { .bytes = typeSection(1, { 0x6D, 0x00 }), .text = "enum type must have at least one variant" },
        { .bytes = componentOf({ section(7, thirtyThree) }), .text = "cannot have more than 32 flags" },
        // binary.wast:723-741: a type index past the space, an own of a type that is no resource.
        { .bytes = typeSection(1, { 0x70, 0x05 }), .text = "unknown type 5: type index out of bounds" },
        { .bytes = typeSection(2, { 0x73, 0x69, 0x00 }), .text = "type index 0 is not a resource type" },
    };
    for (auto const& vector: vectors)
    {
        INFO(vector.text);
        auto const result = validatedBytes(vector.bytes);
        REQUIRE_FALSE(result.has_value());
        CHECK(specMessage(result.error()) == vector.text);
    }

    // test/validation/defined-types.wast:85-117: the same in the text suite's shapes.
    CHECK(refusalText(oneType(VariantType {})) == "variant type must have at least one case");
    CHECK(refusalText(oneType(EnumType {})) == "enum type must have at least one variant");
    CHECK(refusalText(oneType(RecordType {})) == "record type must have at least one field");
    CHECK(refusalText(oneType(FlagsType {})) == "flags must have at least one entry");
    CHECK(refusalText(oneType(TupleType {})) == "tuple type must have at least one type");
}

TEST_CASE("Validator.flagsBounds")
{
    CHECK(accepted(oneType(FlagsType { .labels = numbered("f", 32) })));
    auto const refused = refusal(oneType(FlagsType { .labels = numbered("f", 33) }));
    CHECK(refused.code == ValidationErrorCode::TooManyFlags);
    CHECK(specMessage(refused) == "cannot have more than 32 flags");
    CHECK(refused.position == DefinitionPosition { .scope = 0, .definition = 0 });
}

TEST_CASE("Validator.labels")
{
    CHECK(refusalText(oneType(recordOf({ "A b" }))) == "record field name `A b` is not in kebab case");
    CHECK(refusalText(oneType(recordOf({ "a", "A" })))
          == "record field name `A` conflicts with previous field name `a`");
    CHECK(refusalText(oneType(RecordType {})) == "record type must have at least one field");

    // test/validation/defined-types.wast:29-80, every label kind.
    CHECK(refusalText(oneType(recordOf({ "" }))) == "record field name cannot be empty");
    CHECK(refusalText(oneType(
              VariantType { .cases = { VariantCase { .label = "", .type = PrimitiveValueType::S32 } } }))
          == "variant case name cannot be empty");
    CHECK(refusalText(oneType(FlagsType { .labels = { "" } })) == "flag name cannot be empty");
    CHECK(refusalText(oneType(EnumType { .labels = { "" } })) == "enum tag name cannot be empty");
    auto function = ComponentBuilder {};
    std::ignore = function.type(TypeDefinition {
        .type = functionOf({ LabeledType { .label = "", .type = PrimitiveValueType::String } }) });
    CHECK(refusalText(function.pool()) == "function parameter name cannot be empty");

    CHECK(refusalText(oneType(recordOf({ "a-B-c-D", "A-b-C-d" })))
          == "record field name `A-b-C-d` conflicts with previous field name `a-B-c-D`");
    CHECK(refusalText(oneType(
              VariantType { .cases = { VariantCase { .label = "x", .type = PrimitiveValueType::S64 },
                                       VariantCase { .label = "X", .type = PrimitiveValueType::S64 } } }))
          == "variant case name `X` conflicts with previous case name `x`");
    CHECK(refusalText(oneType(FlagsType { .labels = { "x", "y", "X" } }))
          == "flag name `X` conflicts with previous flag name `x`");
    CHECK(refusalText(oneType(EnumType { .labels = { "x", "y", "X" } }))
          == "enum tag name `X` conflicts with previous tag name `x`");
    auto parameters = ComponentBuilder {};
    std::ignore = parameters.type(
        TypeDefinition { .type = functionOf({
                             LabeledType { .label = "foo", .type = PrimitiveValueType::String },
                             LabeledType { .label = "FOO", .type = PrimitiveValueType::U32 },
                         }) });
    CHECK(refusalText(parameters.pool())
          == "function parameter name `FOO` conflicts with previous parameter name `foo`");

    CHECK(refusalText(oneType(EnumType { .labels = { "NevEr" } }))
          == "enum tag name `NevEr` is not in kebab case");
    CHECK(refusalText(oneType(recordOf({ "GoNnA" }))) == "record field name `GoNnA` is not in kebab case");
    CHECK(refusalText(oneType(VariantType {
              .cases = { VariantCase { .label = "GIVe", .type = PrimitiveValueType::String } } }))
          == "variant case name `GIVe` is not in kebab case");

    // Kebab case as wasmparser defines it: one case per word, digits after the first letter, later
    // words may start with a digit, hyphens neither doubled nor trailing.
    for (auto const& good: { "a", "a1", "a-b", "ABC-def", "a-1", "x-1b", "a1-B2" })
    {
        INFO(good);
        CHECK(accepted(oneType(EnumType { .labels = { good } })));
    }
    for (auto const& bad: { "1a", "aB", "a--b", "a-", "-a", "a_b", "a b", "é" })
    {
        INFO(bad);
        CHECK(refusal(oneType(EnumType { .labels = { bad } })).code
              == ValidationErrorCode::LabelNotKebabCase);
    }
    // Strong uniqueness drops hyphens too: "ab" and "a-b" conflict.
    CHECK(refusalText(oneType(EnumType { .labels = { "ab", "a-b" } }))
          == "enum tag name `a-b` conflicts with previous tag name `ab`");
}

TEST_CASE("Validator.mapKeyTypes")
{
    auto const wasi = ComponentFeatureSet::wasi03();
    CHECK(refusalText(oneType(MapType { .key = PrimitiveValueType::F32, .value = PrimitiveValueType::U8 }),
                      wasi)
          == "invalid map key type");
    CHECK(accepted(oneType(MapType { .key = PrimitiveValueType::String, .value = PrimitiveValueType::U8 }),
                   wasi));

    // A key through a type index: a primitive's index is a key, a list's is not.
    auto keyed = ComponentBuilder {};
    auto const u64 = keyed.type(defined(PrimitiveValueType::U64));
    std::ignore = keyed.type(defined(MapType { .key = at(u64), .value = PrimitiveValueType::U8 }));
    CHECK(accepted(keyed.pool(), wasi));
    auto listed = ComponentBuilder {};
    auto const list = listed.type(defined(ListType { .element = PrimitiveValueType::U8 }));
    std::ignore = listed.type(defined(MapType { .key = at(list), .value = PrimitiveValueType::U8 }));
    CHECK(refusalText(listed.pool(), wasi) == "invalid map key type");

    // The value may hold no handle.
    auto handles = ComponentBuilder {};
    auto const resource = handles.resource();
    auto const own = handles.type(defined(OwnType { .resource = resource }));
    auto const wrapped = handles.type(defined(OptionType { .type = at(own) }));
    std::ignore = handles.type(defined(MapType { .key = PrimitiveValueType::U32, .value = at(wrapped) }));
    CHECK(refusalText(handles.pool(), wasi) == "map value type cannot contain a handle");

    // Without 🗺️ the map is refused in validation, naming the feature.
    auto const refused =
        refusal(oneType(MapType { .key = PrimitiveValueType::String, .value = PrimitiveValueType::U8 }));
    CHECK(refused.code == ValidationErrorCode::FeatureDisabled);
    CHECK(refused.disabled == ComponentFeature::Map);
    CHECK(specMessage(refused) == "Maps require the component model map feature");
}

TEST_CASE("Validator.ownNeedsResource")
{
    auto builder = ComponentBuilder {};
    std::ignore = builder.type(defined(PrimitiveValueType::U32));
    std::ignore = builder.type(defined(OwnType { .resource = 0 }));
    CHECK(refusalText(builder.pool()) == "type index 0 is not a resource type");

    auto borrowed = ComponentBuilder {};
    std::ignore = borrowed.type(defined(BorrowType { .resource = 3 }));
    CHECK(refusalText(borrowed.pool()) == "unknown type 3: type index out of bounds");

    auto good = ComponentBuilder {};
    auto const resource = good.resource();
    std::ignore = good.type(defined(OwnType { .resource = resource }));
    std::ignore = good.type(defined(BorrowType { .resource = resource }));
    CHECK(accepted(good.pool()));
}

TEST_CASE("Validator.asyncRefusedUntilP12")
{
    auto builder = ComponentBuilder {};
    std::ignore = builder.type(TypeDefinition { .type = functionOf({}, std::nullopt, Asyncness::Async) });
    // Refused even under wasi03(), which enables 🔀: P11 decodes async and does not implement it.
    for (auto const features: { ComponentFeatureSet::defaults(), ComponentFeatureSet::wasi03() })
    {
        auto const refused = refusal(builder.pool(), features);
        CHECK(refused.code == ValidationErrorCode::FeatureDisabled);
        CHECK(refused.disabled == ComponentFeature::Async);
        CHECK(specMessage(refused) == "async component functions require the component model async feature");
    }
    // So are stream and future, whatever their payload.
    CHECK(
        refusalText(oneType(StreamType { .element = PrimitiveValueType::U8 }), ComponentFeatureSet::wasi03())
        == "`stream` requires the component model async feature");
    CHECK(refusal(oneType(FutureType {}), ComponentFeatureSet::wasi03()).disabled == ComponentFeature::Async);
    // 🔧 and 📝 are refused whatever the set.
    auto const everything = ComponentFeatureSet::wasi03()
                                .with(ComponentFeature::FixedLengthLists)
                                .with(ComponentFeature::ErrorContext);
    CHECK(refusal(oneType(FixedListType { .element = PrimitiveValueType::U8, .length = 0 }), everything)
              .disabled
          == ComponentFeature::FixedLengthLists);
    CHECK(refusalText(oneType(ListType { .element = PrimitiveValueType::ErrorContext }), everything)
          == "`error-context` requires the component model error-context feature");
    CHECK(refusal(oneType(PrimitiveValueType::ErrorContext), everything).disabled
          == ComponentFeature::ErrorContext);
}

TEST_CASE("Validator.functionTypes")
{
    auto builder = ComponentBuilder {};
    auto const resource = builder.resource();
    auto const borrow = builder.type(defined(BorrowType { .resource = resource }));
    auto const own = builder.type(defined(OwnType { .resource = resource }));
    std::ignore = builder.type(TypeDefinition {
        .type = functionOf({ LabeledType { .label = "self", .type = at(borrow) } }, at(own)) });
    CHECK(accepted(builder.pool()));

    // A result may hold no borrow, however deep.
    auto const list = builder.type(defined(ListType { .element = at(borrow) }));
    std::ignore = builder.type(TypeDefinition { .type = functionOf({}, at(list)) });
    CHECK(refusalText(builder.pool()) == "function result cannot contain a `borrow` type");

    // A function type is no defined type.
    auto misused = ComponentBuilder {};
    auto const function = misused.type(TypeDefinition { .type = functionOf({}) });
    std::ignore = misused.type(
        TypeDefinition { .type = functionOf({ LabeledType { .label = "t", .type = at(function) } }) });
    CHECK(refusalText(misused.pool()) == "type index 0 is not a defined type");
}

TEST_CASE("Validator.resources")
{
    // Only a component defines one; an instance type declares one by export instead.
    auto declarations = TypeScopeBuilder {};
    std::ignore = declarations.type(TypeDefinition {
        .type = ResourceTypeDefinition { .rep = CoreValueKind::I32, .destructor = std::nullopt } });
    auto inType = ComponentBuilder {};
    std::ignore = inType.instanceType(std::move(declarations));
    CHECK(refusalText(inType.pool()) == "resources can only be defined within a concrete component");

    auto const representation = [](CoreValueKind rep) {
        auto builder = ComponentBuilder {};
        std::ignore = builder.type(
            TypeDefinition { .type = ResourceTypeDefinition { .rep = rep, .destructor = std::nullopt } });
        return builder.pool();
    };
    CHECK(refusalText(representation(CoreValueKind::F32))
          == "resources can only be represented by `i32` or `i64`");
    CHECK(refusalText(representation(CoreValueKind::F64))
          == "resources can only be represented by `i32` or `i64`");
    auto const wide = refusal(representation(CoreValueKind::I64));
    CHECK(wide.disabled == ComponentFeature::Memory64);
    CHECK(specMessage(wide) == "resources with `i64` require the `cm64` feature to be enabled");

    // The destructor's core function type is Task 7's: refused, not accepted unchecked.
    auto destroyed = ComponentBuilder {};
    std::ignore = destroyed.resource(0);
    CHECK(refusal(destroyed.pool()).code == ValidationErrorCode::NotYetValidated);

    // `(sub resource)` exports of an instance type mint fresh resources, usable by own.
    auto exported = TypeScopeBuilder {};
    auto const resource = exported.exportItem(
        "r", ExternDesc { .sort = Sort::Type, .index = 0, .bound = TypeBound::SubResource });
    std::ignore = exported.type(defined(OwnType { .resource = resource }));
    auto component = ComponentBuilder {};
    std::ignore = component.instanceType(std::move(exported));
    CHECK(accepted(component.pool()));
}

TEST_CASE("Validator.externDeclarations")
{
    auto builder = ComponentBuilder {};
    auto const function = builder.type(TypeDefinition { .type = functionOf({}) });
    auto const record = builder.type(defined(recordOf({ "a" })));
    std::ignore = builder.import("f", ExternDesc { .sort = Sort::Func, .index = function });
    std::ignore = builder.import("t", ExternDesc { .sort = Sort::Type, .index = record });
    auto const result = validated(builder.pool());
    REQUIRE(result.has_value());
    auto const& imports = (*result)->type.imports;
    REQUIRE(imports.size() == 2);
    CHECK(imports[0].name == "f");
    CHECK(std::holds_alternative<isoline::component::types::FunctionTypeId>(imports[0].type));
    REQUIRE(std::holds_alternative<TypeExtern>(imports[1].type));
    CHECK(std::holds_alternative<isoline::component::types::TypeId>(
        std::get<TypeExtern>(imports[1].type).type));

    // Each sort names a type of its kind.
    auto const mismatch = [&](Sort sort) {
        auto wrong = ComponentBuilder {};
        std::ignore = wrong.type(defined(recordOf({ "a" })));
        std::ignore = wrong.import("x", ExternDesc { .sort = sort, .index = 0 });
        return refusalText(wrong.pool());
    };
    CHECK(mismatch(Sort::Func) == "type index 0 is not a function type");
    CHECK(mismatch(Sort::Instance) == "type index 0 is not an instance type");
    CHECK(mismatch(Sort::Component) == "type index 0 is not a component type");
    auto outOfBounds = ComponentBuilder {};
    std::ignore = outOfBounds.import("a", ExternDesc { .sort = Sort::Func, .index = 100 });
    CHECK(refusalText(outOfBounds.pool()) == "unknown type 100: type index out of bounds");

    // A type import adds an index of its own.
    auto bound = ComponentBuilder {};
    auto const resource =
        bound.import("r", ExternDesc { .sort = Sort::Type, .index = 0, .bound = TypeBound::SubResource });
    std::ignore = bound.type(defined(OwnType { .resource = resource }));
    CHECK(accepted(bound.pool()));

    // The attributes' gates are validation's: implements under its feature only.
    auto attributed = ComponentBuilder {};
    auto declarations = TypeScopeBuilder {};
    auto const instance = attributed.instanceType(std::move(declarations));
    std::ignore = attributed.definition(isoline::component::binary::ImportDefinition {
        .name = NameAttributes { .name = "x", .attributes = { Implements { .interfaceName = "ns:p/i" } } },
        .type = ExternDesc { .sort = Sort::Instance, .index = instance } });
    CHECK(accepted(attributed.pool(), ComponentFeatureSet::defaults().with(ComponentFeature::Implements)));
    auto const refused = refusal(attributed.pool());
    CHECK(refused.disabled == ComponentFeature::Implements);
    CHECK(specMessage(refused) == "the `cm-implements` feature is not active");
}

TEST_CASE("Validator.deferredDefinitions")
{
    // What Task 7 validates is refused, never accepted unchecked; its gated forms first.
    auto lift = ComponentBuilder {};
    auto const function = lift.type(TypeDefinition { .type = functionOf({}) });
    std::ignore = lift.canonLift(0, {}, function);
    auto const refused = refusal(lift.pool());
    CHECK(refused.code == ValidationErrorCode::NotYetValidated);
    CHECK(specMessage(refused) == "not yet validated: canonical function definitions");
    CHECK(refused.position == DefinitionPosition { .scope = 0, .definition = 1 });

    auto gated = ComponentBuilder {};
    std::ignore = gated.canon(CanonDefinition {
        .builtin = CanonBuiltin::TaskReturn, .immediates = {}, .options = {}, .result = std::nullopt });
    CHECK(specMessage(refusal(gated.pool())) == "`task.return` requires the component model async feature");
    auto asyncOption = ComponentBuilder {};
    std::ignore = asyncOption.canonLower(0, { CanonOption { .kind = CanonOptionKind::Async, .index = 0 } });
    CHECK(specMessage(refusal(asyncOption.pool()))
          == "canonical option `async` requires the component model async feature");

    // An item of 🪙's value sort is refused wherever it appears.
    auto value = ComponentBuilder {};
    std::ignore = value.exportItem("v", Sort::Value, 0);
    CHECK(refusal(value.pool(), ComponentFeatureSet::wasi03().with(ComponentFeature::Values)).disabled
          == ComponentFeature::Values);
    auto aliased = ComponentBuilder {};
    std::ignore = aliased.aliasExport(0, "v", Sort::Value);
    CHECK(refusal(aliased.pool()).disabled == ComponentFeature::Values);
    auto instance = ComponentBuilder {};
    std::ignore = instance.inlineInstance({ { "v", SortIndex { .sort = Sort::Value, .index = 0 } } });
    CHECK(refusal(instance.pool()).disabled == ComponentFeature::Values);
    auto argument = ComponentBuilder {};
    std::ignore = argument.instantiate(0, { { "v", SortIndex { .sort = Sort::Value, .index = 0 } } });
    CHECK(refusal(argument.pool()).disabled == ComponentFeature::Values);
    auto ungated = ComponentBuilder {};
    std::ignore = ungated.instantiate(0, { { "f", SortIndex { .sort = Sort::Func, .index = 0 } } });
    CHECK(specMessage(refusal(ungated.pool())) == "not yet validated: component instances");
}

TEST_CASE("Validator.coreModules")
{
    auto builder = ComponentBuilder {};
    std::ignore = builder.coreModule(modulePreamble());
    auto nested = ComponentBuilder {};
    std::ignore = nested.coreModule(modulePreamble());
    std::ignore = builder.nested(std::move(nested));
    auto coreModules = CoreModuleValidator {};
    auto const result =
        validateComponent(builder.pool(), ComponentFeatureSet::defaults(), ComponentLimits {}, coreModules);
    REQUIRE(result.has_value());
    CHECK(coreModules.calls() == 2);
    CHECK((*result)->coreModules.size() == 2);

    // A module the core validator refuses names its ordinal.
    auto broken = modulePreamble();
    appendBytes(broken, { 0x01, 0x05, 0x01 });
    auto refusedBuilder = ComponentBuilder {};
    std::ignore = refusedBuilder.coreModule(modulePreamble());
    std::ignore = refusedBuilder.coreModule(broken);
    auto const refused = refusal(refusedBuilder.pool());
    CHECK(refused.code == ValidationErrorCode::CoreModuleRefused);
    REQUIRE(refused.coreModule.has_value());
    CHECK(refused.coreModule->ordinal == 1);
    CHECK(specMessage(refused) == "invalid core module 1");
}

TEST_CASE("Validator.typeNestingTooDeep")
{
    // tests/cli/component-model/very-nested.wast of wasm-tools 1.263.0, its depth vectors transcribed.
    // A list chain of @p lists levels over u32, then a function taking it for Tail::Function.
    enum class Tail : uint8_t
    {
        None,
        Function,
    };
    auto const listChain = [](uint32_t lists, Tail tail) {
        auto builder = ComponentBuilder {};
        auto previous = builder.type(defined(PrimitiveValueType::U32));
        for ([[maybe_unused]] auto const level: std::views::iota(uint32_t { 0 }, lists))
            previous = builder.type(defined(ListType { .element = at(previous) }));
        if (tail == Tail::Function)
            std::ignore = builder.type(TypeDefinition {
                .type = functionOf({ LabeledType { .label = "x", .type = at(previous) } }) });
        return builder.pool();
    };
    auto const deep = refusal(listChain(100, Tail::None));
    CHECK(specMessage(deep) == "type nesting is too deep");
    CHECK(exceededLimit(deep) == ComponentLimit::TypeDepth);
    CHECK(deep.position.definition == 100);
    CHECK(accepted(listChain(99, Tail::None)));
    CHECK(specMessage(refusal(listChain(99, Tail::Function))) == "type nesting is too deep");
    CHECK(accepted(listChain(98, Tail::Function)));

    // Instance and component types: each level one deeper, through an outer alias of the previous.
    auto const scopeChain = [](uint32_t levels, Sort sort) {
        auto builder = ComponentBuilder {};
        auto const add = [&](TypeScopeBuilder scope) {
            return sort == Sort::Instance ? builder.instanceType(std::move(scope))
                                          : builder.componentType(std::move(scope));
        };
        auto first = TypeScopeBuilder {};
        auto const function = first.type(TypeDefinition { .type = functionOf({}) });
        std::ignore = first.exportItem("f", ExternDesc { .sort = Sort::Func, .index = function });
        auto previous = add(std::move(first));
        for ([[maybe_unused]] auto const level: std::views::iota(uint32_t { 1 }, levels + 1))
        {
            auto scope = TypeScopeBuilder {};
            auto const inner = scope.aliasOuter(Sort::Type, 1, previous);
            std::ignore = scope.exportItem("i", ExternDesc { .sort = sort, .index = inner });
            previous = add(std::move(scope));
        }
        return builder.pool();
    };
    for (auto const sort: { Sort::Instance, Sort::Component })
    {
        CHECK(specMessage(refusal(scopeChain(99, sort))) == "type nesting is too deep");
        CHECK(accepted(scopeChain(98, sort)));
        CHECK(specMessage(refusal(scopeChain(130, sort))) == "type nesting is too deep");
    }
}

TEST_CASE("TypeArena.effectiveTypeSizeCapped")
{
    // very-nested.wast:1569-1688: component types importing ten of the previous each, 10^k large.
    // Refused within the type definitions, before its instantiation.
    {
        auto builder = ComponentBuilder {};
        auto previous = builder.componentType(TypeScopeBuilder {});
        for ([[maybe_unused]] auto const level: std::views::iota(uint32_t { 1 }, uint32_t { 10 }))
        {
            auto scope = TypeScopeBuilder {};
            auto const inner = scope.aliasOuter(Sort::Type, 1, previous);
            for (auto const& name: numbered("i", 10))
                std::ignore = scope.import(name, ExternDesc { .sort = Sort::Component, .index = inner });
            previous = builder.componentType(std::move(scope));
        }
        auto const refused = refusal(builder.pool());
        CHECK(specMessage(refused) == "effective type size exceeds the limit of 1000000");
        CHECK(exceededLimit(refused) == ComponentLimit::TypeSize);
        // $m6's ninth import (after its alias) reaches 1 + 9 x 111'111 = 1'000'000.
        CHECK(refused.position.definition == 9);
    }

    // very-nested.wast:2522-2568: $t16 of size 655'359 aliased into a nested component once is
    // accepted; twice exceeds that component's budget.
    auto const doubled = [](uint32_t aliases) {
        auto builder = ComponentBuilder {};
        auto previous = builder.type(
            defined(TupleType { .elements = std::vector<ValueTypeRef>(8, PrimitiveValueType::U32) }));
        for ([[maybe_unused]] auto const level: std::views::iota(uint32_t { 1 }, uint32_t { 17 }))
            previous = builder.type(defined(TupleType { .elements = { at(previous), at(previous) } }));
        auto nested = ComponentBuilder {};
        for ([[maybe_unused]] auto const alias: std::views::iota(uint32_t { 0 }, aliases))
            std::ignore = nested.aliasOuter(Sort::Type, 1, previous);
        std::ignore = builder.nested(std::move(nested));
        return builder.pool();
    };
    CHECK(accepted(doubled(1)));
    auto const twice = refusal(doubled(2));
    CHECK(specMessage(twice) == "effective type size exceeds the limit of 1000000");
    CHECK(twice.position == DefinitionPosition { .scope = 1, .definition = 1 });
}

TEST_CASE("TypeArena.doublingChainBounded")
{
    // 1'000 levels of t_i = record { a_i: t_{i-1}, b_i: t_{i-1} }: sizes 2^(i+1) - 1, refused at level
    // 19, the first at 1'000'000 or more, in time linear in the levels read.
    auto builder = ComponentBuilder {};
    auto previous = builder.type(defined(PrimitiveValueType::U32));
    for (auto const level: std::views::iota(uint32_t { 1 }, uint32_t { 1'001 }))
        previous = builder.type(defined(RecordType {
            .fields = { LabeledType { .label = std::format("a{}", level), .type = at(previous) },
                        LabeledType { .label = std::format("b{}", level), .type = at(previous) } } }));
    auto const refused = refusal(builder.pool());
    CHECK(exceededLimit(refused) == ComponentLimit::TypeSize);
    CHECK(refused.position.definition == 19);
}

TEST_CASE("Validator.hostileTypeGraphs")
{
    // A wide record: 100'000 fields, all distinct, accepted; one more that repeats the first, refused.
    auto labels = numbered("f", 100'000);
    CHECK(accepted(oneType(recordOf(labels))));
    labels.emplace_back("F1");
    CHECK(refusalText(oneType(recordOf(labels)))
          == "record field name `F1` conflicts with previous field name `f1`");

    // Huge indices and counts name what is not there, never read past the space.
    CHECK(refusalText(oneType(ListType { .element = at(0xFFFF'FFFFU) }))
          == "unknown type 4294967295: type index out of bounds");
    auto outer = ComponentBuilder {};
    std::ignore = outer.aliasOuter(Sort::Type, 0xFFFF'FFFFU, 0);
    CHECK(refusalText(outer.pool()) == "invalid outer alias count of 4294967295");
    auto outerIndex = ComponentBuilder {};
    std::ignore = outerIndex.aliasOuter(Sort::Type, 0, 7);
    CHECK(refusalText(outerIndex.pool()) == "unknown type 7: type index out of bounds");

    // The type index space is capped at maxTypes.
    auto builder = ComponentBuilder {};
    for ([[maybe_unused]] auto const index: std::views::iota(0, 5))
        std::ignore = builder.type(defined(PrimitiveValueType::U8));
    auto limits = ComponentLimits {};
    limits.maxTypes = 4;
    auto coreModules = CoreModuleValidator {};
    auto const capped =
        validateComponent(builder.pool(), ComponentFeatureSet::defaults(), limits, coreModules);
    REQUIRE_FALSE(capped.has_value());
    CHECK(specMessage(capped.error()) == "types count exceeds limit of 4");
}

TEST_CASE("Validator.outerAliases")
{
    // An outer alias reaches its target scope's index space, and adds an index here.
    auto builder = ComponentBuilder {};
    auto const record = builder.type(defined(recordOf({ "a" })));
    auto declarations = TypeScopeBuilder {};
    auto const inner = declarations.aliasOuter(Sort::Type, 1, record);
    std::ignore = declarations.exportItem("t", ExternDesc { .sort = Sort::Type, .index = inner });
    std::ignore = builder.instanceType(std::move(declarations));
    CHECK(accepted(builder.pool()));

    // A count past the scopes open is refused; so is an index past the target's space.
    auto tooFar = TypeScopeBuilder {};
    std::ignore = tooFar.aliasOuter(Sort::Type, 2, 0);
    auto far = ComponentBuilder {};
    std::ignore = far.instanceType(std::move(tooFar));
    CHECK(refusalText(far.pool()) == "invalid outer alias count of 2");

    // Outer aliases of other sorts are Task 7's.
    auto component = ComponentBuilder {};
    std::ignore = component.aliasOuter(Sort::Component, 0, 0);
    CHECK(specMessage(refusal(component.pool())) == "not yet validated: outer aliases of this sort");
}

TEST_CASE("Validator.gatesRefusedAtValidation")
{
    // Task 4's stage audit: every gate wasmparser checks in its validator, not its reader, decodes
    // without its feature and is refused here, naming it.
    struct Vector
    {
        std::string_view text;
        Bytes bytes;
        ComponentFeature disabled;
    };
    auto const oneOf = [](uint8_t id, std::initializer_list<uint8_t> element) {
        auto payload = Bytes { std::byte { 0x01 } };
        appendBytes(payload, element);
        return componentOf({ section(id, payload) });
    };
    auto valueExport = Bytes { std::byte { 0x01 } };
    appendBytes(valueExport, plainName("v"));
    appendBytes(valueExport, { 0x02, 0x00, 0x00 });
    auto valueAlias =
        Bytes { std::byte { 0x01 }, std::byte { 0x02 }, std::byte { 0x00 }, std::byte { 0x00 } };
    appendName(valueAlias, "v");
    auto const vectors = std::vector<Vector> {
        { .text = "Maps require the component model map feature",
          .bytes = oneOf(7, { 0x63, 0x73, 0x79 }),
          .disabled = ComponentFeature::Map },
        { .text = "async component functions require the component model async feature",
          .bytes = oneOf(7, { 0x43, 0x00, 0x01, 0x00 }),
          .disabled = ComponentFeature::Async },
        { .text = "`stream` requires the component model async feature",
          .bytes = oneOf(7, { 0x66, 0x01, 0x7D }),
          .disabled = ComponentFeature::Async },
        { .text = "Fixed-length lists require the component model fixed-length lists feature",
          .bytes = oneOf(7, { 0x67, 0x7D, 0x00 }),
          .disabled = ComponentFeature::FixedLengthLists },
        { .text = "`error-context` requires the component model error-context feature",
          .bytes = oneOf(7, { 0x70, 0x64 }),
          .disabled = ComponentFeature::ErrorContext },
        { .text = "canonical option `async` requires the component model async feature",
          .bytes = oneOf(8, { 0x01, 0x00, 0x00, 0x01, 0x06 }),
          .disabled = ComponentFeature::Async },
        { .text = "`backpressure.inc` requires the component model async feature",
          .bytes = oneOf(8, { 0x24 }),
          .disabled = ComponentFeature::Async },
        { .text = "support for component model `value`s is not enabled",
          .bytes = componentOf({ section(11, valueExport) }),
          .disabled = ComponentFeature::Values },
        { .text = "support for component model `value`s is not enabled",
          .bytes = componentOf({ section(6, valueAlias) }),
          .disabled = ComponentFeature::Values },
    };
    for (auto const& vector: vectors)
    {
        INFO(vector.text);
        auto const result = validatedBytes(vector.bytes);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == ValidationErrorCode::FeatureDisabled);
        CHECK(result.error().disabled == vector.disabled);
        CHECK(specMessage(result.error()) == vector.text);
    }

    // A name's attributes are gated here too (Validator.externDeclarations).
}

TEST_CASE("Validator.canonRefusalsAreWasmToolsTexts")
{
    // Each gated built-in is refused in wasm-tools' words for its gate, the gate its row names.
    for (auto const& row: isoline::component::binary::CanonBuiltinTable)
    {
        if (!row.gate)
            continue;
        INFO(row.text);
        auto builder = ComponentBuilder {};
        std::ignore = builder.canon(CanonDefinition {
            .builtin = row.builtin, .immediates = {}, .options = {}, .result = std::nullopt });
        auto const refused = refusal(builder.pool());
        CHECK(refused.disabled == row.gate);
        auto const expected = std::ranges::find(isoline::component::validate::CanonRefusalTable,
                                                row.builtin,
                                                &isoline::component::validate::CanonRefusalRow::builtin);
        REQUIRE(expected != isoline::component::validate::CanonRefusalTable.end());
        CHECK(specMessage(refused) == expected->text);
    }
    auto const textOf = [](CanonBuiltin builtin) {
        auto builder = ComponentBuilder {};
        std::ignore = builder.canon(
            CanonDefinition { .builtin = builtin, .immediates = {}, .options = {}, .result = std::nullopt });
        return refusalText(builder.pool());
    };
    CHECK(textOf(CanonBuiltin::ErrorContextNew)
          == "`error-context.new` requires the component model error-context feature");
    CHECK(textOf(CanonBuiltin::ThreadIndex)
          == "`thread.index` requires the component model threading feature");
    CHECK(textOf(CanonBuiltin::StreamForward)
          == "`stream.forward` requires the component model forward feature");
    CHECK(textOf(CanonBuiltin::ThreadAvailableParallelism)
          == "`thread.available_parallelism` requires the shared-everything-threads proposal");
    auto callback = ComponentBuilder {};
    std::ignore = callback.canonLower(0, { CanonOption { .kind = CanonOptionKind::Callback, .index = 0 } });
    CHECK(refusalText(callback.pool())
          == "canonical option `callback` requires the component model async feature");
}

TEST_CASE("Validator.externNamesDeferred")
{
    // The externname grammar beyond a plain label is Task 5's, strong uniqueness Task 7's: until
    // then both refuse, never accept unchecked.
    auto const importing = [](std::vector<std::string> names) {
        auto builder = ComponentBuilder {};
        auto const function = builder.type(TypeDefinition { .type = functionOf({}) });
        for (auto& name: names)
            std::ignore =
                builder.import(std::move(name), ExternDesc { .sort = Sort::Func, .index = function });
        return builder.pool();
    };
    CHECK(accepted(importing({ "a", "b-c", "d1" })));
    for (auto const& name: { "A b", "wasi:cli/run@0.2.0", "[method]r.f", "" })
    {
        INFO(name);
        auto const refused = refusal(importing({ name }));
        CHECK(refused.code == ValidationErrorCode::NotYetValidated);
        CHECK(specMessage(refused) == "not yet validated: extern names other than a plain label");
    }
    auto const duplicate = refusal(importing({ "a-b", "AB" }));
    CHECK(specMessage(duplicate) == "not yet validated: extern names not strongly unique");
    CHECK(duplicate.position.definition == 2);

    // Imports and exports are named apart: a component type may import and export "a".
    auto declarations = TypeScopeBuilder {};
    auto const function = declarations.type(TypeDefinition { .type = functionOf({}) });
    std::ignore = declarations.import("a", ExternDesc { .sort = Sort::Func, .index = function });
    std::ignore = declarations.exportItem("a", ExternDesc { .sort = Sort::Func, .index = function });
    auto both = ComponentBuilder {};
    std::ignore = both.componentType(std::move(declarations));
    CHECK(accepted(both.pool()));
    auto twice = TypeScopeBuilder {};
    auto const twiceFunction = twice.type(TypeDefinition { .type = functionOf({}) });
    std::ignore = twice.exportItem("a", ExternDesc { .sort = Sort::Func, .index = twiceFunction });
    std::ignore = twice.exportItem("A", ExternDesc { .sort = Sort::Func, .index = twiceFunction });
    auto exported = ComponentBuilder {};
    std::ignore = exported.instanceType(std::move(twice));
    CHECK(specMessage(refusal(exported.pool())) == "not yet validated: extern names not strongly unique");
}

TEST_CASE("Validator.outerAliasesIntoComponentsDeferred")
{
    // A type crossing into a component may not refer to a resource defined outside it; until Task 7
    // checks that, a type that may is refused, and one that cannot (a tuple of u32, as $t16) is not.
    auto const crossing = [](auto define) {
        auto builder = ComponentBuilder {};
        auto const type = define(builder);
        auto nested = ComponentBuilder {};
        std::ignore = nested.aliasOuter(Sort::Type, 1, type);
        std::ignore = builder.nested(std::move(nested));
        return builder.pool();
    };
    auto const deferred =
        "not yet validated: outer aliases into a component of a type that may refer to a resource";
    // (component (type $r (resource (rep i32))) (component (alias outer 1 $r (type))))
    CHECK(refusalText(crossing([](ComponentBuilder& builder) { return builder.resource(); })) == deferred);
    CHECK(refusalText(crossing([](ComponentBuilder& builder) {
              auto const resource = builder.resource();
              auto const own = builder.type(defined(OwnType { .resource = resource }));
              return builder.type(defined(ListType { .element = at(own) }));
          }))
          == deferred);
    CHECK(refusalText(crossing([](ComponentBuilder& builder) {
              return builder.type(TypeDefinition { .type = functionOf({}) });
          }))
          == deferred);
    CHECK(refusalText(
              crossing([](ComponentBuilder& builder) { return builder.instanceType(TypeScopeBuilder {}); }))
          == deferred);
    CHECK(refusalText(
              crossing([](ComponentBuilder& builder) { return builder.componentType(TypeScopeBuilder {}); }))
          == deferred);
    CHECK(accepted(crossing([](ComponentBuilder& builder) {
        return builder.type(
            defined(TupleType { .elements = { PrimitiveValueType::U32, PrimitiveValueType::U32 } }));
    })));
    // Within the same component, or into a type scope, nothing crosses a component boundary.
    auto declarations = TypeScopeBuilder {};
    std::ignore = declarations.aliasOuter(Sort::Type, 1, 0);
    auto local = ComponentBuilder {};
    std::ignore = local.resource();
    std::ignore = local.instanceType(std::move(declarations));
    std::ignore = local.aliasOuter(Sort::Type, 0, 0);
    CHECK(accepted(local.pool()));
}
