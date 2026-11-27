// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/component/binary/Component.hpp>
#include <isoline/component/binary/Encoder.hpp>

namespace isoline::component::binary::testing
{

/// The number of index spaces of a component, one per Sort.
inline constexpr auto SortCount = static_cast<std::size_t>(Sort::Instance) + 1;

/// The index space a definition adds an index to, as Binary.md's index spaces count definitions.
/// @param definition A definition or declaration.
/// @return Its sort, or nothing for a custom section, which adds none.
[[nodiscard]] inline std::optional<Sort> introducedSort(Definition const& definition)
{
    return std::visit(
        [](auto const& alternative) -> std::optional<Sort> {
            using Alternative = std::remove_cvref_t<decltype(alternative)>;
            if constexpr (std::is_same_v<Alternative, CoreModuleDefinition>)
                return Sort::CoreModule;
            else if constexpr (std::is_same_v<Alternative, CoreInstanceDefinition>)
                return Sort::CoreInstance;
            else if constexpr (std::is_same_v<Alternative, CoreTypeDefinition>)
                return Sort::CoreType;
            else if constexpr (std::is_same_v<Alternative, NestedComponent>)
                return Sort::Component;
            else if constexpr (std::is_same_v<Alternative, InstanceDefinition>)
                return Sort::Instance;
            else if constexpr (std::is_same_v<Alternative, AliasDefinition>)
                return alternative.sort;
            else if constexpr (std::is_same_v<Alternative, TypeDefinition>)
                return Sort::Type;
            else if constexpr (std::is_same_v<Alternative, CanonDefinition>)
                // `canon lift` defines a component function; every other built-in a core function.
                return alternative.builtin == CanonBuiltin::Lift ? Sort::Func : Sort::CoreFunc;
            else if constexpr (std::is_same_v<Alternative, ImportDefinition>)
                return alternative.type.sort;
            else if constexpr (std::is_same_v<Alternative, ExportDefinition>)
                return alternative.item.sort;
            else if constexpr (std::is_same_v<Alternative, ExportDeclaration>)
                return alternative.type.sort;
            else
            {
                static_assert(std::is_same_v<Alternative, CustomDefinition>, "every alternative has a case");
                return std::nullopt;
            }
        },
        definition);
}

/// A name without attributes.
/// @param name The name.
/// @return It, as `nameattributes` of the plain form.
[[nodiscard]] inline NameAttributes plain(std::string name)
{
    return NameAttributes { .name = std::move(name), .attributes = {} };
}

/// The scope being built by a ComponentBuilder or a TypeScopeBuilder: its pool entry first, the
/// entries of the scopes adopted into it after, and the bytes their ranges index.
///
/// Every scope it adopts lands after its own entry, as encodeComponent() requires of a pool.
class ScopeAssembly
{
  public:
    /// An assembly of one empty scope.
    /// @param kind What the scope is.
    explicit ScopeAssembly(ScopeKind kind): _components { Component { .kind = kind, .definitions = {} } } {}

    /// Appends a definition to the scope.
    /// @param definition The definition; it names no scope and no byte range (see adopt() and range()).
    /// @return Its index in the index space of its sort.
    uint32_t add(Definition definition)
    {
        auto const sort = introducedSort(definition);
        Require(sort.has_value());
        _components.front().definitions.push_back(std::move(definition));
        return _counts[static_cast<std::size_t>(*sort)]++;
    }

    /// Appends a custom section, which adds no index.
    /// @param name The section's name.
    /// @param payload Its payload.
    void custom(std::string name, std::span<std::byte const> payload)
    {
        auto const [offset, size] = range(payload);
        auto definition = CustomDefinition { .name = std::move(name), .offset = offset, .size = size };
        _components.front().definitions.emplace_back(std::move(definition));
    }

    /// Copies @p bytes to the end of the assembly's bytes.
    /// @param bytes The bytes to keep.
    /// @return Their offset and size in ComponentPool::bytes.
    std::pair<uint64_t, uint64_t> range(std::span<std::byte const> bytes)
    {
        auto const offset = static_cast<uint64_t>(_bytes.size());
        _bytes.insert(_bytes.end(), bytes.begin(), bytes.end());
        return { offset, static_cast<uint64_t>(bytes.size()) };
    }

