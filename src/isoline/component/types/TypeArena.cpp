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
#include <map>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
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
        return std::tie(lhs.kind,
                        lhs.nominal,
                        lhs.resourceAlias,
                        lhs.length,
                        lhs.resource,
                        lhs.children,
                        lhs.labels)
               <=> std::tie(rhs.kind,
                            rhs.nominal,
                            rhs.resourceAlias,
                            rhs.length,
                            rhs.resource,
                            rhs.children,
                            rhs.labels);
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
    [[nodiscard]] std::vector<SharedName> positionalLabels(std::size_t count)
    {
        auto labels = std::vector<SharedName> {};
        labels.reserve(count);
        for (auto const index: std::views::iota(std::size_t { 0 }, count))
            labels.emplace_back(std::to_string(index));
        return labels;
    }
} // namespace

TypeArena::TypeArena(ComponentLimits const& limits):
    _id { TypeArenaId { .value = freshArenaId() } },
    _maxTypeDepth { limits.maxTypeDepth },
    _maxTypeSize { limits.maxTypeSize },
    _maxResourceTypes { limits.maxResourceTypes },
    _maxSubstitutedTypes { limits.maxSubstitutedTypes },
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
    type.handles = HandleContent::None;
    for (auto const& child: children)
    {
        if (child)
            type.handles = type.handles | (*this)[*child].handles;
    }
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

    /// Whether any of @p externs names a resource.
    [[nodiscard]] ResourceReference referencesOf(TypeArena const& arena,
                                                 std::vector<NamedExtern> const& externs)
    {
        for (auto const& entry: externs)
        {
            auto const named = std::visit(
                [&](auto const& alternative) -> bool {
                    using Alternative = std::remove_cvref_t<decltype(alternative)>;
                    if constexpr (std::is_same_v<Alternative, ValueExtern>)
                        return arena.namesResource(alternative.type);
                    else if constexpr (std::is_same_v<Alternative, TypeExtern>)
                        return arena.namesResource(alternative.type);
                    else if constexpr (std::is_same_v<Alternative, CoreModuleTypeId>)
                        return false; // core types name no component resource
                    else
                        return arena.namesResource(alternative);
                },
                entry.type);
            if (named)
                return ResourceReference::Some;
        }
        return ResourceReference::None;
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
    type.resources = referencesOf(*this, type.imports) == ResourceReference::Some
                             || !type.importedResources.empty() || !type.definedResources.empty()
                         ? ResourceReference::Some
                         : referencesOf(*this, type.exports);
    _components.push_back(std::move(type));
    return ComponentTypeId { .value = static_cast<uint32_t>(_components.size() - 1) };
}

