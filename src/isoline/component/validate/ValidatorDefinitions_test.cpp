// SPDX-License-Identifier: Apache-2.0

// Task 7's validation: index spaces built incrementally, core modules, types and instances matched in
// the component's one canonical core type space, component instantiation, exports and their
// visibility, resource function names, and the canonical definitions with their options. The texts
// are wasm-tools 1.263.0's (wasmparser 0.263.0 src/validator/component.rs), which the component
// suites' assert_invalid match.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/binary/Types.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>
#include <isoline/component/Features.hpp>
#include <isoline/component/Limits.hpp>
#include <isoline/component/binary/Component.hpp>
#include <isoline/component/binary/testing/Builder.hpp>
#include <isoline/component/binary/testing/DecoderFixtures.hpp>
#include <isoline/component/types/TypeArena.hpp>
#include <isoline/component/validate/Validator.hpp>
#include <isoline/component/validate/testing/CoreModuleValidator.hpp>

using isoline::binary::ExternKind;
using isoline::binary::FuncType;
using isoline::binary::GlobalType;
using isoline::binary::I32;
using isoline::binary::Limits;
using isoline::binary::TableType;
using isoline::binary::testing::ModuleBuilder;
using isoline::component::ComponentFeatureSet;
using isoline::component::ComponentLimit;
using isoline::component::ComponentLimits;
using isoline::component::binary::BorrowType;
using isoline::component::binary::CanonOption;
using isoline::component::binary::CanonOptionKind;
using isoline::component::binary::ComponentPool;
using isoline::component::binary::CoreExportDeclaration;
using isoline::component::binary::CoreExternType;
using isoline::component::binary::CoreImportDeclaration;
using isoline::component::binary::CoreInlineExport;
using isoline::component::binary::CoreModuleTypeDefinition;
using isoline::component::binary::CoreOuterAliasDeclaration;
using isoline::component::binary::CoreSort;
using isoline::component::binary::CoreTypeDeclaration;
using isoline::component::binary::CoreTypeDefinition;
using isoline::component::binary::DefinedValueType;
using isoline::component::binary::ExternDesc;
using isoline::component::binary::FunctionTypeDefinition;
using isoline::component::binary::LabeledType;
using isoline::component::binary::ListType;
using isoline::component::binary::OwnType;
using isoline::component::binary::PrimitiveValueType;
using isoline::component::binary::RecordType;
using isoline::component::binary::ResultType;
using isoline::component::binary::Sort;
using isoline::component::binary::SortIndex;
using isoline::component::binary::TypeBound;
using isoline::component::binary::TypeDefinition;
using isoline::component::binary::TypeIndex;
using isoline::component::binary::ValueTypeRef;
using isoline::component::binary::testing::Bytes;
using isoline::component::binary::testing::ComponentBuilder;
using isoline::component::binary::testing::modulePreamble;
using isoline::component::binary::testing::TypeScopeBuilder;
using isoline::component::validate::exceededLimit;
using isoline::component::validate::specMessage;
using isoline::component::validate::validateComponent;
using isoline::component::validate::ValidatedComponent;
using isoline::component::validate::ValidationError;
using isoline::component::validate::testing::CoreModuleValidator;

namespace
{
using Validated = std::expected<std::shared_ptr<ValidatedComponent const>, ValidationError>;

[[nodiscard]] Validated validated(ComponentPool pool, ComponentLimits const& limits = {})
{
    auto coreModules = CoreModuleValidator {};
    return validateComponent(std::move(pool), ComponentFeatureSet::defaults(), limits, coreModules);
}

[[nodiscard]] ValidationError refusal(ComponentPool pool, ComponentLimits const& limits = {})
{
    auto result = validated(std::move(pool), limits);
    REQUIRE_FALSE(result.has_value());
    return result.error();
}

[[nodiscard]] std::string refusalText(ComponentPool pool, ComponentLimits const& limits = {})
{
    return specMessage(refusal(std::move(pool), limits));
}

[[nodiscard]] bool accepted(ComponentPool pool)
{
    auto const result = validated(std::move(pool));
    if (!result)
        UNSCOPED_INFO(specMessage(result.error()));
    return result.has_value();
}

[[nodiscard]] ValueTypeRef at(uint32_t index)
{
    return TypeIndex { .index = index };
}

[[nodiscard]] TypeDefinition defined(DefinedValueType type)
{
    return TypeDefinition { .type = std::move(type) };
}

[[nodiscard]] TypeDefinition function(std::vector<LabeledType> params,
                                      std::optional<ValueTypeRef> result = {})
{
    return TypeDefinition { .type =
                                FunctionTypeDefinition { .params = std::move(params), .result = result } };
}

[[nodiscard]] std::vector<LabeledType> u32Params(uint32_t count)
{
    auto params = std::vector<LabeledType> {};
    for (auto const index: std::views::iota(uint32_t { 0 }, count))
        params.push_back(
            LabeledType { .label = "p" + std::to_string(index), .type = PrimitiveValueType::U32 });
    return params;
}

[[nodiscard]] Bytes moduleBytes(ModuleBuilder& builder)
{
    auto const raw = builder.build();
    auto bytes = Bytes {};
    bytes.reserve(raw.size());
    for (auto const byte: raw)
        bytes.push_back(std::byte { byte });
    return bytes;
}

/// The library module of the canonical definition tests: memory "memory", realloc "realloc" of
/// [i32 i32 i32 i32] -> [i32], "nothing" of [] -> [], "number" of [] -> [i32], "wide" of
/// [i32 x 16] -> [], "one" of [i32] -> [], "pair" of [i32 i32] -> [], "badRealloc" of [i32] -> [i32],
/// and memory "memory64" of i64 addresses.
[[nodiscard]] Bytes library()
{
    auto builder = ModuleBuilder {};
    builder
        .type(FuncType { .params = { I32, I32, I32, I32 }, .results = { I32 } }) // 0
        .type(FuncType {})                                                       // 1
        .type(FuncType { .params = {}, .results = { I32 } })                     // 2
        .type(FuncType { .params = std::vector(16, I32), .results = {} })        // 3
        .type(FuncType { .params = { I32 }, .results = {} })                     // 4
        .type(FuncType { .params = { I32, I32 }, .results = {} })                // 5
        .type(FuncType { .params = { I32 }, .results = { I32 } })                // 6
        .function(0, {}, { 0x20, 0x00 })                                         // local.get 0
        .function(1, {}, {})
        .function(2, {}, { 0x41, 0x00 }) // i32.const 0
        .function(3, {}, {})
        .function(4, {}, {})
        .function(5, {}, {})
        .function(6, {}, { 0x20, 0x00 })
        .memory(Limits { .min = 1, .max = std::nullopt })
        .memory(Limits { .min = 1, .max = std::nullopt, .addressType = isoline::binary::AddressType::I64 })
        .exportEntity("memory", ExternKind::Memory, 0)
        .exportEntity("memory64", ExternKind::Memory, 1)
        .exportFunction("realloc", 0)
        .exportFunction("nothing", 1)
        .exportFunction("number", 2)
        .exportFunction("wide", 3)
        .exportFunction("one", 4)
        .exportFunction("pair", 5)
        .exportFunction("badRealloc", 6);
    return moduleBytes(builder);
}

/// A component instantiating library() as core instance 0, its exports aliased: core memory 0
/// "memory", core memory 1 "memory64", then core functions realloc 0, nothing 1, number 2, wide 3,
/// one 4, pair 5, badRealloc 6.
[[nodiscard]] ComponentBuilder withLibrary()
{
    auto builder = ComponentBuilder {};
    std::ignore = builder.coreModule(library());
    std::ignore = builder.coreInstantiate(0, {});
    std::ignore = builder.aliasCoreExport(0, "memory", CoreSort::Memory);
    std::ignore = builder.aliasCoreExport(0, "memory64", CoreSort::Memory);
    for (auto const* name: { "realloc", "nothing", "number", "wide", "one", "pair", "badRealloc" })
        std::ignore = builder.aliasCoreExport(0, name, CoreSort::Func);
    return builder;
}

/// The core function indices of withLibrary().
enum LibraryFunction : uint8_t
{
    Realloc = 0,
    Nothing = 1,
    Number = 2,
    Wide = 3,
    One = 4,
    Pair = 5,
    BadRealloc = 6,
};

/// Which resource an instantiation argument's function names.
enum class Supplied : uint8_t
{
    Same,  ///< The resource supplied for the import.
    Other, ///< Another one.
};

/// Which entry an exported function's type names.
enum class Named : uint8_t
{
    AsDefined, ///< The type as defined.
    ByExport,  ///< The entry its export added.
};

[[nodiscard]] CanonOption option(CanonOptionKind kind, uint32_t index = 0)
{
    return CanonOption { .kind = kind, .index = index };
}

/// The memory and realloc options of withLibrary().
[[nodiscard]] std::vector<CanonOption> memoryAndRealloc()
{
    return { option(CanonOptionKind::Memory, 0), option(CanonOptionKind::Realloc, Realloc) };
}
} // namespace

TEST_CASE("Validator.instantiateArgs")
{
    // instantiate_component (component.rs:3281-3530): every import supplied, by its exact name, by a
    // subtype of its type (open_instance_type's texts).
    auto const child = [] {
        auto nested = ComponentBuilder {};
        auto const type = nested.type(function({}));
        std::ignore = nested.import("a", ExternDesc { .sort = Sort::Func, .index = type });
        return nested;
    };
    auto missing = ComponentBuilder {};
    std::ignore = missing.nested(child());
    std::ignore = missing.instantiate(0, {});
    CHECK(refusalText(missing.pool()) == "missing import named `a`");

    auto const supplying = [&](TypeDefinition supplied, std::string name = "a") {
        auto builder = ComponentBuilder {};
        std::ignore = builder.nested(child());
        auto const type = builder.type(std::move(supplied));
        std::ignore = builder.import("f", ExternDesc { .sort = Sort::Func, .index = type });
        std::ignore =
            builder.instantiate(0, { { std::move(name), SortIndex { .sort = Sort::Func, .index = 0 } } });
        return builder.pool();
    };
    CHECK(accepted(supplying(function({}))));
    auto const wrong = refusalText(supplying(function(u32Params(1))));
    CHECK(wrong.starts_with("type mismatch for import `a`\n"));
    CHECK(wrong.contains("expected 0 parameters, found 1"));
    // By its exact name: an extra argument is ignored, a differently spelled one supplies nothing.
    CHECK(refusalText(supplying(function({}), "A")) == "missing import named `a`");

    // Each argument name once.
    auto twice = ComponentBuilder {};
    std::ignore = twice.nested(child());
    auto const type = twice.type(function({}));
    std::ignore = twice.import("f", ExternDesc { .sort = Sort::Func, .index = type });
    std::ignore = twice.instantiate(0,
                                    { { "a", SortIndex { .sort = Sort::Func, .index = 0 } },
                                      { "a", SortIndex { .sort = Sort::Func, .index = 0 } } });
    CHECK(refusalText(twice.pool()) == "instantiation argument `a` conflicts with previous argument `a`");

    // The argument's index is checked first.
    auto unknown = ComponentBuilder {};
    std::ignore = unknown.nested(child());
    std::ignore = unknown.instantiate(0, { { "a", SortIndex { .sort = Sort::Func, .index = 9 } } });
    CHECK(refusalText(unknown.pool()) == "unknown function 9: function index out of bounds");
    auto noComponent = ComponentBuilder {};
    std::ignore = noComponent.instantiate(3, {});
    CHECK(refusalText(noComponent.pool()) == "unknown component 3: component index out of bounds");

    // A type import is substituted as supplied: the child's (sub resource) is the parent's resource,
    // so the child's function of an own of it takes the parent's.
    auto resourceChild = ComponentBuilder {};
    auto const imported = resourceChild.import(
        "r", ExternDesc { .sort = Sort::Type, .index = 0, .bound = TypeBound::SubResource });
    auto const childOwn = resourceChild.type(defined(OwnType { .resource = imported }));
    auto const childTake =
        resourceChild.type(function({ LabeledType { .label = "x", .type = at(childOwn) } }));
    std::ignore = resourceChild.import("f", ExternDesc { .sort = Sort::Func, .index = childTake });
    auto const parent = [&](Supplied supplied) {
        auto builder = ComponentBuilder {};
        std::ignore = builder.nested(resourceChild);
        auto const first = builder.import(
            "r", ExternDesc { .sort = Sort::Type, .index = 0, .bound = TypeBound::SubResource });
        auto const second = builder.import(
            "s", ExternDesc { .sort = Sort::Type, .index = 0, .bound = TypeBound::SubResource });
        auto const own =
            builder.type(defined(OwnType { .resource = supplied == Supplied::Same ? first : second }));
        auto const take = builder.type(function({ LabeledType { .label = "x", .type = at(own) } }));
        std::ignore = builder.import("f", ExternDesc { .sort = Sort::Func, .index = take });
        std::ignore = builder.instantiate(0,
                                          { { "r", SortIndex { .sort = Sort::Type, .index = first } },
                                            { "f", SortIndex { .sort = Sort::Func, .index = 0 } } });
        return builder.pool();
    };
    CHECK(accepted(parent(Supplied::Same)));
    auto const otherResource = refusalText(parent(Supplied::Other));
    CHECK(otherResource.starts_with("type mismatch for import `f`\n"));
    CHECK(otherResource.contains("resource types are not the same"));
}

