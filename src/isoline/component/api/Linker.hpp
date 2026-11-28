// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/api/Module.hpp>
#include <isoline/component/api/Component.hpp>
#include <isoline/component/api/ComponentStore.hpp>
#include <isoline/component/runtime/ComponentStore.hpp>
#include <isoline/component/types/TypeArena.hpp>

namespace isoline::component::api
{

/// The separator of a Linker path's segments: `#` is in no extern name, unlike `.` (`[method]r.f`) and
/// `/` and `@` (`ns:p/i@1.0.0`), so `ns:p/i@0.1.2#f` is the function `f` of that interface (R10).
inline constexpr auto PathSeparator = '#';

/// Whether a host function's parameter labels are part of its type.
enum class ParameterNames : uint8_t
{
    /// Its labels name its parameters: an import matches it only under the same names, as a functype's
    /// names are compared (Explainer.md, at the pin, lines 1083-1089), and as two guests link.
    Named,
    /// It names no parameter, as a Wasmtime host function (`func_wrap`) names none: an import's
    /// parameters match whatever they are called, their types and the result still compared. Its labels
    /// only label the definition (they are checked as a Named one's are).
    Unnamed,
};

/// A host function's type as a linker defines it (R10): labelled parameters and an optional result, every
/// type a value type of the linker's arena (Linker::types()), and whether the labels are names to match.
struct FunctionTypeSpec
{
    std::vector<std::pair<std::string, types::TypeId>> params {}; ///< The parameters, labelled, in order.
    std::optional<types::TypeId> result {};                       ///< The result's type, if any.
    ParameterNames names = ParameterNames::Named; ///< Whether an import must match the labels.
};

/// Why a Linker refused a definition.
///
/// The enumerators are the rows of LinkerErrorTable, in the same order.
enum class LinkerErrorCode : uint8_t
{
    InvalidPath,          ///< A segment is empty or not an extern name (detail: the grammar's message).
    Redefinition,         ///< The path is defined already: a definition is never replaced.
    PathConflict,         ///< The path is, or lies inside, a definition that is not the linker's instance.
    NullDefinition,       ///< The function or destructor-bearing definition is null.
    InvalidType,          ///< A type is not one of the linker's arena (detail: which).
    InvalidParameterName, ///< A parameter's label is not kebab-case, or repeats one (detail: the label).
    InvalidResourceName,  ///< A host resource type's name is not UTF-8.
    UntypedComponent,     ///< The component's own type could not be measured within its caps.
    TypeLimit,            ///< The linker's arena refused the type at one of its caps (detail: which).
};

/// The last LinkerErrorCode enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastLinkerErrorCode = LinkerErrorCode::TypeLimit;

/// One row of LinkerErrorTable: a code and its text, a std::format string of the path and the detail.
struct LinkerErrorRow
{
    LinkerErrorCode code;  ///< The code; equal to the row's index.
    std::string_view text; ///< Its text.
};

/// Every linker refusal, in LinkerErrorCode's order.
inline constexpr auto LinkerErrorTable = std::to_array<LinkerErrorRow>({
    { .code = LinkerErrorCode::InvalidPath, .text = "invalid linker path `{0}`: {1}" },
    { .code = LinkerErrorCode::Redefinition, .text = "`{0}` is defined already{1}" },
    { .code = LinkerErrorCode::PathConflict, .text = "`{0}` conflicts with the definition `{1}`" },
    { .code = LinkerErrorCode::NullDefinition, .text = "`{0}` is defined as null{1}" },
    { .code = LinkerErrorCode::InvalidType, .text = "`{0}` names a type not of the linker: {1}" },
    { .code = LinkerErrorCode::InvalidParameterName, .text = "`{0}` has an invalid parameter name `{1}`" },
    { .code = LinkerErrorCode::InvalidResourceName, .text = "`{0}` names a resource type not in UTF-8{1}" },
    { .code = LinkerErrorCode::UntypedComponent, .text = "`{0}` is a component of no measured type{1}" },
    { .code = LinkerErrorCode::TypeLimit, .text = "`{0}` passes the linker's type caps: {1}" },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: LinkerErrorTable)
        {
            if (static_cast<std::size_t>(row.code) != index)
                return false;
            ++index;
        }
        return index == static_cast<std::size_t>(LastLinkerErrorCode) + 1;
    }(),
    "LinkerErrorTable must hold one row per LinkerErrorCode, at the index of its enumerator");

/// A definition a Linker refused; the linker is as it was.
struct LinkerError
{
    LinkerErrorCode code = LinkerErrorCode::InvalidPath; ///< Why.
    std::string path {};                                 ///< The path defined.
    std::string detail {}; ///< The row's second argument: what was wrong, or empty.

    /// The refusal's text: its row's, formatted with the path and the detail.
    /// @return It.
    /// @throws std::bad_alloc When the text cannot be stored.
    [[nodiscard]] std::string message() const;

    /// Two refusals are equal when their fields are.
    bool operator==(LinkerError const&) const = default;
};

