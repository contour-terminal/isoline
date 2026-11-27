// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <isoline/component/Limits.hpp>
#include <isoline/component/binary/Component.hpp>
#include <isoline/component/types/TypeArena.hpp>

using isoline::component::ComponentLimit;
using isoline::component::ComponentLimits;
using isoline::component::binary::CoreValueKind;
using isoline::component::types::ComponentTypeInfo;
using isoline::component::types::DefinedType;
using isoline::component::types::FunctionTypeInfo;
using isoline::component::types::HandleContent;
using isoline::component::types::holds;
using isoline::component::types::NamedExtern;
using isoline::component::types::ResourceTypeInfo;
using isoline::component::types::TypeArena;
using isoline::component::types::TypeExtern;
using isoline::component::types::TypeId;
using isoline::component::types::TypeLimitError;
using isoline::component::types::TypeMeasure;
using isoline::component::types::ValueKind;

namespace
{
/// A value type of @p kind over @p children and @p labels.
[[nodiscard]] DefinedType typeOf(ValueKind kind,
                                 std::vector<std::optional<TypeId>> children,
                                 std::vector<isoline::component::types::SharedName> labels = {})
{
    return DefinedType { .kind = kind, .children = std::move(children), .labels = std::move(labels) };
}

/// Interns @p type, which must be within the arena's caps.
[[nodiscard]] TypeId interned(TypeArena& arena, DefinedType type)
{
    auto const id = arena.intern(std::move(type));
    REQUIRE(id.has_value());
    return *id;
}
} // namespace

TEST_CASE("TypeArena.interningIsStructural")
{
    auto arena = TypeArena { ComponentLimits {} };
    auto const u32 = arena.primitive(ValueKind::U32);
    CHECK(arena[u32].kind == ValueKind::U32);
    // A primitive interned is the primitive the arena holds.
    CHECK(interned(arena, typeOf(ValueKind::U32, {})) == u32);

    auto const first = interned(arena, typeOf(ValueKind::Record, { u32 }, { "a" }));
    auto const second = interned(arena, typeOf(ValueKind::Record, { u32 }, { "a" }));
    CHECK(first == second);
    CHECK(interned(arena, typeOf(ValueKind::Record, { u32 }, { "b" })) != first);
    CHECK(arena[first].depth == 2);
    CHECK(arena[first].effectiveSize == 2);

    // enum {a} and variant {a} are different types with the same despecialization.
    auto const enumeration = interned(arena, typeOf(ValueKind::Enum, {}, { "a" }));
    auto const variant = interned(arena, typeOf(ValueKind::Variant, { std::nullopt }, { "a" }));
    CHECK(enumeration != variant);
    auto const despecialized = arena.despecialize(enumeration);
    REQUIRE(despecialized.has_value());
    CHECK(*despecialized == variant);
    auto const variantItself = arena.despecialize(variant);
    REQUIRE(variantItself.has_value());
    CHECK(*variantItself == variant);

    // tuple → record of "0", "1"; option → variant none/some; result → variant ok/error.
    auto const pair = interned(arena, typeOf(ValueKind::Tuple, { u32, u32 }));
    auto const record = arena.despecialize(pair);
    REQUIRE(record.has_value());
    CHECK(*record == interned(arena, typeOf(ValueKind::Record, { u32, u32 }, { "0", "1" })));
    auto const option = interned(arena, typeOf(ValueKind::Option, { u32 }));
    auto const someOrNone = arena.despecialize(option);
    REQUIRE(someOrNone.has_value());
    CHECK(*someOrNone
          == interned(arena, typeOf(ValueKind::Variant, { std::nullopt, u32 }, { "none", "some" })));
    auto const result = interned(arena, typeOf(ValueKind::Result, { std::nullopt, u32 }));
    auto const okOrError = arena.despecialize(result);
    REQUIRE(okOrError.has_value());
    CHECK(*okOrError
          == interned(arena, typeOf(ValueKind::Variant, { std::nullopt, u32 }, { "ok", "error" })));

    // Two arenas are two identities.
    CHECK(TypeArena { ComponentLimits {} }.id() != arena.id());
}

