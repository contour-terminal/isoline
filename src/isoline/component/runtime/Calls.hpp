// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/api/Extern.hpp>
#include <isoline/api/Types.hpp>
#include <isoline/component/canonical/GuestMemory.hpp>
#include <isoline/component/canonical/Trap.hpp>
#include <isoline/component/canonical/Values.hpp>
#include <isoline/component/runtime/ComponentStore.hpp>
#include <isoline/component/types/TypeArena.hpp>

namespace isoline::component::runtime
{

/// A non-owning reference to a callable: what canonLift() calls back, for the one call it runs. It binds
/// only to an lvalue, so it never outlives a temporary.
template <typename Signature>
class CallbackRef;

/// A non-owning reference to a callable returning @p R from @p Args.
template <typename R, typename... Args>
class CallbackRef<R(Args...)>
{
  public:
    /// Refers to @p callable, which must outlive the reference.
    /// @param callable The callable.
    template <typename F>
        requires(!std::is_same_v<std::remove_cvref_t<F>, CallbackRef>
                 && std::is_invocable_r_v<R, F&, Args...>)
    CallbackRef(F& callable) noexcept:
        _object { std::addressof(callable) }, _call { [](void* object, Args... args) -> R {
            return std::invoke(*static_cast<F*>(object), std::forward<Args>(args)...);
        } }
    {
    }

    /// Calls the callable.
    R operator()(Args... args) const { return _call(_object, std::forward<Args>(args)...); }

  private:
    void* _object;
    R (*_call)(void*, Args...);
};

/// The arguments of a component call: lifted for it (owned, destroyed once lowered into the callee), or
/// lent by the embedder, which keeps them (R16).
using CallArguments = std::variant<canonical::Values, std::reference_wrapper<canonical::Values const>>;

/// definitions.py's on_start: supplies the arguments, one root per parameter of the callee's type.
using OnStart = CallbackRef<std::expected<CallArguments, CallError>()>;

/// definitions.py's on_resolve: takes the results, one root per result of the callee's type, lifted in
/// the callee's arena; it runs before `post-return`.
using OnResolve = CallbackRef<std::expected<void, CallError>(canonical::Values)>;

/// Runs once the arguments are lowered into the callee, before it runs: from then on its table holds
/// the owns among them. A lowering that fails took none (ResourceOperations::rollBackLoweredOwns()).
using OnLowered = CallbackRef<void()>;

/// Every own among @p values, in pre-order. A list of scalars holds none, so it is skipped whole.
/// @param values Lifted values.
/// @return The owns.
/// @throws std::bad_alloc When they cannot be stored.
[[nodiscard]] std::vector<canonical::ResourceValue> ownsOf(canonical::Values const& values);

/// Calls a lifted function through the canonical ABI (definitions.py's canon_lift, synchronous).
///
/// It refuses a locked-down store first (CannotEnter, no guest code runs). In one InvocationScope, so
/// that argument `realloc`s, the callee and `post-return` share one step budget and one GrowthRecord, it
/// starts a Task, asks @p onStart for the arguments, lowers them into the callee (its `realloc` with
/// `may_leave` cleared), destroys owned arguments, calls the core function, lifts the results, ends the
/// task (BorrowOutstanding while a borrow lent to it is held), hands the results to @p onResolve, then
/// runs `post-return` with `may_leave` cleared. After the callee, every `realloc` and the `post-return`
/// it re-checks the store: locked down (a host swallowed a nested trap) is CannotEnter, even after
/// success. Every failure locks the store down. The host running out of memory (std::bad_alloc from a
/// lift, a lower or a ValueBuilder) terminates the process, as a core call's does (api::Store).
/// @param store The store.
/// @param function The function, of @p store.
/// @param onStart Supplies the arguments.
/// @param onResolve Takes the results.
/// @return Nothing, or the trap or host failure the call ended in.
[[nodiscard]] std::expected<void, CallError> canonLift(ComponentStore& store,
                                                       LiftedFunction const& function,
                                                       OnStart onStart,
                                                       OnResolve onResolve) noexcept;

/// canonLift(), telling @p onLowered when the arguments are lowered into the callee.
/// @param store The store.
/// @param function The function, of @p store.
/// @param onStart Supplies the arguments.
/// @param onLowered Runs once they are lowered, before the callee; not when lowering fails.
/// @param onResolve Takes the results.
/// @return As canonLift().
[[nodiscard]] std::expected<void, CallError> canonLift(ComponentStore& store,
                                                       LiftedFunction const& function,
                                                       OnStart onStart,
                                                       OnLowered onLowered,
                                                       OnResolve onResolve) noexcept;

/// Calls any component function: canonLift() for a lifted one; for a host one, its IHostCallee with the
/// arguments as @p onStart supplies them and results built in @p resultArena's types.
/// @param store The store.
/// @param function The function.
/// @param resultSide The types the results are built in, for a host function: the caller's.
/// @param resultType The caller's view of the function's type, for a host function.
/// @param onStart Supplies the arguments.
/// @param onResolve Takes the results.
/// @return Nothing, or how the call failed; the store is locked down then. As canonLift(), the host
///         running out of memory terminates the process.
[[nodiscard]] std::expected<void, CallError> callComponentFunction(ComponentStore& store,
                                                                   ComponentFunction const& function,
                                                                   InstanceSide const& resultSide,
                                                                   types::FunctionTypeId resultType,
                                                                   OnStart onStart,
                                                                   OnResolve onResolve) noexcept;

/// An instance's linear memory and `realloc` reached through its store (A2, R4): every access copies,
/// bounds-checked against the memory's size at that moment, and `realloc` runs with the instance's
/// `may_leave` cleared, the store re-checked after it.
class GuestMemory final: public canonical::IGuestMemoryAccess
{
  public:
    /// @param store The store holding the memory; outlives this.
    /// @param memory The memory.
    /// @param realloc The instance's `realloc`, if its options give one.
    /// @param instance The component instance whose `may_leave` the `realloc` clears.
    GuestMemory(ComponentStore& store,
                isoline::api::Memory memory,
                std::optional<isoline::api::Function> realloc,
                uint32_t instance) noexcept:
        _store { &store },
        _memory { std::move(memory) },
        _realloc { std::move(realloc) },
        _instance { instance }
    {
    }

