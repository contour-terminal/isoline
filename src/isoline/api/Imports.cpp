// SPDX-License-Identifier: Apache-2.0
#include <isoline/api/Imports.hpp>

#include <core/Assert.hpp>
#include <core/Overloaded.hpp>

#include <algorithm>
#include <compare>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/api/Module.hpp>
#include <isoline/api/Store.hpp>
#include <isoline/validate/Subtyping.hpp>

namespace isoline::api
{

namespace
{
    /// Whether @p type names a concrete type index, among its params or results.
    [[nodiscard]] bool namesConcreteType(FuncType const& type) noexcept
    {
        return std::ranges::any_of(type.params, binary::isConcreteRef)
               || std::ranges::any_of(type.results, binary::isConcreteRef);
    }

    /// Whether a function or tag of @p actual type, in its store's form, fits an import requiring
    /// @p required, in its module's form: equal types, as linking compares them. A concrete type
    /// index on one side only never matches; on both, only a store compares them, so it fits here.
    [[nodiscard]] bool functionTypeFits(FuncType const& actual, FuncType const& required) noexcept
    {
        auto const actualConcrete = namesConcreteType(actual);
        auto const requiredConcrete = namesConcreteType(required);
        if (actualConcrete && requiredConcrete)
            return true;
        return actualConcrete == requiredConcrete && actual == required;
    }

    /// Whether a global of @p actual type fits an import requiring @p required: the same mutability, and
    /// for a mutable global the same value type, for an immutable one a subtype, as linking decides. On
    /// concrete types on both sides only a store decides, so it fits here.
    [[nodiscard]] bool globalTypeFits(GlobalType const& actual, GlobalType const& required) noexcept
    {
        if (actual.mutability != required.mutability)
            return false;
        if (binary::isConcreteRef(actual.type) && binary::isConcreteRef(required.type))
            return true;
        if (actual.mutability == binary::Mutability::Var)
            return actual.type == required.type;
        return validate::isStoreSubtype(actual.type, required.type);
    }

    /// Whether @p actual limits fit @p required, their current size aside, which may grow before
    /// instantiation: the same address type, and no maximum beyond a required one.
    [[nodiscard]] bool limitsFitButSize(Limits const& actual, Limits const& required) noexcept
    {
        if (actual.addressType != required.addressType)
            return false;
        return !required.max || (actual.max && *actual.max <= *required.max);
    }

    /// Whether a table of @p actual type fits an import requiring @p required, its size aside: its
    /// element type exactly, as linking compares it (when both name a concrete type, only a store
    /// decides, so it fits here), and its limits.
    [[nodiscard]] bool tableTypeFits(TableType const& actual, TableType const& required) noexcept
    {
        auto const bothConcrete =
            binary::isConcreteRef(actual.element) && binary::isConcreteRef(required.element);
        return (bothConcrete || actual.element == required.element)
               && limitsFitButSize(actual.limits, required.limits);
    }

    /// Whether a memory of @p actual type fits an import requiring @p required, its size aside: its page
    /// size exactly, and its limits.
    [[nodiscard]] bool memoryTypeFits(MemoryType const& actual, MemoryType const& required) noexcept
    {
        return actual.pageSizeLog2 == required.pageSizeLog2
               && limitsFitButSize(actual.limits, required.limits);
    }

    /// The kind of entity an import of @p type requires.
    [[nodiscard]] ExternKind kindOf(ExternType const& type) noexcept
    {
        return std::visit(core::Overloaded {
                              [](FuncType const*) { return ExternKind::Func; },
                              [](TableType const*) { return ExternKind::Table; },
                              [](MemoryType const*) { return ExternKind::Memory; },
                              [](GlobalType const*) { return ExternKind::Global; },
                              [](TagType) { return ExternKind::Tag; },
                          },
                          type);
    }
} // namespace

Imports& Imports::define(std::string module,
                         std::string name,
                         FuncType type,
                         std::shared_ptr<IHostFunction> function)
{
    Require(function != nullptr);
    Require(Store::checkHostFunctionType(type).has_value());
    put(std::move(module),
        std::move(name),
        HostDefinition { .type = std::move(type), .function = std::move(function) });
    return *this;
}

Imports& Imports::define(std::string module,
                         std::string name,
                         Instance const& instance,
                         std::string_view exportName)
{
    Require(instance.isAlive());
    auto const& runtime = instance._store->_runtime;
    auto const& exports = runtime.moduleInstance(instance._address).exports;
    auto const found = std::ranges::find(
        exports, exportName, [](auto const& entry) -> std::string_view { return entry.first; });
    Require(found != exports.end());
    auto const value = found->second;
    put(std::move(module),
        std::move(name),
        StoreDefinition {
            .store = instance._store->_id, .value = value, .type = instance._store->entityTypeOf(value) });
    return *this;
}

Imports& Imports::define(std::string module, std::string name, Extern const& value)
{
    auto const& store = Store::ownerOf(value);
    auto const entity = Store::externValueOf(value);
    put(std::move(module),
        std::move(name),
        StoreDefinition { .store = store._id, .value = entity, .type = store.entityTypeOf(entity) });
    return *this;
}

std::vector<Linkage> Imports::resolve(Module const& module) const
{
    auto const fits = [](Definition const& definition, ExternType const& required) {
        return std::visit(core::Overloaded {
                              [&](HostDefinition const& host) {
                                  auto const* const* type = std::get_if<FuncType const*>(&required);
                                  return type != nullptr && functionTypeFits(host.type, **type);
                              },
                              [&](StoreDefinition const& other) {
                                  if (other.value.kind != kindOf(required))
                                      return false;
                                  // The kinds agree, so the snapshot holds the alternative of the required
                                  // type's kind.
                                  return std::visit(
                                      core::Overloaded {
                                          [&](FuncType const* type) {
                                              return functionTypeFits(std::get<FuncType>(other.type), *type);
                                          },
                                          [&](TagType tag) {
                                              return functionTypeFits(std::get<FuncType>(other.type),
                                                                      *tag.type);
                                          },
                                          [&](TableType const* type) {
                                              return tableTypeFits(std::get<TableType>(other.type), *type);
                                          },
                                          [&](MemoryType const* type) {
                                              return memoryTypeFits(std::get<MemoryType>(other.type), *type);
                                          },
                                          [&](GlobalType const* type) {
                                              return globalTypeFits(std::get<GlobalType>(other.type), *type);
                                          },
                                      },
                                      required);
                              },
                          },
                          definition);
    };
    auto linkage = std::vector<Linkage> {};
    linkage.reserve(module.imports().size());
    for (auto const& import: module.imports())
    {
        auto const* definition = find(import.module, import.name);
        linkage.push_back(definition != nullptr && fits(*definition, import.type) ? Linkage::Satisfied
                                                                                  : Linkage::Unsatisfied);
    }
    return linkage;
}

std::strong_ordering operator<=>(Imports::ImportName const& stored, Imports::ImportNameView viewed) noexcept
{
    return std::tie(stored.module, stored.name) <=> std::tuple { viewed.module, viewed.name };
}

Imports::Definition const* Imports::find(std::string_view module, std::string_view name) const noexcept
{
    auto const found = _definitions.find(ImportNameView { .module = module, .name = name });
    return found == _definitions.end() ? nullptr : &found->second;
}

void Imports::put(std::string module, std::string name, Definition definition)
{
    _definitions.insert_or_assign(ImportName { .module = std::move(module), .name = std::move(name) },
                                  std::move(definition));
}

} // namespace isoline::api
