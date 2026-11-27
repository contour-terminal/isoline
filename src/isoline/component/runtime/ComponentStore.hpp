// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/api/EngineIdentity.hpp>
#include <isoline/api/Extern.hpp>
#include <isoline/api/Module.hpp>
#include <isoline/api/Store.hpp>
#include <isoline/api/Types.hpp>
#include <isoline/component/Limits.hpp>
#include <isoline/component/binary/Component.hpp>
#include <isoline/component/canonical/Layout.hpp>
#include <isoline/component/canonical/Trap.hpp>
#include <isoline/component/canonical/Values.hpp>
#include <isoline/component/runtime/Builtins.hpp>
#include <isoline/component/runtime/ComponentPlan.hpp>
#include <isoline/component/runtime/Resources.hpp>
#include <isoline/component/types/TypeArena.hpp>
#include <isoline/component/validate/ValidatedComponent.hpp>

namespace isoline::component::runtime
{

class ComponentStore;

/// Whether a ComponentStore may still run guest code (D6, Component Invariant #1).
enum class StoreState : uint8_t
{
    Live,       ///< It may.
    LockedDown, ///< A trap ended a call or an instantiation: no guest code of it runs again.
};

/// How a component call failed: a trap of the canonical ABI or of a core call, or a host function's
/// failure (R13), which the api layer reports as such rather than as a trap. Either locks the store down.
using CallError = std::variant<canonical::Trap, isoline::api::HostFailure>;

/// A component function the host implements, as a ComponentStore calls it: the runtime half of the api
/// layer's host functions (R4), which its Linker adapts onto this.
class IHostCallee
{
  public:
    virtual ~IHostCallee() = default;

    /// Runs the function. @p args are lifted from the caller in the types of the caller's `canon lower`,
    /// and @p results takes values of that function's result types; ValueBuilder::finish() checks them
    /// (a mismatch is HostFailure{ResultMismatch}). The host may call back into the store (canonLift()):
    /// synchronous re-entrance is allowed.
    /// @param store The store the call runs in.
    /// @param args The arguments, one root per parameter.
    /// @param results Receives the results, one root per result.
    /// @return Nothing, or the host's failure, which ends the call and locks the store down.
    [[nodiscard]] virtual std::expected<void, isoline::api::HostFailure> call(
        ComponentStore& store, canonical::Values const& args, canonical::ValueBuilder& results) noexcept = 0;