std::expected<InstanceTypeId, TypeLimitError> TypeArena::add(InstanceTypeInfo type)
{
    auto const measure = combineExterns(*this, TypeMeasure {}, type.exports);
    if (!measure)
        return std::unexpected { measure.error() };
    type.measure = *measure;
    type.resources =
        type.definedResources.empty() ? referencesOf(*this, type.exports) : ResourceReference::Some;
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

CoreFunctionTypeId TypeArena::intern(isoline::binary::FuncType type)
{
    auto const next = static_cast<uint32_t>(_coreFunctions.size());
    std::ignore = isoline::binary::mapTypeIndices(type, [next](uint32_t referenced) {
        Require(referenced < next);
        return referenced;
    });
    auto const [entry, inserted] = _coreFunctionIndex.try_emplace(type, next);
    if (inserted)
        _coreFunctions.push_back(std::move(type));
    return CoreFunctionTypeId { .value = entry->second };
}

isoline::binary::FuncType const& TypeArena::operator[](CoreFunctionTypeId id) const
{
    Require(id.value < _coreFunctions.size());
    return _coreFunctions[id.value];
}

namespace
{
    /// The positions of @p entries ordered by @p key, for lookups by binary search.
    template <typename Entry, typename Key>
    [[nodiscard]] std::vector<uint32_t> orderedBy(std::vector<Entry> const& entries, Key const& key)
    {
        auto order = std::vector<uint32_t> {};
        order.reserve(entries.size());
        for (auto const position: std::views::iota(std::size_t { 0 }, entries.size()))
            order.push_back(static_cast<uint32_t>(position));
        std::ranges::stable_sort(
            order, std::less<> {}, [&](uint32_t position) { return key(entries[position]); });
        return order;
    }

    /// The entry of @p entries keyed @p wanted through @p order, or nothing.
    template <typename Entry, typename Key, typename Wanted>
    [[nodiscard]] Entry const* findOrdered(std::vector<Entry> const& entries,
                                           std::vector<uint32_t> const& order,
                                           Key const& key,
                                           Wanted const& wanted)
    {
        auto const found = std::ranges::lower_bound(
            order, wanted, std::less<> {}, [&](uint32_t position) { return key(entries[position]); });
        if (found == order.end() || key(entries[*found]) != wanted)
            return nullptr;
        return &entries[*found];
    }

    [[nodiscard]] std::string_view exportKey(CoreExportType const& entry) noexcept
    {
        return entry.name.str();
    }

    [[nodiscard]] std::pair<std::string_view, std::string_view> importKey(
        CoreImportType const& entry) noexcept
    {
        return { entry.module.str(), entry.name.str() };
    }
} // namespace

CoreModuleTypeId TypeArena::add(CoreModuleTypeInfo type)
{
    type.importsByName = orderedBy(type.imports, importKey);
    type.exportsByName = orderedBy(type.exports, exportKey);
    _coreModules.push_back(std::move(type));
    return CoreModuleTypeId { .value = static_cast<uint32_t>(_coreModules.size() - 1) };
}

CoreInstanceTypeId TypeArena::add(CoreInstanceTypeInfo type)
{
    Require(!type.instantiated || (type.instantiated->value < _coreModules.size() && type.exports.empty()));
    type.exportsByName = orderedBy(type.exports, exportKey);
    _coreInstances.push_back(std::move(type));
    return CoreInstanceTypeId { .value = static_cast<uint32_t>(_coreInstances.size() - 1) };
}

CoreModuleTypeInfo const& TypeArena::operator[](CoreModuleTypeId id) const
{
    Require(id.value < _coreModules.size());
    return _coreModules[id.value];
}

CoreInstanceTypeInfo const& TypeArena::operator[](CoreInstanceTypeId id) const
{
    Require(id.value < _coreInstances.size());
    return _coreInstances[id.value];
}

std::span<CoreExportType const> TypeArena::exportsOf(CoreInstanceTypeId id) const
{
    auto const& instance = (*this)[id];
    if (instance.instantiated)
        return (*this)[*instance.instantiated].exports;
    return instance.exports;
}

CoreExportType const* TypeArena::findExport(CoreInstanceTypeId id, std::string_view name) const
{
    auto const& instance = (*this)[id];
    if (instance.instantiated)
        return findExport(*instance.instantiated, name);
    return findOrdered(instance.exports, instance.exportsByName, exportKey, name);
}

CoreExportType const* TypeArena::findExport(CoreModuleTypeId id, std::string_view name) const
{
    auto const& module = (*this)[id];
    return findOrdered(module.exports, module.exportsByName, exportKey, name);
}

CoreImportType const* TypeArena::findImport(CoreModuleTypeId id,
                                            std::string_view module,
                                            std::string_view name) const
{
    auto const& type = (*this)[id];
    return findOrdered(type.imports, type.importsByName, importKey, std::pair { module, name });
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
            else if constexpr (std::is_same_v<Entry, CoreModuleTypeId>)
                return (*this)[entry].measure;
            else
                return measure(AnyTypeId { entry });
        },
        type);
}

bool TypeArena::namesResource(AnyTypeId type) const
{
    return std::visit(
        [&](auto const id) -> bool {
            using Id = std::remove_cvref_t<decltype(id)>;
            if constexpr (std::is_same_v<Id, TypeId> || std::is_same_v<Id, FunctionTypeId>)
                return (*this)[id].handles != HandleContent::None;
            else if constexpr (std::is_same_v<Id, ResourceTypeId>)
            {
                std::ignore = (*this)[id];
                return true;
            }
            else
                return (*this)[id].resources == ResourceReference::Some;
        },
        type);
}

namespace
{
    /// The types an extern type names, by which it is rewritten: none for a core module, whose types
    /// name no resource; a type extern's referenced entry too, when it is another.
    void namedTypes(ExternType const& type, std::vector<AnyTypeId>& named)
    {
        std::visit(
            [&](auto const& alternative) {
                using Alternative = std::remove_cvref_t<decltype(alternative)>;
                if constexpr (std::is_same_v<Alternative, ValueExtern>)
                    named.emplace_back(alternative.type);
                else if constexpr (std::is_same_v<Alternative, TypeExtern>)
                {
                    named.push_back(alternative.type);
                    if (alternative.referenced)
                        named.push_back(alternative.referenced->type);
                }
                else if constexpr (!std::is_same_v<Alternative, CoreModuleTypeId>)
                    named.emplace_back(alternative);
            },
            type);
    }

    /// The first type an extern type names, by which it is rewritten: none for a core module.
    [[nodiscard]] std::optional<AnyTypeId> namedType(ExternType const& type)
    {
        auto named = std::vector<AnyTypeId> {};
        namedTypes(type, named);
        if (named.empty())
            return std::nullopt;
        return named.front();
    }

