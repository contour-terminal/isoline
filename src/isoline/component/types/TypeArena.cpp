// SPDX-License-Identifier: Apache-2.0
#include <isoline/component/types/TypeArena.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <atomic>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <ranges>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace isoline::component::types
{

namespace
{
    /// The source of TypeArenaIds. An identity source, not configuration: arenas only ever take a
    /// fresh value from it.
    [[nodiscard]] uint64_t freshArenaId() noexcept
    {
        static auto next = std::atomic<uint64_t> { 1 };
        return next.fetch_add(1, std::memory_order_relaxed);
    }

    /// The order of two value types by the fields that decide their identity; the computed fields
    /// follow from these.
    [[nodiscard]] std::strong_ordering compareStructure(DefinedType const& lhs, DefinedType const& rhs)
    {
        return std::tie(lhs.kind, lhs.length, lhs.resource, lhs.children, lhs.labels)
               <=> std::tie(rhs.kind, rhs.length, rhs.resource, rhs.children, rhs.labels);
    }

    /// The handles a type of @p kind holds itself, before its children's.
    [[nodiscard]] constexpr HandleContent ownHandles(ValueKind kind) noexcept
    {
        switch (kind)
        {
            case ValueKind::Own: return HandleContent::Own;
            case ValueKind::Borrow: return HandleContent::Borrow;
            default: return HandleContent::None;
        }
    }

    /// The labels "0", "1", … of a tuple of @p count elements, as its despecialized record names them.
    [[nodiscard]] std::vector<std::string> positionalLabels(std::size_t count)
    {
        auto labels = std::vector<std::string> {};
        labels.reserve(count);
        for (auto const index: std::views::iota(std::size_t { 0 }, count))
            labels.push_back(std::to_string(index));
        return labels;
    }
} // namespace

TypeArena::TypeArena(ComponentLimits const& limits):
    _id { TypeArenaId { .value = freshArenaId() } },
    _maxTypeDepth { limits.maxTypeDepth },
    _maxTypeSize { limits.maxTypeSize },
    _maxResourceTypes { limits.maxResourceTypes },
    _store { std::make_unique<ValueStore>() },
    _index { StructuralOrder { .store = _store.get() } }
{
    for (auto const& row: ValueKindTable)
    {
        if (!isPrimitive(row.kind))
            break;
        auto primitive = DefinedType { .kind = row.kind };
        _store->types.push_back(std::move(primitive));
        _index.insert(static_cast<uint32_t>(_store->types.size() - 1));
    }
}

std::expected<TypeMeasure, TypeLimitError> TypeArena::combine(TypeMeasure aggregate,
                                                              TypeMeasure child) const noexcept
{
    // wasmparser 0.263.0, src/validator/types.rs:238-247 (TypeInfo::combine) and src/validator.rs:88-96
    // (combine_type_sizes): the depth is checked first, against `depth > MAX_WASM_COMPONENT_TYPE_DEPTH`;
    // then the size, refused unless `sum < MAX_WASM_TYPE_SIZE`.
    auto const depth = std::max<uint64_t>(aggregate.depth, uint64_t { child.depth } + 1);
    if (depth > _maxTypeDepth)
        return std::unexpected { TypeLimitError { .limit = ComponentLimit::TypeDepth,
                                                  .value = _maxTypeDepth } };
    auto const size = uint64_t { aggregate.effectiveSize } + child.effectiveSize;
    if (size >= _maxTypeSize)
        return std::unexpected { TypeLimitError { .limit = ComponentLimit::TypeSize,
                                                  .value = _maxTypeSize } };
    return TypeMeasure { .depth = static_cast<uint32_t>(depth),
                         .effectiveSize = static_cast<uint32_t>(size) };
}

std::expected<TypeMeasure, TypeLimitError> TypeArena::measureChildren(
    std::vector<std::optional<TypeId>> const& children) const
{
    // Every compound starts as a type of its own (TypeInfo::new(): size 1, depth 1), then takes in each
    // child in order. Flags, enums, own and borrow have no children, so they measure 1 whatever their
    // labels (component_types.rs:1616-1634).
    auto measure = TypeMeasure {};
    for (auto const& child: children)
    {
        if (!child)
            continue;
        auto const& type = (*this)[*child];
        auto combined =
            combine(measure, TypeMeasure { .depth = type.depth, .effectiveSize = type.effectiveSize });
        if (!combined)
            return combined;
        measure = *combined;
    }
    return measure;
}

std::expected<TypeId, TypeLimitError> TypeArena::intern(DefinedType type)
{
    Require(!type.resource || type.resource->value < _resources.size());
    Require((type.kind == ValueKind::Own || type.kind == ValueKind::Borrow) == type.resource.has_value());
    auto const measure = measureChildren(type.children);
    if (!measure)
        return std::unexpected { measure.error() };
    type.depth = measure->depth;
    type.effectiveSize = measure->effectiveSize;
    type.handles = ownHandles(type.kind);
    for (auto const& child: type.children)
    {
        if (child)
            type.handles = type.handles | _store->types[child->value].handles;
    }

    // The candidate is stored first so the index compares it like any other; it is dropped again when
    // an equal type is already held.
    auto const index = static_cast<uint32_t>(_store->types.size());
    _store->types.push_back(std::move(type));
    auto const [position, inserted] = _index.insert(index);
    if (!inserted)
    {
        _store->types.pop_back();
        return TypeId { .value = *position };
    }
    return TypeId { .value = index };
}

TypeArena::TypeArena(TypeArena&& other) noexcept = default;
TypeArena& TypeArena::operator=(TypeArena&& other) noexcept = default;
TypeArena::~TypeArena() = default;

bool TypeArena::StructuralOrder::operator()(uint32_t lhs, uint32_t rhs) const
{
    ++store->comparisons;
    return std::is_lt(compareStructure(store->types[lhs], store->types[rhs]));
}

DefinedType const& TypeArena::operator[](TypeId id) const
{
    Require(id.value < _store->types.size());
    return _store->types[id.value];
}

TypeId TypeArena::primitive(ValueKind kind) const noexcept
{
    Require(isPrimitive(kind));
    return TypeId { .value = static_cast<uint32_t>(kind) };
}

std::expected<TypeId, TypeLimitError> TypeArena::despecialize(TypeId id)
{
    auto const& type = (*this)[id];
    auto despecialized = DefinedType {};
    switch (type.kind)
    {
        case ValueKind::Tuple:
            despecialized = DefinedType { .kind = ValueKind::Record,
                                          .children = type.children,
                                          .labels = positionalLabels(type.children.size()) };
            break;
        case ValueKind::Enum:
            despecialized = DefinedType { .kind = ValueKind::Variant,
                                          .children = std::vector<std::optional<TypeId>>(type.labels.size()),
                                          .labels = type.labels };
            break;
        case ValueKind::Option:
            despecialized = DefinedType { .kind = ValueKind::Variant,
                                          .children = { std::nullopt, type.children.front() },
                                          .labels = { "none", "some" } };
            break;
        case ValueKind::Result:
            despecialized = DefinedType { .kind = ValueKind::Variant,
                                          .children = type.children,
                                          .labels = { "ok", "error" } };
            break;
        default: return id;
    }
    return intern(std::move(despecialized));
}

std::expected<FunctionTypeId, TypeLimitError> TypeArena::add(FunctionTypeInfo type)
{
    auto children = std::vector<std::optional<TypeId>> {};
    children.reserve(type.params.size() + 1);
    for (auto const& [label, param]: type.params)
        children.emplace_back(param);
    children.push_back(type.result);
    auto const measure = measureChildren(children);
    if (!measure)
        return std::unexpected { measure.error() };
    type.measure = *measure;
    _functions.push_back(std::move(type));
    return FunctionTypeId { .value = static_cast<uint32_t>(_functions.size() - 1) };
}

namespace
{
    /// Folds the measures of @p externs into @p measure, as a component or instance type takes in
    /// each import and export (validate_extern, wasmparser 0.263.0 src/validator/component.rs:4709).
    [[nodiscard]] std::expected<TypeMeasure, TypeLimitError> combineExterns(
        TypeArena const& arena, TypeMeasure measure, std::vector<NamedExtern> const& externs)
    {
        for (auto const& entry: externs)
        {
            auto combined = arena.combine(measure, arena.measure(entry.type));
            if (!combined)
                return combined;
            measure = *combined;
        }
        return measure;
    }
} // namespace

std::expected<ComponentTypeId, TypeLimitError> TypeArena::add(ComponentTypeInfo type)
{
    auto const measure =
        combineExterns(*this, TypeMeasure {}, type.imports).and_then([&](TypeMeasure imports) {
            return combineExterns(*this, imports, type.exports);
        });
    if (!measure)
        return std::unexpected { measure.error() };
    type.measure = *measure;
    _components.push_back(std::move(type));
    return ComponentTypeId { .value = static_cast<uint32_t>(_components.size() - 1) };
}

std::expected<InstanceTypeId, TypeLimitError> TypeArena::add(InstanceTypeInfo type)
{
    auto const measure = combineExterns(*this, TypeMeasure {}, type.exports);
    if (!measure)
        return std::unexpected { measure.error() };
    type.measure = *measure;
    _instances.push_back(std::move(type));
    return InstanceTypeId { .value = static_cast<uint32_t>(_instances.size() - 1) };
}

std::expected<ResourceTypeId, TypeLimitError> TypeArena::newResource(ResourceTypeInfo definition)
{
    if (_resources.size() >= _maxResourceTypes)
        return std::unexpected { TypeLimitError { .limit = ComponentLimit::ResourceTypes,
                                                  .value = _maxResourceTypes } };
    _resources.push_back(definition);
    return ResourceTypeId { .value = static_cast<uint32_t>(_resources.size() - 1) };
}

FunctionTypeInfo const& TypeArena::operator[](FunctionTypeId id) const
{
    Require(id.value < _functions.size());
    return _functions[id.value];
}

ComponentTypeInfo const& TypeArena::operator[](ComponentTypeId id) const
{
    Require(id.value < _components.size());
    return _components[id.value];
}

InstanceTypeInfo const& TypeArena::operator[](InstanceTypeId id) const
{
    Require(id.value < _instances.size());
    return _instances[id.value];
}

ResourceTypeInfo const& TypeArena::operator[](ResourceTypeId id) const
{
    Require(id.value < _resources.size());
    return _resources[id.value];
}

TypeMeasure TypeArena::measure(AnyTypeId type) const
{
    return std::visit(
        [&](auto const id) -> TypeMeasure {
            using Id = std::remove_cvref_t<decltype(id)>;
            if constexpr (std::is_same_v<Id, TypeId>)
            {
                auto const& value = (*this)[id];
                return TypeMeasure { .depth = value.depth, .effectiveSize = value.effectiveSize };
            }
            else if constexpr (std::is_same_v<Id, ResourceTypeId>)
            {
                std::ignore = (*this)[id];
                return TypeMeasure {};
            }
            else
                return (*this)[id].measure;
        },
        type);
}

TypeMeasure TypeArena::measure(ExternType const& type) const
{
    return std::visit(
        [&](auto const& entry) -> TypeMeasure {
            using Entry = std::remove_cvref_t<decltype(entry)>;
            if constexpr (std::is_same_v<Entry, ValueExtern>)
                return measure(AnyTypeId { entry.type });
            else if constexpr (std::is_same_v<Entry, TypeExtern>)
                return measure(entry.type);
            else
                return measure(AnyTypeId { entry });
        },
        type);
}

} // namespace isoline::component::types