  protected:
    IHostCallee() = default;
    IHostCallee(IHostCallee const&) = default;
    IHostCallee& operator=(IHostCallee const&) = default;
    IHostCallee(IHostCallee&&) = default;
    IHostCallee& operator=(IHostCallee&&) = default;
};

/// The canonical options of one `canon lift` or `canon lower`, resolved to the core entities of the
/// instance that defines it.
struct ResolvedOptions
{
    std::optional<isoline::api::Memory> memory {};                        ///< `memory`, if given.
    std::optional<isoline::api::Function> realloc {};                     ///< `realloc`, if given.
    std::optional<isoline::api::Function> postReturn {};                  ///< `post-return`, if given.
    canonical::StringEncoding encoding = canonical::StringEncoding::Utf8; ///< `string-encoding`.
};

/// What one side of a canonical call is typed in and bound by: the component's arena and layouts, and
/// the instance whose resources its types name.
struct InstanceSide
{
    uint32_t instance = 0;                           ///< The component instance.
    types::TypeArena const* arena = nullptr;         ///< Its component's types; never null.
    canonical::LayoutTable const* layouts = nullptr; ///< Their layouts; never null.
    ResourceBindings const* bindings = nullptr;      ///< Its resource bindings; never null.
};

/// A component function made by `canon lift`: a core function of an instance, called through the
/// canonical ABI.
struct LiftedFunction
{
    InstanceSide side;             ///< The instance it belongs to, and its types.
    isoline::api::Function core;   ///< The core function lifted.
    types::FunctionTypeId type {}; ///< Its component type, in the side's arena.
    ResolvedOptions options {};    ///< Its canonical options.
};

/// A lifted function of a store, by its number there: stable for the store's life.
struct LiftedRef
{
    uint32_t index = 0; ///< Its number.
};

/// A component function: one an instance lifts, or one the host implements.
using ComponentFunction = std::variant<LiftedRef, std::shared_ptr<IHostCallee>>;

/// A core instance of a store, by its number there: an instantiated module's, or one of inline
/// exports.
struct CoreInstanceRef
{
    uint32_t index = 0; ///< Its number.
};

/// A component instance's exports, by its number among the store's instance values.
struct InstanceRef
{
    uint32_t index = 0; ///< Its number.
};

/// The code a ComponentStore instantiates: a validated component, its plans and its loaded core modules,
/// and what keeps them alive. The store keeps @ref owner for its own life, as its instances read the
/// rest.
struct ComponentCode
{
    std::shared_ptr<void const> owner;                       ///< Keeps the others alive; never null.
    validate::ValidatedComponent const* validated = nullptr; ///< The component; never null.
    ComponentPlans const* plans = nullptr;                   ///< Its plans; never null.
    std::span<isoline::api::Module const> modules;           ///< Its core modules, by ordinal.
    isoline::api::EngineIdentity engine;                     ///< The engine its modules are for.
};

struct Frame;

/// A component, as a slot of sort component holds it: a plan of some code, and the frame its
/// definition ran in, which its outer aliases read (the lexical chain), whoever instantiates it.
struct ComponentRef
{
    ComponentCode const* code = nullptr; ///< Its code, kept by the store; never null.
    uint32_t plan = 0;                   ///< Its plan.
    Frame const* lexical = nullptr;      ///< The frame that defined it; null for a root.
};

/// What a slot of a plan holds at run time (ComponentPlan.hpp): empty for a static entry (a type
/// that is no resource, a core type, a value).
using SlotValue = std::variant<std::monostate,
                               isoline::api::Function,
                               isoline::api::Table,
                               isoline::api::Memory,
                               isoline::api::Global,
                               isoline::api::Tag,
                               isoline::api::Module,
                               CoreInstanceRef,
                               ComponentFunction,
                               ResourceTypeIndex,
                               ComponentRef,
                               InstanceRef>;

/// One run of a plan: the component instance it makes, its slots, and the frame its component was
/// defined in. The store owns every frame for its life, so a component value's lexical frame never
/// dangles.
struct Frame
{
    uint32_t instance = 0;                                          ///< The component instance it makes.
    ComponentRef component {};                                      ///< The component it runs.
    std::array<std::vector<SlotValue>, binary::SortCount> slots {}; ///< By sort, then slot.
};

/// A named value: an export of an instance value.
struct NamedValue
{
    std::string_view name; ///< Its name: the plan's, or the store's copy of the host's.
    SlotValue value;       ///< Its value.
};

/// A host resource type imported into a store: its identity token and destructor (HostImplementor).
struct HostResourceImport
{
    std::shared_ptr<HostResourceIdentity const> identity;   ///< Its identity; never null.
    std::shared_ptr<IHostResourceDestructor> destructor {}; ///< What destroys one; null for nothing.
};

/// An instance the host supplies: its exports, each an entry of ResolvedImports::values before it.
struct HostInstanceImport
{
    std::vector<std::pair<std::string, uint32_t>> exports {}; ///< Name and value index, each below its own.
};

/// A component the host supplies for a component-typed import.
struct ComponentImport
{
    ComponentCode code; ///< Its code, of the store's engine.
};

/// One value supplied for an import: nothing for an instance of types only (D8's SatisfiedByTypes), a
/// host function, a host resource type, a core module, a component, an instance of host values, or an
/// instance of this store.
using ResolvedValue = std::variant<std::monostate,
                                   std::shared_ptr<IHostCallee>,
                                   HostResourceImport,
                                   isoline::api::Module,
                                   ComponentImport,
                                   HostInstanceImport,
                                   InstanceRef>;

/// What a root component's imports are bound to: a flat table of values, nested instances by index so
/// that nothing recurses, and the value of each import of the root plan, in its order.
struct ResolvedImports
{
    std::vector<ResolvedValue> values {}; ///< Every value; an instance's exports precede it.
    std::vector<uint32_t> imports {};     ///< Per import of the root plan, its value's index.
};

/// Why ComponentStore::instantiate() failed.
enum class InstantiationFailureKind : uint8_t
{
    Reentrant,      ///< Called while a call or instantiation of the store runs (a host function).
    LockedDown,     ///< The store is locked down.
    EngineMismatch, ///< The code is of another engine than the store's: nothing ran.
    /// The imports do not fit: before the first step nothing ran (detail: the import); later, a value an
    /// import supplied lacks what the component reads of it (an export, a resource type).
    ImportMismatch,
    LimitExceeded,       ///< A component cap (detail: which).
    CoreInstantiation,   ///< A core instantiation failed (detail: the core error).
    CoreDefinition,      ///< A lowered function or built-in could not be added (detail: the core error).
    Trap,                ///< A trap: a start function's lowered import, a built-in, a resource type cap.
    HostFailure,         ///< A host function a start function called failed.
    HostMemoryExhausted, ///< The host ran out of memory.
};

/// A failed instantiation. Every failure after the first executed step locked the store down.
struct InstantiationFailure
{
    InstantiationFailureKind kind = InstantiationFailureKind::Reentrant; ///< Why.
    /// The import (ImportMismatch, when known), the limit (LimitExceeded), the core error
    /// (CoreInstantiation, CoreDefinition), the trap (Trap) or the host's failure (HostFailure); else
    /// nothing.
    std::variant<std::monostate,
                 uint32_t,
                 ComponentLimit,
                 isoline::api::InstantiateError,
                 isoline::api::DefineError,
                 canonical::Trap,
                 isoline::api::HostFailure>
        detail {};
};

/// A component store (D2): one api::Store holding every core instance of every component instance,
/// the canonical ABI's per-instance state, the resource types and tasks, the store-wide budgets, and
/// whether it is locked down (D6).
///
/// Neither copied nor moved: its functions and frames point at it. One thread at a time, as its
/// api::Store; its interrupter (coreStore().interrupter()) may be raised from any.
class ComponentStore final: public IBuiltinHost
{
  public:
    /// An empty, live store.
    /// @param engine The engine whose modules it instantiates; its configuration is copied.
    /// @param allocator Allocates every linear memory; outlives the store.
    /// @param limits The component caps it enforces.
    /// @param observer Observes every poll; none when null; outlives the store.
    /// @throws std::bad_alloc When the host has no memory for the empty store.
    ComponentStore(isoline::api::Engine const& engine,
                   isoline::api::IMemoryAllocator& allocator,
                   ComponentLimits const& limits,
                   isoline::api::IPollObserver* observer = nullptr);