TEST_CASE("TypeArena.measuresAsWasmTools")
{
    auto limits = ComponentLimits {};
    limits.maxTypeDepth = 3;
    limits.maxTypeSize = 10;
    auto arena = TypeArena { limits };
    auto const u8 = arena.primitive(ValueKind::U8);
    // Flags and enums measure 1 whatever their labels; a list one more than its element.
    auto const flags = interned(arena, typeOf(ValueKind::Flags, {}, { "a", "b", "c" }));
    CHECK(arena.measure(flags) == TypeMeasure { .depth = 1, .effectiveSize = 1 });
    auto const list = interned(arena, typeOf(ValueKind::List, { u8 }));
    auto const nested = interned(arena, typeOf(ValueKind::List, { list }));
    CHECK(arena.measure(nested) == TypeMeasure { .depth = 3, .effectiveSize = 3 });

    // Depth: refused past maxTypeDepth.
    auto const deeper = arena.intern(typeOf(ValueKind::List, { nested }));
    REQUIRE_FALSE(deeper.has_value());
    CHECK(deeper.error() == TypeLimitError { .limit = ComponentLimit::TypeDepth, .value = 3 });

    // Size: refused once it reaches maxTypeSize; each use of a child counts again.
    auto const nine = interned(arena, typeOf(ValueKind::Tuple, { u8, u8, u8, u8, u8, u8, u8, u8 }));
    CHECK(arena[nine].effectiveSize == 9);
    auto const ten = arena.intern(typeOf(ValueKind::Tuple, { u8, u8, u8, u8, u8, u8, u8, u8, u8 }));
    REQUIRE_FALSE(ten.has_value());
    CHECK(ten.error() == TypeLimitError { .limit = ComponentLimit::TypeSize, .value = 10 });
    auto const shared = arena.intern(typeOf(ValueKind::Tuple, { list, list, list, list, list }));
    REQUIRE_FALSE(shared.has_value());
    CHECK(shared.error().limit == ComponentLimit::TypeSize);

    // Function and component types are measured from what they name.
    auto const function = arena.add(FunctionTypeInfo { .params = { { "a", list } }, .result = u8 });
    REQUIRE(function.has_value());
    CHECK(arena[*function].measure == TypeMeasure { .depth = 3, .effectiveSize = 4 });
    auto const component = arena.add(
        ComponentTypeInfo { .imports = { NamedExtern { .name = "f", .type = *function } },
                            .exports = { NamedExtern { .name = "t", .type = TypeExtern { .type = u8 } } } });
    REQUIRE_FALSE(component.has_value());
    CHECK(component.error().limit == ComponentLimit::TypeDepth);
}

TEST_CASE("TypeArena.handlesPropagate")
{
    auto arena = TypeArena { ComponentLimits {} };
    auto const resource = arena.newResource(ResourceTypeInfo {});
    REQUIRE(resource.has_value());
    auto const own = interned(arena, DefinedType { .kind = ValueKind::Own, .resource = *resource });
    auto const borrow = interned(arena, DefinedType { .kind = ValueKind::Borrow, .resource = *resource });
    auto const both = interned(arena, typeOf(ValueKind::Tuple, { own, borrow }));
    CHECK(holds(arena[own].handles, HandleContent::Own));
    CHECK_FALSE(holds(arena[own].handles, HandleContent::Borrow));
    CHECK(holds(arena[both].handles, HandleContent::Borrow));
    CHECK(holds(arena[both].handles, HandleContent::Own));
    CHECK_FALSE(holds(arena[arena.primitive(ValueKind::String)].handles, HandleContent::Own));
}

TEST_CASE("TypeArena.resourceFlags")
{
    // Whether a type names a resource is a flag computed when it is added, which substitution and the
    // outer-alias rule read instead of walking the type.
    auto arena = TypeArena { ComponentLimits {} };
    auto const resource = arena.newResource(ResourceTypeInfo {});
    REQUIRE(resource.has_value());
    auto const own = interned(arena, DefinedType { .kind = ValueKind::Own, .resource = *resource });
    auto const u32 = arena.primitive(ValueKind::U32);
    auto const takesOwn = arena.add(FunctionTypeInfo { .params = { { "r", u32 } }, .result = own });
    auto const takesU32 = arena.add(FunctionTypeInfo { .params = { { "r", u32 } } });
    REQUIRE(takesOwn.has_value());
    REQUIRE(takesU32.has_value());
    CHECK(holds(arena[*takesOwn].handles, HandleContent::Own));
    CHECK(arena[*takesU32].handles == HandleContent::None);
    CHECK(arena.namesResource(*takesOwn));
    CHECK_FALSE(arena.namesResource(*takesU32));
    CHECK(arena.namesResource(*resource));
    CHECK_FALSE(arena.namesResource(u32));

    using isoline::component::types::InstanceTypeInfo;
    using isoline::component::types::ResourcePath;
    auto const plain = arena.add(InstanceTypeInfo { .exports = { { "f", *takesU32 } } });
    auto const exportsResource =
        arena.add(InstanceTypeInfo { .exports = { { "r", TypeExtern { .type = *resource } } } });
    auto const defines = arena.add(
        InstanceTypeInfo { .exports = { { "f", *takesU32 } },
                           .definedResources = { ResourcePath { .resource = *resource, .path = { 0 } } } });
    REQUIRE(plain.has_value());
    REQUIRE(exportsResource.has_value());
    REQUIRE(defines.has_value());
    CHECK_FALSE(arena.namesResource(*plain));
    CHECK(arena.namesResource(*exportsResource));
    CHECK(arena.namesResource(*defines));
    auto const component =
        arena.add(ComponentTypeInfo { .imports = { { "i", *plain } }, .exports = { { "f", *takesOwn } } });
    auto const plainComponent = arena.add(ComponentTypeInfo { .imports = { { "i", *plain } } });
    REQUIRE(component.has_value());
    REQUIRE(plainComponent.has_value());
    CHECK(arena.namesResource(*component));
    CHECK_FALSE(arena.namesResource(*plainComponent));
}