    /// Takes @p child's scopes and bytes into this assembly, its root becoming a scope of @p kind.
    /// @param child The scope to adopt.
    /// @param kind What its root is in this assembly.
    /// @return The pool index of its root here.
    uint32_t adopt(ScopeAssembly child, ScopeKind kind)
    {
        // Only a component type declares imports (DeclarationTable): the decoder refuses one elsewhere.
        Require(
            kind != ScopeKind::InstanceType
            || std::ranges::none_of(child._components.front().definitions, [](Definition const& definition) {
                   return std::holds_alternative<ImportDefinition>(definition);
               }));
        auto const scopeBase = static_cast<uint32_t>(_components.size());
        auto const byteBase = static_cast<uint64_t>(_bytes.size());
        child._components.front().kind = kind;
        for (auto& scope: child._components)
        {
            for (auto& definition: scope.definitions)
                rebase(definition, scopeBase, byteBase);
            _components.push_back(std::move(scope));
        }
        _bytes.insert(_bytes.end(), child._bytes.begin(), child._bytes.end());
        return scopeBase;
    }

    /// The number of indices the scope's definitions added to @p sort's index space.
    /// @param sort An index space.
    /// @return Its size so far.
    [[nodiscard]] uint32_t count(Sort sort) const { return _counts[static_cast<std::size_t>(sort)]; }

    /// The assembly as a pool, its scope at index 0.
    /// @return The pool.
    [[nodiscard]] ComponentPool pool() const
    {
        return ComponentPool { .bytes = std::make_shared<std::vector<std::byte> const>(_bytes),
                               .components = _components };
    }

  private:
    /// Moves the scope indices and byte ranges of @p definition by the bases of its new pool.
    static void rebase(Definition& definition, uint32_t scopeBase, uint64_t byteBase)
    {
        if (auto* module = std::get_if<CoreModuleDefinition>(&definition))
            module->offset += byteBase;
        else if (auto* custom = std::get_if<CustomDefinition>(&definition))
            custom->offset += byteBase;
        else if (auto* nested = std::get_if<NestedComponent>(&definition))
            nested->index += scopeBase;
        else if (auto* type = std::get_if<TypeDefinition>(&definition))
        {
            if (auto* component = std::get_if<ComponentTypeDefinition>(&type->type))
                component->scope += scopeBase;
            else if (auto* instance = std::get_if<InstanceTypeDefinition>(&type->type))
                instance->scope += scopeBase;
        }
    }

    std::vector<Component> _components;
    std::vector<std::byte> _bytes;
    std::array<uint32_t, SortCount> _counts {};
};

/// Whether @p type opens a scope of declarations, which only the builders' scope methods may add.
[[nodiscard]] inline bool opensScope(TypeDefinition const& type)
{
    return std::holds_alternative<ComponentTypeDefinition>(type.type)
           || std::holds_alternative<InstanceTypeDefinition>(type.type);
}

/// Whether @p definition names a scope or a byte range, which only the builders' own methods may add.
[[nodiscard]] inline bool namesPoolParts(Definition const& definition)
{
    if (auto const* type = std::get_if<TypeDefinition>(&definition))
        return opensScope(*type);
    return std::holds_alternative<CoreModuleDefinition>(definition)
           || std::holds_alternative<NestedComponent>(definition)
           || std::holds_alternative<CustomDefinition>(definition);
}

/// Builds the declarations of a component or an instance type, for ComponentBuilder::componentType()
/// and instanceType(): which of the two it is is decided where it is added.
class TypeScopeBuilder
{
  public:
    TypeScopeBuilder(): _scope { ScopeKind::InstanceType } {}

    /// Declares a core type. @return Its core type index.
    uint32_t coreType(CoreTypeDefinition type) { return _scope.add(std::move(type)); }

    /// Declares a type that opens no scope (see componentType() and instanceType()). @return Its type index.
    uint32_t type(TypeDefinition type)
    {
        Require(!opensScope(type));
        return _scope.add(std::move(type));
    }

    /// Declares a component type of @p declarations. @return Its type index.
    uint32_t componentType(TypeScopeBuilder declarations)
    {
        auto const scope = _scope.adopt(std::move(declarations._scope), ScopeKind::ComponentType);
        return _scope.add(TypeDefinition { .type = ComponentTypeDefinition { .scope = scope } });
    }

    /// Declares an instance type of @p declarations. @return Its type index.
    uint32_t instanceType(TypeScopeBuilder declarations)
    {
        auto const scope = _scope.adopt(std::move(declarations._scope), ScopeKind::InstanceType);
        return _scope.add(TypeDefinition { .type = InstanceTypeDefinition { .scope = scope } });
    }

