// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include <isoline/api/Engine.hpp>
#include <isoline/api/Store.hpp>
#include <isoline/api/Types.hpp>
#include <isoline/component/api/CallError.hpp>
#include <isoline/component/api/Component.hpp>
#include <isoline/component/canonical/Trap.hpp>
#include <isoline/component/canonical/Values.hpp>
#include <isoline/component/runtime/ComponentStore.hpp>
#include <isoline/component/runtime/Resources.hpp>

namespace isoline::component::api
{

class ComponentStore;
class Linker;

using canonical::ResourceValue;
using runtime::HostResourceIdentity;
using runtime::IHostResourceDestructor;

/// A resource type the host defines (Linker::defineResource()): a name for messages and what destroys
/// one.
///
/// Its identity is one shared token: copies of one HostResourceType are the same resource type, so two
/// imports a linker satisfies with copies of one (`resource1` and `resource1-again`) pass handles
/// between them. Two types constructed apart are two, whatever their names.
class HostResourceType
{
  public:
    /// A fresh resource type.
    /// @param name Its name, for messages; Linker::defineResource() refuses one that is not UTF-8.
    /// @param destructor What destroys one, once per own dropped or left in a guest table at the store's
    ///                   end; null for a type with nothing to destroy.
    /// @throws std::bad_alloc When the identity cannot be stored.
    HostResourceType(std::string name, std::shared_ptr<IHostResourceDestructor> destructor);

    /// Its name.
    [[nodiscard]] std::string const& name() const noexcept { return _identity->name; }

    /// Its identity token, shared by its copies.
    [[nodiscard]] std::shared_ptr<HostResourceIdentity const> const& identity() const noexcept
    {
        return _identity;
    }

    /// What destroys one; null for nothing.
    [[nodiscard]] std::shared_ptr<IHostResourceDestructor> const& destructor() const noexcept
    {
        return _destructor;
    }

    /// Two types are equal when they share an identity: when one is a copy of the other.
    [[nodiscard]] bool operator==(HostResourceType const& other) const noexcept
    {
        return _identity == other._identity;
    }

  private:
    std::shared_ptr<HostResourceIdentity const> _identity;
    std::shared_ptr<IHostResourceDestructor> _destructor;
};

/// What a host function is given beside its values: the store it runs in.
class HostComponentContext
{
  public:
    /// The context of a call in @p store.
    /// @param store The store; outlives the context.
    explicit HostComponentContext(ComponentStore& store) noexcept: _store { &store } {}

    /// The store the call runs in: a host function may call back into its instances (synchronous
    /// re-entrance), but not instantiate in it, nor move it.
    [[nodiscard]] ComponentStore& store() noexcept { return *_store; }

    /// An own of a host resource, to return to the guest: the guest's table takes it when it is lowered.
    /// @param type The resource's type, one the store's instantiations imported.
    /// @param rep The host's representation of the resource.
    /// @return The handle value; or WrongResource when no instantiation of the store imported @p type.
    /// @throws std::bad_alloc When the trap's message cannot be stored.
    [[nodiscard]] std::expected<ResourceValue, canonical::Trap> newOwn(HostResourceType const& type,
                                                                       uint32_t rep) const;

  private:
    ComponentStore* _store;
};

/// A component function the host implements, bound to a name by Linker::defineFunction().
class IHostComponentFunction
{
  public:
    virtual ~IHostComponentFunction() = default;

    /// Runs the function. @p args are of the importer's view of the function type, which the linker
    /// matched against the type it was defined with; @p results takes values of its result type, which
    /// ValueBuilder::finish() checks after the call (a mismatch is HostFailure{ResultMismatch} and locks
    /// the store down). An own among @p args is the host's now (R12): it drops it with
    /// ComponentStore::drop(), or hands it back. Destroying @p args runs no destructor.
    ///
    /// It must not throw: the function is noexcept, so an exception ends the process, and nothing
    /// unwinds into the guest. A host that fails returns its failure, which ends the call, as a trap
    /// does, and locks the store down.
    /// @param context The store the call runs in.
    /// @param args The arguments, one root per parameter.
    /// @param results Receives the results, one root per result.
    /// @return Nothing, or the host's failure.
    [[nodiscard]] virtual std::expected<void, isoline::api::HostFailure> call(
        HostComponentContext& context,
        canonical::Values const& args,
        canonical::ValueBuilder& results) noexcept = 0;

  protected:
    IHostComponentFunction() = default;
    IHostComponentFunction(IHostComponentFunction const&) = default;
    IHostComponentFunction& operator=(IHostComponentFunction const&) = default;
    IHostComponentFunction(IHostComponentFunction&&) = default;
    IHostComponentFunction& operator=(IHostComponentFunction&&) = default;
};

/// A function an instance exports, as ComponentInstance::function() found it: valid for calls through
/// any instance of the store it came from, while that store lives.
class ComponentFunction
{
  public:
    /// The function's type, in its component's arena: its parameters and result.
    [[nodiscard]] types::FunctionTypeInfo const& type() const noexcept;

