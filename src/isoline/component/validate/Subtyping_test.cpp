// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/component/Features.hpp>
#include <isoline/component/Limits.hpp>
#include <isoline/component/binary/Component.hpp>
#include <isoline/component/binary/testing/Builder.hpp>
#include <isoline/component/types/TypeArena.hpp>
#include <isoline/component/validate/Subtyping.hpp>
#include <isoline/component/validate/Validator.hpp>
#include <isoline/component/validate/testing/CoreModuleValidator.hpp>

using isoline::component::ComponentFeatureSet;
using isoline::component::ComponentLimit;
using isoline::component::ComponentLimits;
using isoline::component::types::AnyTypeId;
using isoline::component::types::ComponentTypeId;
using isoline::component::types::ComponentTypeInfo;
using isoline::component::types::DefinedType;
using isoline::component::types::ExternType;
using isoline::component::types::FunctionTypeId;
using isoline::component::types::FunctionTypeInfo;
using isoline::component::types::InstanceTypeId;
using isoline::component::types::InstanceTypeInfo;
using isoline::component::types::NamedExtern;
using isoline::component::types::ResourcePath;
using isoline::component::types::ResourceSubstitution;
using isoline::component::types::ResourceTypeId;
using isoline::component::types::ResourceTypeInfo;
using isoline::component::types::TypeArena;
using isoline::component::types::TypeExtern;
using isoline::component::types::TypeId;
using isoline::component::types::ValueExtern;
using isoline::component::types::ValueKind;
using isoline::component::validate::freshResourcesFor;
using isoline::component::validate::instantiatedExports;
using isoline::component::validate::RelationError;
using isoline::component::validate::RelationErrorCode;
using isoline::component::validate::ResourceMapping;
using isoline::component::validate::TypeRelation;

namespace
{
template <typename T>
[[nodiscard]] T held(std::expected<T, isoline::component::types::TypeLimitError> result)
{
    REQUIRE(result.has_value());
    return *result;
}

[[nodiscard]] TypeId primitive(TypeArena const& arena, ValueKind kind)
{
    return arena.primitive(kind);
}

[[nodiscard]] TypeId record(TypeArena& arena, std::vector<std::pair<std::string, TypeId>> const& fields)
{
    auto type = DefinedType { .kind = ValueKind::Record };
    for (auto const& [label, field]: fields)
    {
        type.labels.emplace_back(label);
        type.children.emplace_back(field);
    }
    return held(arena.intern(std::move(type)));
}

[[nodiscard]] TypeId tuple(TypeArena& arena, std::vector<TypeId> const& elements)
{
    auto type = DefinedType { .kind = ValueKind::Tuple };
    for (auto const element: elements)
        type.children.emplace_back(element);
    return held(arena.intern(std::move(type)));
}

[[nodiscard]] TypeId own(TypeArena& arena, ResourceTypeId resource)
{
    return held(arena.intern(DefinedType { .kind = ValueKind::Own, .resource = resource }));
}

[[nodiscard]] FunctionTypeId function(
    TypeArena& arena,
    std::vector<std::pair<isoline::component::types::SharedName, TypeId>> params,
    std::optional<TypeId> result = std::nullopt)
{
    return held(arena.add(FunctionTypeInfo { .params = std::move(params), .result = result }));
}

[[nodiscard]] InstanceTypeId instance(TypeArena& arena, std::vector<NamedExtern> exports)
{
    return held(arena.add(InstanceTypeInfo { .exports = std::move(exports) }));
}

/// The error relating @p sub to @p super reports; the relation must fail.
[[nodiscard]] RelationError mismatch(std::expected<void, RelationError> const& related)
{
    REQUIRE_FALSE(related.has_value());
    return related.error();
}

/// The message relating two extern types in one arena fails with.
[[nodiscard]] std::string mismatchText(TypeArena const& arena, ExternType const& sub, ExternType const& super)
{
    auto relation = TypeRelation { arena, arena, ResourceMapping {}, ComponentLimits {} };
    auto const error = mismatch(relation.isSubtype(sub, super));
    CHECK(error.code != RelationErrorCode::StepLimit);
    return error.message;
}

/// Whether two extern types of one arena are related.
[[nodiscard]] bool related(TypeArena const& arena, ExternType const& sub, ExternType const& super)
{
    auto relation = TypeRelation { arena, arena, ResourceMapping {}, ComponentLimits {} };
    auto const outcome = relation.isSubtype(sub, super);
    if (!outcome)
        UNSCOPED_INFO(outcome.error().message);
    return outcome.has_value();
}

/// The 30-level doubling graph t_i = tuple<t_{i-1}, t_{i-1}> over @p leaf, whose effective size is
/// 2^31 - 1: only an arena of a raised maxTypeSize holds it.
[[nodiscard]] TypeId doubling(TypeArena& arena, TypeId leaf, uint32_t levels)
{
    auto type = leaf;
    for ([[maybe_unused]] auto const level: std::views::iota(uint32_t { 0 }, levels))
        type = tuple(arena, { type, type });
    return type;
}
} // namespace