TEST_CASE("Validator.instantiationMintsResources")
{
    // Each instantiation of a component that defines a resource mints it afresh (Explainer,
    // "generative"): the two instances' resources differ, so one's function cannot stand for the other's.
    auto definer = ComponentBuilder {};
    std::ignore = definer.coreModule(library());
    std::ignore = definer.coreInstantiate(0, {});
    std::ignore = definer.aliasCoreExport(0, "number", CoreSort::Func);
    auto const resource = definer.resource();
    auto const exportedResource = definer.exportItem("r", Sort::Type, resource);
    auto const own = definer.type(defined(OwnType { .resource = exportedResource }));
    auto const make = definer.type(function({}, at(own)));
    auto const lifted = definer.canonLift(0, {}, make);
    std::ignore = definer.exportItem("make", Sort::Func, lifted);

    auto builder = ComponentBuilder {};
    std::ignore = builder.nested(definer);
    std::ignore = builder.instantiate(0, {});
    std::ignore = builder.instantiate(0, {});
    auto const first = builder.aliasExport(0, "r", Sort::Type);
    auto const second = builder.aliasExport(1, "r", Sort::Type);
    std::ignore = builder.aliasExport(0, "make", Sort::Func);
    auto const result = validated(builder.pool());
    REQUIRE(result.has_value());
    auto const& scope = (*result)->scopes.front();
    REQUIRE(scope.types.size() == 2);
    CHECK(scope.types[first].type != scope.types[second].type);
    CHECK(scope.instances.size() == 2);
    CHECK(scope.functions.size() == 1);

    // A child's resource whose representation the parent does not know is not the parent's own.
    auto foreign = ComponentBuilder {};
    std::ignore = foreign.nested(definer);
    std::ignore = foreign.instantiate(0, {});
    auto const aliased = foreign.aliasExport(0, "r", Sort::Type);
    std::ignore = foreign.canon(isoline::component::binary::CanonDefinition {
        .builtin = isoline::component::binary::CanonBuiltin::ResourceNew,
        .immediates = { aliased },
        .options = {},
        .result = {} });
    CHECK(refusalText(foreign.pool()) == "type 0 is not a local resource");
}

TEST_CASE("Validator.exportsIntroduceIndex")
{
    // Binary.md: an export adds an entry to its sort's index space, which later definitions name.
    auto builder = withLibrary();
    auto const type = builder.type(function({}));
    auto const lifted = builder.canonLift(Nothing, {}, type);
    auto const exported = builder.exportItem("f", Sort::Func, lifted);
    CHECK(exported == lifted + 1);
    std::ignore = builder.exportItem("g", Sort::Func, exported);
    auto const result = validated(builder.pool());
    REQUIRE(result.has_value());
    auto const& root = (*result)->type;
    REQUIRE(root.exports.size() == 2);
    CHECK(root.exports[1].name == "g");
    CHECK((*result)->scopes.front().functions.size() == 3);

    auto past = withLibrary();
    auto const pastType = past.type(function({}));
    auto const pastLifted = past.canonLift(Nothing, {}, pastType);
    std::ignore = past.exportItem("f", Sort::Func, pastLifted);
    std::ignore = past.exportItem("g", Sort::Func, 2);
    CHECK(refusalText(past.pool()) == "unknown function 2: function index out of bounds");

    // A type export adds a type index of its own, a copy of the type, which an exported function must
    // name: the record itself is not nameable by an export.
    auto const exporting = [](Named namedBy) {
        auto component = withLibrary();
        auto const record = component.type(defined(
            RecordType { .fields = { LabeledType { .label = "a", .type = PrimitiveValueType::U32 } } }));
        auto const named = component.exportItem("t", Sort::Type, record);
        auto const take = component.type(function(
            { LabeledType { .label = "x", .type = at(namedBy == Named::ByExport ? named : record) } }));
        auto const lifted = component.canonLift(One, {}, take);
        std::ignore = component.exportItem("f", Sort::Func, lifted);
        return component.pool();
    };
    CHECK(accepted(exporting(Named::ByExport)));
    CHECK(refusalText(exporting(Named::AsDefined)) == "func not valid to be used as export");
}

TEST_CASE("Validator.externalVisibility")
{
    // validate_and_register_namedBytypes (component.rs:816-913): what an export names must be exported.
    auto const lifting = [](auto handleOf) {
        auto builder = withLibrary();
        auto const resource = builder.resource();
        auto const named = builder.exportItem("r", Sort::Type, resource);
        auto const own = builder.type(defined(OwnType { .resource = handleOf(resource, named) }));
        auto const make = builder.type(function({}, at(own)));
        auto const lifted = builder.canonLift(Number, {}, make);
        std::ignore = builder.exportItem("make", Sort::Func, lifted);
        return builder.pool();
    };
    // An own of the exported entry, as wit-component makes them: accepted.
    CHECK(accepted(lifting([](uint32_t, uint32_t named) { return named; })));
    // An own of the resource as defined, which is not the exported entry: refused.
    CHECK(refusalText(lifting([](uint32_t resource, uint32_t) { return resource; }))
          == "func not valid to be used as export");

    // A resource never exported at all.
    auto unexported = withLibrary();
    auto const resource = unexported.resource();
    auto const own = unexported.type(defined(OwnType { .resource = resource }));
    auto const make = unexported.type(function({}, at(own)));
    auto const lifted = unexported.canonLift(Number, {}, make);
    std::ignore = unexported.exportItem("make", Sort::Func, lifted);
    CHECK(refusalText(unexported.pool()) == "func not valid to be used as export");

    // A type export naming an unexported record.
    auto records = ComponentBuilder {};
    auto const inner = records.type(
        defined(RecordType { .fields = { LabeledType { .label = "a", .type = PrimitiveValueType::U32 } } }));
    auto const outer =
        records.type(defined(RecordType { .fields = { LabeledType { .label = "b", .type = at(inner) } } }));
    std::ignore = records.exportItem("outer", Sort::Type, outer);
    CHECK(refusalText(records.pool()) == "type not valid to be used as export");
    // Anonymous types need no name: a tuple or a list of u32 is exportable as it is.
    auto anonymous = ComponentBuilder {};
    auto const list = anonymous.type(defined(ListType { .element = PrimitiveValueType::U32 }));
    std::ignore = anonymous.exportItem("l", Sort::Type, list);
    CHECK(accepted(anonymous.pool()));

    // Imports may not name what exports introduced.
    auto imports = ComponentBuilder {};
    auto const record = imports.type(
        defined(RecordType { .fields = { LabeledType { .label = "a", .type = PrimitiveValueType::U32 } } }));
    auto const exportedRecord = imports.exportItem("t", Sort::Type, record);
    std::ignore = imports.import(
        "u", ExternDesc { .sort = Sort::Type, .index = exportedRecord, .bound = TypeBound::Equal });
    CHECK(refusalText(imports.pool()) == "type not valid to be used as import");
    // An imported type is nameable by imports and exports alike.
    auto reexport = ComponentBuilder {};
    auto const plain = reexport.type(
        defined(RecordType { .fields = { LabeledType { .label = "a", .type = PrimitiveValueType::U32 } } }));
    auto const imported =
        reexport.import("t", ExternDesc { .sort = Sort::Type, .index = plain, .bound = TypeBound::Equal });
    auto const take = reexport.type(function({ LabeledType { .label = "x", .type = at(imported) } }));
    std::ignore = reexport.import("f", ExternDesc { .sort = Sort::Func, .index = take });
    std::ignore = reexport.exportItem("f", Sort::Func, 0);
    CHECK(accepted(reexport.pool()));
}

TEST_CASE("Validator.methodShapes")
{
    // ComponentNameContext::validate (component.rs:4885-5090): the function a resource name names.
    auto const importing = [](std::string name, auto signature) {
        auto builder = ComponentBuilder {};
        auto const resource = builder.import(
            "r", ExternDesc { .sort = Sort::Type, .index = 0, .bound = TypeBound::SubResource });
        auto const type = signature(builder, resource);
        std::ignore = builder.import(std::move(name), ExternDesc { .sort = Sort::Func, .index = type });
        return builder.pool();
    };
    auto const borrowSelf = [](ComponentBuilder& builder, uint32_t resource) {
        auto const handle = builder.type(defined(BorrowType { .resource = resource }));
        return builder.type(function({ LabeledType { .label = "self", .type = at(handle) } }));
    };
    CHECK(accepted(importing("[method]r.f", borrowSelf)));
    CHECK(refusalText(importing(
              "[method]r.f", [](ComponentBuilder& builder, uint32_t) { return builder.type(function({})); }))
          == "import name `[method]r.f` is not valid\nfunction should have at least one argument");
    CHECK(refusalText(importing("[method]r.f",
                                [](ComponentBuilder& builder, uint32_t resource) {
                                    auto const handle =
                                        builder.type(defined(BorrowType { .resource = resource }));
                                    return builder.type(
                                        function({ LabeledType { .label = "this", .type = at(handle) } }));
                                }))
          == "import name `[method]r.f` is not valid\nfunction should have a first argument called `self`");
    CHECK(
        refusalText(importing("[method]r.f",
                              [](ComponentBuilder& builder, uint32_t) {
                                  return builder.type(function(
                                      { LabeledType { .label = "self", .type = PrimitiveValueType::U32 } }));
                              }))
        == "import name `[method]r.f` is not valid\nfunction should take a first argument of `(borrow $T)`");
    CHECK(refusalText(importing("[method]s.f", borrowSelf))
          == "import name `[method]s.f` is not valid\nfunction does not match expected resource name `r`");

    auto const ownResult = [](ComponentBuilder& builder, uint32_t resource) {
        auto const handle = builder.type(defined(OwnType { .resource = resource }));
        return builder.type(function({}, at(handle)));
    };
    CHECK(accepted(importing("[constructor]r", ownResult)));
    CHECK(accepted(importing("[constructor]r", [](ComponentBuilder& builder, uint32_t resource) {
        auto const handle = builder.type(defined(OwnType { .resource = resource }));
        auto const result =
            builder.type(defined(ResultType { .ok = at(handle), .error = PrimitiveValueType::String }));
        return builder.type(function({}, at(result)));
    })));
    CHECK(refusalText(importing("[constructor]r",
                                [](ComponentBuilder& builder, uint32_t) {
                                    return builder.type(function({}, PrimitiveValueType::U32));
                                }))
          == "import name `[constructor]r` is not valid\nfunction should return `(own $T)` or `(result (own "
             "$T))`");
    CHECK(
        refusalText(importing("[constructor]r",
                              [](ComponentBuilder& builder, uint32_t) { return builder.type(function({})); }))
        == "import name `[constructor]r` is not valid\nfunction should return one value");
    CHECK(accepted(importing("[static]r.make", ownResult)));
    CHECK(refusalText(importing("[static]q.make", ownResult))
          == "import name `[static]q.make` is not valid\nstatic resource name is not known in this context");

    // A resource that is not one of the imports' named ones.
    auto local = ComponentBuilder {};
    auto const definedResource = local.resource();
    auto const handle = local.type(defined(BorrowType { .resource = definedResource }));
    auto const method = local.type(function({ LabeledType { .label = "self", .type = at(handle) } }));
    std::ignore = local.import("[method]r.f", ExternDesc { .sort = Sort::Func, .index = method });
    // Its visibility is checked first: the resource is not nameable by an import at all.
    CHECK(refusalText(local.pool()) == "func not valid to be used as import");

    // A name of a resource function on an item that is no function.
    auto notFunction = ComponentBuilder {};
    std::ignore = notFunction.import(
        "[static]r.f", ExternDesc { .sort = Sort::Type, .index = 0, .bound = TypeBound::SubResource });
    CHECK(refusalText(notFunction.pool()) == "import name `[static]r.f` is not valid\nitem is not a func");
}