    ComponentStore(ComponentStore const&) = delete;
    ComponentStore& operator=(ComponentStore const&) = delete;
    ComponentStore(ComponentStore&&) = delete;
    ComponentStore& operator=(ComponentStore&&) = delete;
    ~ComponentStore() override;

    /// The core store every core instance lives in.
    [[nodiscard]] isoline::api::Store& coreStore() noexcept { return *_store; }

    /// Whether guest code may run.
    [[nodiscard]] StoreState state() const noexcept { return _state; }

    /// Locks the store down for good: later calls fail CannotEnter and instantiations LockedDown, without
    /// entering a guest. Idempotent (Component Invariant #1).
    void lockDown() noexcept { _state = StoreState::LockedDown; }

    /// The carrier of a trap crossing core frames (canonical::PendingTrap).
    [[nodiscard]] canonical::PendingTrap& pendingTrap() noexcept override { return _pendingTrap; }

    /// The store-wide budget of live lifted values (maxLiveValueBytes).
    [[nodiscard]] std::shared_ptr<canonical::LiveBytesBudget> const& liveBytes() const noexcept
    {
        return _liveBytes;
    }

    /// The resource state: handle tables, resource types, tasks.
    [[nodiscard]] ResourceStore& resources() noexcept override { return _resources; }

    /// Instantiates @p code's root plan with @p imports, executing its plans on an explicit stack of
    /// frames (D1). Before any step: refuses a call re-entering the store (Reentrant), a locked-down store,
    /// code of another engine, and imports that do not fit; none of these locks down. Each component
    /// instance, core instance and step is counted before what it counts is created, and a frame's slots
    /// are charged as steps before they are allocated. All core instantiations share one invocation, so
    /// one step budget, their start functions included. Any failure past those refusals locks down, the
    /// root's own slot charge included, as the store has begun converting the imports by then.
    /// @param code The code; kept by the store for its life.
    /// @param imports The root plan's imports.
    /// @return The root component instance's number; or why it failed.
    [[nodiscard]] std::expected<uint32_t, InstantiationFailure> instantiate(
        ComponentCode const& code, ResolvedImports const& imports) noexcept;

    /// The exports of a component instance instantiate() returned.
    /// @param instance Its number (checked).
    /// @return Its exports, ordered by name.
    [[nodiscard]] std::span<NamedValue const> exportsOf(uint32_t instance) const noexcept;

    /// The exports of an instance value.
    /// @param instance It (checked).
    /// @return Its exports, ordered by name.
    [[nodiscard]] std::span<NamedValue const> exportsOf(InstanceRef instance) const noexcept;