TEST_CASE("Subtyping.functionsInvariant")
{
    auto arena = TypeArena { ComponentLimits {} };
    auto const u32 = primitive(arena, ValueKind::U32);
    auto const u64 = primitive(arena, ValueKind::U64);
    auto const takesU32 = function(arena, { { "x", u32 } });
    auto const takesU64 = function(arena, { { "x", u64 } });
    CHECK(related(arena, takesU32, takesU32));
    // Equality, not subtyping, in both directions (wasm-tools' intended divergence).
    CHECK(mismatchText(arena, takesU32, takesU64)
          == "type mismatch in function parameter `x`\nexpected primitive `u64` found primitive `u32`");
    CHECK(mismatchText(arena, takesU64, takesU32)
          == "type mismatch in function parameter `x`\nexpected primitive `u32` found primitive `u64`");
    CHECK(mismatchText(arena, takesU32, function(arena, { { "y", u32 } }))
          == "expected parameter named `y`, found `x`");
    CHECK(mismatchText(arena, takesU32, function(arena, {})) == "expected 0 parameters, found 1");
    CHECK(mismatchText(arena, function(arena, {}, u32), function(arena, {}))
          == "expected a result, found none");
    CHECK(mismatchText(arena, function(arena, {}), function(arena, {}, u32))
          == "expected no result, found one");
    CHECK(mismatchText(arena, function(arena, {}, u32), function(arena, {}, u64))
          == "type mismatch with result type\nexpected primitive `u64` found primitive `u32`");
    auto const asyncFunction = held(arena.add(
        FunctionTypeInfo { .asyncness = isoline::component::binary::Asyncness::Async, .params = {} }));
    CHECK(mismatchText(arena, asyncFunction, function(arena, {}))
          == "expected async function, found sync function");

    // An instance with an extra export is a subtype of one with fewer, not the reverse.
    auto const wide = instance(arena, { { "a", takesU32 }, { "b", takesU64 } });
    auto const narrow = instance(arena, { { "a", takesU32 } });
    CHECK(related(arena, wide, narrow));
    CHECK(mismatchText(arena, narrow, wide) == "missing expected export `b`");
    CHECK(mismatchText(arena, instance(arena, { { "a", takesU64 } }), narrow)
          == "type mismatch in instance export `a`\ntype mismatch in function parameter `x`\n"
             "expected primitive `u32` found primitive `u64`");

    // Distinct resources are distinct types, however alike; a resource is itself.
    auto const first = held(arena.newResource(ResourceTypeInfo {}));
    auto const second = held(arena.newResource(ResourceTypeInfo {}));
    CHECK(related(arena, TypeExtern { .type = first }, TypeExtern { .type = first }));
    CHECK(mismatchText(arena, TypeExtern { .type = first }, TypeExtern { .type = second })
          == "resource types are not the same");
    CHECK(mismatchText(arena,
                       function(arena, { { "r", own(arena, first) } }),
                       function(arena, { { "r", own(arena, second) } }))
          == "type mismatch in function parameter `r`\nresource types are not the same");

    // The kinds, in wasm-tools' words.
    CHECK(mismatchText(arena, takesU32, narrow) == "expected instance, found func");
    CHECK(mismatchText(arena, ValueExtern { .type = u32 }, TypeExtern { .type = u32 })
          == "expected type, found value");
    CHECK(mismatchText(arena, TypeExtern { .type = first }, TypeExtern { .type = u32 })
          == "expected defined type, found resource");
    CHECK(mismatchText(arena, TypeExtern { .type = takesU32 }, TypeExtern { .type = first })
          == "expected resource, found func type");
}

TEST_CASE("Subtyping.wasmToolsTexts")
{
    // The instantiation vectors of the component model's validation/instantiation.wast and wasm-tools'
    // instantiate.wast (lines 724-759): a type import bound to a type of the wrong shape.
    auto arena = TypeArena { ComponentLimits {} };
    auto const u32 = primitive(arena, ValueKind::U32);
    auto const fieldF = record(arena, { { "f", u32 } });
    // (import "x" (type (eq $t))), $t u32, given (record (field "f" u32)).
    CHECK(mismatchText(arena, TypeExtern { .type = fieldF }, TypeExtern { .type = u32 })
          == "expected primitive, found record");
    CHECK(mismatchText(arena, TypeExtern { .type = u32 }, TypeExtern { .type = fieldF })
          == "expected record, found u32");
    // A field u32 given a tuple: inline, so named.
    auto const fieldX = record(arena, { { "x", u32 } });
    auto const givenTuple = record(arena, { { "x", tuple(arena, { primitive(arena, ValueKind::U8) }) } });
    CHECK(mismatchText(arena, TypeExtern { .type = givenTuple }, TypeExtern { .type = fieldX })
          == "type mismatch in record field `x`\nexpected u32, found tuple");
    // (tuple u32 u32) given (tuple string string).
    auto const strings =
        tuple(arena, { primitive(arena, ValueKind::String), primitive(arena, ValueKind::String) });
    CHECK(
        mismatchText(arena, TypeExtern { .type = strings }, TypeExtern { .type = tuple(arena, { u32, u32 }) })
        == "type mismatch in tuple field 0\nexpected primitive `u32` found primitive `string`");
    CHECK(mismatchText(arena, TypeExtern { .type = fieldF }, TypeExtern { .type = fieldX })
          == "expected field name `x`, found `f`");
    CHECK(mismatchText(arena,
                       TypeExtern { .type = tuple(arena, { u32 }) },
                       TypeExtern { .type = tuple(arena, { u32, u32 }) })
          == "expected 2 types, found 1");
    auto const flags = [&](std::vector<isoline::component::types::SharedName> labels) {
        return held(arena.intern(DefinedType { .kind = ValueKind::Flags, .labels = std::move(labels) }));
    };
    CHECK(mismatchText(arena, TypeExtern { .type = flags({ "a" }) }, TypeExtern { .type = flags({ "b" }) })
          == "mismatch in flags elements");
    auto const result = [&](std::optional<TypeId> ok, std::optional<TypeId> error) {
        return held(arena.intern(DefinedType { .kind = ValueKind::Result, .children = { ok, error } }));
    };
    CHECK(mismatchText(arena,
                       TypeExtern { .type = result(std::nullopt, u32) },
                       TypeExtern { .type = result(u32, u32) })
          == "expected ok type, but found none");
    CHECK(mismatchText(arena,
                       TypeExtern { .type = result(u32, u32) },
                       TypeExtern { .type = result(u32, std::nullopt) })
          == "expected err type to not be present");
    CHECK(mismatchText(
              arena, TypeExtern { .type = result(u32, fieldF) }, TypeExtern { .type = result(u32, fieldX) })
          == "type mismatch in err variant\nexpected field name `x`, found `f`");
    auto const list = held(arena.intern(DefinedType { .kind = ValueKind::List, .children = { u32 } }));
    CHECK(mismatchText(arena, ValueExtern { .type = list }, ValueExtern { .type = fieldF })
          == "expected record, found list");
}