TEST_CASE("Validator.canonOptions")
{
    // check_options and ComponentFuncType::lower (component.rs:2652-2875, component_types.rs:1236-1359).
    auto const lowering = [](TypeDefinition type, std::vector<CanonOption> options) {
        auto builder = withLibrary();
        auto const signature = builder.type(std::move(type));
        std::ignore = builder.import("f", ExternDesc { .sort = Sort::Func, .index = signature });
        std::ignore = builder.canonLower(0, std::move(options));
        return builder.pool();
    };
    auto const stringParam = function({ LabeledType { .label = "s", .type = PrimitiveValueType::String } });
    CHECK(refusalText(lowering(stringParam, {})) == "canonical option `memory` is required");
    CHECK(accepted(lowering(stringParam, { option(CanonOptionKind::Memory, 0) })));
    // A lowered result holding a pointer needs the realloc too.
    auto const stringResult = function({}, PrimitiveValueType::String);
    CHECK(refusalText(lowering(stringResult, { option(CanonOptionKind::Memory, 0) }))
          == "canonical option `realloc` is required");
    CHECK(accepted(lowering(stringResult, memoryAndRealloc())));

    CHECK(
        refusalText(lowering(function({}), { option(CanonOptionKind::Utf8), option(CanonOptionKind::Utf16) }))
        == "canonical encoding option `utf8` conflicts with option `utf16`");
    CHECK(refusalText(
              lowering(function({}), { option(CanonOptionKind::Latin1Utf16), option(CanonOptionKind::Utf8) }))
          == "canonical encoding option `latin1-utf16` conflicts with option `utf8`");
    CHECK(refusalText(lowering(function({}), { option(CanonOptionKind::Realloc, Realloc) }))
          == "canonical option `realloc` requires `memory` to also be specified");
    CHECK(refusalText(lowering(function({}),
                               { option(CanonOptionKind::Memory, 0), option(CanonOptionKind::Memory, 0) }))
          == "canonical option `memory` is specified more than once");
    CHECK(refusalText(lowering(function({}), { option(CanonOptionKind::Memory, 7) }))
          == "unknown memory 7: memory index out of bounds");
    CHECK(refusalText(lowering(function({}), { option(CanonOptionKind::Memory, 1) }))
          == "64-bit memories require the `cm64` feature to be enabled");
    CHECK(refusalText(
              lowering(function({}),
                       { option(CanonOptionKind::Memory, 0), option(CanonOptionKind::Realloc, BadRealloc) }))
          == "canonical option `realloc` uses a core function with an incorrect signature");
    CHECK(refusalText(lowering(function({}),
                               { option(CanonOptionKind::Memory, 0), option(CanonOptionKind::Realloc, 99) }))
          == "unknown core function 99: function index out of bounds");
    CHECK(refusalText(lowering(function({}), { option(CanonOptionKind::PostReturn, Nothing) }))
          == "canonical option `post-return` cannot be specified for lowerings");
    CHECK(refusalText(lowering(function({}), { option(CanonOptionKind::Callback, Nothing) }))
          == "cannot specify callback without async");

    // A lift: the parameters past 16 flat values spill through memory, which then needs the realloc.
    auto const lifting = [](uint32_t core, TypeDefinition type, std::vector<CanonOption> options) {
        auto builder = withLibrary();
        auto const signature = builder.type(std::move(type));
        std::ignore = builder.canonLift(core, std::move(options), signature);
        return builder.pool();
    };
    CHECK(accepted(lifting(Wide, function(u32Params(16)), {})));
    CHECK(refusalText(lifting(One, function(u32Params(17)), {})) == "canonical option `memory` is required");
    CHECK(refusalText(lifting(One, function(u32Params(17)), { option(CanonOptionKind::Memory, 0) }))
          == "canonical option `realloc` is required");
    CHECK(accepted(lifting(One, function(u32Params(17)), memoryAndRealloc())));
    // A lifted string parameter needs both.
    CHECK(refusalText(lifting(Pair, stringParam, { option(CanonOptionKind::Memory, 0) }))
          == "canonical option `realloc` is required");
    CHECK(accepted(lifting(Pair, stringParam, memoryAndRealloc())));
    // A lifted result past one flat value returns a pointer, which needs the memory.
    auto const pairResult = [](ComponentBuilder& builder) {
        auto const tuple = builder.type(defined(isoline::component::binary::TupleType {
            .elements = { PrimitiveValueType::U32, PrimitiveValueType::U32 } }));
        return builder.type(function({}, at(tuple)));
    };
    auto wideResult = withLibrary();
    std::ignore = wideResult.canonLift(Number, {}, pairResult(wideResult));
    CHECK(refusalText(wideResult.pool()) == "canonical option `memory` is required");
    auto wideResultWithMemory = withLibrary();
    std::ignore = wideResultWithMemory.canonLift(
        Number, { option(CanonOptionKind::Memory, 0) }, pairResult(wideResultWithMemory));
    CHECK(accepted(wideResultWithMemory.pool()));

    // The core function's type must be the lowered one.
    CHECK(refusalText(lifting(Nothing, function(u32Params(1)), {}))
          == "lowered parameter types `[I32]` do not match parameter types `[]` of core function 1");
    CHECK(refusalText(lifting(Nothing, function({}, PrimitiveValueType::U64), {}))
          == "lowered result types `[I64]` do not match result types `[]` of core function 1");
    // post-return takes the lifted function's results and returns nothing.
    CHECK(accepted(lifting(
        Number, function({}, PrimitiveValueType::U32), { option(CanonOptionKind::PostReturn, One) })));
    CHECK(refusalText(lifting(Number,
                              function({}, PrimitiveValueType::U32),
                              { option(CanonOptionKind::PostReturn, Nothing) }))
          == "canonical option `post-return` uses a core function with an incorrect signature");
    // The type and the core function are looked up first.
    CHECK(refusalText(lifting(42, function({}), {}))
          == "unknown core function 42: function index out of bounds");
    auto notFunction = withLibrary();
    std::ignore = notFunction.type(defined(PrimitiveValueType::U32));
    std::ignore = notFunction.canonLift(Nothing, {}, 0);
    CHECK(refusalText(notFunction.pool()) == "type index 0 is not a function type");
}

TEST_CASE("Validator.resourceBuiltins")
{
    // resource.new, .rep and .drop (component.rs:1408-1427): new and rep of a resource this component
    // defined, drop of any; each a core function of its fixed core type.
    auto builder = ComponentBuilder {};
    auto const local = builder.resource();
    auto const imported =
        builder.import("r", ExternDesc { .sort = Sort::Type, .index = 0, .bound = TypeBound::SubResource });
    using isoline::component::binary::CanonBuiltin;
    using isoline::component::binary::CanonDefinition;
    auto const builtin = [](CanonBuiltin which, uint32_t resource) {
        return CanonDefinition { .builtin = which, .immediates = { resource }, .options = {}, .result = {} };
    };
    std::ignore = builder.canon(builtin(CanonBuiltin::ResourceNew, local));
    std::ignore = builder.canon(builtin(CanonBuiltin::ResourceRep, local));
    std::ignore = builder.canon(builtin(CanonBuiltin::ResourceDrop, local));
    std::ignore = builder.canon(builtin(CanonBuiltin::ResourceDrop, imported));
    auto const result = validated(builder.pool());
    REQUIRE(result.has_value());
    auto const& component = **result;
    auto const& functions = component.scopes.front().coreFunctions;
    REQUIRE(functions.size() == 4);
    CHECK(component.types[functions[0]] == FuncType { .params = { I32 }, .results = { I32 } });
    CHECK(component.types[functions[2]] == FuncType { .params = { I32 }, .results = {} });
    CHECK(functions[2] == functions[3]); // one canonical type

    auto foreign = ComponentBuilder {};
    std::ignore =
        foreign.import("r", ExternDesc { .sort = Sort::Type, .index = 0, .bound = TypeBound::SubResource });
    std::ignore = foreign.canon(builtin(CanonBuiltin::ResourceRep, 0));
    CHECK(refusalText(foreign.pool()) == "type 0 is not a local resource");
    auto notResource = ComponentBuilder {};
    std::ignore = notResource.type(defined(PrimitiveValueType::U32));
    std::ignore = notResource.canon(builtin(CanonBuiltin::ResourceDrop, 0));
    CHECK(refusalText(notResource.pool()) == "type index 0 is not a resource type");

    // A destructor is a core function of type [rep] -> [].
    auto const destroyed = [](uint32_t destructor) {
        auto component = withLibrary();
        std::ignore = component.resource(destructor);
        return component.pool();
    };
    CHECK(accepted(destroyed(One)));
    CHECK(refusalText(destroyed(Number)) == "core function 2 has wrong signature for a destructor");
}

TEST_CASE("Validator.coreInstances")
{
    // instantiate_core_module (component.rs:3204-3279).
    auto importer = ModuleBuilder {};
    importer.type(FuncType {}).importFunction("lib", "nothing", 0);
    auto const importing = moduleBytes(importer);
    auto const instantiating = [&](std::vector<std::pair<std::string, uint32_t>> arguments) {
        auto builder = ComponentBuilder {};
        std::ignore = builder.coreModule(library());
        std::ignore = builder.coreModule(importing);
        std::ignore = builder.coreInstantiate(0, {});
        std::ignore = builder.coreInstantiate(1, std::move(arguments));
        return builder.pool();
    };
    CHECK(accepted(instantiating({ { "lib", 0 } })));
    CHECK(refusalText(instantiating({})) == "missing module instantiation argument named `lib`");
    CHECK(refusalText(instantiating({ { "lib", 0 }, { "lib", 0 } }))
          == "duplicate module instantiation argument named `lib`");
    CHECK(refusalText(instantiating({ { "lib", 5 } }))
          == "unknown core instance 5: instance index out of bounds");

    // A module whose import the argument has no export of, or one of another type.
    auto const mismatching = [&](std::string_view field, uint32_t typeIndex) {
        auto other = ModuleBuilder {};
        other.type(FuncType {})
            .type(FuncType { .params = { I32 }, .results = {} })
            .importFunction("lib", field, typeIndex);
        auto builder = ComponentBuilder {};
        std::ignore = builder.coreModule(library());
        std::ignore = builder.coreModule(moduleBytes(other));
        std::ignore = builder.coreInstantiate(0, {});
        std::ignore = builder.coreInstantiate(1, { { "lib", 0 } });
        return builder.pool();
    };
    CHECK(refusalText(mismatching("absent", 0))
          == "module instantiation argument `lib` does not export an item named `absent`");
    CHECK(refusalText(mismatching("nothing", 1))
          == "type mismatch for export `nothing` of module instantiation argument `lib`\n"
             "expected: (func (param i32))\nfound:    (func)");
    CHECK(refusalText(mismatching("memory", 0))
          == "type mismatch for export `memory` of module instantiation argument `lib`\nexpected func, found "
             "memory");

    // A core instance of inline exports, each name once; an alias of one of its exports of its sort.
    auto inlined = withLibrary();
    std::ignore = inlined.coreInlineExports(
        { CoreInlineExport { .name = "f", .sort = CoreSort::Func, .index = Nothing },
          CoreInlineExport { .name = "m", .sort = CoreSort::Memory, .index = 0 } });
    std::ignore = inlined.aliasCoreExport(1, "f", CoreSort::Func);
    CHECK(accepted(inlined.pool()));
    auto repeated = withLibrary();
    std::ignore = repeated.coreInlineExports(
        { CoreInlineExport { .name = "f", .sort = CoreSort::Func, .index = Nothing },
          CoreInlineExport { .name = "f", .sort = CoreSort::Func, .index = Number } });
    CHECK(refusalText(repeated.pool()) == "duplicate instantiation export name `f` already defined");
    auto badSort = withLibrary();
    std::ignore = badSort.aliasCoreExport(0, "memory", CoreSort::Func);
    CHECK(refusalText(badSort.pool()) == "export `memory` for core instance 0 is not a function");
    auto absent = withLibrary();
    std::ignore = absent.aliasCoreExport(0, "absent", CoreSort::Func);
    CHECK(refusalText(absent.pool()) == "core instance 0 has no export named `absent`");
    auto unknownTable = withLibrary();
    std::ignore = unknownTable.coreInlineExports(
        { CoreInlineExport { .name = "t", .sort = CoreSort::Table, .index = 0 } });
    CHECK(refusalText(unknownTable.pool()) == "unknown table 0: table index out of bounds");

    // A module's imports are unique by (module, name) in a component.
    auto duplicate = ModuleBuilder {};
    duplicate.type(FuncType {}).importFunction("a", "b", 0).importFunction("a", "b", 0);
    auto duplicating = ComponentBuilder {};
    std::ignore = duplicating.coreModule(moduleBytes(duplicate));
    CHECK(refusalText(duplicating.pool())
          == "module has a duplicate import name `a:b` that is not allowed in components");
}

TEST_CASE("Validator.coreInstantiateRecGroups")
{
    // core-import-func-subtype.wast, core-type-out-of-group-deps.wast and empty-rec-groups.wast
    // (Wasmtime v51 misc_testsuite), transcribed without GC (R7): Isoline's core has no rec groups or
    // declared supertypes, so their forms are refused at decode, and what remains is matching by
    // canonical structure, typed references included, across modules.
    auto const provider = [](std::vector<FuncType> types, uint32_t exported) {
        auto builder = ModuleBuilder {};
        for (auto& type: types)
            builder.type(std::move(type));
        builder.function(exported, {}, {}).exportFunction("f", 0);
        return moduleBytes(builder);
    };
    auto const consumer = [](std::vector<FuncType> types, uint32_t imported) {
        auto builder = ModuleBuilder {};
        for (auto& type: types)
            builder.type(std::move(type));
        builder.importFunction("a", "f", imported);
        return moduleBytes(builder);
    };
    auto const linked = [](Bytes a, Bytes b) {
        auto builder = ComponentBuilder {};
        std::ignore = builder.coreModule(std::move(a));
        std::ignore = builder.coreModule(std::move(b));
        std::ignore = builder.coreInstantiate(0, {});
        std::ignore = builder.coreInstantiate(1, { { "a", 0 } });
        return builder.pool();
    };
    auto const target = FuncType { .params = {}, .results = { I32 } };
    auto const takesRef = [](uint32_t index) {
        return FuncType { .params = { isoline::binary::concreteRef(index, isoline::binary::Nullable::Yes) },
                          .results = {} };
    };
    // The same typed-reference shape at other positions of the two type sections: one canonical type.
    CHECK(accepted(
        linked(provider({ target, takesRef(0) }, 1), consumer({ FuncType {}, target, takesRef(1) }, 2))));
    // A reference to a type of another structure: refused, as Wasmtime refuses the instantiation.
    CHECK(refusalText(linked(provider({ target, takesRef(0) }, 1), consumer({ FuncType {}, takesRef(0) }, 1)))
              .starts_with("type mismatch for export `f` of module instantiation argument `a`\n"));
    // A declared module type matches an embedded module the same way: the component's one space.
    auto declared = ComponentBuilder {};
    std::ignore = declared.coreType(CoreTypeDefinition {
        .type = CoreModuleTypeDefinition {
            .declarations = { CoreTypeDeclaration { .type = target },
                              CoreTypeDeclaration { .type = takesRef(0) },
                              CoreImportDeclaration { .module = "",
                                                      .name = "f",
                                                      .type = CoreExternType { .kind = ExternKind::Func,
                                                                               .typeIndex = 1 } } } } });
    std::ignore = declared.import("m", ExternDesc { .sort = Sort::CoreModule, .index = 0 });
    CHECK(accepted(declared.pool()));
    // Empty rec groups and GC forms do not decode (empty-rec-groups.wast is gated `core feature cut gc`).
}

