// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <compare>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/api/Extern.hpp>
#include <isoline/api/Types.hpp>

namespace isoline::api
{

class Instance;
class Module;
class Store;

/// Whether an import of a module has a definition that fits it (Imports::resolve()).
enum class Linkage : uint8_t
{
    Unsatisfied, ///< Nothing is defined under its name, or what is defined is of another kind or type.
    Satisfied,   ///< A definition of its name, kind and type exists.
};

/// The definitions a module's imports are bound to, by module and field name: host functions, and
/// entities other instances export.
///
/// Imports holds definitions only, never a store address of its own: a host function is added to a
/// store, once per definition, by the store that instantiates with it (Store::instantiate()). Built
/// once, it is immutable and may be shared by any number of stores, on any number of threads at once.
///
/// Imports is a plain value that touches no store: define() and resolve() allocate, and may throw
/// std::bad_alloc while the embedder builds or queries it, leaving it as it was (define() inserts one
/// entry, or none).
class Imports
{
  public:
    /// Defines a host function under @p module and @p name.
    ///
    /// The same implementation may be defined under several names and several types: each definition
    /// is a function of its own type.
    /// @param module The import module name.
    /// @param name The import field name.
    /// @param type Its type (checked, aborting: one Store::define() admits, naming no concrete type index
    ///             and not the bot heap). A host function's type is the same in every store, so it can
    ///             name no type a module defines.
    /// @param function Its implementation (checked: not null).
    /// @return This definition set. A later definition of the same names replaces an earlier one.
    Imports& define(std::string module,
                    std::string name,
                    FuncType type,
                    std::shared_ptr<IHostFunction> function);

    /// Defines what @p instance exports as @p exportName under @p module and @p name: another instance's
    /// function, table, memory, global or tag.
    ///
    /// Only a store holding @p instance may link it (Store::instantiate() refuses it into any other
    /// with ForeignImport). The definition outlives the instance's store safely: it keeps the store's
    /// identity, never the store.
    /// @param module The import module name.
    /// @param name The import field name.
    /// @param instance The instance (checked, aborting: its store is alive).
    /// @param exportName The name of one of its exports (checked: it exists).
    /// @return This definition set. A later definition of the same names replaces an earlier one.
    Imports& define(std::string module,
                    std::string name,
                    Instance const& instance,
                    std::string_view exportName);

    /// Defines an entity of a store under @p module and @p name: a function, table, memory, global or tag
    /// an instance exports, or one the store made itself (Store::define(), defineMemory(), defineTable(),
    /// defineGlobal()).
    ///
    /// Only that store may link it (Store::instantiate() refuses it into any other with ForeignImport).
    /// The definition keeps the store's identity, never the store, and the entity's type as the store
    /// holds it now, which resolve() compares.
    /// @param module The import module name.
    /// @param name The import field name.
    /// @param value The entity (checked, aborting: its store is alive).
    /// @return This definition set. A later definition of the same names replaces an earlier one.
    Imports& define(std::string module, std::string name, Extern const& value);

    /// Whether each of @p module's imports has a fitting definition: of its name and kind, and, where it
    /// can be told without a store, of its type.
    ///
    /// A host function's type is compared exactly. A store's entity is compared with its type as its
    /// store held it at define(): a function or tag exactly, a global by mutability and, when
    /// immutable, by subtyping, as linking does; a table by element type, address type and maximum; a
    /// memory by page size, address type and maximum. When both sides name a concrete type, only a
    /// store can compare them, and instantiation decides. The current size of a table or memory is not
    /// compared: it may grow before instantiation, which checks it.
    /// @param module The module.
    /// @return One Linkage per import, in import order.
    [[nodiscard]] std::vector<Linkage> resolve(Module const& module) const;

  private:
    friend class Store;

    /// A host function, with the type it is defined under.
    struct HostDefinition
    {
        FuncType type;                           ///< Its type, closed.
        std::shared_ptr<IHostFunction> function; ///< Its implementation; never null.
    };

    /// The type of a store's entity, as its store held it at define(): FuncType for a function or tag.
    using EntityType = std::variant<FuncType, TableType, MemoryType, GlobalType>;

    /// An entity of a store: a function, table, memory, global or tag an instance exports, or a host
    /// function the store added.
    struct StoreDefinition
    {
        std::shared_ptr<StoreId const> store; ///< The identity of the store that holds it.
        runtime::ExternValue value;           ///< Its kind and address in that store.
        EntityType type;                      ///< Its type, in its store's form.
    };

    /// What an import may be bound to.
    using Definition = std::variant<HostDefinition, StoreDefinition>;

    /// An import's names, (module, field), as a definition is stored under.
    struct ImportName
    {
        std::string module; ///< The import module name.
        std::string name;   ///< The import field name.

        /// Ordered by module name, then field name.
        auto operator<=>(ImportName const&) const = default;
    };

    /// An import's names, viewed: what a lookup compares stored names with, allocating nothing.
    struct ImportNameView
    {
        std::string_view module; ///< The import module name.
        std::string_view name;   ///< The import field name.
    };

    /// Orders stored names against viewed ones, as ImportName orders its own. Its return type is spelled
    /// and its body is in Imports.cpp, the one place that compares the two (find()): a deduced return
    /// type would make every TU that sees this class deduce it, and an importer of module isoline on
    /// MSVC then fails with C3889 unless it sees <string_view> itself.
    /// @param stored The stored names.
    /// @param viewed The names looked up.
    /// @return Their order, by module name, then field name.
    friend std::strong_ordering operator<=>(ImportName const& stored, ImportNameView viewed) noexcept;

    /// The definition under @p module and @p name, found without allocating.
    /// @return It, or null when there is none.
    [[nodiscard]] Definition const* find(std::string_view module, std::string_view name) const noexcept;

    /// Stores @p definition under @p module and @p name, replacing whatever was there.
    void put(std::string module, std::string name, Definition definition);

    std::map<ImportName, Definition, std::less<>> _definitions; ///< By (module, field) name.
};

} // namespace isoline::api