TEST_CASE("Subtyping.crossArena")
{
    auto left = TypeArena { ComponentLimits {} };
    auto right = TypeArena { ComponentLimits {} };
    auto const leftRecord = record(left, { { "a", primitive(left, ValueKind::U32) } });
    auto const rightRecord = record(right, { { "a", primitive(right, ValueKind::U32) } });
    auto const otherLabel = record(right, { { "b", primitive(right, ValueKind::U32) } });
    auto relation = TypeRelation { left, right, ResourceMapping {}, ComponentLimits {} };
    CHECK(relation.isEqual(leftRecord, rightRecord).has_value());
    CHECK(mismatch(relation.isEqual(leftRecord, otherLabel)).message == "expected field name `b`, found `a`");

    // Across arenas a resource is equal to none, unless the mapping binds the super's to the sub's.
    auto const leftResource = held(left.newResource(ResourceTypeInfo {}));
    auto const rightResource = held(right.newResource(ResourceTypeInfo {}));
    auto const leftOwn = own(left, leftResource);
    auto const rightOwn = own(right, rightResource);
    CHECK(mismatch(relation.isEqual(leftOwn, rightOwn)).message == "resource types are not the same");
    auto mapping = ResourceMapping {};
    mapping.bind(rightResource, leftResource);
    auto bound = TypeRelation { left, right, mapping, ComponentLimits {} };
    CHECK(bound.isEqual(leftOwn, rightOwn).has_value());
}

TEST_CASE("Subtyping.memoisedOnSharedGraph")
{
    // Two 30-level doubling graphs, a tree of 2^31 - 1 nodes each when walked unshared, in two arenas
    // (one arena would make them one TypeId). The memo decides each of the 31 pairs once; without it
    // the walk would take 2^31 steps, and is refused at a cap far below that instead of taking that
    // long. The depth stays within wasm-tools' 100; the effective size is raised past its 1'000'000,
    // which a doubling graph of more than 18 levels exceeds, and is a uint32_t, which bounds the levels.
    auto limits = ComponentLimits {};
    limits.maxTypeSize = UINT32_MAX;
    limits.maxSubtypingSteps = 10'000;
    constexpr auto Levels = uint32_t { 30 };
    auto left = TypeArena { limits };
    auto right = TypeArena { limits };
    auto const leftGraph = doubling(left, primitive(left, ValueKind::U32), Levels);
    auto const rightGraph = doubling(right, primitive(right, ValueKind::U32), Levels);
    auto relation = TypeRelation { left, right, ResourceMapping {}, limits };
    auto const equal = relation.isEqual(leftGraph, rightGraph);
    if (!equal)
        UNSCOPED_INFO(equal.error().message);
    REQUIRE(equal.has_value());
    auto const nodes = Levels + 1;
    CHECK(relation.steps() <= std::size_t { 2 } * nodes);

    // A second query over the same pairs is decided by the memo at once.
    auto const before = relation.steps();
    CHECK(relation.isSubtype(ValueExtern { .type = leftGraph }, ValueExtern { .type = rightGraph })
              .has_value());
    CHECK(relation.steps() == before + 1);

    // A mismatch at the leaves is found, and reported once, at the first path to it.
    auto const different = doubling(right, primitive(right, ValueKind::U64), Levels);
    auto const refused = mismatch(relation.isEqual(leftGraph, different));
    CHECK(refused.message.starts_with("type mismatch in tuple field 0\ntype mismatch in tuple field 0\n"));
    CHECK(refused.message.ends_with("expected primitive `u64` found primitive `u32`"));
}