TEST_CASE("Validator.coreModuleTypes")
{
    // create_module_type (component.rs:2987-3045) and Module::check_type_ref (core.rs:680-930).
    auto const declaring = [](std::vector<isoline::component::binary::CoreModuleDeclaration> declarations) {
        auto builder = ComponentBuilder {};
        std::ignore = builder.coreType(CoreTypeDefinition { .type = FuncType {} });
        std::ignore = builder.coreType(CoreTypeDefinition {
            .type = CoreModuleTypeDefinition { .declarations = std::move(declarations) } });
        std::ignore = builder.import("m", ExternDesc { .sort = Sort::CoreModule, .index = 1 });
        return builder.pool();
    };
    using isoline::component::binary::CoreModuleDeclaration;
    auto const func = [](uint32_t index) {
        return CoreExternType { .kind = ExternKind::Func, .typeIndex = index };
    };
    CHECK(accepted(declaring({ CoreTypeDeclaration { .type = FuncType {} },
                               CoreExportDeclaration { .name = "f", .type = func(0) } })));
    CHECK(refusalText(declaring({ CoreExportDeclaration { .name = "f", .type = func(0) } }))
          == "unknown type 0: type index out of bounds");
    CHECK(refusalText(declaring({ CoreTypeDeclaration { .type = FuncType {} },
                                  CoreExportDeclaration { .name = "f", .type = func(0) },
                                  CoreExportDeclaration { .name = "f", .type = func(0) } }))
          == "duplicate export name `f` already defined");
    CHECK(refusalText(declaring({ CoreTypeDeclaration { .type = FuncType {} },
                                  CoreImportDeclaration { .module = "a", .name = "b", .type = func(0) },
                                  CoreImportDeclaration { .module = "a", .name = "b", .type = func(0) } }))
          == "module has a duplicate import name `a:b` that is not allowed in components");
    // An outer alias of the component's core function type 0; of its module type 1, refused.
    CHECK(accepted(declaring({ CoreOuterAliasDeclaration { .outerCount = 1, .outerIndex = 0 },
                               CoreExportDeclaration { .name = "f", .type = func(0) } })));
    CHECK(refusalText(declaring({ CoreOuterAliasDeclaration { .outerCount = 2, .outerIndex = 0 } }))
          == "invalid outer alias count of 1");
    auto selfAlias = ComponentBuilder {};
    std::ignore = selfAlias.coreType(CoreTypeDefinition { .type = CoreModuleTypeDefinition {} });
    std::ignore = selfAlias.coreType(CoreTypeDefinition {
        .type = CoreModuleTypeDefinition {
            .declarations = { CoreOuterAliasDeclaration { .outerCount = 1, .outerIndex = 0 } } } });
    CHECK(refusalText(selfAlias.pool())
          == "not implemented: aliasing core module types into a core module's types index space");
    // Limits and sizes by the core rules.
    auto const memory = [](Limits limits, uint8_t pageSizeLog2 = isoline::binary::DefaultPageSizeLog2) {
        return CoreExternType { .kind = ExternKind::Memory,
                                .memory = isoline::binary::MemoryType { .limits = limits,
                                                                        .pageSizeLog2 = pageSizeLog2 } };
    };
    CHECK(refusalText(declaring({ CoreImportDeclaration {
              .module = "a", .name = "m", .type = memory(Limits { .min = 2, .max = 1 }) } }))
          == "size minimum must not be greater than maximum");
    CHECK(refusalText(declaring({ CoreImportDeclaration {
              .module = "a", .name = "m", .type = memory(Limits { .min = 65'537, .max = std::nullopt }) } }))
          == "memory size must be at most 0x10000 65536-byte pages");
    CHECK(refusalText(declaring({ CoreImportDeclaration {
              .module = "a", .name = "m", .type = memory(Limits { .min = 1, .max = std::nullopt }, 3) } }))
          == "invalid custom page size");
    // The import of a module needs a module type.
    auto notModule = ComponentBuilder {};
    std::ignore = notModule.coreType(CoreTypeDefinition { .type = FuncType {} });
    std::ignore = notModule.import("m", ExternDesc { .sort = Sort::CoreModule, .index = 0 });
    CHECK(refusalText(notModule.pool()) == "core type index 0 is not a module type");
    // A module of a declared type matches an embedded module's by subtyping: it may import less.
    auto subtyped = ComponentBuilder {};
    std::ignore = subtyped.coreType(CoreTypeDefinition { .type = CoreModuleTypeDefinition {} });
    std::ignore = subtyped.coreModule(library());
    std::ignore =
        subtyped.exportItem("m", Sort::CoreModule, 0, ExternDesc { .sort = Sort::CoreModule, .index = 0 });
    CHECK(accepted(subtyped.pool()));
}

TEST_CASE("Validator.exportAliases")
{
    // alias_instance_export (component.rs:3813-3861): an existing export, of the alias' sort.
    auto declarations = TypeScopeBuilder {};
    auto const type = declarations.type(function({}));
    std::ignore = declarations.exportItem("f", ExternDesc { .sort = Sort::Func, .index = type });
    auto builder = ComponentBuilder {};
    auto const instanceType = builder.instanceType(std::move(declarations));
    std::ignore = builder.import("i", ExternDesc { .sort = Sort::Instance, .index = instanceType });
    auto good = builder;
    std::ignore = good.aliasExport(0, "f", Sort::Func);
    CHECK(accepted(good.pool()));
    auto absent = builder;
    std::ignore = absent.aliasExport(0, "g", Sort::Func);
    CHECK(refusalText(absent.pool()) == "instance 0 has no export named `g`");
    auto wrongSort = builder;
    std::ignore = wrongSort.aliasExport(0, "f", Sort::Instance);
    CHECK(refusalText(wrongSort.pool()) == "export `f` for instance 0 is not a instance");
    auto noInstance = ComponentBuilder {};
    std::ignore = noInstance.aliasExport(4, "f", Sort::Func);
    CHECK(refusalText(noInstance.pool()) == "unknown instance 4: instance index out of bounds");

    // A type scope aliases only types and instances.
    auto scope = TypeScopeBuilder {};
    std::ignore = scope.alias(isoline::component::binary::AliasDefinition {
        .sort = Sort::Func,
        .target = isoline::component::binary::AliasTarget::Export,
        .instance = 0,
        .name = "f" });
    auto inType = ComponentBuilder {};
    std::ignore = inType.instanceType(std::move(scope));
    CHECK(refusalText(inType.pool())
          == "aliases in a component or instance type may only refer to types or instances");
}

TEST_CASE("Validator.ascribedExports")
{
    // export_to_entity_type (component.rs:2936-2985): an ascribed type the item's type is a subtype of.
    auto const ascribing = [](TypeDefinition ascribed) {
        auto builder = withLibrary();
        auto const type = builder.type(function({}));
        auto const other = builder.type(std::move(ascribed));
        auto const lifted = builder.canonLift(Nothing, {}, type);
        std::ignore =
            builder.exportItem("f", Sort::Func, lifted, ExternDesc { .sort = Sort::Func, .index = other });
        return builder.pool();
    };
    CHECK(accepted(ascribing(function({}))));
    CHECK(refusalText(ascribing(function(u32Params(1))))
          == "ascribed type of export is not compatible with item's type\nexpected 1 parameters, found 0");
}

TEST_CASE("Validator.finishRefusesHiddenResources")
{
    // finish (component.rs:4560-4651): a resource the component defines and its exports name must be
    // exported itself.
    auto builder = withLibrary();
    auto const resource = builder.resource();
    auto declarations = TypeScopeBuilder {};
    auto const inner = declarations.aliasOuter(Sort::Type, 1, resource);
    std::ignore = declarations.exportItem("r", ExternDesc { .sort = Sort::Type, .index = inner });
    auto const instanceType = builder.instanceType(std::move(declarations));
    std::ignore = builder.inlineInstance({ { "r", SortIndex { .sort = Sort::Type, .index = resource } } });
    std::ignore = builder.exportItem(
        "i", Sort::Instance, 0, ExternDesc { .sort = Sort::Instance, .index = instanceType });
    CHECK(accepted(builder.pool()));
}

TEST_CASE("Validator.indexSpaceCaps")
{
    // check_max of each space (wasm-tools' limits): one past the cap is refused, in its words.
    auto limits = ComponentLimits {};
    limits.maxModules = 1;
    limits.maxComponents = 1;
    limits.maxInstances = 1;
    limits.maxExports = 1;
    auto modules = ComponentBuilder {};
    std::ignore = modules.coreModule(modulePreamble());
    std::ignore = modules.coreModule(modulePreamble());
    CHECK(refusalText(modules.pool(), limits) == "modules count exceeds limit of 1");
    auto components = ComponentBuilder {};
    std::ignore = components.nested(ComponentBuilder {});
    std::ignore = components.nested(ComponentBuilder {});
    CHECK(refusalText(components.pool(), limits) == "components count exceeds limit of 1");
    auto instances = ComponentBuilder {};
    std::ignore = instances.coreModule(modulePreamble());
    std::ignore = instances.coreInstantiate(0, {});
    std::ignore = instances.inlineInstance({});
    CHECK(exceededLimit(refusal(instances.pool(), limits)) == ComponentLimit::Instances);
    auto exports = ComponentBuilder {};
    auto const record = exports.type(defined(ListType { .element = PrimitiveValueType::U32 }));
    std::ignore = exports.exportItem("a", Sort::Type, record);
    std::ignore = exports.exportItem("b", Sort::Type, record);
    CHECK(refusalText(exports.pool(), limits) == "exports count exceeds limit of 1");
}

TEST_CASE("Validator.instantiationSizeCapped")
{
    // very-nested.wast:1690-1778 (wasm-tools 1.263.0), the case Task 4 left to instantiation: each
    // component exports ten instances of the previous one, so its type grows tenfold per level, and
    // the instance types of the instantiations measure what their components export.
    auto const levels = [](uint32_t count) {
        auto builder = ComponentBuilder {};
        std::ignore = builder.nested(ComponentBuilder {});
        for (auto const level: std::views::iota(uint32_t { 1 }, count + 1))
        {
            auto nested = ComponentBuilder {};
            auto const previous = nested.aliasOuter(Sort::Component, 1, level - 1);
            for (auto const index: std::views::iota(0, 10))
            {
                auto const instance = nested.instantiate(previous, {});
                std::ignore = nested.exportItem("e" + std::to_string(index), Sort::Instance, instance);
            }
            std::ignore = builder.nested(std::move(nested));
        }
        return builder.pool();
    };
    CHECK(accepted(levels(5)));
    auto const refused = refusal(levels(7));
    CHECK(specMessage(refused) == "effective type size exceeds the limit of 1000000");
    CHECK(exceededLimit(refused) == ComponentLimit::TypeSize);
    // At the tenth export of $m6, which takes its component type past the limit: an instance type
    // measures what its component exports, which the component's type already bounds.
    CHECK(refused.position
          == isoline::component::binary::DefinitionPosition { .scope = 7, .definition = 18 });
}

TEST_CASE("Validator.coreTypesShareOneSpace")
{
    // The component's core types are one canonical space (carried from Task 6): every module's type
    // section is interned once, so equal types across modules are one id, and a match is an equality.
    auto builder = ComponentBuilder {};
    std::ignore = builder.coreModule(library());
    std::ignore = builder.coreModule(library());
    std::ignore =
        builder.coreType(CoreTypeDefinition { .type = FuncType { .params = { I32 }, .results = {} } });
    auto const result = validated(builder.pool());
    REQUIRE(result.has_value());
    auto const& component = **result;
    CHECK(component.types.coreFunctionTypeCount() == 7);
    REQUIRE(component.coreModules.size() == 2);
    auto const& first = component.types[component.coreModules[0].type];
    auto const& second = component.types[component.coreModules[1].type];
    CHECK(first.exports == second.exports);
}

TEST_CASE("Validator.localResourcesStayLocal")
{
    // finish (component.rs:4560-4651): no import may name a resource the component defines. A type
    // import of it passes the visibility rule (a resource is always nameable) and is refused here.
    auto builder = ComponentBuilder {};
    auto const resource = builder.resource();
    std::ignore =
        builder.import("x", ExternDesc { .sort = Sort::Type, .index = resource, .bound = TypeBound::Equal });
    CHECK(refusalText(builder.pool()) == "local resource type found in imports");

    // An exported instance type names what its exports name: a function of an unexported record is
    // not nameable by an export, though the instance type itself may declare it.
    auto declarations = TypeScopeBuilder {};
    auto const record = declarations.type(
        defined(RecordType { .fields = { LabeledType { .label = "a", .type = PrimitiveValueType::U32 } } }));
    auto const take = declarations.type(function({ LabeledType { .label = "x", .type = at(record) } }));
    std::ignore = declarations.exportItem("f", ExternDesc { .sort = Sort::Func, .index = take });
    auto component = ComponentBuilder {};
    auto const instanceType = component.instanceType(std::move(declarations));
    std::ignore = component.exportItem("t", Sort::Type, instanceType);
    CHECK(refusalText(component.pool()) == "type not valid to be used as export");
    // Exported with its record, it is nameable.
    auto withRecord = TypeScopeBuilder {};
    auto const innerRecord = withRecord.type(
        defined(RecordType { .fields = { LabeledType { .label = "a", .type = PrimitiveValueType::U32 } } }));
    auto const exportedRecord =
        withRecord.exportItem("r", ExternDesc { .sort = Sort::Type, .index = innerRecord });
    auto const innerTake =
        withRecord.type(function({ LabeledType { .label = "x", .type = at(exportedRecord) } }));
    std::ignore = withRecord.exportItem("f", ExternDesc { .sort = Sort::Func, .index = innerTake });
    auto nameable = ComponentBuilder {};
    auto const nameableType = nameable.instanceType(std::move(withRecord));
    std::ignore = nameable.exportItem("t", Sort::Type, nameableType);
    CHECK(accepted(nameable.pool()));
}

TEST_CASE("Validator.coreModuleTypeLimits")
{
    // Module::check_type_ref's table and reference rules (core.rs:719-879).
    auto const importing = [](CoreExternType type) {
        auto builder = ComponentBuilder {};
        std::ignore = builder.coreType(CoreTypeDefinition {
            .type = CoreModuleTypeDefinition {
                .declarations = { CoreTypeDeclaration { .type = FuncType {} },
                                  CoreImportDeclaration { .module = "a", .name = "b", .type = type } } } });
        return builder.pool();
    };
    auto const table = [](Limits limits, isoline::binary::ValueType element = isoline::binary::FuncRef) {
        return CoreExternType { .kind = ExternKind::Table,
                                .table =
                                    isoline::binary::TableType { .element = element, .limits = limits } };
    };
    CHECK(accepted(importing(table(Limits { .min = 1, .max = 2 }))));
    CHECK(refusalText(importing(table(Limits { .min = 3, .max = 2 })))
          == "size minimum must not be greater than maximum");
    CHECK(refusalText(importing(table(Limits { .min = uint64_t { 1 } << 32U, .max = std::nullopt })))
          == "table size must be at most 0xffffffff entries");
    // A concrete reference names a type of the module type's own space.
    CHECK(accepted(importing(table(Limits { .min = 0, .max = std::nullopt },
                                   isoline::binary::concreteRef(0, isoline::binary::Nullable::Yes)))));
    CHECK(refusalText(importing(table(Limits { .min = 0, .max = std::nullopt },
                                      isoline::binary::concreteRef(5, isoline::binary::Nullable::Yes))))
          == "unknown type 5: type index out of bounds");
    // A tag's type has no results.
    CHECK(accepted(importing(CoreExternType { .kind = ExternKind::Tag, .typeIndex = 0 })));
    auto results = ComponentBuilder {};
    std::ignore = results.coreType(CoreTypeDefinition {
        .type = CoreModuleTypeDefinition {
            .declarations = { CoreTypeDeclaration { .type = FuncType { .params = {}, .results = { I32 } } },
                              CoreImportDeclaration {
                                  .module = "a",
                                  .name = "t",
                                  .type = CoreExternType { .kind = ExternKind::Tag, .typeIndex = 0 } } } } });
    CHECK(refusalText(results.pool()) == "invalid exception type: non-empty tag result type");
}

TEST_CASE("Validator.moduleTypeSize")
{
    // Module::type_size (core.rs:598, 630): 1, then 2 plus its parameters and results per function
    // import or export, 1 per other one; refused once it reaches maxTypeSize.
    auto builder = ComponentBuilder {};
    std::ignore = builder.coreModule(library());
    auto limits = ComponentLimits {};
    limits.maxTypeSize = 40;
    auto const refused = refusal(builder.pool(), limits);
    CHECK(exceededLimit(refused) == ComponentLimit::TypeSize);
    CHECK(specMessage(refused) == "effective type size exceeds the limit of 40");
    limits.maxTypeSize = 1'000;
    auto coreModules = CoreModuleValidator {};
    auto const result =
        validateComponent(builder.pool(), ComponentFeatureSet::defaults(), limits, coreModules);
    REQUIRE(result.has_value());
    // 1 + 2 memories + realloc 7 + nothing 2 + number 3 + wide 18 + one 3 + pair 4 + badRealloc 4.
    CHECK((*result)->types[(*result)->coreModules.front().type].measure.effectiveSize == 44);
}

TEST_CASE("Validator.moduleTypeSubtyping")
{
    // module_type (component_types.rs:3796-3831): a module may import less than its ascribed type,
    // never more, and must export what it promises.
    auto importer = ModuleBuilder {};
    importer.type(FuncType {}).importFunction("lib", "nothing", 0).function(0, {}, {}).exportFunction("f", 1);
    auto const ascribing = [&](std::vector<isoline::component::binary::CoreModuleDeclaration> declarations) {
        auto builder = ComponentBuilder {};
        std::ignore = builder.coreType(CoreTypeDefinition {
            .type = CoreModuleTypeDefinition { .declarations = std::move(declarations) } });
        std::ignore = builder.coreModule(moduleBytes(importer));
        std::ignore =
            builder.exportItem("m", Sort::CoreModule, 0, ExternDesc { .sort = Sort::CoreModule, .index = 0 });
        return builder.pool();
    };
    auto const func = CoreExternType { .kind = ExternKind::Func, .typeIndex = 0 };
    CHECK(accepted(ascribing({ CoreTypeDeclaration { .type = FuncType {} },
                               CoreImportDeclaration { .module = "lib", .name = "nothing", .type = func },
                               CoreImportDeclaration { .module = "lib", .name = "other", .type = func },
                               CoreExportDeclaration { .name = "f", .type = func } })));
    CHECK(refusalText(ascribing({ CoreTypeDeclaration { .type = FuncType {} } }))
          == "ascribed type of export is not compatible with item's type\nmissing expected import "
             "`lib::nothing`");
    CHECK(refusalText(ascribing({ CoreTypeDeclaration { .type = FuncType {} },
                                  CoreImportDeclaration { .module = "lib", .name = "nothing", .type = func },
                                  CoreExportDeclaration { .name = "g", .type = func } }))
          == "ascribed type of export is not compatible with item's type\nmissing expected export `g`");
}

TEST_CASE("Validator.moreCaps")
{
    // The component functions and the four core spaces wasm-tools caps besides those of
    // Validator.indexSpaceCaps: one past the cap is refused, in its words.
    auto limits = ComponentLimits {};
    limits.maxFunctions = 1;
    limits.maxCoreTables = 1;
    limits.maxCoreMemories = 1;
    limits.maxCoreGlobals = 1;
    limits.maxCoreTags = 1;
    auto functions = ComponentBuilder {};
    auto const type = functions.type(function({}));
    std::ignore = functions.import("a", ExternDesc { .sort = Sort::Func, .index = type });
    std::ignore = functions.import("b", ExternDesc { .sort = Sort::Func, .index = type });
    CHECK(refusalText(functions.pool(), limits) == "functions count exceeds limit of 1");
    CHECK(refusalText(withLibrary().pool(), limits) == "memories count exceeds limit of 1");

    auto module = ModuleBuilder {};
    module.type(FuncType {})
        .table(TableType { .limits = Limits { .min = 1, .max = std::nullopt } })
        .global(GlobalType { .type = I32 }, { 0x41, 0x00 }) // i32.const 0
        .tag(0)
        .exportEntity("table", ExternKind::Table, 0)
        .exportEntity("global", ExternKind::Global, 0)
        .exportEntity("tag", ExternKind::Tag, 0);
    auto const entities = moduleBytes(module);
    auto const twice = [&](std::string const& name, CoreSort sort) {
        auto builder = ComponentBuilder {};
        std::ignore = builder.coreModule(entities);
        std::ignore = builder.coreInstantiate(0, {});
        std::ignore = builder.aliasCoreExport(0, name, sort);
        std::ignore = builder.aliasCoreExport(0, name, sort);
        return builder.pool();
    };
    CHECK(refusalText(twice("table", CoreSort::Table), limits) == "tables count exceeds limit of 1");
    CHECK(refusalText(twice("global", CoreSort::Global), limits) == "globals count exceeds limit of 1");
    CHECK(refusalText(twice("tag", CoreSort::Tag), limits) == "tags count exceeds limit of 1");
}

TEST_CASE("Validator.repeatedOptions")
{
    // check_options (component.rs:2652-2875): every option once, whatever it is.
    auto lowered = withLibrary();
    auto const type = lowered.type(function({}));
    std::ignore = lowered.import("f", ExternDesc { .sort = Sort::Func, .index = type });
    auto const lowering = [&](std::vector<CanonOption> options) {
        auto builder = lowered;
        std::ignore = builder.canonLower(0, std::move(options));
        return builder.pool();
    };
    CHECK(refusalText(lowering({ option(CanonOptionKind::Memory, 0),
                                 option(CanonOptionKind::Realloc, Realloc),
                                 option(CanonOptionKind::Realloc, Realloc) }))
          == "canonical option `realloc` is specified more than once");
    CHECK(refusalText(lowering(
              { option(CanonOptionKind::Callback, Nothing), option(CanonOptionKind::Callback, Nothing) }))
          == "canonical option `callback` is specified more than once");
    auto lifted = withLibrary();
    auto const result = lifted.type(function({}, PrimitiveValueType::U32));
    std::ignore = lifted.canonLift(
        Number,
        { option(CanonOptionKind::PostReturn, One), option(CanonOptionKind::PostReturn, One) },
        result);
    CHECK(refusalText(lifted.pool()) == "canonical option `post-return` is specified more than once");
}

TEST_CASE("Validator.loweredParametersSpill")
{
    // A lowering of more than 16 flat parameters passes them through memory (ComponentFuncType::lower,
    // component_types.rs:1236-1359), which then needs the memory option; the realloc it does not.
    auto const lowering = [](uint32_t params, std::vector<CanonOption> options) {
        auto builder = withLibrary();
        auto const signature = builder.type(function(u32Params(params)));
        std::ignore = builder.import("f", ExternDesc { .sort = Sort::Func, .index = signature });
        std::ignore = builder.canonLower(0, std::move(options));
        return builder.pool();
    };
    CHECK(accepted(lowering(16, {})));
    CHECK(refusalText(lowering(17, {})) == "canonical option `memory` is required");
    CHECK(accepted(lowering(17, { option(CanonOptionKind::Memory, 0) })));
}

namespace
{
/// withLibrary(), then an instance of @p imports inline exports f0, f1, ... of Nothing (@p prefix in
/// place of f), and @p count instantiations of a module importing each of them from "lib".
[[nodiscard]] ComponentPool coreInstantiations(uint32_t imports,
                                               uint32_t count,
                                               std::string const& prefix = "f")
{
    auto importer = ModuleBuilder {};
    importer.type(FuncType {});
    auto exports = std::vector<CoreInlineExport> {};
    for (auto const index: std::views::iota(uint32_t { 0 }, imports))
    {
        importer.importFunction("lib", prefix + std::to_string(index), 0);
        exports.push_back(CoreInlineExport {
            .name = prefix + std::to_string(index), .sort = CoreSort::Func, .index = Nothing });
    }
    auto builder = withLibrary();
    auto const instance = builder.coreInlineExports(std::move(exports));
    auto const module = builder.coreModule(moduleBytes(importer));
    for ([[maybe_unused]] auto const instantiation: std::views::iota(uint32_t { 0 }, count))
        std::ignore = builder.coreInstantiate(module, { { "lib", instance } });
    return builder.pool();
}

/// The core matching steps of library() instantiated: its nine exports measured.
constexpr auto LibrarySteps = uint64_t { 9 };
} // namespace

TEST_CASE("Validator.coreMatchingBudget")
{
    // Every core import looked up and matched, and every export or inline export measured, is a step
    // of maxCoreMatchingSteps, over all of a validation's core instantiations: k instantiations of a
    // module of n imports cost k x n, and twice the instantiations twice that.
    auto const steps = [](uint32_t imports, uint32_t count) {
        auto const result = validated(coreInstantiations(imports, count));
        REQUIRE(result.has_value());
        return (*result)->work.coreMatchingSteps;
    };
    auto const single = steps(100, 10);
    auto const twice = steps(100, 20);
    INFO(std::format("core matching {} at n, {} at 2n", single, twice));
    // Required, not checked: a validation that charges nothing would run the crafted case below unbounded.
    REQUIRE(single == LibrarySteps + 100 + (uint64_t { 10 } * 100));
    REQUIRE(twice - single == uint64_t { 10 } * 100);

    auto limits = ComponentLimits {};
    limits.maxCoreMatchingSteps = static_cast<uint32_t>(single);
    CHECK(validated(coreInstantiations(100, 10), limits).has_value());
    limits.maxCoreMatchingSteps = static_cast<uint32_t>(single - 1);
    auto const refused = refusal(coreInstantiations(100, 10), limits);
    CHECK(exceededLimit(refused) == ComponentLimit::CoreMatchingSteps);
    CHECK(specMessage(refused) == std::format("core matching steps exceed the limit of {}", single - 1));

    // The crafted case: a module of 100'000 imports instantiated 4'095 times is 4 x 10^8 lookups to
    // wasm-tools. An instantiation of it costs 199'990 steps here, a lookup and the bytes of its names'
    // searches each, after the 198'985 of library() and the inline exports: the default budget of 10^7
    // refuses it at its 50th instantiation, a budget of 10^6, which keeps the suite quick and counts the
    // same, at its fifth. Definitions 0-12 are withLibrary()'s eleven, the inline exports and the module.
    limits.maxCoreMatchingSteps = 1'000'000;
    auto const crafted = refusal(coreInstantiations(100'000, 4'095), limits);
    CHECK(exceededLimit(crafted) == ComponentLimit::CoreMatchingSteps);
    CHECK(crafted.position.definition == 13 + 4);
}

TEST_CASE("Validator.subtypingBudgetSpansValidation")
{
    // One relation serves the whole validation: its steps add up over every ascribed export and
    // every instantiation, and running out of them reports maxSubtypingSteps, not a mismatch.
    auto const ascribing = [](uint32_t exports) {
        auto builder = withLibrary();
        for (auto const index: std::views::iota(uint32_t { 0 }, exports))
        {
            auto const type = builder.type(function(u32Params(16)));
            auto const ascribed = builder.type(function(u32Params(16)));
            auto const lifted = builder.canonLift(Wide, {}, type);
            std::ignore = builder.exportItem("f" + std::to_string(index),
                                             Sort::Func,
                                             lifted,
                                             ExternDesc { .sort = Sort::Func, .index = ascribed });
        }
        return builder.pool();
    };
    auto const one = validated(ascribing(1));
    auto const two = validated(ascribing(2));
    REQUIRE(one.has_value());
    REQUIRE(two.has_value());
    auto const single = (*one)->work.subtypingSteps;
    auto const both = (*two)->work.subtypingSteps;
    INFO(std::format("subtyping {} for one export, {} for two", single, both));
    CHECK(single > 0);
    CHECK(both == 2 * single);

    auto limits = ComponentLimits {};
    limits.maxSubtypingSteps = static_cast<uint32_t>(both);
    CHECK(validated(ascribing(2), limits).has_value());
    limits.maxSubtypingSteps = static_cast<uint32_t>(both - 1);
    auto const refused = refusal(ascribing(2), limits);
    CHECK(exceededLimit(refused) == ComponentLimit::SubtypingSteps);
    CHECK(specMessage(refused) == std::format("subtyping steps exceed the limit of {}", both - 1));

    // An instantiation's arguments are checked by the same relation.
    auto child = ComponentBuilder {};
    auto const childType = child.type(function({}));
    std::ignore = child.import("a", ExternDesc { .sort = Sort::Func, .index = childType });
    auto parent = ComponentBuilder {};
    std::ignore = parent.nested(std::move(child));
    auto const parentType = parent.type(function({}));
    std::ignore = parent.import("f", ExternDesc { .sort = Sort::Func, .index = parentType });
    std::ignore = parent.instantiate(0, { { "a", SortIndex { .sort = Sort::Func, .index = 0 } } });
    limits.maxSubtypingSteps = 0;
    CHECK(exceededLimit(refusal(parent.pool(), limits)) == ComponentLimit::SubtypingSteps);
}

TEST_CASE("Validator.exportLookupWork")
{
    // An instance type's exports are indexed by name once per validation: n aliases of an instance of n
    // exports cost the n entries indexed and n lookups, at n as at 2n, never n x n.
    auto const work = [](uint32_t exports) {
        auto declarations = TypeScopeBuilder {};
        auto const type = declarations.type(function({}));
        for (auto const index: std::views::iota(uint32_t { 0 }, exports))
            std::ignore = declarations.exportItem("f" + std::to_string(index),
                                                  ExternDesc { .sort = Sort::Func, .index = type });
        auto builder = ComponentBuilder {};
        auto const instance = builder.instanceType(std::move(declarations));
        std::ignore = builder.import("i", ExternDesc { .sort = Sort::Instance, .index = instance });
        for (auto const index: std::views::iota(uint32_t { 0 }, exports))
            std::ignore = builder.aliasExport(0, "f" + std::to_string(index), Sort::Func);
        auto const result = validated(builder.pool());
        REQUIRE(result.has_value());
        return (*result)->work.exportLookupSteps;
    };
    auto const single = work(500);
    auto const twice = work(1'000);
    INFO(std::format("export lookup work {} at n, {} at 2n", single, twice));
    CHECK(single == uint64_t { 2 } * 500);
    CHECK(twice == 2 * single);
}

TEST_CASE("Validator.coreMatchingChargesNameBytes")
{
    // A lookup compares its name with up to bit_width(n) others: 100 imports of 100 KB names sharing
    // their prefix, instantiated 4'095 times, are 410'000 lookups but 300 GB compared. The names'
    // bytes are charged, so at a fixed count twice the name length is twice the steps.
    auto const steps = [](std::size_t length) {
        auto const result = validated(coreInstantiations(100, 10, std::string(length, 'n')));
        REQUIRE(result.has_value());
        return (*result)->work.coreMatchingSteps;
    };
    auto const single = steps(10'000);
    auto const twice = steps(20'000);
    INFO(std::format("core matching {} at length n, {} at 2n", single, twice));
    // Ten instantiations of 100 imports, each name searched among 100 exports (bit_width 7).
    CHECK(single >= uint64_t { 10 } * 100 * (10'000 * 7 / 64));
    CHECK(twice * 10 >= single * 19);
    CHECK(twice * 10 <= single * 21);
}

namespace
{
/// A canonical built-in of one immediate and no options.
[[nodiscard]] isoline::component::binary::CanonDefinition resourceBuiltin(
    isoline::component::binary::CanonBuiltin which, uint32_t resource)
{
    return isoline::component::binary::CanonDefinition {
        .builtin = which, .immediates = { resource }, .options = {}, .result = {}
    };
}

/// A module of a table of funcref, an immutable i32 global, a tag of [] and a memory, exported as
/// "t", "g", "e" and "m", and functions "floats" of [f32] -> [f64] and "refs" of [i32 funcref] -> [].
[[nodiscard]] Bytes everyKindLibrary()
{
    auto builder = ModuleBuilder {};
    builder.type(FuncType {})
        .type(FuncType { .params = { isoline::binary::F32 }, .results = { isoline::binary::F64 } })
        .type(FuncType { .params = { I32, isoline::binary::FuncRef }, .results = {} })
        .function(1, {}, { 0x20, 0x00, 0xBB }) // f64.promote_f32 (local.get 0)
        .function(2, {}, {})
        .table(TableType { .element = isoline::binary::FuncRef,
                           .limits = Limits { .min = 1, .max = std::nullopt } })
        .global(GlobalType { .type = I32, .mutability = isoline::binary::Mutability::Const }, { 0x41, 0x00 })
        .tag(0)
        .memory(Limits { .min = 1, .max = std::nullopt })
        .exportEntity("t", ExternKind::Table, 0)
        .exportEntity("g", ExternKind::Global, 0)
        .exportEntity("e", ExternKind::Tag, 0)
        .exportEntity("m", ExternKind::Memory, 0)
        .exportFunction("floats", 0)
        .exportFunction("refs", 1);
    return moduleBytes(builder);
}

/// A component instantiating everyKindLibrary() as core instance 0, its exports aliased: core table 0,
/// global 0, tag 0, memory 0, then core functions floats 0 and refs 1.
[[nodiscard]] ComponentBuilder withEveryKind()
{
    auto builder = ComponentBuilder {};
    std::ignore = builder.coreModule(everyKindLibrary());
    std::ignore = builder.coreInstantiate(0, {});
    std::ignore = builder.aliasCoreExport(0, "t", CoreSort::Table);
    std::ignore = builder.aliasCoreExport(0, "g", CoreSort::Global);
    std::ignore = builder.aliasCoreExport(0, "e", CoreSort::Tag);
    std::ignore = builder.aliasCoreExport(0, "m", CoreSort::Memory);
    std::ignore = builder.aliasCoreExport(0, "floats", CoreSort::Func);
    std::ignore = builder.aliasCoreExport(0, "refs", CoreSort::Func);
    return builder;
}
} // namespace

TEST_CASE("Validator.liftsFloatsAndSpellsReferenceTypes")
{
    // ComponentFuncType::lower's flat f32 and f64 (component_types.rs:1236-1359), and a lifting mismatch
    // spelling a list of two types, a reference among them, as Rust's Debug of ValType does.
    auto floats = withEveryKind();
    auto const promote = floats.type(
        function({ LabeledType { .label = "a", .type = PrimitiveValueType::F32 } }, PrimitiveValueType::F64));
    std::ignore = floats.canonLift(0, {}, promote);
    CHECK(accepted(floats.pool()));

    auto refs = withEveryKind();
    auto const pair = refs.type(function({ LabeledType { .label = "a", .type = PrimitiveValueType::U32 },
                                           LabeledType { .label = "b", .type = PrimitiveValueType::U32 } }));
    std::ignore = refs.canonLift(1, {}, pair);
    CHECK(
        refusalText(refs.pool())
        == "lowered parameter types `[I32, I32]` do not match parameter types `[I32, Ref(funcref)]` of core "
           "function 1");
}

TEST_CASE("Validator.coreInlineExportsOfEveryKind")
{
    // instantiate_core_exports (component.rs:3633-3721) of a table, a global and a tag, which a module
    // then imports from the instance (instantiate_core_module's matching).
    auto const importer = [](GlobalType global) {
        auto builder = ModuleBuilder {};
        builder.type(FuncType {})
            .importTable("lib",
                         "t",
                         TableType { .element = isoline::binary::FuncRef,
                                     .limits = Limits { .min = 1, .max = std::nullopt } })
            .importGlobal("lib", "g", global)
            .importTag("lib", "e", 0)
            .importMemory("lib", "m", Limits { .min = 1, .max = std::nullopt });
        return moduleBytes(builder);
    };
    auto const instantiating = [&](GlobalType global) {
        auto builder = withEveryKind();
        std::ignore = builder.coreInlineExports(
            { CoreInlineExport { .name = "t", .sort = CoreSort::Table, .index = 0 },
              CoreInlineExport { .name = "g", .sort = CoreSort::Global, .index = 0 },
              CoreInlineExport { .name = "e", .sort = CoreSort::Tag, .index = 0 },
              CoreInlineExport { .name = "m", .sort = CoreSort::Memory, .index = 0 } });
        std::ignore = builder.coreModule(importer(global));
        std::ignore = builder.coreInstantiate(1, { { "lib", 1 } });
        return builder.pool();
    };
    CHECK(accepted(
        instantiating(GlobalType { .type = I32, .mutability = isoline::binary::Mutability::Const })));
    CHECK(
        refusalText(instantiating(GlobalType { .type = I32, .mutability = isoline::binary::Mutability::Var }))
        == "type mismatch for export `g` of module instantiation argument `lib`\nglobal types differ in "
           "mutability");

    for (auto const& [sort, text]:
         { std::pair { CoreSort::Global, "unknown global 5: global index out of bounds" },
           std::pair { CoreSort::Tag, "unknown tag 5: tag index out of bounds" } })
    {
        INFO(text);
        auto builder = withEveryKind();
        std::ignore =
            builder.coreInlineExports({ CoreInlineExport { .name = "x", .sort = sort, .index = 5 } });
        CHECK(refusalText(builder.pool()) == text);
    }
}

TEST_CASE("Validator.coreFunctionTypesNameEarlierTypes")
{
    // add_core_type (component.rs:510-534): a concrete reference of a core function type names an earlier
    // core type of the scope, which must be a function type.
    auto const typed = [](std::vector<CoreTypeDefinition> types) {
        auto builder = ComponentBuilder {};
        for (auto& type: types)
            std::ignore = builder.coreType(std::move(type));
        return builder.pool();
    };
    auto const takes = [](uint32_t index) {
        return CoreTypeDefinition {
            .type =
                FuncType {
                    .params = { isoline::binary::concreteRef(index, isoline::binary::Nullable::No) },
                    .results = { isoline::binary::concreteRef(index, isoline::binary::Nullable::Yes) } }
        };
    };
    auto const accepted = validated(typed({ CoreTypeDefinition { .type = FuncType {} }, takes(0) }));
    REQUIRE(accepted.has_value());
    CHECK(refusalText(typed({ takes(4) })) == "unknown type 4: type index out of bounds");
    CHECK(refusalText(typed({ CoreTypeDefinition { .type = CoreModuleTypeDefinition {} }, takes(0) }))
          == "type index 0 is a module type, not a sub type");
}

TEST_CASE("Validator.moduleTypeDeclarations")
{
    // create_module_type (component.rs:2987-3045) and Module::check_type_ref (core.rs:680-930): a type
    // space of the module type's own, outer aliases of this scope and the enclosing one, and imports of
    // every kind checked by the core rules.
    using isoline::component::binary::CoreModuleDeclaration;
    auto const moduleType = [](std::vector<CoreModuleDeclaration> declarations,
                               ComponentLimits const& limits = {}) {
        auto builder = ComponentBuilder {};
        std::ignore =
            builder.coreType(CoreTypeDefinition { .type = FuncType { .params = { I32 }, .results = {} } });
        std::ignore = builder.coreType(CoreTypeDefinition {
            .type = CoreModuleTypeDefinition { .declarations = std::move(declarations) } });
        return validated(builder.pool(), limits);
    };
    auto const importing = [](CoreExternType type) {
        return CoreImportDeclaration { .module = "m", .name = "x", .type = type };
    };
    auto const global = [](isoline::binary::ValueType type) {
        return CoreExternType { .kind = ExternKind::Global,
                                .global = GlobalType { .type = type,
                                                       .mutability = isoline::binary::Mutability::Var } };
    };
    auto const memory = [](Limits limits) {
        return CoreExternType { .kind = ExternKind::Memory,
                                .memory = isoline::binary::MemoryType { .limits = limits } };
    };
    auto const message = [](Validated const& result) {
        REQUIRE_FALSE(result.has_value());
        return specMessage(result.error());
    };
    auto const every = moduleType(
        { CoreTypeDeclaration { .type = FuncType {} },
          CoreOuterAliasDeclaration { .outerCount = 0, .outerIndex = 0 },
          CoreOuterAliasDeclaration { .outerCount = 1, .outerIndex = 0 },
          importing(global(I32)),
          CoreImportDeclaration { .module = "m", .name = "y", .type = memory(Limits { .min = 1, .max = 2 }) },
          CoreExportDeclaration { .name = "f",
                                  .type = CoreExternType { .kind = ExternKind::Func, .typeIndex = 2 } } });
    REQUIRE(every.has_value());
    CHECK(message(moduleType({ CoreOuterAliasDeclaration { .outerCount = 0, .outerIndex = 3 } }))
          == "unknown type 3: type index out of bounds");
    CHECK(message(moduleType({ CoreOuterAliasDeclaration { .outerCount = 1, .outerIndex = 7 } }))
          == "unknown type 7: type index out of bounds");
    CHECK(message(moduleType(
              { importing(global(isoline::binary::concreteRef(9, isoline::binary::Nullable::Yes))) }))
          == "unknown type 9: type index out of bounds");
    CHECK(message(moduleType({ importing(memory(Limits { .min = 1, .max = 65537 })) }))
          == "memory size must be at most 0x10000 65536-byte pages");

    // The module type's own type space counts toward maxTypes, by a declaration or an alias; and its
    // type size toward maxTypeSize, by its imports and its exports (Module::type_size).
    auto limits = ComponentLimits {};
    limits.maxTypes = 2;
    for (auto const& third:
         { CoreModuleDeclaration { CoreTypeDeclaration { .type = FuncType {} } },
           CoreModuleDeclaration { CoreOuterAliasDeclaration { .outerCount = 0, .outerIndex = 0 } } })
    {
        auto const refused = moduleType({ CoreTypeDeclaration { .type = FuncType {} },
                                          CoreTypeDeclaration { .type = FuncType {} },
                                          third },
                                        limits);
        REQUIRE_FALSE(refused.has_value());
        CHECK(exceededLimit(refused.error()) == ComponentLimit::Types);
    }
    auto const tableOf = [] {
        return CoreExternType { .kind = ExternKind::Table,
                                .table = TableType { .element = isoline::binary::FuncRef,
                                                     .limits = Limits { .min = 0, .max = std::nullopt } } };
    };
    auto sized = ComponentLimits {};
    sized.maxTypeSize = 3;
    for (auto const& second:
         { CoreModuleDeclaration { CoreImportDeclaration { .module = "m", .name = "b", .type = tableOf() } },
           CoreModuleDeclaration { CoreExportDeclaration { .name = "b", .type = tableOf() } } })
    {
        auto const refused = moduleType(
            { CoreImportDeclaration { .module = "m", .name = "a", .type = tableOf() }, second }, sized);
        REQUIRE_FALSE(refused.has_value());
        CHECK(exceededLimit(refused.error()) == ComponentLimit::TypeSize);
    }
    auto const exportsOnly = moduleType({ CoreExportDeclaration { .name = "a", .type = tableOf() },
                                          CoreExportDeclaration { .name = "b", .type = tableOf() } },
                                        sized);
    REQUIRE_FALSE(exportsOnly.has_value());
    CHECK(exceededLimit(exportsOnly.error()) == ComponentLimit::TypeSize);

    // A declared type of a reference past the module type's space, and a tag of an unknown type.
    CHECK(
        message(moduleType({ CoreTypeDeclaration {
            .type = FuncType { .params = { isoline::binary::concreteRef(9, isoline::binary::Nullable::Yes) },
                               .results = {} } } }))
        == "unknown type 9: type index out of bounds");
    CHECK(message(moduleType({ importing(CoreExternType { .kind = ExternKind::Tag, .typeIndex = 9 }) }))
          == "unknown type 9: type index out of bounds");
}

TEST_CASE("Validator.exportAliasNamesTheSortExpected")
{
    // alias_instance_export (component.rs:3813-3861): an export of another sort than the alias's,
    // named by ComponentExternalKind::desc; an instance exporting a module, a component, a function and
    // a type.
    auto const aliasing = [](std::string name, Sort sort) {
        auto builder = ComponentBuilder {};
        auto const signature = builder.type(function({}));
        auto const imported = builder.import("f", ExternDesc { .sort = Sort::Func, .index = signature });
        auto const list = builder.type(defined(ListType { .element = PrimitiveValueType::U32 }));
        auto const module = builder.coreModule(modulePreamble());
        auto const component = builder.nested(ComponentBuilder {});
        auto const instance =
            builder.inlineInstance({ { "f", SortIndex { .sort = Sort::Func, .index = imported } },
                                     { "t", SortIndex { .sort = Sort::Type, .index = list } },
                                     { "m", SortIndex { .sort = Sort::CoreModule, .index = module } },
                                     { "c", SortIndex { .sort = Sort::Component, .index = component } } });
        std::ignore = builder.aliasExport(instance, std::move(name), sort);
        return builder.pool();
    };
    CHECK(accepted(aliasing("c", Sort::Component)));
    CHECK(refusalText(aliasing("f", Sort::Type)) == "export `f` for instance 0 is not a type");
    CHECK(refusalText(aliasing("t", Sort::CoreModule)) == "export `t` for instance 0 is not a module");
    CHECK(refusalText(aliasing("m", Sort::Component)) == "export `m` for instance 0 is not a component");
    CHECK(refusalText(aliasing("c", Sort::Func)) == "export `c` for instance 0 is not a func");
}

TEST_CASE("Validator.exportItemsAndAscriptions")
{
    // component_export_section (validator.rs:1363-1392): the item, then the type ascribed to it.
    auto past = ComponentBuilder {};
    std::ignore = past.exportItem("t", Sort::Type, 9);
    CHECK(refusalText(past.pool()) == "unknown type 9: type index out of bounds");
    auto badAscription = ComponentBuilder {};
    auto const signature = badAscription.type(function({}));
    auto const imported = badAscription.import("f", ExternDesc { .sort = Sort::Func, .index = signature });
    std::ignore =
        badAscription.exportItem("g", Sort::Func, imported, ExternDesc { .sort = Sort::Func, .index = 9 });
    CHECK(refusalText(badAscription.pool()) == "unknown type 9: type index out of bounds");
    auto component = ComponentBuilder {};
    std::ignore = component.exportItem("c", Sort::Component, component.nested(ComponentBuilder {}));
    CHECK(accepted(component.pool()));
}

TEST_CASE("Validator.canonIndicesChecked")
{
    // Each built-in's operands, out of bounds (lift_function, lower_function, resource_new/drop/rep).
    using isoline::component::binary::CanonBuiltin;
    auto lift = withLibrary();
    std::ignore = lift.canonLift(Nothing, {}, 9);
    CHECK(refusalText(lift.pool()) == "unknown type 9: type index out of bounds");
    auto postReturn = withLibrary();
    auto const result = postReturn.type(function({}, PrimitiveValueType::U32));
    std::ignore = postReturn.canonLift(Number, { option(CanonOptionKind::PostReturn, 99) }, result);
    CHECK(refusalText(postReturn.pool()) == "unknown core function 99: function index out of bounds");
    auto lower = withLibrary();
    std::ignore = lower.canonLower(9, {});
    CHECK(refusalText(lower.pool()) == "unknown function 9: function index out of bounds");
    for (auto const which:
         { CanonBuiltin::ResourceNew, CanonBuiltin::ResourceDrop, CanonBuiltin::ResourceRep })
    {
        auto builder = ComponentBuilder {};
        std::ignore = builder.canon(resourceBuiltin(which, 9));
        CHECK(refusalText(builder.pool()) == "unknown type 9: type index out of bounds");
    }
}

TEST_CASE("Validator.coreInstancesChecked")
{
    // instance_section's room (validator.rs:1091-1115), then the module instantiated.
    auto limits = ComponentLimits {};
    limits.maxInstances = 0;
    auto const capped = validated(withLibrary().pool(), limits);
    REQUIRE_FALSE(capped.has_value());
    CHECK(exceededLimit(capped.error()) == ComponentLimit::Instances);
    auto unknown = ComponentBuilder {};
    std::ignore = unknown.coreInstantiate(5, {});
    CHECK(refusalText(unknown.pool()) == "unknown module 5: module index out of bounds");
    auto outer = ComponentBuilder {};
    auto inner = ComponentBuilder {};
    std::ignore = inner.aliasOuter(Sort::CoreType, 1, 9);
    std::ignore = outer.nested(std::move(inner));
    CHECK(refusalText(outer.pool()) == "unknown type 9: type index out of bounds");
}

TEST_CASE("Validator.visibilityOfExportedTypes")
{
    // validate_and_register_named_types (component.rs:816-913) and the all_valtypes_named rules
    // (component.rs:915-1045, component_types.rs:3108-3165): what an exported type names.
    auto const exportedType = [](auto define) {
        auto builder = ComponentBuilder {};
        auto const resource = builder.resource();
        std::ignore = builder.exportItem("r", Sort::Type, resource);
        std::ignore = builder.exportItem("t", Sort::Type, define(builder, resource + 1));
        return builder.pool();
    };
    // Flags and enums name nothing; a handle names its resource, here exported.
    CHECK(accepted(exportedType([](ComponentBuilder& builder, uint32_t) {
        return builder.type(defined(isoline::component::binary::FlagsType { .labels = { "a" } }));
    })));
    CHECK(accepted(exportedType([](ComponentBuilder& builder, uint32_t) {
        return builder.type(defined(isoline::component::binary::EnumType { .labels = { "a" } }));
    })));
    CHECK(accepted(exportedType([](ComponentBuilder& builder, uint32_t resource) {
        return builder.type(defined(OwnType { .resource = resource }));
    })));
    // A function type, and a tuple of one list twice, whose list is checked once.
    CHECK(accepted(exportedType([](ComponentBuilder& builder, uint32_t) {
        auto const list = builder.type(defined(ListType { .element = PrimitiveValueType::U32 }));
        auto const pair = builder.type(
            defined(isoline::component::binary::TupleType { .elements = { at(list), at(list) } }));
        return builder.type(function({ LabeledType { .label = "p", .type = at(pair) } }));
    })));

    // An instance type, whose exports are named within it: a nested instance, an instance type and a
    // function type bound as types, a function of a record it exports.
    auto const instanceOf = [](bool recordExported) {
        auto builder = ComponentBuilder {};
        auto nestedDeclarations = TypeScopeBuilder {};
        auto const nestedType = builder.instanceType(std::move(nestedDeclarations));
        auto declarations = TypeScopeBuilder {};
        auto const nested = declarations.aliasOuter(Sort::Type, 1, nestedType);
        std::ignore = declarations.exportItem("n", ExternDesc { .sort = Sort::Instance, .index = nested });
        std::ignore = declarations.exportItem(
            "nt", ExternDesc { .sort = Sort::Type, .index = nested, .bound = TypeBound::Equal });
        auto const record = declarations.type(defined(
            RecordType { .fields = { LabeledType { .label = "a", .type = PrimitiveValueType::U32 } } }));
        auto named = record;
        if (recordExported)
            named = declarations.exportItem(
                "rec", ExternDesc { .sort = Sort::Type, .index = record, .bound = TypeBound::Equal });
        auto const take = declarations.type(function({ LabeledType { .label = "x", .type = at(named) } }));
        std::ignore = declarations.exportItem(
            "ft", ExternDesc { .sort = Sort::Type, .index = take, .bound = TypeBound::Equal });
        std::ignore = declarations.exportItem("f", ExternDesc { .sort = Sort::Func, .index = take });
        auto const instanceType = builder.instanceType(std::move(declarations));
        std::ignore = builder.exportItem("i", Sort::Type, instanceType);
        return builder.pool();
    };
    CHECK(accepted(instanceOf(true)));
    CHECK(refusalText(instanceOf(false)) == "type not valid to be used as export");

    // An exported instance whose function, lifted here, names an unexported record, within an
    // instance it exports.
    auto unnamed = withLibrary();
    auto const record = unnamed.type(
        defined(RecordType { .fields = { LabeledType { .label = "a", .type = PrimitiveValueType::U32 } } }));
    auto const take = unnamed.type(function({ LabeledType { .label = "x", .type = at(record) } }));
    auto const lifted = unnamed.canonLift(One, {}, take);
    auto const inner = unnamed.inlineInstance({ { "f", SortIndex { .sort = Sort::Func, .index = lifted } } });
    auto const outer =
        unnamed.inlineInstance({ { "inner", SortIndex { .sort = Sort::Instance, .index = inner } } });
    std::ignore = unnamed.exportItem("outer", Sort::Instance, outer);
    CHECK(refusalText(unnamed.pool()) == "instance not valid to be used as export");

    // An instance of inline exports names each export as an extern name.
    auto badName = ComponentBuilder {};
    auto const list = badName.type(defined(ListType { .element = PrimitiveValueType::U32 }));
    std::ignore = badName.inlineInstance({ { "A B", SortIndex { .sort = Sort::Type, .index = list } } });
    CHECK(refusalText(badName.pool())
          == "export name `A B` is not a valid extern name\n`A B` is not in kebab case");
}

TEST_CASE("Validator.exportedResourcePaths")
{
    // explicit_resource_paths (component_types.rs:3034-3106): a local resource is found past an exported
    // instance that holds none; and a component exported whose type defines a resource of its own.
    auto builder = ComponentBuilder {};
    auto const resource = builder.resource();
    auto const signature = builder.type(function({}));
    auto const imported = builder.import("f", ExternDesc { .sort = Sort::Func, .index = signature });
    auto const plain =
        builder.inlineInstance({ { "f", SortIndex { .sort = Sort::Func, .index = imported } } });
    auto const holding =
        builder.inlineInstance({ { "r", SortIndex { .sort = Sort::Type, .index = resource } } });
    std::ignore = builder.exportItem("plain", Sort::Instance, plain);
    std::ignore = builder.exportItem("holding", Sort::Instance, holding);
    auto const validatedPaths = validated(builder.pool());
    REQUIRE(validatedPaths.has_value());
    auto const& type = (**validatedPaths).type;
    REQUIRE(type.definedResources.size() == 1);
    CHECK(type.definedResources.front().path == std::vector<uint32_t> { 1, 0 });

    auto outer = ComponentBuilder {};
    auto child = ComponentBuilder {};
    std::ignore = child.exportItem("r", Sort::Type, child.resource());
    std::ignore = outer.exportItem("c", Sort::Component, outer.nested(std::move(child)));
    CHECK(accepted(outer.pool()));
}

TEST_CASE("Validator.exportedInstanceTypesMintResources")
{
    // prepare_instance_export (component.rs:1060-1168): a component type's export of an instance type
    // defining a resource defines a fresh one at its path; and the component type's exports are capped.
    auto const componentType = [](ComponentLimits const& limits) {
        auto builder = ComponentBuilder {};
        auto instanceDeclarations = TypeScopeBuilder {};
        std::ignore = instanceDeclarations.exportItem(
            "r", ExternDesc { .sort = Sort::Type, .index = 0, .bound = TypeBound::SubResource });
        auto declarations = TypeScopeBuilder {};
        auto const instanceType = declarations.instanceType(std::move(instanceDeclarations));
        std::ignore =
            declarations.exportItem("i", ExternDesc { .sort = Sort::Instance, .index = instanceType });
        std::ignore = builder.componentType(std::move(declarations));
        return validated(builder.pool(), limits);
    };
    auto const minted = componentType({});
    CHECK(minted.has_value());
    auto limits = ComponentLimits {};
    limits.maxExports = 0;
    auto const capped = componentType(limits);
    REQUIRE_FALSE(capped.has_value());
    CHECK(exceededLimit(capped.error()) == ComponentLimit::Exports);
}

TEST_CASE("Validator.resourceFunctionNamesItsResource")
{
    // validate_resource_name (component.rs:4885-5090): an exported method on a resource that only an
    // import names has no name among the exports.
    auto builder = ComponentBuilder {};
    auto const resource =
        builder.import("r", ExternDesc { .sort = Sort::Type, .index = 0, .bound = TypeBound::SubResource });
    auto const handle = builder.type(defined(BorrowType { .resource = resource }));
    auto const method = builder.type(function({ LabeledType { .label = "self", .type = at(handle) } }));
    auto const imported = builder.import("f", ExternDesc { .sort = Sort::Func, .index = method });
    std::ignore = builder.exportItem("[method]r.f", Sort::Func, imported);
    CHECK(refusalText(builder.pool())
          == "export name `[method]r.f` is not valid\nresource used in function does not have a name in this "
             "context");
}

namespace
{
/// A component of every kind of definition the caps count: core instances of instantiations and of
/// inline exports of every core sort, core function and module types, resources and their built-ins,
/// a lifted and a lowered function, a nested component instantiated, an instance of inline exports, a
/// component type exporting an instance type that defines a resource, and exports of each sort, one of
/// them ascribed a type.
[[nodiscard]] ComponentPool everyCountedDefinition()
{
    using isoline::component::binary::CanonBuiltin;
    using isoline::component::binary::CoreModuleDeclaration;
    auto builder = withEveryKind();
    std::ignore = builder.coreInlineExports(
        { CoreInlineExport { .name = "t", .sort = CoreSort::Table, .index = 0 },
          CoreInlineExport { .name = "g", .sort = CoreSort::Global, .index = 0 },
          CoreInlineExport { .name = "e", .sort = CoreSort::Tag, .index = 0 },
          CoreInlineExport { .name = "m", .sort = CoreSort::Memory, .index = 0 },
          CoreInlineExport { .name = "floats", .sort = CoreSort::Func, .index = 0 } });
    auto importer = ModuleBuilder {};
    importer.type(FuncType { .params = { isoline::binary::F32 }, .results = { isoline::binary::F64 } })
        .type(FuncType {})
        .importFunction("lib", "floats", 0)
        .importTable("lib",
                     "t",
                     TableType { .element = isoline::binary::FuncRef,
                                 .limits = Limits { .min = 1, .max = std::nullopt } })
        .importGlobal(
            "lib", "g", GlobalType { .type = I32, .mutability = isoline::binary::Mutability::Const })
        .importTag("lib", "e", 1)
        .importMemory("lib", "m", Limits { .min = 1, .max = std::nullopt });
    std::ignore = builder.coreModule(moduleBytes(importer));
    std::ignore = builder.coreInstantiate(1, { { "lib", 1 } });

    std::ignore = builder.coreType(CoreTypeDefinition { .type = FuncType {} });
    std::ignore = builder.coreType(CoreTypeDefinition {
        .type = FuncType { .params = { isoline::binary::concreteRef(0, isoline::binary::Nullable::Yes) },
                           .results = {} } });
    std::ignore = builder.coreType(CoreTypeDefinition {
        .type = CoreModuleTypeDefinition {
            .declarations = {
                CoreModuleDeclaration { CoreTypeDeclaration { .type = FuncType {} } },
                CoreModuleDeclaration { CoreOuterAliasDeclaration { .outerCount = 1, .outerIndex = 1 } },
                CoreModuleDeclaration { CoreImportDeclaration {
                    .module = "m",
                    .name = "f",
                    .type = CoreExternType { .kind = ExternKind::Func, .typeIndex = 1 } } },
                CoreModuleDeclaration { CoreExportDeclaration {
                    .name = "g",
                    .type = CoreExternType { .kind = ExternKind::Func, .typeIndex = 0 } } } } } });

    auto const resource = builder.resource();
    for (auto const which:
         { CanonBuiltin::ResourceNew, CanonBuiltin::ResourceDrop, CanonBuiltin::ResourceRep })
        std::ignore = builder.canon(resourceBuiltin(which, resource));
    auto const promote = builder.type(
        function({ LabeledType { .label = "a", .type = PrimitiveValueType::F32 } }, PrimitiveValueType::F64));
    auto const lifted = builder.canonLift(0, {}, promote);
    std::ignore = builder.canonLower(lifted, {});

    auto child = ComponentBuilder {};
    std::ignore = child.exportItem("r", Sort::Type, child.resource());
    auto const component = builder.nested(std::move(child));
    auto const instance = builder.instantiate(component, {});
    auto const inlined =
        builder.inlineInstance({ { "f", SortIndex { .sort = Sort::Func, .index = lifted } },
                                 { "i", SortIndex { .sort = Sort::Instance, .index = instance } } });

    auto instanceDeclarations = TypeScopeBuilder {};
    std::ignore = instanceDeclarations.exportItem(
        "r", ExternDesc { .sort = Sort::Type, .index = 0, .bound = TypeBound::SubResource });
    auto declarations = TypeScopeBuilder {};
    auto const instanceType = declarations.instanceType(std::move(instanceDeclarations));
    std::ignore = declarations.exportItem("i", ExternDesc { .sort = Sort::Instance, .index = instanceType });
    std::ignore = builder.componentType(std::move(declarations));

    std::ignore = builder.exportItem("r", Sort::Type, resource);
    std::ignore =
        builder.exportItem("f", Sort::Func, lifted, ExternDesc { .sort = Sort::Func, .index = promote });
    std::ignore = builder.exportItem("c", Sort::Component, component);
    std::ignore = builder.exportItem("i", Sort::Instance, inlined);
    return builder.pool();
}

/// One cap of ComponentLimits: the limit it reports and its field.
struct CapRow
{
    ComponentLimit limit;
    uint32_t ComponentLimits::* field;
};

/// Validates @p pool under @p row's cap at 0, 1, … until it is accepted: each value below that is
/// refused by that cap, and some value above zero accepts.
void checkRefusedBelowNeed(ComponentPool const& pool, CapRow const& row)
{
    INFO(std::format("limit {}", static_cast<int>(row.limit)));
    auto limits = ComponentLimits {};
    auto needed = std::optional<uint32_t> {};
    for (auto const cap: std::views::iota(uint32_t { 0 }, uint32_t { 100'000 }))
    {
        limits.*row.field = cap;
        auto const result = validated(pool, limits);
        if (result)
        {
            needed = cap;
            break;
        }
        if (exceededLimit(result.error()) != row.limit)
        {
            FAIL_CHECK(std::format("refused at {} otherwise: {}", cap, specMessage(result.error())));
            return;
        }
    }
    REQUIRE(needed.has_value());
    CHECK(*needed > 0);
}
} // namespace

TEST_CASE("Validator.everyCapRefusesAtEveryCount")
{
    // Each cap the validator enforces, at every value below what the component takes: refused, and by
    // that cap, wherever its count falls; at what it takes, accepted.
    auto const pool = everyCountedDefinition();
    REQUIRE(accepted(pool));
    auto const rows = std::vector<CapRow> {
        { .limit = ComponentLimit::TypeSize, .field = &ComponentLimits::maxTypeSize },
        { .limit = ComponentLimit::TypeDepth, .field = &ComponentLimits::maxTypeDepth },
        { .limit = ComponentLimit::Types, .field = &ComponentLimits::maxTypes },
        { .limit = ComponentLimit::ResourceTypes, .field = &ComponentLimits::maxResourceTypes },
        { .limit = ComponentLimit::SubtypingSteps, .field = &ComponentLimits::maxSubtypingSteps },
        { .limit = ComponentLimit::SubstitutedTypes, .field = &ComponentLimits::maxSubstitutedTypes },
        { .limit = ComponentLimit::Modules, .field = &ComponentLimits::maxModules },
        { .limit = ComponentLimit::Components, .field = &ComponentLimits::maxComponents },
        { .limit = ComponentLimit::Instances, .field = &ComponentLimits::maxInstances },
        { .limit = ComponentLimit::Functions, .field = &ComponentLimits::maxFunctions },
        { .limit = ComponentLimit::Exports, .field = &ComponentLimits::maxExports },
        { .limit = ComponentLimit::CoreTables, .field = &ComponentLimits::maxCoreTables },
        { .limit = ComponentLimit::CoreMemories, .field = &ComponentLimits::maxCoreMemories },
        { .limit = ComponentLimit::CoreGlobals, .field = &ComponentLimits::maxCoreGlobals },
        { .limit = ComponentLimit::CoreTags, .field = &ComponentLimits::maxCoreTags },
        { .limit = ComponentLimit::CoreMatchingSteps, .field = &ComponentLimits::maxCoreMatchingSteps },
    };
    for (auto const& row: rows)
        checkRefusedBelowNeed(pool, row);

    // The measure of an instance, a core one instantiated or of inline exports, or a component one of
    // inline exports: its depth over its exports', and its size their sum, which outgrows any one of
    // them when one entity is exported many times.
    auto instantiated = withEveryKind();
    auto coreInline = withEveryKind();
    auto inlineExports = std::vector<CoreInlineExport> {};
    for (auto const index: std::views::iota(0, 20))
        inlineExports.push_back(
            CoreInlineExport { .name = std::format("f{}", index), .sort = CoreSort::Func, .index = 0 });
    std::ignore = coreInline.coreInlineExports(std::move(inlineExports));
    auto componentInline = withEveryKind();
    auto const promote = componentInline.type(
        function({ LabeledType { .label = "a", .type = PrimitiveValueType::F32 } }, PrimitiveValueType::F64));
    auto const lifted = componentInline.canonLift(0, {}, promote);
    auto items = std::vector<std::pair<std::string, SortIndex>> {};
    for (auto const index: std::views::iota(0, 20))
        items.emplace_back(std::format("f{}", index), SortIndex { .sort = Sort::Func, .index = lifted });
    std::ignore = componentInline.inlineInstance(std::move(items));
    for (auto const* builder: { &instantiated, &coreInline, &componentInline })
    {
        auto const instancePool = builder->pool();
        checkRefusedBelowNeed(instancePool,
                              { .limit = ComponentLimit::TypeSize, .field = &ComponentLimits::maxTypeSize });
        checkRefusedBelowNeed(
            instancePool, { .limit = ComponentLimit::TypeDepth, .field = &ComponentLimits::maxTypeDepth });
    }
}