    /// Two handles are equal when they name one function of one store.
    [[nodiscard]] bool operator==(ComponentFunction const& other) const noexcept
    {
        return _serial == other._serial && _lifted.index == other._lifted.index;
    }

  private:
    friend class ComponentInstance;

    ComponentFunction(uint64_t serial,
                      runtime::LiftedRef lifted,
                      types::TypeArena const& arena,
                      types::FunctionTypeId type) noexcept:
        _serial { serial }, _lifted { lifted }, _arena { &arena }, _type { type }
    {
    }

    uint64_t _serial;
    runtime::LiftedRef _lifted;
    types::TypeArena const* _arena;
    types::FunctionTypeId _type;
};

struct StoreState;

/// An instance of a component in a ComponentStore: its exports, called through the canonical ABI.
///
/// Cheap to copy; it refers to its store, which must outlive it, and keeps its component alive.
class ComponentInstance
{
  public:
    /// The function exported at @p path: export names joined by `#` (`api#run` for the export `run` of
    /// the instance the root exports as `api`).
    /// @param path The path.
    /// @return The function; none for no such export, an export of another sort, or a function the
    ///         component re-exports from its imports (which the host calls where it defined it).
    [[nodiscard]] std::optional<ComponentFunction> function(std::string_view path) const;

    /// Calls @p function with @p args, which the call lends and the caller keeps. Before anything runs
    /// it refuses a function of another store (ForeignFunction), a locked-down store (LockedDown), and
    /// arguments not of the function's arena, count or types, or handles not of the resource types its
    /// parameters name (ArgumentMismatch); none of these changes the store. Any failure past them locks
    /// the store down (D6). As canonical calls do, the host running out of memory ends the process.
    /// @param function A function of this instance's store.
    /// @param args One root per parameter, built in the function's arena (argumentsFor()).
    /// @return The results, one root per result, in the function's arena; or how the call failed.
    [[nodiscard]] std::expected<canonical::Values, ComponentCallError> call(
        ComponentFunction const& function, canonical::Values const& args) noexcept;

    /// A builder of @p function's arguments: its arena, one root per parameter, charged to the store's
    /// live-bytes budget.
    /// @param function A function.
    /// @return The builder.
    /// @throws std::bad_alloc When the builder cannot be stored.
    [[nodiscard]] canonical::ValueBuilder argumentsFor(ComponentFunction const& function) const;

    /// The component it is an instance of.
    [[nodiscard]] Component const& component() const noexcept { return _component; }

    /// Whether it belongs to @p store.
    [[nodiscard]] bool belongsTo(ComponentStore const& store) const noexcept;

  private:
    friend class ComponentStore;
    friend class Linker;

    ComponentInstance(StoreState& state,
                      uint64_t serial,
                      uint32_t instance,
                      Component const& component) noexcept:
        _state { &state }, _serial { serial }, _instance { instance }, _component { component }
    {
    }

    StoreState* _state;
    uint64_t _serial;
    uint32_t _instance;
    Component _component;
};

/// An import no definition of the linker satisfies.
struct UnsatisfiedImport
{
    std::string name; ///< The import's name.

    /// Two refusals are equal when they name one import.
    bool operator==(UnsatisfiedImport const&) const = default;
};

/// An import whose definition is not of its type.
struct ImportTypeMismatch
{
    std::string name;   ///< The import's name.
    std::string detail; ///< Why not, in wasm-tools' words: its contexts, then the mismatch.

    /// Two refusals are equal when their fields are.
    bool operator==(ImportTypeMismatch const&) const = default;
};

/// An import a linker satisfies with an instance of another store than the one instantiating.
struct ForeignInstance
{
    std::string name; ///< The import's name.

    /// Two refusals are equal when they name one import.
    bool operator==(ForeignInstance const&) const = default;
};

/// Why ComponentStore::instantiate() failed. A component of another engine, an unsatisfied import, a
/// mistyped one and a foreign instance are refused before anything runs; so are the runtime's
/// refusals before its first step (a locked-down store among them). Any later failure, a start
/// function's trap or host failure included, is the runtime's InstantiationFailure and locked the store
/// down.
using InstantiateComponentError = std::variant<isoline::api::EngineMismatch,
                                               UnsatisfiedImport,
                                               ImportTypeMismatch,
                                               ForeignInstance,
                                               runtime::InstantiationFailure>;

/// The store-side state every handle of a ComponentStore reaches: the runtime store, at a fixed address
/// for the store's life however the ComponentStore moves.
struct StoreState
{
    /// @copydoc ComponentStore::ComponentStore
    StoreState(isoline::api::Engine const& engine,
               isoline::api::IMemoryAllocator& allocator,
               ComponentConfig const& config,
               isoline::api::IPollObserver* observer);