/// An import a definition satisfies: the path of the definition chosen (`ns:p/i@0.1.2` for an import of
/// `ns:p/i@0.1.1`, or the interface an `implements` names).
struct Satisfied
{
    std::string matched; ///< The definition's path.

    /// Two outcomes are equal when they match one path.
    bool operator==(Satisfied const&) const = default;
};

/// An import of types only (D8: an instance whose exports are all `eq`-bound types, or a type), which
/// needs no definition and has none.
struct SatisfiedByTypes
{
    /// All such outcomes are equal.
    bool operator==(SatisfiedByTypes const&) const = default;
};

/// An import no definition satisfies.
struct Unsatisfied
{
    /// All such outcomes are equal.
    bool operator==(Unsatisfied const&) const = default;
};

/// An import whose definition is not of its type.
struct TypeMismatch
{
    std::string matched; ///< The definition's path.
    std::string detail;  ///< Why not, in wasm-tools' words: its contexts, then the mismatch.

    /// Two outcomes are equal when their fields are.
    bool operator==(TypeMismatch const&) const = default;
};

/// How a linker resolves one import.
using ImportOutcome = std::variant<Satisfied, SatisfiedByTypes, Unsatisfied, TypeMismatch>;

/// One import of a component, resolved.
struct ImportResolution
{
    std::string name;      ///< The import's name.
    ImportOutcome outcome; ///< How it resolves.

    /// Two resolutions are equal when their fields are.
    bool operator==(ImportResolution const&) const = default;
};

/// What a ComponentStore instantiates a component with (D7, D8): host functions, host resource types,
/// instances of a store, core modules and components, each under a path of extern names joined by
/// PathSeparator. A path's leading segments name the linker's instances, made by defining what lies
/// inside them: `ns:p/i@0.1.2#f` defines the function `f` of the instance `ns:p/i@0.1.2`.
///
/// An import matches the definition of its exact name; else, for an interface name, the highest
/// semver-compatible version (validate::isSemverCompatible()); else, for an instance import that
/// `implements` an interface, that interface's definition. Types match by subtyping (TypeRelation), host
/// resources by identity (HostResourceType), and an instance import of types only needs no definition.
///
/// Built by its define calls, then shared: resolve() and every instantiation read it, from any number
/// of threads and stores at once, while nothing defines more. A definition never replaces another.
/// Every misuse is a LinkerError, the linker unchanged.
class Linker
{
  public:
    /// An empty linker.
    /// @param config The features its names are parsed with, and the caps of its arena and its type
    ///               comparisons.
    /// @throws std::bad_alloc When its arena cannot be stored.
    explicit Linker(ComponentConfig config);

    Linker(Linker const&) = delete;
    Linker& operator=(Linker const&) = delete;
    /// Moves the definitions; @p other is left for destruction or assignment only.
    Linker(Linker&&) noexcept;
    /// Moves the definitions; @p other is left for destruction or assignment only.
    Linker& operator=(Linker&&) noexcept;
    ~Linker();

    /// The arena a FunctionTypeSpec's types are interned in.
    [[nodiscard]] types::TypeArena& types() noexcept { return _arena; }
    /// The arena a FunctionTypeSpec's types are interned in.
    [[nodiscard]] types::TypeArena const& types() const noexcept { return _arena; }

    /// The type `own<type>` in the linker's arena.
    /// @param type A host resource type; the linker knows it by its identity from now on.
    /// @return Its own type; or TypeLimit.
    /// @throws std::bad_alloc When the type cannot be stored.
    [[nodiscard]] std::expected<types::TypeId, LinkerError> ownType(HostResourceType const& type);

    /// The type `borrow<type>` in the linker's arena.
    /// @param type A host resource type.
    /// @return Its borrow type; or TypeLimit.
    /// @throws std::bad_alloc When the type cannot be stored.
    [[nodiscard]] std::expected<types::TypeId, LinkerError> borrowType(HostResourceType const& type);

    /// Defines a host function at @p path.
    /// @param path Its path.
    /// @param type Its type, of this linker's arena (checked), its labels kebab-case and distinct.
    /// @param function Its body; not null (checked).
    /// @return Nothing, or the refusal.
    /// @throws std::bad_alloc When the definition cannot be stored.
    [[nodiscard]] std::expected<void, LinkerError> defineFunction(
        std::string path, FunctionTypeSpec const& type, std::shared_ptr<IHostComponentFunction> function);

    /// Defines a host resource type at @p path.
    /// @param path Its path.
    /// @param type The type; copies of one share its identity, so two paths defined with one type are one
    ///             resource type.
    /// @return Nothing, or the refusal.
    /// @throws std::bad_alloc When the definition cannot be stored.
    [[nodiscard]] std::expected<void, LinkerError> defineResource(std::string path, HostResourceType type);

    /// Defines an instance of the linker at @p path that holds no definition yet: what an import of an
    /// instance resolves to however few of its exports the linker defines beneath it (none is needed for
    /// an instance of types only). As Wasmtime's Linker::instance(), which its script runner calls for
    /// every named component, its exports or none.
    /// @param path Its path.
    /// @return Nothing, or the refusal; an instance already there is no refusal.
    /// @throws std::bad_alloc When the definition cannot be stored.
    [[nodiscard]] std::expected<void, LinkerError> defineEmptyInstance(std::string const& path);

