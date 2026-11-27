// SPDX-License-Identifier: Apache-2.0
#pragma once

// One side of a canonical call for the canonical layer's tests: an arena, a fake memory, a fake handle
// table and a budget, and the helpers that lift and lower through them. Uses Catch2's assertions.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include <isoline/api/Types.hpp>
#include <isoline/component/Limits.hpp>
#include <isoline/component/canonical/GuestMemory.hpp>
#include <isoline/component/canonical/Layout.hpp>
#include <isoline/component/canonical/LiftLower.hpp>
#include <isoline/component/canonical/Trap.hpp>
#include <isoline/component/canonical/Values.hpp>
#include <isoline/component/canonical/testing/GuestDoubles.hpp>
#include <isoline/component/types/TypeArena.hpp>

namespace isoline::component::canonical::testing
{

using isoline::api::Value;
using types::DefinedType;
using types::ResourceTypeId;
using types::ResourceTypeInfo;
using types::SharedName;
using types::TypeArena;
using types::TypeId;
using types::ValueKind;

/// Ample for every test that is not about the budget.
inline constexpr auto AmpleBudget = uint64_t { 1 } << 30U;

/// The labels @p names.
[[nodiscard]] inline std::vector<SharedName> labelsOf(std::initializer_list<char const*> names)
{
    auto labels = std::vector<SharedName> {};
    for (auto const* name: names)
        labels.emplace_back(name);
    return labels;
}

/// @p values as bytes.
[[nodiscard]] inline std::vector<std::byte> bytesOf(std::initializer_list<unsigned> values)
{
    auto bytes = std::vector<std::byte> {};
    for (auto const value: values)
        bytes.push_back(static_cast<std::byte>(value));
    return bytes;
}

/// The little-endian bytes of @p value, @p width of them.
[[nodiscard]] inline std::vector<std::byte> littleEndian(uint64_t value, std::size_t width)
{
    auto bytes = std::vector<std::byte> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, width))
        bytes.push_back(static_cast<std::byte>((value >> (8U * index)) & 0xFFU));
    return bytes;
}

/// The trap @p result failed with.
template <typename T>
[[nodiscard]] Trap trapIn(std::expected<T, Trap> const& result)
{
    REQUIRE_FALSE(result.has_value());
    return result.error();
}

/// Types of one arena, a memory, a handle table and a budget: one side of a call.
class Side
{
  public:
    explicit Side(FakeGuestMemoryOptions memory = {}, uint64_t budget = AmpleBudget):
        _arena { ComponentLimits {} },
        _memory { std::move(memory) },
        _budget { std::make_shared<LiveBytesBudget>(budget) }
    {
    }

    [[nodiscard]] TypeId primitive(ValueKind kind) const { return _arena.primitive(kind); }

    [[nodiscard]] TypeId intern(DefinedType type)
    {
        auto const id = _arena.intern(std::move(type));
        REQUIRE(id.has_value());
        return *id;
    }

    [[nodiscard]] TypeId compound(ValueKind kind, std::vector<std::optional<TypeId>> children)
    {
        return intern(DefinedType { .kind = kind, .children = std::move(children) });
    }

    [[nodiscard]] TypeId record(std::initializer_list<char const*> labels,
                                std::vector<std::optional<TypeId>> fields)
    {
        return intern(DefinedType {
            .kind = ValueKind::Record, .children = std::move(fields), .labels = labelsOf(labels) });
    }

    [[nodiscard]] TypeId variant(std::initializer_list<char const*> labels,
                                 std::vector<std::optional<TypeId>> payloads)
    {
        return intern(DefinedType {
            .kind = ValueKind::Variant, .children = std::move(payloads), .labels = labelsOf(labels) });
    }

    [[nodiscard]] TypeId labelled(ValueKind kind, std::initializer_list<char const*> labels)
    {
        return intern(DefinedType { .kind = kind, .labels = labelsOf(labels) });
    }

    [[nodiscard]] TypeId handle(ValueKind kind)
    {
        if (!_resource)
        {
            auto const resource = _arena.newResource(ResourceTypeInfo {});
            REQUIRE(resource.has_value());
            _resource = *resource;
        }
        return intern(DefinedType { .kind = kind, .resource = _resource });
    }