    [[nodiscard]] std::expected<void, canonical::Trap> read(uint32_t address,
                                                            std::span<std::byte> out) override;
    [[nodiscard]] std::expected<void, canonical::Trap> write(uint32_t address,
                                                             std::span<std::byte const> bytes) override;
    /// @return As IGuestMemoryAccess::reallocate(); CannotLeave from its own check is the callee's
    ///         (a lowered import it calls), and CannotEnter when the store locked down meanwhile.
    ///         Precondition (checked): the options gave a `realloc`, which validation requires wherever
    ///         one is called.
    [[nodiscard]] std::expected<uint32_t, canonical::Trap> reallocate(uint32_t old,
                                                                      uint32_t oldSize,
                                                                      uint32_t alignment,
                                                                      uint32_t newSize) override;
    [[nodiscard]] std::expected<void, canonical::Trap> poll() override;
    [[nodiscard]] uint64_t size() override;

  private:
    ComponentStore* _store;
    isoline::api::Memory _memory;
    std::optional<isoline::api::Function> _realloc;
    uint32_t _instance;
};

/// The core function `canon lower` makes of a component function (definitions.py's canon_lower,
/// synchronous): a host function of the core store. It traps CannotEnter in a locked-down store and
/// CannotLeave while its caller's `may_leave` is cleared; it starts a Subtask, lifts its arguments from
/// the caller, calls the callee (canonLift(), or the host's IHostCallee), lowers the results into the
/// caller, and ends the subtask's lends. A trap crosses the core frames as canonical::carrierTrap(),
/// stored in the store's PendingTrap, and locks the store down; a host failure ends the core call as
/// itself. It runs with no task in progress too: during instantiation, from a core start function. Its
/// call() is noexcept, as IHostFunction's: the host running out of memory in it terminates the process.
class LoweredFunction final: public isoline::api::IHostFunction
{
  public:
    /// @param store The store; outlives the function.
    /// @param caller The instance whose core code calls it, its types and bindings.
    /// @param type The lowered function's type, in the caller's arena.
    /// @param options The caller's canonical options.
    /// @param callee The component function called.
    LoweredFunction(ComponentStore& store,
                    InstanceSide caller,
                    types::FunctionTypeId type,
                    ResolvedOptions options,
                    ComponentFunction callee) noexcept:
        _store { &store },
        _caller { caller },
        _type { type },
        _options { std::move(options) },
        _callee { std::move(callee) }
    {
    }

    [[nodiscard]] std::expected<void, isoline::api::Abrupt> call(
        isoline::api::HostContext& context,
        std::span<isoline::api::Value const> args,
        std::span<isoline::api::Value> results) noexcept override;

  private:
    [[nodiscard]] std::expected<void, CallError> run(std::span<isoline::api::Value const> args,
                                                     std::span<isoline::api::Value> results);

    ComponentStore* _store;
    InstanceSide _caller;
    types::FunctionTypeId _type;
    ResolvedOptions _options;
    ComponentFunction _callee;
};

/// The core function type a lowered function of @p type has (canonical::flattenFunction, Lower).
/// @param side The caller's types.
/// @param type The function's type there.
/// @return Its core type.
/// @throws std::bad_alloc When it cannot be stored.
[[nodiscard]] isoline::api::FuncType loweredCoreType(InstanceSide const& side, types::FunctionTypeId type);

/// The component trap a failed core call is (canon lift's step 4): a carrier HostTrap is the trap waiting
/// in @p pending; an uncaught exception is UncaughtException; any other trap fromCore(); a host failure
/// stays one.
/// @param error The core call's error.
/// @param pending The store's carrier.
/// @return The call's error.
/// @throws std::bad_alloc When a trap's message cannot be stored.
[[nodiscard]] CallError fromCallError(isoline::api::CallError const& error, canonical::PendingTrap& pending);

/// The trap a call error is where only a trap can be reported (a `realloc`, a destructor): a host
/// failure becomes the core HostTrap's trap.
/// @param error The error.
/// @return The trap.
/// @throws std::bad_alloc When its message cannot be stored.
[[nodiscard]] canonical::Trap asTrap(CallError error);

} // namespace isoline::component::runtime