    /// The instance value of a component instance instantiate() returned: what another instantiation of
    /// the store imports it as (ResolvedValue's InstanceRef).
    /// @param instance Its number (checked).
    /// @return Its exports' instance value.
    [[nodiscard]] InstanceRef instanceValueOf(uint32_t instance) const noexcept;

    /// The export @p name of @p instance's root, if a function.
    /// @param instance A number instantiate() returned (checked).
    /// @param name The export's name.
    /// @return The function; none for no such export or another sort.
    [[nodiscard]] std::optional<ComponentFunction> function(uint32_t instance, std::string_view name) const;

    /// The runtime resource type a host resource type is bound to in the store, as a host function
    /// names it in the handles it returns.
    /// @param identity The host type's identity.
    /// @return Its runtime type; none while no instantiation imported it.
    [[nodiscard]] std::optional<ResourceTypeIndex> hostResourceTypeOf(
        HostResourceIdentity const& identity) const;

    /// A lifted function of the store.
    /// @param function Its reference (checked).
    /// @return It.
    [[nodiscard]] LiftedFunction const& lifted(LiftedRef function) const noexcept;

    /// The component instance's canonical state.
    /// @param instance Its number (checked).
    [[nodiscard]] ComponentInstanceState& instanceState(uint32_t instance) noexcept
    {
        return _resources.instances()[instance];
    }

    /// The calls of the store in progress, nested ones included: what refuses a re-entrant instantiate().
    [[nodiscard]] uint32_t activeCalls() const noexcept { return _activeCalls; }

    /// Counts a call in progress (Calls.cpp), so instantiate() refuses to run inside it.
    void enterCall() noexcept { ++_activeCalls; }

    /// Ends what enterCall() counted.
    void leaveCall() noexcept { --_activeCalls; }

    [[nodiscard]] std::expected<void, canonical::Trap> callGuestDestructor(
        GuestImplementor const& implementor, uint32_t callerInstance, uint32_t rep) override;

  private:
    struct Instantiation;

    /// The exports of a core instance, ordered by name.
    struct CoreInstanceExports
    {
        std::vector<std::pair<std::string_view, isoline::api::Extern>> exports {};
    };

    [[nodiscard]] std::expected<void, InstantiationFailure> checkImports(
        ComponentCode const& code, ResolvedImports const& imports) const;
    [[nodiscard]] std::expected<uint32_t, InstantiationFailure> run(ComponentCode const& code,
                                                                    ResolvedImports const& imports);
    [[nodiscard]] std::expected<SlotValue, InstantiationFailure> importValue(
        ResolvedValue const& value, std::span<SlotValue const> converted);
    [[nodiscard]] std::expected<ResourceTypeIndex, InstantiationFailure> hostResourceType(
        HostResourceImport const& resource);
    [[nodiscard]] ComponentCode const& keep(ComponentCode const& code);
    [[nodiscard]] InstanceRef addInstance(std::vector<NamedValue> exports);

    friend struct Instantiation;

    ComponentLimits _limits;
    isoline::api::EngineIdentity _identity;
    std::unique_ptr<isoline::api::Store> _store;
    ResourceStore _resources;
    std::shared_ptr<canonical::LiveBytesBudget> _liveBytes;
    canonical::PendingTrap _pendingTrap {};
    StoreState _state = StoreState::Live;
    uint32_t _activeCalls = 0;
    bool _isInstantiating = false; ///< Whether an instantiate() runs: a predicate, read by instantiate().
    uint32_t _coreInstances = 0;   ///< Core instances created, against maxCoreInstances.
    std::deque<ComponentCode> _codes {};       ///< Every code instantiated, kept for the store's life.
    std::deque<Frame> _frames {};              ///< Every plan run.
    std::deque<ResourceBindings> _bindings {}; ///< Per component instance, by its number.
    std::deque<LiftedFunction> _lifted {};     ///< Every lifted function, by LiftedRef.
    std::deque<CoreInstanceExports> _coreInstanceExports {}; ///< Every core instance, by CoreInstanceRef.
    std::deque<std::vector<NamedValue>> _instanceValues {};  ///< Every instance value, by InstanceRef.
    std::vector<InstanceRef> _roots {};                      ///< Per component instance, its exports.
    std::deque<std::string> _hostNames {};                   ///< The host's export names, copied.
    std::map<HostResourceIdentity const*, ResourceTypeIndex> _hostResources {};  ///< By identity.
    std::vector<std::shared_ptr<HostResourceIdentity const>> _hostIdentities {}; ///< Keeps them alive.
};

} // namespace isoline::component::runtime