    [[nodiscard]] ResourceTypeId resource() const
    {
        REQUIRE(_resource.has_value());
        return *_resource;
    }

    /// A context lifting and lowering on this side, its layouts computed for every type so far.
    /// @param encoding Its `string-encoding` option.
    [[nodiscard]] LiftContext context(StringEncoding encoding = StringEncoding::Utf8)
    {
        return contextOver(_memory, encoding);
    }

    /// A context lifting and lowering on this side, but through @p memory.
    /// @param memory The memory; it must outlive the context.
    /// @param encoding Its `string-encoding` option.
    [[nodiscard]] LiftContext contextOver(IGuestMemoryAccess& memory,
                                          StringEncoding encoding = StringEncoding::Utf8)
    {
        auto layouts = computeLayouts(_arena);
        REQUIRE(layouts.has_value());
        // Every context keeps its table: a later one, laid out after more types, leaves it valid.
        auto const& table = *_layouts.emplace_back(std::make_unique<LayoutTable>(std::move(*layouts)));
        return LiftContext { .arena = _arena,
                             .layouts = table.all(),
                             .memory = &memory,
                             .encoding = encoding,
                             .budget = _budget,
                             .handles = _handles };
    }

    /// The values @p steps build for @p roots; they must build.
    [[nodiscard]] Values build(std::vector<TypeId> const& roots,
                               std::function<void(ValueBuilder&)> const& steps)
    {
        auto builder = ValueBuilder { _arena, roots, _budget };
        steps(builder);
        auto values = builder.finish();
        if (!values)
            FAIL(values.error().message());
        REQUIRE(values.has_value());
        return std::move(*values);
    }

    [[nodiscard]] TypeArena const& arena() const noexcept { return _arena; }
    [[nodiscard]] std::shared_ptr<LiveBytesBudget> const& budgetPointer() const noexcept { return _budget; }
    [[nodiscard]] FakeGuestMemory& memory() noexcept { return _memory; }
    [[nodiscard]] FakeHandleOps& handles() noexcept { return _handles; }
    [[nodiscard]] LiveBytesBudget const& budget() const noexcept { return *_budget; }

  private:
    TypeArena _arena;
    FakeGuestMemory _memory;
    FakeHandleOps _handles {};
    std::shared_ptr<LiveBytesBudget> _budget;
    std::vector<std::unique_ptr<LayoutTable>> _layouts {};
    std::optional<ResourceTypeId> _resource {};
};

/// Every root of @p values.
[[nodiscard]] inline std::vector<ValueView> rootsOf(Values const& values)
{
    auto roots = std::vector<ValueView> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, values.rootCount()))
        roots.push_back(values.root(index));
    return roots;
}

/// The values @p flat lifts to as @p types; they must lift.
[[nodiscard]] inline Values lifted(LiftContext& context,
                                   std::vector<TypeId> const& types,
                                   std::vector<Value> const& flat,
                                   uint32_t maxFlat = MaxFlatParams)
{
    auto values = liftFlat(context, types, flat, maxFlat);
    if (!values)
        FAIL(values.error().message());
    REQUIRE(values.has_value());
    return std::move(*values);
}

/// The core values @p values lower to as @p types; they must lower.
[[nodiscard]] inline std::vector<Value> lowered(LiftContext& context,
                                                std::vector<TypeId> const& types,
                                                Values const& values,
                                                uint32_t maxFlat = MaxFlatParams,
                                                std::optional<uint32_t> resultPointer = {})
{
    auto flat = lowerFlat(context, types, rootsOf(values), maxFlat, resultPointer);
    if (!flat)
        FAIL(flat.error().message());
    REQUIRE(flat.has_value());
    return std::move(*flat);
}

/// Whether @p lhs and @p rhs hold equal roots.
[[nodiscard]] inline bool equalValues(Values const& lhs, Values const& rhs)
{
    if (lhs.rootCount() != rhs.rootCount())
        return false;
    return std::ranges::all_of(std::views::iota(std::size_t { 0 }, lhs.rootCount()),
                               [&](std::size_t index) { return equal(lhs.root(index), rhs.root(index)); });
}

} // namespace isoline::component::canonical::testing