    /// Declares an alias. @return Its index in the index space of its sort.
    uint32_t alias(AliasDefinition alias) { return _scope.add(std::move(alias)); }

    /// Declares an outer alias of @p sort. @return Its index in the index space of @p sort.
    uint32_t aliasOuter(Sort sort, uint32_t outerCount, uint32_t outerIndex)
    {
        return alias(AliasDefinition { .sort = sort,
                                       .target = AliasTarget::Outer,
                                       .instance = 0,
                                       .name = {},
                                       .outerCount = outerCount,
                                       .outerIndex = outerIndex });
    }

    /// Declares an import (a component type's only). @return Its index in the index space of its sort.
    uint32_t import(std::string name, ExternDesc type)
    {
        return _scope.add(ImportDefinition { .name = plain(std::move(name)), .type = type });
    }

    /// Declares an export. @return Its index in the index space of its sort.
    uint32_t exportItem(std::string name, ExternDesc type)
    {
        return _scope.add(ExportDeclaration { .name = plain(std::move(name)), .type = type });
    }

  private:
    friend class ComponentBuilder;
    ScopeAssembly _scope;
};

/// A fluent builder of components for tests: each method appends one definition and returns its
/// index in the index space of its sort, and build() encodes the component canonically.
class ComponentBuilder
{
  public:
    ComponentBuilder(): _scope { ScopeKind::Component } {}

    /// Embeds a core module. @return Its core module index.
    uint32_t coreModule(std::vector<std::byte> moduleBytes)
    {
        auto const [offset, size] = _scope.range(moduleBytes);
        return _scope.add(CoreModuleDefinition { .offset = offset, .size = size });
    }

    /// Instantiates a core module with core instances as its arguments. @return Its core instance index.
    uint32_t coreInstantiate(uint32_t module, std::vector<std::pair<std::string, uint32_t>> args)
    {
        auto instantiate = CoreInstantiate { .module = module, .arguments = {} };
        for (auto& [name, instance]: args)
            instantiate.arguments.push_back({ .name = std::move(name), .instance = instance });
        return _scope.add(CoreInstanceDefinition { .expression = std::move(instantiate) });
    }

    /// A core instance of inline exports. @return Its core instance index.
    uint32_t coreInlineExports(std::vector<CoreInlineExport> exports)
    {
        return _scope.add(
            CoreInstanceDefinition { .expression = CoreInlineExports { .exports = std::move(exports) } });
    }

    /// Defines a core type. @return Its core type index.
    uint32_t coreType(CoreTypeDefinition type) { return _scope.add(std::move(type)); }

    /// Defines a type that opens no scope (see componentType() and instanceType()). @return Its type index.
    uint32_t type(TypeDefinition type)
    {
        Require(!opensScope(type));
        return _scope.add(std::move(type));
    }

    /// Defines a resource type. @return Its type index.
    uint32_t resource(std::optional<uint32_t> destructor = std::nullopt)
    {
        return type(TypeDefinition {
            .type = ResourceTypeDefinition { .rep = CoreValueKind::I32, .destructor = destructor } });
    }

    /// Defines a component type of @p declarations. @return Its type index.
    uint32_t componentType(TypeScopeBuilder declarations)
    {
        auto const scope = _scope.adopt(std::move(declarations._scope), ScopeKind::ComponentType);
        return _scope.add(TypeDefinition { .type = ComponentTypeDefinition { .scope = scope } });
    }

    /// Defines an instance type of @p declarations. @return Its type index.
    uint32_t instanceType(TypeScopeBuilder declarations)
    {
        auto const scope = _scope.adopt(std::move(declarations._scope), ScopeKind::InstanceType);
        return _scope.add(TypeDefinition { .type = InstanceTypeDefinition { .scope = scope } });
    }

    /// Lifts a core function to a component function of type @p type. @return Its function index.
    uint32_t canonLift(uint32_t coreFunc, std::vector<CanonOption> options, uint32_t type)
    {
        return canon(CanonDefinition { .builtin = CanonBuiltin::Lift,
                                       .immediates = { coreFunc, type },
                                       .options = std::move(options),
                                       .result = std::nullopt });
    }

    /// Lowers a component function to a core function. @return Its core function index.
    uint32_t canonLower(uint32_t func, std::vector<CanonOption> options)
    {
        return canon(CanonDefinition { .builtin = CanonBuiltin::Lower,
                                       .immediates = { func },
                                       .options = std::move(options),
                                       .result = std::nullopt });
    }