TEST_CASE("TypeArena.resourcesAreNominal")
{
    auto limits = ComponentLimits {};
    limits.maxResourceTypes = 2;
    auto arena = TypeArena { limits };
    auto const first = arena.newResource(ResourceTypeInfo { .rep = CoreValueKind::I32 });
    auto const second = arena.newResource(ResourceTypeInfo { .rep = CoreValueKind::I32 });
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(*first != *second);
    CHECK(arena[*first] == arena[*second]);
    // own<first> and own<second> are different value types too.
    CHECK(interned(arena, DefinedType { .kind = ValueKind::Own, .resource = *first })
          != interned(arena, DefinedType { .kind = ValueKind::Own, .resource = *second }));
    auto const third = arena.newResource(ResourceTypeInfo {});
    REQUIRE_FALSE(third.has_value());
    CHECK(third.error() == TypeLimitError { .limit = ComponentLimit::ResourceTypes, .value = 2 });
}

TEST_CASE("TypeArena.depthCap")
{
    // A list chain 100'000 deep, interned bottom-up as validation does: refused at depth 101, the
    // first type past maxTypeDepth, without walking anything.
    auto arena = TypeArena { ComponentLimits {} };
    auto element = arena.primitive(ValueKind::U32);
    auto refusedAt = std::optional<uint32_t> {};
    for (auto const level: std::views::iota(uint32_t { 2 }, uint32_t { 100'002 }))
    {
        auto const list = arena.intern(typeOf(ValueKind::List, { element }));
        if (!list)
        {
            CHECK(list.error() == TypeLimitError { .limit = ComponentLimit::TypeDepth, .value = 100 });
            refusedAt = level;
            break;
        }
        element = *list;
    }
    CHECK(refusedAt == ComponentLimits {}.maxTypeDepth + 1);
}

TEST_CASE("TypeArena.solvedHashCollisionsStayLogarithmic")
{
    // The hash the arena once indexed by (a boost-style mix over the kind and identity hashes of the
    // child ids, under a public seed) can be solved: for each first child a, the second child b that
    // brings every tuple<a, b> to one state. Thousands of such tuples once landed in one bucket and
    // interning compared each with all before it. The ordered index makes O(log n) comparisons per
    // lookup whatever the types; the bound below is about 20x under what the bucket made.
    constexpr auto Children = uint32_t { 200'000 };
    constexpr auto Golden = uint64_t { 0x9e3779b97f4a7c15ULL };
    auto const mix = [](uint64_t seed, uint64_t value) {
        return seed ^ (value + Golden + (seed << 6U) + (seed >> 2U));
    };
    auto arena = TypeArena { ComponentLimits {} };
    auto const first = static_cast<uint32_t>(arena.valueTypeCount());
    for (auto const index: std::views::iota(uint32_t { 0 }, Children))
        std::ignore = interned(arena, typeOf(ValueKind::Enum, {}, { std::format("l{}", index) }));
    auto const last = first + Children;

    auto const tuple = static_cast<uint64_t>(ValueKind::Tuple);
    auto const anchor = mix(tuple, first + (Children / 2));
    auto const target = mix(anchor, first + (Children / 3));
    auto pairs = std::vector<std::pair<uint32_t, uint32_t>> {};
    for (auto const a: std::views::iota(first, last))
    {
        auto const state = mix(tuple, a);
        auto const b = (target ^ state) - Golden - (state << 6U) - (state >> 2U);
        if (b >= first && b < last)
            pairs.emplace_back(a, static_cast<uint32_t>(b));
    }
    REQUIRE(pairs.size() > 1'000);
    for (auto const& [a, b]: pairs)
        CHECK(mix(mix(tuple, a), b) == target);

    auto const before = arena.structuralComparisons();
    for (auto const& [a, b]: pairs)
        std::ignore =
            interned(arena, typeOf(ValueKind::Tuple, { TypeId { .value = a }, TypeId { .value = b } }));
    auto const made = arena.structuralComparisons() - before;
    auto const depth = static_cast<std::size_t>(std::bit_width(arena.valueTypeCount()));
    INFO(std::format("{} tuples, {} comparisons", pairs.size(), made));
    CHECK(made <= pairs.size() * 2 * (depth + 2));
}