TEST_CASE("Subtyping.stepsCapped")
{
    // A hostile pair of wide instance types: every name lookup a step, refused at the cap, never
    // quadratic. With the default cap the same pair is related.
    auto arena = TypeArena { ComponentLimits {} };
    auto const empty = function(arena, {});
    auto exports = std::vector<NamedExtern> {};
    for (auto const index: std::views::iota(0, 20'000))
        exports.push_back(NamedExtern { .name = std::format("e{}", index), .type = empty });
    auto const wide = instance(arena, exports);
    auto const copy = instance(arena, std::move(exports));
    auto relation = TypeRelation { arena, arena, ResourceMapping {}, ComponentLimits {} };
    CHECK(relation.isSubtype(wide, copy).has_value());
    CHECK(relation.steps() < std::size_t { 6 } * 20'000);

    auto limits = ComponentLimits {};
    limits.maxSubtypingSteps = 1'000;
    auto capped = TypeRelation { arena, arena, ResourceMapping {}, limits };
    auto const refused = mismatch(capped.isSubtype(wide, copy));
    CHECK(refused.code == RelationErrorCode::StepLimit);
    CHECK(refused.message == "subtyping steps exceed the limit of 1000");
    CHECK(capped.steps() == 1'000);

    // Refused at the step that reaches the cap, not only for a batch past it: the index fits under
    // 50'000, the lookups after it do not.
    limits.maxSubtypingSteps = 50'000;
    auto later = TypeRelation { arena, arena, ResourceMapping {}, limits };
    auto const refusedLater = mismatch(later.isSubtype(wide, copy));
    CHECK(refusedLater.code == RelationErrorCode::StepLimit);
    CHECK(later.steps() == 50'000);

    // Names compared cost their bytes: ten 64 KB names take a few dozen steps by count, and are
    // refused at a cap of 1'000 by their bytes.
    auto longExports = std::vector<NamedExtern> {};
    for (auto const index: std::views::iota(0, 10))
        longExports.push_back(
            NamedExtern { .name = std::format("{}-{}", std::string(65'536, 'a'), index), .type = empty });
    auto const longNamed = instance(arena, longExports);
    auto const longCopy = instance(arena, std::move(longExports));
    limits.maxSubtypingSteps = 1'000;
    auto bytesCharged = TypeRelation { arena, arena, ResourceMapping {}, limits };
    CHECK(mismatch(bytesCharged.isSubtype(longNamed, longCopy)).code == RelationErrorCode::StepLimit);
}

TEST_CASE("Subtyping.componentTypes")
{
    // (component (import "r" (type (sub resource))) (export "f" (func (result (own 0))))), built in two
    // arenas: each binds its own imported resource, so the two are related across arenas without a
    // mapping, and a component whose export names a different resource is not.
    auto const build = [](TypeArena& arena, bool exportsForeign) {
        auto const imported = held(arena.newResource(ResourceTypeInfo {}));
        auto const foreign = held(arena.newResource(ResourceTypeInfo {}));
        auto const make = function(arena, {}, own(arena, exportsForeign ? foreign : imported));
        return held(arena.add(ComponentTypeInfo {
            .imports = { { "r", TypeExtern { .type = imported } } },
            .exports = { { "f", make } },
            .importedResources = { ResourcePath { .resource = imported, .path = { 0 } } } }));
    };
    auto left = TypeArena { ComponentLimits {} };
    auto right = TypeArena { ComponentLimits {} };
    auto const leftComponent = build(left, false);
    auto const rightComponent = build(right, false);
    auto const foreignComponent = build(right, true);
    auto relation = TypeRelation { left, right, ResourceMapping {}, ComponentLimits {} };
    CHECK(relation.isSubtype(leftComponent, rightComponent).has_value());
    auto const refused = mismatch(relation.isSubtype(leftComponent, foreignComponent));
    CHECK(refused.message
          == "type mismatch for export `f`\ntype mismatch with result type\nresource types are not the same");
    // The bindings of one query are undone: the related pair still is.
    CHECK(relation.isSubtype(leftComponent, rightComponent).has_value());

    // A component lacking an export required, or requiring an import not given.
    auto same = TypeArena { ComponentLimits {} };
    auto const none = held(same.add(ComponentTypeInfo {}));
    auto const exporting = held(same.add(ComponentTypeInfo { .exports = { { "f", function(same, {}) } } }));
    auto const importing = held(same.add(ComponentTypeInfo { .imports = { { "g", function(same, {}) } } }));
    CHECK(related(same, exporting, none));
    CHECK(mismatchText(same, none, exporting) == "missing export named `f`");
    CHECK(related(same, none, importing));
    CHECK(mismatchText(same, importing, none) == "missing import named `g`");
}

TEST_CASE("Subtyping.bindingsResolveThroughBindings")
{
    // A component re-exporting its imported resource satisfies a type that exports a fresh one: the
    // export binding records what the supplied resource stands for under the import binding already in
    // force, as wasm-tools remaps A's exports before opening B's (component_types.rs:3655-3664).
    // (component (import "a" (component $A (import "r" (type $r (sub resource))) (export "s" (type (eq
    // $r))))) (component $consumer (import "b" (component (import "r" (type (sub resource))) (export "s"
    // (type (sub resource)))))) (instance (instantiate $consumer (with "b" (component $A))))), which
    // wasm-tools 1.263.0 accepts.
    auto const reexporting = [](TypeArena& arena) {
        auto const imported = held(arena.newResource(ResourceTypeInfo {}));
        return held(arena.add(ComponentTypeInfo {
            .imports = { { "r", TypeExtern { .type = imported } } },
            .exports = { { "s", TypeExtern { .type = imported } } },
            .importedResources = { ResourcePath { .resource = imported, .path = { 0 } } } }));
    };
    auto const freshExporting = [](TypeArena& arena) {
        auto const imported = held(arena.newResource(ResourceTypeInfo {}));
        auto const defined = held(arena.newResource(ResourceTypeInfo {}));
        return held(arena.add(ComponentTypeInfo {
            .imports = { { "r", TypeExtern { .type = imported } } },
            .exports = { { "s", TypeExtern { .type = defined } } },
            .importedResources = { ResourcePath { .resource = imported, .path = { 0 } } },
            .definedResources = { ResourcePath { .resource = defined, .path = { 0 } } } }));
    };
    auto same = TypeArena { ComponentLimits {} };
    CHECK(related(same, reexporting(same), freshExporting(same)));
    auto left = TypeArena { ComponentLimits {} };
    auto right = TypeArena { ComponentLimits {} };
    auto across = TypeRelation { left, right, ResourceMapping {}, ComponentLimits {} };
    auto const acrossOutcome = across.isSubtype(reexporting(left), freshExporting(right));
    if (!acrossOutcome)
        UNSCOPED_INFO(acrossOutcome.error().message);
    CHECK(acrossOutcome.has_value());

    // Nested: the export binding of an inner component type resolves through the outer import binding.
    // (type $X (component (import "r" (type $r (sub resource))) (export "c" (component (alias outer 1 0
    // (type $o)) (export "s" (type (eq $o))))))) against (component (import "r" (type (sub resource)))
    // (export "c" (component (export "s" (type (sub resource)))))), which wasm-tools accepts.
    auto const outerReexporting = [](TypeArena& arena) {
        auto const imported = held(arena.newResource(ResourceTypeInfo {}));
        auto const inner =
            held(arena.add(ComponentTypeInfo { .exports = { { "s", TypeExtern { .type = imported } } } }));
        return held(arena.add(ComponentTypeInfo {
            .imports = { { "r", TypeExtern { .type = imported } } },
            .exports = { { "c", inner } },
            .importedResources = { ResourcePath { .resource = imported, .path = { 0 } } } }));
    };
    auto const outerFresh = [](TypeArena& arena) {
        auto const imported = held(arena.newResource(ResourceTypeInfo {}));
        auto const defined = held(arena.newResource(ResourceTypeInfo {}));
        auto const inner = held(arena.add(ComponentTypeInfo {
            .exports = { { "s", TypeExtern { .type = defined } } },
            .definedResources = { ResourcePath { .resource = defined, .path = { 0 } } } }));
        return held(arena.add(ComponentTypeInfo {
            .imports = { { "r", TypeExtern { .type = imported } } },
            .exports = { { "c", inner } },
            .importedResources = { ResourcePath { .resource = imported, .path = { 0 } } } }));
    };
    CHECK(related(same, outerReexporting(same), outerFresh(same)));
    auto nestedAcross = TypeRelation { left, right, ResourceMapping {}, ComponentLimits {} };
    auto const nestedOutcome = nestedAcross.isSubtype(outerReexporting(left), outerFresh(right));
    if (!nestedOutcome)
        UNSCOPED_INFO(nestedOutcome.error().message);
    CHECK(nestedOutcome.has_value());
}

TEST_CASE("Subtyping.errorsCarryTheirCode")
{
    // Every mismatch names its row, so a validator maps it without reading the text.
    auto arena = TypeArena { ComponentLimits {} };
    auto relation = TypeRelation { arena, arena, ResourceMapping {}, ComponentLimits {} };
    auto const u32 = primitive(arena, ValueKind::U32);
    auto const takes = function(arena, { { "x", u32 } });
    auto const refused =
        mismatch(relation.isSubtype(takes, function(arena, { { "x", primitive(arena, ValueKind::U64) } })));
    CHECK(refused.code == RelationErrorCode::PrimitiveMismatch);
    CHECK(mismatch(relation.isSubtype(takes, function(arena, {}))).code == RelationErrorCode::ParameterCount);
    CHECK(mismatch(relation.isSubtype(instance(arena, {}), instance(arena, { { "a", takes } }))).code
          == RelationErrorCode::MissingExport);
}

TEST_CASE("Resources.generativePerInstantiation")
{
    // (component (type $c (component (export "r" (type (sub resource))) (export "make" (func (result
    // (own 0))))))): each instantiation mints its own R, so instance 1's own<R> is not instance 2's.
    auto arena = TypeArena { ComponentLimits {} };
    auto const defined = held(arena.newResource(ResourceTypeInfo {}));
    auto const make = function(arena, {}, own(arena, defined));
    auto const child = held(arena.add(
        ComponentTypeInfo { .exports = { { "r", TypeExtern { .type = defined } }, { "make", make } },
                            .definedResources = { ResourcePath { .resource = defined, .path = { 0 } } } }));

    auto const first = held(freshResourcesFor(arena, arena[child]));
    auto const second = held(freshResourcesFor(arena, arena[child]));
    REQUIRE(first.find(defined).has_value());
    REQUIRE(second.find(defined).has_value());
    CHECK(*first.find(defined) != *second.find(defined));
    CHECK(*first.find(defined) != defined);

    auto const firstInstance = held(instantiatedExports(arena, child, first));
    auto const secondInstance = held(instantiatedExports(arena, child, second));
    auto const& firstExports = arena[firstInstance].exports;
    REQUIRE(firstExports.size() == 2);
    CHECK(std::get<TypeExtern>(firstExports[0].type).type == AnyTypeId { *first.find(defined) });
    CHECK(arena[firstInstance].definedResources.empty());
    auto const firstMake = std::get<FunctionTypeId>(firstExports[1].type);
    auto const secondMake = std::get<FunctionTypeId>(arena[secondInstance].exports[1].type);
    CHECK(arena[*arena[firstMake].result].resource == *first.find(defined));

    // Passing instance 1's `make` where instance 2's is expected.
    auto const refused = mismatchText(arena, firstMake, secondMake);
    CHECK(refused.contains("type mismatch"));
    CHECK(refused == "type mismatch with result type\nresource types are not the same");
    CHECK(mismatchText(arena, firstInstance, secondInstance)
              .starts_with("type mismatch in instance export `r`"));
    CHECK(related(arena, firstInstance, firstInstance));
}

TEST_CASE("Resources.importedResourceIsSubstituted")
{
    // (component (type $c (component (import "r" (type (sub resource))) (export "get" (func (result (own
    // 0)))))) instantiated with the parent's R: the instance's `get` returns the parent's own<R>.
    auto arena = TypeArena { ComponentLimits {} };
    auto const imported = held(arena.newResource(ResourceTypeInfo {}));
    auto const child = held(arena.add(
        ComponentTypeInfo { .imports = { { "r", TypeExtern { .type = imported } } },
                            .exports = { { "get", function(arena, {}, own(arena, imported)) } },
                            .importedResources = { ResourcePath { .resource = imported, .path = { 0 } } } }));
    auto const parent = held(arena.newResource(ResourceTypeInfo {}));

    auto relation = TypeRelation { arena, arena, ResourceMapping {}, ComponentLimits {} };
    auto const arguments = std::vector<NamedExtern> { { "r", TypeExtern { .type = parent } } };
    auto const bound = relation.satisfyImports(arguments, child);
    REQUIRE(bound.has_value());
    REQUIRE(bound->find(imported).has_value());
    CHECK(*bound->find(imported) == parent);
    // The bindings are the query's: afterwards the child's R is itself again, not the parent's.
    CHECK(mismatch(relation.isSubtype(TypeExtern { .type = parent }, TypeExtern { .type = imported })).message
          == "resource types are not the same");

    auto const instanceType = held(instantiatedExports(arena, child, *bound));
    auto const get = std::get<FunctionTypeId>(arena[instanceType].exports[0].type);
    CHECK(arena[*arena[get].result].resource == parent);
    CHECK(related(arena, get, function(arena, {}, own(arena, parent))));

    // An argument missing, or of the wrong kind.
    auto const missing = relation.satisfyImports({}, child);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().message == "missing import named `r`");
    auto const wrong =
        std::vector<NamedExtern> { { "r", TypeExtern { .type = primitive(arena, ValueKind::U32) } } };
    auto const refused = relation.satisfyImports(wrong, child);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().message == "type mismatch for import `r`\nexpected resource, found defined type");
}

TEST_CASE("Resources.substitutionCapped")
{
    // Each substitution creates the types naming a replaced resource, a shared one once, each charged to
    // the arena's maxSubstitutedTypes one unit plus one per type it names.
    auto limits = ComponentLimits {};
    limits.maxSubstitutedTypes = 13;
    auto arena = TypeArena { limits };
    auto const resource = held(arena.newResource(ResourceTypeInfo {}));
    auto const image = held(arena.newResource(ResourceTypeInfo {}));
    auto const handle = own(arena, resource);
    auto const shared = tuple(arena, { handle, handle, handle });
    auto substitution = ResourceSubstitution {};
    substitution.bind(resource, image);
    auto const rewritten = arena.substitute(ValueExtern { .type = shared }, substitution);
    REQUIRE(rewritten.has_value());
    CHECK(arena.substitutedTypes() == 5);
    auto const& elements = arena[std::get<ValueExtern>(*rewritten).type].children;
    REQUIRE(elements.size() == 3);
    CHECK(arena[*elements[0]].resource == image);
    // A type naming no resource is kept, and costs nothing.
    auto const plain = tuple(arena, { primitive(arena, ValueKind::U32) });
    CHECK(arena.substitute(ValueExtern { .type = plain }, substitution)
          == ExternType { ValueExtern { .type = plain } });
    CHECK(arena.substitutedTypes() == 5);

    auto const wider = function(arena, { { "a", shared }, { "b", handle } });
    // Nothing to replace: kept as it is, at no cost.
    CHECK(arena.substitute(wider, ResourceSubstitution {}) == ExternType { wider });
    CHECK(arena.substitutedTypes() == 5);
    auto other = ResourceSubstitution {};
    other.bind(resource, held(arena.newResource(ResourceTypeInfo {})));
    auto const first = arena.substitute(wider, other);
    REQUIRE(first.has_value());
    auto const exceeded = arena.substitute(wider, substitution);
    REQUIRE_FALSE(exceeded.has_value());
    CHECK(exceeded.error().limit == ComponentLimit::SubstitutedTypes);
}

TEST_CASE("Resources.instanceImportsMintFresh")
{
    // (component (type $i (instance (export "r" (type (sub resource))))) (import "a" (instance (type
    // $i))) (import "b" (instance (type $i)))): each import of an instance type defining a resource
    // mints its own (wasm-tools' prepare_instance_import), so a's r is not b's, and the component
    // imports both.
    using isoline::component::binary::ExternDesc;
    using isoline::component::binary::Sort;
    using isoline::component::binary::TypeBound;
    using isoline::component::binary::testing::ComponentBuilder;
    using isoline::component::binary::testing::TypeScopeBuilder;
    auto declarations = TypeScopeBuilder {};
    std::ignore = declarations.exportItem(
        "r", ExternDesc { .sort = Sort::Type, .index = 0, .bound = TypeBound::SubResource });
    auto builder = ComponentBuilder {};
    auto const instanceType = builder.instanceType(std::move(declarations));
    std::ignore = builder.import("a", ExternDesc { .sort = Sort::Instance, .index = instanceType });
    std::ignore = builder.import("b", ExternDesc { .sort = Sort::Instance, .index = instanceType });
    auto coreModules = isoline::component::validate::testing::CoreModuleValidator {};
    auto const validated = isoline::component::validate::validateComponent(
        builder.pool(), ComponentFeatureSet::defaults(), ComponentLimits {}, coreModules);
    REQUIRE(validated.has_value());
    auto const& component = **validated;
    auto const& imports = component.type.imports;
    REQUIRE(imports.size() == 2);
    auto const resourceOf = [&](NamedExtern const& import) {
        auto const& exports = component.types[std::get<InstanceTypeId>(import.type)].exports;
        REQUIRE(exports.size() == 1);
        return std::get<ResourceTypeId>(std::get<TypeExtern>(exports[0].type).type);
    };
    auto const a = resourceOf(imports[0]);
    auto const b = resourceOf(imports[1]);
    CHECK(a != b);
    CHECK(component.types[std::get<InstanceTypeId>(imports[0].type)].definedResources.empty());
    auto const& importedResources = component.type.importedResources;
    REQUIRE(importedResources.size() == 2);
    CHECK(importedResources[0] == ResourcePath { .resource = a, .path = { 0, 0 } });
    CHECK(importedResources[1] == ResourcePath { .resource = b, .path = { 1, 0 } });
    // The declared instance type keeps its own resource, defined.
    auto const declared =
        std::ranges::find_if(std::views::iota(uint32_t { 0 }, uint32_t { 3 }), [&](uint32_t index) {
            return !component.types[InstanceTypeId { .value = index }].definedResources.empty();
        });
    CHECK(*declared == 0);
}

TEST_CASE("Subtyping.compoundShapes")
{
    // Each shape check of component_defined_type (component_types.rs:4233-4418) in wasm-tools' words,
    // from RelationErrorTexts.
    auto arena = TypeArena { ComponentLimits {} };
    auto const u32 = primitive(arena, ValueKind::U32);
    auto const fieldX = record(arena, { { "x", u32 } });
    auto const fieldsXY = record(arena, { { "x", u32 }, { "y", u32 } });
    CHECK(mismatchText(arena, TypeExtern { .type = fieldX }, TypeExtern { .type = fieldsXY })
          == "expected 2 fields, found 1");

    auto const variant = [&](std::vector<std::pair<std::string, std::optional<TypeId>>> const& cases) {
        auto type = DefinedType { .kind = ValueKind::Variant };
        for (auto const& [label, payload]: cases)
        {
            type.labels.emplace_back(label);
            type.children.push_back(payload);
        }
        return held(arena.intern(std::move(type)));
    };
    auto const bare = variant({ { "a", std::nullopt } });
    auto const carrying = variant({ { "a", u32 } });
    CHECK(mismatchText(arena,
                       TypeExtern { .type = bare },
                       TypeExtern { .type = variant({ { "a", std::nullopt }, { "b", std::nullopt } }) })
          == "expected 2 cases, found 1");
    CHECK(mismatchText(arena, TypeExtern { .type = bare }, TypeExtern { .type = carrying })
          == "expected case `a` to have a type, found none");
    CHECK(mismatchText(arena, TypeExtern { .type = carrying }, TypeExtern { .type = bare })
          == "expected case `a` to have no type");
    // A case without a payload is passed over, the next one compared.
    auto const mixed = variant({ { "a", std::nullopt }, { "b", u32 } });
    auto const mixedOther = variant({ { "a", std::nullopt }, { "b", primitive(arena, ValueKind::U8) } });
    CHECK(mismatchText(arena, TypeExtern { .type = mixed }, TypeExtern { .type = mixedOther })
          == "type mismatch in variant case `b`\nexpected primitive `u8` found primitive `u32`");

    auto const fixed = [&](uint32_t length) {
        return held(arena.intern(
            DefinedType { .kind = ValueKind::FixedList, .children = { u32 }, .length = length }));
    };
    CHECK(mismatchText(arena, TypeExtern { .type = fixed(2) }, TypeExtern { .type = fixed(3) })
          == "expected fixed-length 3, found size 2");

    auto const stream = [&](std::optional<TypeId> element) {
        return held(
            arena.intern(DefinedType { .kind = ValueKind::Stream,
                                       .children = element ? std::vector<std::optional<TypeId>> { element }
                                                           : std::vector<std::optional<TypeId>> {} }));
    };
    CHECK(mismatchText(arena, TypeExtern { .type = stream(std::nullopt) }, TypeExtern { .type = stream(u32) })
          == "expected stream type, but found none");
    CHECK(mismatchText(arena, TypeExtern { .type = stream(u32) }, TypeExtern { .type = stream(std::nullopt) })
          == "expected stream type to not be present");
    // A stream's element: the context of a kind with no argument.
    CHECK(mismatchText(arena,
                       TypeExtern { .type = stream(u32) },
                       TypeExtern { .type = stream(primitive(arena, ValueKind::U8)) })
          == "type mismatch in stream\nexpected primitive `u8` found primitive `u32`");
    // A list's element: no context line at all.
    auto const list = [&](TypeId element) {
        return held(arena.intern(DefinedType { .kind = ValueKind::List, .children = { element } }));
    };
    CHECK(mismatchText(arena, TypeExtern { .type = list(u32) }, TypeExtern { .type = list(fieldX) })
          == "expected record, found u32");
}

TEST_CASE("Subtyping.entityKinds")
{
    // A component where a function is required, and a function, instance or component bound as a type.
    auto arena = TypeArena { ComponentLimits {} };
    auto const empty = function(arena, {});
    auto const component = held(arena.add(ComponentTypeInfo {}));
    CHECK(mismatchText(arena, component, empty) == "expected func, found component");
    CHECK(mismatchText(arena, empty, component) == "expected component, found func");
    auto relation = TypeRelation { arena, arena, ResourceMapping {}, ComponentLimits {} };
    CHECK(relation.isSubtype(TypeExtern { .type = empty }, TypeExtern { .type = empty }).has_value());
    auto const exportsOne = instance(arena, { NamedExtern { .name = "f", .type = empty } });
    CHECK(relation.isSubtype(TypeExtern { .type = exportsOne }, TypeExtern { .type = instance(arena, {}) })
              .has_value());
    CHECK(relation.isSubtype(TypeExtern { .type = component }, TypeExtern { .type = component }).has_value());
}

namespace
{

/// One component type of every frame kind, built into @p arena: it imports an instance `i`
/// exporting the resource `t` (quantified over at the path {0, 0}) and a function over a record, a
/// variant, an enum, flags and a fixed list of it, and a component `c` quantifying over the same
/// resource, which itself imports a component `d` doing so too; it exports a function of the record.
/// Built once per arena, the two copies share no id, so every value is compared by structure; and
/// the resource is bound three levels deep, the import of an import flipping the side back.
[[nodiscard]] ComponentTypeId richComponent(TypeArena& arena)
{
    auto const resource = held(arena.newResource(ResourceTypeInfo {}));
    auto const u32 = primitive(arena, ValueKind::U32);
    auto const handle = own(arena, resource);
    auto const fields = record(arena, { { "handle", handle }, { "count", u32 } });
    auto const choice = held(arena.intern(DefinedType {
        .kind = ValueKind::Variant, .children = { std::nullopt, fields }, .labels = { "none", "some" } }));
    auto const letters = held(arena.intern(DefinedType { .kind = ValueKind::Enum, .labels = { "a", "b" } }));
    auto const bits = held(arena.intern(DefinedType { .kind = ValueKind::Flags, .labels = { "x", "y" } }));
    auto const three =
        held(arena.intern(DefinedType { .kind = ValueKind::FixedList, .children = { u32 }, .length = 3 }));
    auto const use =
        function(arena,
                 { { "choice", choice }, { "letters", letters }, { "bits", bits }, { "three", three } },
                 fields);
    auto const exported =
        instance(arena,
                 { NamedExtern { .name = "t", .type = TypeExtern { .type = AnyTypeId { resource } } },
                   NamedExtern { .name = "use", .type = use } });
    auto const quantified = [&](std::vector<NamedExtern> more) {
        auto imports = std::vector<NamedExtern> { NamedExtern { .name = "i", .type = exported } };
        imports.insert(imports.end(), more.begin(), more.end());
        return held(arena.add(ComponentTypeInfo {
            .imports = std::move(imports),
            .importedResources = { ResourcePath { .resource = resource, .path = { 0, 0 } } } }));
    };
    auto const innermost = quantified({});
    auto const inner = quantified({ NamedExtern { .name = "d", .type = innermost } });
    return held(arena.add(ComponentTypeInfo {
        .imports = { NamedExtern { .name = "i", .type = exported },
                     NamedExtern { .name = "c", .type = inner } },
        .exports = { NamedExtern { .name = "make", .type = function(arena, { { "n", u32 } }, fields) } },
        .importedResources = { ResourcePath { .resource = resource, .path = { 0, 0 } } } }));
}

/// richComponent() in two arenas.
struct RichComponents
{
    TypeArena left { ComponentLimits {} };
    TypeArena right { ComponentLimits {} };
    ComponentTypeId first = richComponent(left);
    ComponentTypeId second = richComponent(right);
};

} // namespace

TEST_CASE("Subtyping.stepCapAtEveryStep")
{
    // The same relation under every cap below what it takes: refused at that step, wherever in the
    // walk it falls (a step, a name's bytes, an index, a lookup, a path into an instance).
    auto types = RichComponents {};
    auto relation = TypeRelation { types.left, types.right, ResourceMapping {}, ComponentLimits {} };
    REQUIRE(relation.isSubtype(types.first, types.second).has_value());
    auto const total = relation.steps();
    REQUIRE(total > 10);
    for (auto const cap: std::views::iota(std::size_t { 1 }, total))
    {
        auto limits = ComponentLimits {};
        limits.maxSubtypingSteps = static_cast<uint32_t>(cap);
        auto capped = TypeRelation { types.left, types.right, ResourceMapping {}, limits };
        auto const refused = capped.isSubtype(types.first, types.second);
        REQUIRE_FALSE(refused.has_value());
        REQUIRE(refused.error().code == RelationErrorCode::StepLimit);
    }
}

TEST_CASE("Subtyping.pathToAMissingExport")
{
    // A resource quantified over at {0, 0} whose argument instance lacks the export the path names:
    // the path binds nothing, and the instance check reports the missing export.
    auto arena = TypeArena { ComponentLimits {} };
    auto const resource = held(arena.newResource(ResourceTypeInfo {}));
    auto const exported = instance(
        arena, { NamedExtern { .name = "t", .type = TypeExtern { .type = AnyTypeId { resource } } } });
    auto const quantifying = held(arena.add(ComponentTypeInfo {
        .imports = { NamedExtern { .name = "i", .type = exported } },
        .importedResources = { ResourcePath { .resource = resource, .path = { 0, 0 } } } }));
    auto const other = instance(arena, { NamedExtern { .name = "u", .type = function(arena, {}) } });
    auto const takingOther =
        held(arena.add(ComponentTypeInfo { .imports = { NamedExtern { .name = "i", .type = other } } }));
    CHECK(mismatchText(arena, quantifying, takingOther)
          == "type mismatch for import `i`\nmissing expected export `t`");
}

TEST_CASE("Subtyping.pathIntoAnArgumentOfAnotherKind")
{
    // A resource quantified over at {0, 0} whose argument at 0 is a function, not an instance: the path
    // binds nothing, and the type check reports the function where the instance is required.
    auto arena = TypeArena { ComponentLimits {} };
    auto const resource = held(arena.newResource(ResourceTypeInfo {}));
    auto const exported = instance(
        arena, { NamedExtern { .name = "t", .type = TypeExtern { .type = AnyTypeId { resource } } } });
    auto const quantifying = held(arena.add(ComponentTypeInfo {
        .imports = { NamedExtern { .name = "i", .type = exported } },
        .importedResources = { ResourcePath { .resource = resource, .path = { 0, 0 } } } }));
    auto const takingAFunction = held(arena.add(
        ComponentTypeInfo { .imports = { NamedExtern { .name = "i", .type = function(arena, {}) } } }));
    CHECK(mismatchText(arena, quantifying, takingAFunction)
          == "type mismatch for import `i`\nexpected instance, found func");
}

TEST_CASE("Resources.freshResourcesCapped")
{
    // Minting a component's defined resources afresh is refused at maxResourceTypes.
    auto limits = ComponentLimits {};
    limits.maxResourceTypes = 1;
    auto arena = TypeArena { limits };
    auto const resource = held(arena.newResource(ResourceTypeInfo {}));
    auto const defining =
        ComponentTypeInfo { .definedResources = { ResourcePath { .resource = resource, .path = { 0 } } } };
    auto const fresh = freshResourcesFor(arena, defining);
    REQUIRE_FALSE(fresh.has_value());
    CHECK(fresh.error().limit == ComponentLimit::ResourceTypes);
}

TEST_CASE("Subtyping.instancePairComparedOnce")
{
    // An instance type exported twice on both sides, across two arenas: the second export finds the
    // pair memoised and takes one step, not the instance's walk again.
    auto left = TypeArena { ComponentLimits {} };
    auto right = TypeArena { ComponentLimits {} };
    auto const inner = [](TypeArena& arena) {
        return instance(
            arena,
            { NamedExtern { .name = "f",
                            .type = function(arena, { { "x", primitive(arena, ValueKind::U32) } }) } });
    };
    auto const outer = [](TypeArena& arena, InstanceTypeId shared) {
        return instance(
            arena,
            { NamedExtern { .name = "a", .type = shared }, NamedExtern { .name = "b", .type = shared } });
    };
    auto const leftOuter = outer(left, inner(left));
    auto const rightOuter = outer(right, inner(right));
    auto relation = TypeRelation { left, right, ResourceMapping {}, ComponentLimits {} };
    REQUIRE(relation.isSubtype(leftOuter, rightOuter).has_value());
    auto const twice = relation.steps();

    auto single = TypeRelation { left, right, ResourceMapping {}, ComponentLimits {} };
    REQUIRE(single.isSubtype(inner(left), inner(right)).has_value());
    INFO(std::format("{} steps for the pair, {} for one instance", twice, single.steps()));
    CHECK(twice <= single.steps() + 6);
}

TEST_CASE("Subtyping.nameIndexChargedOnce")
{
    // One relation serves a whole validation (Task 7), so an instance's exports are indexed by name and
    // charged once, however many queries read them: comparing it with a second expected instance costs
    // the lookups, not the 1'000 names again.
    auto arena = TypeArena { ComponentLimits {} };
    auto const nothing = function(arena, {});
    auto exports = std::vector<NamedExtern> {};
    for (auto const index: std::views::iota(0, 1'000))
        exports.push_back(NamedExtern { .name = "f" + std::to_string(index), .type = nothing });
    auto const provided = instance(arena, std::move(exports));
    auto const first = instance(arena, { NamedExtern { .name = "f1", .type = nothing } });
    auto const second = instance(arena, { NamedExtern { .name = "f2", .type = nothing } });
    auto relation = TypeRelation { arena, arena, ResourceMapping {}, ComponentLimits {} };
    REQUIRE(relation.isSubtype(provided, first).has_value());
    auto const afterFirst = relation.steps();
    CHECK(afterFirst > 1'000);
    REQUIRE(relation.isSubtype(provided, second).has_value());
    CHECK(relation.steps() - afterFirst < 50);
}