    /// @p type with every type it names replaced by its image under @p image.
    template <typename Image>
    [[nodiscard]] ExternType renamed(ExternType const& type, Image const& image)
    {
        return std::visit(
            [&](auto const& alternative) -> ExternType {
                using Alternative = std::remove_cvref_t<decltype(alternative)>;
                if constexpr (std::is_same_v<Alternative, ValueExtern>)
                    return ValueExtern { .type = std::get<TypeId>(image(AnyTypeId { alternative.type })) };
                else if constexpr (std::is_same_v<Alternative, TypeExtern>)
                {
                    auto copy = alternative;
                    copy.type = image(alternative.type);
                    if (copy.referenced)
                        copy.referenced->type = image(copy.referenced->type);
                    return copy;
                }
                else if constexpr (std::is_same_v<Alternative, CoreModuleTypeId>)
                    return alternative;
                else
                    return std::get<Alternative>(image(AnyTypeId { alternative }));
            },
            type);
    }

    /// The types a type names directly, each of which is rewritten before it.
    void childrenOf(TypeArena const& arena, AnyTypeId type, std::vector<AnyTypeId>& children)
    {
        auto const externs = [&](std::vector<NamedExtern> const& list) {
            for (auto const& entry: list)
                namedTypes(entry.type, children);
        };
        std::visit(
            [&](auto const id) {
                using Id = std::remove_cvref_t<decltype(id)>;
                if constexpr (std::is_same_v<Id, TypeId>)
                {
                    for (auto const& child: arena[id].children)
                    {
                        if (child)
                            children.emplace_back(*child);
                    }
                }
                else if constexpr (std::is_same_v<Id, FunctionTypeId>)
                {
                    auto const& function = arena[id];
                    for (auto const& [label, param]: function.params)
                        children.emplace_back(param);
                    if (function.result)
                        children.emplace_back(*function.result);
                }
                else if constexpr (std::is_same_v<Id, InstanceTypeId>)
                    externs(arena[id].exports);
                else if constexpr (std::is_same_v<Id, ComponentTypeId>)
                {
                    externs(arena[id].imports);
                    externs(arena[id].exports);
                }
            },
            type);
    }

    /// The resource paths of @p paths, each resource replaced by its image.
    [[nodiscard]] std::vector<ResourcePath> substitutedPaths(std::vector<ResourcePath> paths,
                                                             ResourceSubstitution const& substitution)
    {
        for (auto& entry: paths)
            entry.resource = substitution.apply(entry.resource);
        return paths;
    }
} // namespace

std::expected<ExternType, TypeLimitError> TypeArena::substitute(ExternType const& type,
                                                                ResourceSubstitution const& substitution)
{
    // A type extern that copied the entry it named names two types: each is rewritten, the copy (a
    // value type of the same children, which the arena shares) and the entry named.
    auto const* bound = std::get_if<TypeExtern>(&type);
    if (bound == nullptr || !bound->referenced || bound->referenced->type == bound->type)
        return substituteNamed(type, substitution);
    auto created = substituteNamed(TypeExtern { .type = bound->type }, substitution);
    if (!created)
        return created;
    auto referenced = substituteNamed(TypeExtern { .type = bound->referenced->type }, substitution);
    if (!referenced)
        return referenced;
    auto result = *bound;
    result.type = std::get<TypeExtern>(*created).type;
    result.referenced->type = std::get<TypeExtern>(*referenced).type;
    return ExternType { result };
}