    runtime::ComponentStore runtime;     ///< The runtime store.
    ComponentStore* owner = nullptr;     ///< The ComponentStore holding it: re-pointed when it moves.
    uint64_t serial = 0;                 ///< Unique in the process: what tells its handles apart.
    isoline::api::EngineIdentity engine; ///< The engine its components must be of.
    ComponentLimits limits;              ///< Its caps.
};

/// A store of component instances (D2), the embedder's handle on a runtime::ComponentStore.
///
/// Move-only (R11): its instances and functions refer to its state, which stays where it is; moving the
/// store while a call of it runs is a precondition violation (checked). One thread at a time; its
/// interrupter may be raised from any.
class ComponentStore
{
  public:
    /// An empty, live store.
    /// @param engine The engine whose components it instantiates; outlives the store.
    /// @param allocator Allocates every linear memory; outlives the store.
    /// @param config Its caps (ComponentConfig::limits); the features are the components'.
    /// @param observer Observes every poll; none when null; outlives the store.
    /// @throws std::bad_alloc When the host has no memory for the empty store.
    ComponentStore(isoline::api::Engine const& engine,
                   isoline::api::IMemoryAllocator& allocator,
                   ComponentConfig const& config,
                   isoline::api::IPollObserver* observer = nullptr);

    ComponentStore(ComponentStore const&) = delete;
    ComponentStore& operator=(ComponentStore const&) = delete;
    /// Takes @p other's state; @p other is left empty, for destruction or assignment only. Precondition
    /// (checked): no call of it runs.
    ComponentStore(ComponentStore&& other) noexcept;
    /// As the move constructor; this store's own state is destroyed first. Precondition (checked): no
    /// call of either runs.
    ComponentStore& operator=(ComponentStore&& other) noexcept;
    ~ComponentStore();

    /// The store's interrupter: interrupt() it from any thread to stop the call running in the store,
    /// which then fails Interrupted and locks the store down.
    [[nodiscard]] std::shared_ptr<isoline::api::Interrupter> const& interrupter() const noexcept;

    /// Instantiates @p component with the definitions of @p linker. Before anything runs it refuses a
    /// component of another engine, an import the linker does not satisfy, one it satisfies with a
    /// definition not of the import's type, and an instance of another store; then the runtime
    /// instantiates (runtime::ComponentStore::instantiate()), refusing a call re-entering the store and a
    /// locked-down store before its first step. Nothing is thrown: the host running out of memory is the
    /// runtime's HostMemoryExhausted.
    /// @param component The component.
    /// @param linker The definitions; the store keeps what it binds of them.
    /// @return The instance; or why not.
    [[nodiscard]] std::expected<ComponentInstance, InstantiateComponentError> instantiate(
        Component const& component, Linker const& linker) noexcept;

    /// Drops an own the host holds (R12): a host type's destructor runs, once for this drop; a guest
    /// type's destructor runs through the canonical ABI, as `resource.drop` would run it. A borrow, a
    /// resource type the store does not have, a guest drop in a locked-down store, and a drop while the
    /// store instantiates are refused, nothing run.
    /// @param value The own.
    /// @return Nothing; or ArgumentMismatch (a borrow, an unknown type), LockedDown, or the guest
    ///         destructor's Trap, Interrupted or HostFailure, which lock the store down.
    [[nodiscard]] std::expected<void, ComponentCallError> drop(ResourceValue value) noexcept;

    /// A handle value of a host resource, for the embedder to pass to a guest: an own the guest's table
    /// takes when it is lowered, or a borrow lent for one call.
    /// @param type The resource's type, one the store's instantiations imported.
    /// @param rep The host's representation of the resource.
    /// @param ownership Own or Borrow.
    /// @return The handle value; or WrongResource when no instantiation of the store imported @p type.
    /// @throws std::bad_alloc When the trap's message cannot be stored.
    [[nodiscard]] std::expected<ResourceValue, canonical::Trap> hostResource(
        HostResourceType const& type, uint32_t rep, canonical::Ownership ownership) const;

    /// Whether a trap locked the store down (D6): no guest code of it runs again.
    [[nodiscard]] bool isLockedDown() const noexcept;

    /// The runtime store, for what the api does not cover.
    [[nodiscard]] runtime::ComponentStore& runtime() noexcept { return _state->runtime; }

  private:
    friend class ComponentInstance;
    friend class HostComponentContext;
    friend class Linker;

    /// The runtime's view of a host function of a linker, bound to @p state: it calls @p function with a
    /// HostComponentContext of the store.
    [[nodiscard]] static std::shared_ptr<runtime::IHostCallee> hostCallee(
        StoreState& state, std::shared_ptr<IHostComponentFunction> function);

    std::unique_ptr<StoreState> _state;
};

} // namespace isoline::component::api