    /// Defines an instance of a store at @p path: only that store instantiates with it (ForeignInstance
    /// elsewhere), and it must outlive the linker's use there.
    /// @param path Its path.
    /// @param instance The instance; its exports are what the import reads.
    /// @return Nothing, or the refusal.
    /// @throws std::bad_alloc When the definition cannot be stored.
    [[nodiscard]] std::expected<void, LinkerError> defineInstance(std::string path,
                                                                  ComponentInstance const& instance);

    /// Defines a core module at @p path (spectest's `simple-module`). Its type, interned into the linker's
    /// arena here, must be a subtype of an import's to satisfy it (as Wasmtime checks one at link time);
    /// its engine is checked before anything runs.
    /// @param path Its path.
    /// @param module The module.
    /// @return Nothing, or the refusal.
    /// @throws std::bad_alloc When the definition cannot be stored.
    [[nodiscard]] std::expected<void, LinkerError> defineModule(std::string path,
                                                                isoline::api::Module const& module);

    /// Defines a component at @p path, for a component-typed import.
    /// @param path Its path.
    /// @param component The component; UntypedComponent when validation could not measure its type.
    /// @return Nothing, or the refusal.
    /// @throws std::bad_alloc When the definition cannot be stored.
    [[nodiscard]] std::expected<void, LinkerError> defineComponent(std::string path,
                                                                   Component const& component);

    /// How the linker resolves each import of @p component's root, in order. No store is involved, so
    /// an instance of any store satisfies an import here.
    /// @param component The component.
    /// @return One resolution per import.
    /// @throws std::bad_alloc When the resolutions cannot be stored.
    [[nodiscard]] std::vector<ImportResolution> resolve(Component const& component) const;

  private:
    friend class ComponentStore;

    struct FunctionDefinition
    {
        types::FunctionTypeId type;
        std::shared_ptr<IHostComponentFunction> function;
        ParameterNames names = ParameterNames::Named;
    };
    struct ResourceDefinition
    {
        HostResourceType type;
        types::ResourceTypeId resource;
    };
    struct InstanceDefinition
    {
        ComponentInstance instance;
    };
    struct ModuleDefinition
    {
        isoline::api::Module module;
        types::CoreModuleTypeId type; ///< Its type, in the linker's arena.
    };
    struct ComponentDefinition
    {
        Component component;
    };
    using Definition = std::variant<FunctionDefinition,
                                    ResourceDefinition,
                                    InstanceDefinition,
                                    ModuleDefinition,
                                    ComponentDefinition>;

    /// What one resolution of a component's imports came to: the outcome per import, and, for a store,
    /// the runtime's imports and the first refusal.
    struct Resolution
    {
        std::vector<ImportResolution> outcomes {};           ///< Per import, in order.
        runtime::ResolvedImports imports {};                 ///< For a store: the runtime's imports.
        std::optional<InstantiateComponentError> refusal {}; ///< For a store: the first refusal.
    };
    class Walk;

    [[nodiscard]] Resolution resolveFor(Component const& component, StoreState* store) const;
    [[nodiscard]] static uint64_t serialOf(ComponentInstance const& instance) noexcept
    {
        return instance._serial;
    }
    [[nodiscard]] static uint32_t numberOf(ComponentInstance const& instance) noexcept
    {
        return instance._instance;
    }
    [[nodiscard]] static std::shared_ptr<runtime::IHostCallee> hostCallee(
        StoreState& store, std::shared_ptr<IHostComponentFunction> const& function);
    [[nodiscard]] std::expected<std::vector<std::string>, LinkerError> checkPath(
        std::string const& path) const;
    [[nodiscard]] std::expected<void, LinkerError> define(std::string path, Definition definition);
    [[nodiscard]] std::expected<types::ResourceTypeId, LinkerError> resourceOf(HostResourceType const& type,
                                                                               std::string const& path);
    [[nodiscard]] std::expected<types::TypeId, LinkerError> handleType(HostResourceType const& type,
                                                                       types::ValueKind kind);
    [[nodiscard]] Definition const* find(std::string_view path) const;
    [[nodiscard]] std::optional<std::string> lookup(std::string const& parent, std::string_view name) const;

    ComponentConfig _config;
    types::TypeArena _arena;
    std::map<std::string, Definition, std::less<>> _definitions {};
    /// Every instance the definitions make, by path: the proper prefixes of each definition's path.
    std::set<std::string, std::less<>> _instances {};
    /// The segments under each instance path ("" for the root), for the semver search.
    std::map<std::string, std::set<std::string, std::less<>>, std::less<>> _children {};
    std::map<HostResourceIdentity const*, types::ResourceTypeId> _resources {}; ///< By identity.
    std::vector<std::shared_ptr<HostResourceIdentity const>> _identities {};    ///< Keeps them alive.
};

} // namespace isoline::component::api