std::expected<ExternType, TypeLimitError> TypeArena::substituteNamed(ExternType const& type,
                                                                     ResourceSubstitution const& substitution)
{
    // Post-order over an explicit stack: a type is rebuilt once every type it names has its image in
    // `images`. A type naming no resource is its own image and is never visited, nor is anything
    // beneath it; a shared type is visited once.
    if (substitution.size() == 0)
        return type;
    auto images = std::map<AnyTypeId, AnyTypeId> {};
    auto const imageOf = [&](AnyTypeId id) -> std::optional<AnyTypeId> {
        if (auto const* resource = std::get_if<ResourceTypeId>(&id))
            return AnyTypeId { substitution.apply(*resource) };
        if (!namesResource(id))
            return id;
        auto const entry = images.find(id);
        if (entry == images.end())
            return std::nullopt;
        return entry->second;
    };
    auto const imageExtern = [&](ExternType const& entry) {
        return renamed(entry, [&](AnyTypeId id) { return *imageOf(id); });
    };
    auto const imageExterns = [&](std::vector<NamedExtern> list) {
        for (auto& entry: list)
            entry.type = imageExtern(entry.type);
        return list;
    };
    auto const rebuild = [&](AnyTypeId id, std::size_t children) -> std::expected<AnyTypeId, TypeLimitError> {
        if (auto charged = chargeSubstitution(1 + children); !charged)
            return std::unexpected { charged.error() };
        return std::visit(
            [&](auto const typed) -> std::expected<AnyTypeId, TypeLimitError> {
                using Id = std::remove_cvref_t<decltype(typed)>;
                if constexpr (std::is_same_v<Id, TypeId>)
                {
                    auto copy = (*this)[typed];
                    for (auto& child: copy.children)
                    {
                        if (child)
                            child = std::get<TypeId>(*imageOf(*child));
                    }
                    if (copy.resource)
                        copy.resource = substitution.apply(*copy.resource);
                    return intern(std::move(copy)).transform([](TypeId added) {
                        return AnyTypeId { added };
                    });
                }
                else if constexpr (std::is_same_v<Id, FunctionTypeId>)
                {
                    auto copy = (*this)[typed];
                    for (auto& [label, param]: copy.params)
                        param = std::get<TypeId>(*imageOf(param));
                    if (copy.result)
                        copy.result = std::get<TypeId>(*imageOf(*copy.result));
                    return add(std::move(copy)).transform([](FunctionTypeId added) {
                        return AnyTypeId { added };
                    });
                }
                else if constexpr (std::is_same_v<Id, InstanceTypeId>)
                {
                    auto const& original = (*this)[typed];
                    auto copy = InstanceTypeInfo {
                        .exports = imageExterns(original.exports),
                        .definedResources = substitutedPaths(original.definedResources, substitution),
                    };
                    return add(std::move(copy)).transform([](InstanceTypeId added) {
                        return AnyTypeId { added };
                    });
                }
                else if constexpr (std::is_same_v<Id, ComponentTypeId>)
                {
                    auto const& original = (*this)[typed];
                    auto copy = ComponentTypeInfo {
                        .imports = imageExterns(original.imports),
                        .exports = imageExterns(original.exports),
                        .importedResources = substitutedPaths(original.importedResources, substitution),
                        .definedResources = substitutedPaths(original.definedResources, substitution),
                    };
                    return add(std::move(copy)).transform([](ComponentTypeId added) {
                        return AnyTypeId { added };
                    });
                }
                else
                // uncovered-begin(resource-rebuild)
                {
                    Require(false);
                    return typed;
                }
                // uncovered-end(resource-rebuild)
            },
            id);
    };

    // Types form a DAG, so a type not yet rebuilt is never on the path to itself: each is expanded
    // once, and the work is the number of edges among the types naming a resource.
    struct Pending
    {
        AnyTypeId type;
        std::vector<AnyTypeId> children {};
        std::size_t next = 0;
    };
    auto const pending = [&](AnyTypeId id) {
        auto entry = Pending { .type = id };
        childrenOf(*this, id, entry.children);
        return entry;
    };
    auto stack = std::vector<Pending> {};
    if (auto const root = namedType(type); root && !imageOf(*root))
        stack.push_back(pending(*root));
    while (!stack.empty())
    {
        auto& top = stack.back();
        while (top.next < top.children.size() && imageOf(top.children[top.next]))
            ++top.next;
        if (top.next < top.children.size())
        {
            auto const child = top.children[top.next];
            stack.push_back(pending(child)); // invalidates `top`
            continue;
        }
        auto const current = top.type;
        auto const children = top.children.size();
        stack.pop_back();
        auto const image = rebuild(current, children);
        if (!image)
            return std::unexpected { image.error() };
        images.emplace(current, *image);
    }
    return imageExtern(type);
}

std::expected<void, TypeLimitError> TypeArena::chargeSubstitution(std::size_t units)
{
    if (units > _maxSubstitutedTypes - std::min<std::size_t>(_substitutedTypes, _maxSubstitutedTypes))
        return std::unexpected { TypeLimitError { .limit = ComponentLimit::SubstitutedTypes,
                                                  .value = _maxSubstitutedTypes } };
    _substitutedTypes += units;
    return {};
}

std::expected<InstanceTypeId, TypeLimitError> TypeArena::substitutedInstance(
    std::vector<NamedExtern> exports, ResourceSubstitution const& substitution)
{
    if (auto charged = chargeSubstitution(1 + exports.size()); !charged)
        return std::unexpected { charged.error() };
    for (auto& entry: exports)
    {
        auto substituted = substitute(entry.type, substitution);
        if (!substituted)
            return std::unexpected { substituted.error() };
        entry.type = *substituted;
    }
    return add(InstanceTypeInfo { .exports = std::move(exports) });
}

} // namespace isoline::component::types