    /// Defines any canonical built-in. @return Its function index for lift, else its core function index.
    uint32_t canon(CanonDefinition definition) { return _scope.add(std::move(definition)); }

    /// Aliases a core instance's export. @return Its index in the index space of @p sort.
    uint32_t aliasCoreExport(uint32_t coreInstance, std::string name, CoreSort sort)
    {
        return alias(AliasDefinition { .sort = sortOf(sort),
                                       .target = AliasTarget::CoreExport,
                                       .instance = coreInstance,
                                       .name = std::move(name),
                                       .outerCount = 0,
                                       .outerIndex = 0 });
    }

    /// Aliases a component instance's export. @return Its index in the index space of @p sort.
    uint32_t aliasExport(uint32_t instance, std::string name, Sort sort)
    {
        return alias(AliasDefinition { .sort = sort,
                                       .target = AliasTarget::Export,
                                       .instance = instance,
                                       .name = std::move(name),
                                       .outerCount = 0,
                                       .outerIndex = 0 });
    }

    /// Aliases a definition of an enclosing scope. @return Its index in the index space of @p sort.
    uint32_t aliasOuter(Sort sort, uint32_t outerCount, uint32_t outerIndex)
    {
        return alias(AliasDefinition { .sort = sort,
                                       .target = AliasTarget::Outer,
                                       .instance = 0,
                                       .name = {},
                                       .outerCount = outerCount,
                                       .outerIndex = outerIndex });
    }

    /// Defines any alias. @return Its index in the index space of its sort.
    uint32_t alias(AliasDefinition definition) { return _scope.add(std::move(definition)); }

    /// Imports a definition. @return Its index in the index space of its sort.
    uint32_t import(std::string name, ExternDesc type)
    {
        return _scope.add(ImportDefinition { .name = plain(std::move(name)), .type = type });
    }

    /// Nests a component. @return Its component index.
    uint32_t nested(ComponentBuilder child)
    {
        auto const scope = _scope.adopt(std::move(child._scope), ScopeKind::Component);
        return _scope.add(NestedComponent { .index = scope });
    }

    /// Instantiates a component. @return Its instance index.
    uint32_t instantiate(uint32_t component, std::vector<std::pair<std::string, SortIndex>> args)
    {
        auto instantiation = Instantiate { .component = component, .arguments = {} };
        for (auto& [name, item]: args)
            instantiation.arguments.push_back({ .name = std::move(name), .item = item });
        return _scope.add(InstanceDefinition { .expression = std::move(instantiation) });
    }

    /// An instance of inline exports. @return Its instance index.
    uint32_t inlineInstance(std::vector<std::pair<std::string, SortIndex>> exports)
    {
        auto instance = InlineExports {};
        for (auto& [name, item]: exports)
            instance.exports.push_back({ .name = plain(std::move(name)), .item = item });
        return _scope.add(InstanceDefinition { .expression = std::move(instance) });
    }

    /// Exports a definition, optionally ascribing it a type. Exports add an index, as Binary.md has it.
    /// @return Its index in the index space of @p sort.
    uint32_t exportItem(std::string name, Sort sort, uint32_t index, std::optional<ExternDesc> type = {})
    {
        return _scope.add(ExportDefinition { .name = plain(std::move(name)),
                                             .item = SortIndex { .sort = sort, .index = index },
                                             .type = type });
    }

    /// Appends a custom section.
    void custom(std::string name, std::span<std::byte const> payload)
    {
        _scope.custom(std::move(name), payload);
    }

    /// Appends any definition that names no scope or byte range (see the methods above for those).
    /// @return Its index in the index space of its sort.
    uint32_t definition(Definition definition)
    {
        // An export declaration belongs to a component or instance type, never to a component.
        Require(!namesPoolParts(definition) && !std::holds_alternative<ExportDeclaration>(definition));
        return _scope.add(std::move(definition));
    }

    /// The number of indices the definitions so far added to @p sort's index space.
    [[nodiscard]] uint32_t count(Sort sort) const { return _scope.count(sort); }

    /// The component as a pool, its own scope at index 0.
    [[nodiscard]] ComponentPool pool() const { return _scope.pool(); }

    /// The component's canonical encoding (encodeComponent()).
    [[nodiscard]] std::vector<std::byte> build() const { return encodeComponent(pool()); }

  private:
    ScopeAssembly _scope;
};

} // namespace isoline::component::binary::testing
