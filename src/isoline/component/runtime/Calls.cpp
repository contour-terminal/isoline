// SPDX-License-Identifier: Apache-2.0
#include <isoline/component/runtime/Calls.hpp>

#include <core/Assert.hpp>
#include <core/Utils.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/binary/Types.hpp>
#include <isoline/component/canonical/Layout.hpp>
#include <isoline/component/canonical/LiftLower.hpp>
#include <isoline/component/runtime/Resources.hpp>

namespace isoline::component::runtime
{

using canonical::Trap;
using canonical::TrapCode;
using canonical::trapOf;

namespace
{
    /// The core value type of a flat core value kind, by CoreValueKind's order.
    constexpr auto CoreValueTypes = std::to_array<isoline::binary::ValueType>({
        isoline::binary::I32,
        isoline::binary::I64,
        isoline::binary::F32,
        isoline::binary::F64,
    });
    static_assert(CoreValueTypes.size() == static_cast<std::size_t>(binary::CoreValueKind::F64) + 1);

    /// Counts a call of the store in progress while it lives, so instantiate() refuses to run in it.
    class ActiveCall
    {
      public:
        explicit ActiveCall(ComponentStore& store) noexcept: _store { &store } { store.enterCall(); }
        ActiveCall(ActiveCall const&) = delete;
        ActiveCall& operator=(ActiveCall const&) = delete;
        ActiveCall(ActiveCall&&) = delete;
        ActiveCall& operator=(ActiveCall&&) = delete;
        ~ActiveCall() { _store->leaveCall(); }

      private:
        ComponentStore* _store;
    };

    /// Clears an instance's `may_leave` while it lives (definitions.py's realloc and post-return), and
    /// restores what it was, so a nested clearing never sets it where an outer one cleared it.
    class MayLeaveCleared
    {
      public:
        explicit MayLeaveCleared(ComponentInstanceState& state) noexcept:
            _state { &state }, _previous { std::exchange(state.mayLeave, MayLeave::No) }
        {
        }
        MayLeaveCleared(MayLeaveCleared const&) = delete;
        MayLeaveCleared& operator=(MayLeaveCleared const&) = delete;
        MayLeaveCleared(MayLeaveCleared&&) = delete;
        MayLeaveCleared& operator=(MayLeaveCleared&&) = delete;
        ~MayLeaveCleared() { _state->mayLeave = _previous; }

      private:
        ComponentInstanceState* _state;
        MayLeave _previous;
    };

    /// A Task in progress while it lives: started on construction, ended (returned or trapped) on
    /// destruction, innermost first as synchronous calls nest.
    class TaskInProgress
    {
      public:
        explicit TaskInProgress(TaskStack& tasks): _tasks { &tasks }, _id { tasks.begin() } {}
        TaskInProgress(TaskInProgress const&) = delete;
        TaskInProgress& operator=(TaskInProgress const&) = delete;
        TaskInProgress(TaskInProgress&&) = delete;
        TaskInProgress& operator=(TaskInProgress&&) = delete;
        ~TaskInProgress() { _tasks->end(_id); }
        [[nodiscard]] TaskId id() const noexcept { return _id; }

      private:
        TaskStack* _tasks;
        TaskId _id;
    };

    /// Ends a Subtask's lends while it lives, however the call ends (definitions.py's deliver_resolve).
    class SubtaskInProgress
    {
      public:
        explicit SubtaskInProgress(InstanceTables& instances) noexcept: _instances { &instances } {}
        SubtaskInProgress(SubtaskInProgress const&) = delete;
        SubtaskInProgress& operator=(SubtaskInProgress const&) = delete;
        SubtaskInProgress(SubtaskInProgress&&) = delete;
        SubtaskInProgress& operator=(SubtaskInProgress&&) = delete;
        ~SubtaskInProgress() { subtask.deliverResolve(*_instances); }

        Subtask subtask {}; ///< The subtask.

      private:
        InstanceTables* _instances;
    };

    /// The error of a store that locked down or holds a pending trap after guest code returned: what a
    /// host swallowed reaches the caller still (D6). Nothing when it is live and nothing is pending.
    [[nodiscard]] std::expected<void, CallError> recheck(ComponentStore& store)
    {
        if (auto pending = store.pendingTrap().take())
            return std::unexpected { CallError { std::move(*pending) } }; // uncovered(pending-after-success)
        if (store.state() == StoreState::LockedDown)
            return std::unexpected { CallError { trapOf(TrapCode::CannotEnter) } };
        return {};
    }

    /// The parameter types of @p function.
    [[nodiscard]] std::vector<types::TypeId> parameterTypes(types::FunctionTypeInfo const& function)
    {
        auto result = std::vector<types::TypeId> {};
        result.reserve(function.params.size());
        for (auto const& param: function.params)
            result.push_back(param.second);
        return result;
    }

    /// The result types of @p function: none or one.
    [[nodiscard]] std::vector<types::TypeId> resultTypes(types::FunctionTypeInfo const& function)
    {
        if (function.result)
            return { *function.result };
        return {};
    }

    /// A view of each root of @p values.
    [[nodiscard]] std::vector<canonical::ValueView> rootsOf(canonical::Values const& values)
    {
        auto views = std::vector<canonical::ValueView> {};
        views.reserve(values.rootCount());
        for (auto const index: std::views::iota(std::size_t { 0 }, values.rootCount()))
            views.push_back(values.root(index));
        return views;
    }

    /// The values the arguments hold, owned or lent.
    [[nodiscard]] canonical::Values const& valuesOf(CallArguments const& arguments) noexcept
    {
        if (auto const* owned = std::get_if<canonical::Values>(&arguments))
            return *owned;
        return std::get<std::reference_wrapper<canonical::Values const>>(arguments).get();
    }

    /// The guest memory of a canonical side, if its options give one.
    [[nodiscard]] std::optional<GuestMemory> memoryOf(ComponentStore& store,
                                                      ResolvedOptions const& options,
                                                      uint32_t instance)
    {
        if (!options.memory)
            return std::nullopt;
        return std::optional<GuestMemory> {
            std::in_place, store, *options.memory, options.realloc, instance
        };
    }

    /// Hands @p values to @p consumer, which lowers them: the one undo every lowering path shares. When
    /// the consumer fails, the owns among the values are in no table (a lowering that fails rolls back
    /// what it added), so they go back to their holder (ComponentStore::undeliver()). Only values whose
    /// type may hold an own are walked for them.
    [[nodiscard]] std::expected<void, CallError> deliver(ComponentStore& store,
                                                         canonical::Values values,
                                                         types::HandleContent handles,
                                                         OwnHolder holder,
                                                         OnResolve consumer)
    {
        auto const owns = types::holds(handles, types::HandleContent::Own)
                              ? ownsOf(values)
                              : std::vector<canonical::ResourceValue> {};
        auto delivered = consumer(std::move(values));
        if (!delivered)
            store.undeliver(owns, holder);
        return delivered;
    }

    /// Lifts values out of a guest with @p lift: the lift-side twin of deliver(). liftOwn() takes each own
    /// out of the guest's table as it goes, so when a later value fails the owns lifted before it are in
    /// no table: they go back to the guest (ComponentStore::undeliver()), whose trap locks the store down.
    template <typename Lift>
    [[nodiscard]] auto liftFrom(ComponentStore& store, ResourceOperations& handles, Lift lift)
    {
        auto lifted = lift();
        if (!lifted)
            store.undeliver(handles.takeLiftedOwns(), OwnHolder::Guest);
        return lifted;
    }

    /// canon_lift's body (canonLift() guards it).
    [[nodiscard]] std::expected<void, CallError> liftedCall(ComponentStore& store,
                                                            LiftedFunction const& function,
                                                            OnStart onStart,
                                                            OnResolve onResolve)
    {
        auto const& side = function.side;
        auto const& type = (*side.arena)[function.type];
        auto& resources = store.resources();
        auto const task = TaskInProgress { resources.tasks() };
        auto handles = ResourceOperations { resources, side.instance, *side.bindings, task.id() };
        auto memory = memoryOf(store, function.options, side.instance);
        auto context = canonical::LiftContext { .arena = *side.arena,
                                                .layouts = side.layouts->all(),
                                                .memory = memory ? &*memory : nullptr,
                                                .encoding = function.options.encoding,
                                                .budget = store.liveBytes(),
                                                .handles = handles };

        // task.start(), then lower_flat_values into the callee; owned arguments end once lowered.
        auto started = onStart();
        if (!started)
            return std::unexpected { std::move(started.error()) };
        // on_start is the caller's code: an embedder's may have called into the store and swallowed a trap.
        if (auto checked = recheck(store); !checked)
            return checked;
        auto arguments = std::optional<CallArguments> { std::move(*started) };
        auto const params = parameterTypes(type);
        auto flatArgs = [&] {
            auto const views = rootsOf(valuesOf(*arguments));
            Require(views.size() == params.size());
            return canonical::lowerFlat(context, params, views, canonical::MaxFlatParams, std::nullopt);
        }();
        // Each realloc re-checks the store itself (GuestMemory::reallocate()). A failed lowering passed no
        // own: those it added leave the callee's table, and the arguments' go back to their holder, the
        // guest that lifted them or the host that lent them.
        if (!flatArgs)
        {
            handles.rollBackLoweredOwns();
            if (types::holds(type.handles, types::HandleContent::Own))
                store.undeliver(ownsOf(valuesOf(*arguments)),
                                std::holds_alternative<canonical::Values>(*arguments) ? OwnHolder::Guest
                                                                                      : OwnHolder::Host);
            return std::unexpected { CallError { std::move(flatArgs.error()) } };
        }
        arguments.reset();

        auto const signature =
            canonical::flattenFunction(*side.layouts, type, canonical::FlattenContext::Lift);
        auto flatResults = std::vector<isoline::api::Value>(signature.results.size());
        if (auto called = store.coreStore().call(function.core, *flatArgs, flatResults); !called)
            return std::unexpected { fromCallError(called.error(), store.pendingTrap()) };
        // Defence in depth: every host function of the store re-checks after its host returns, so a core
        // call that succeeded in a locked-down store is one P12's paths may add, not one this layer makes.
        if (auto checked = recheck(store); !checked)
            return checked; // uncovered(defence)

        // lift_flat_values, then task.return_: on_resolve sees the results before post-return runs.
        auto results = liftFrom(store, handles, [&] {
            return canonical::liftFlat(context, resultTypes(type), flatResults, canonical::MaxFlatResults);
        });
        if (!results)
            return std::unexpected { CallError { std::move(results.error()) } };
        if (auto resolved = resources.tasks().resolve(task.id()); !resolved)
            return std::unexpected { CallError { std::move(resolved.error()) } };
        // The results were lifted out of the callee's table: a caller that cannot take them hands them back.
        if (auto delivered = deliver(store, std::move(*results), type.handles, OwnHolder::Guest, onResolve);
            !delivered)
            return delivered;
        // on_resolve, like on_start, may have re-entered the store and swallowed a trap: no post-return then.
        if (auto checked = recheck(store); !checked)
            return checked;

        if (function.options.postReturn)
        {
            auto const cleared = MayLeaveCleared { store.instanceState(side.instance) };
            if (auto called = store.coreStore().call(*function.options.postReturn, flatResults, {}); !called)
                return std::unexpected { fromCallError(called.error(), store.pendingTrap()) };
        }
        return recheck(store);
    }

    /// A host callee's call (callComponentFunction() guards it).
    [[nodiscard]] std::expected<void, CallError> hostCall(ComponentStore& store,
                                                          IHostCallee& callee,
                                                          InstanceSide const& resultSide,
                                                          types::FunctionTypeId resultType,
                                                          OnStart onStart,
                                                          OnResolve onResolve)
    {
        auto started = onStart();
        if (!started)
            return std::unexpected { std::move(started.error()) };
        auto const results = resultTypes((*resultSide.arena)[resultType]);
        auto builder = canonical::ValueBuilder { *resultSide.arena, results, store.liveBytes() };
        if (auto called = callee.call(store, valuesOf(*started), builder); !called)
            return std::unexpected { CallError { called.error() } };
        if (auto checked = recheck(store); !checked)
            return checked;
        auto built = builder.finish();
        if (!built)
        {
            if (built.error().code == canonical::ValueErrorCode::Budget)
                return std::unexpected { CallError {
                    trapOf(TrapCode::LiveValueBytes, store.liveBytes()->limit()) } };
            return std::unexpected { CallError {
                isoline::api::HostFailure { .kind = isoline::api::HostFailureKind::ResultMismatch,
                                            .code = 0,
                                            .mismatch = std::nullopt } } };
        }
        if (auto checked = callee.checkResults(store, *built); !checked)
            return std::unexpected { CallError { checked.error() } };
        // A lowering of the results that fails passes none of their owns: the host holds them again.
        return deliver(
            store, std::move(*built), (*resultSide.arena)[resultType].handles, OwnHolder::Host, onResolve);
    }
} // namespace

std::vector<canonical::ResourceValue> ownsOf(canonical::Values const& values)
{
    auto owns = std::vector<canonical::ResourceValue> {};
    auto pending = std::vector<canonical::ValueView> {};
    for (auto const root: std::views::iota(std::size_t { 0 }, values.rootCount()))
        pending.push_back(values.root(root));
    while (!pending.empty())
    {
        auto const view = pending.back();
        pending.pop_back();
        if (view.kind() == types::ValueKind::Own)
            owns.push_back(view.resource());
        else if (view.kind() != types::ValueKind::Borrow && view.childCount() != 0
                 && view.packedElementBytes().empty())
            for (auto const child: view.children())
                pending.push_back(child);
    }
    return owns;
}

std::expected<void, CallError> canonLift(ComponentStore& store,
                                         LiftedFunction const& function,
                                         OnStart onStart,
                                         OnResolve onResolve) noexcept
{
    if (store.state() == StoreState::LockedDown)
        return std::unexpected { CallError { trapOf(TrapCode::CannotEnter) } };
    // One invocation: argument reallocs, the callee and post-return share its step budget and record.
    auto const scope = store.coreStore().openInvocation();
    auto const active = ActiveCall { store };
    auto outcome = liftedCall(store, function, onStart, onResolve);
    if (!outcome)
        store.lockDown();
    return outcome;
}

std::expected<void, CallError> callComponentFunction(ComponentStore& store,
                                                     ComponentFunction const& function,
                                                     InstanceSide const& resultSide,
                                                     types::FunctionTypeId resultType,
                                                     OnStart onStart,
                                                     OnResolve onResolve) noexcept
{
    if (auto const* lifted = std::get_if<LiftedRef>(&function))
        return canonLift(store, store.lifted(*lifted), onStart, onResolve);
    if (store.state() == StoreState::LockedDown)
        return std::unexpected { CallError { trapOf(TrapCode::CannotEnter) } };
    auto const active = ActiveCall { store };
    auto const& callee = std::get<std::shared_ptr<IHostCallee>>(function);
    Require(callee != nullptr);
    auto outcome = hostCall(store, *callee, resultSide, resultType, onStart, onResolve);
    if (!outcome)
        store.lockDown();
    return outcome;
}

std::expected<void, Trap> GuestMemory::read(uint32_t address, std::span<std::byte> out)
{
    if (auto inRange = canonical::checkRange(address, out.size(), 1, size()); !inRange)
        return inRange;
    auto range = _store->coreStore().memoryRange(_memory, address, out.size());
    Require(range.has_value()); // checked above, and no guest code ran since
    std::ranges::copy(*range, out.begin());
    return {};
}

std::expected<void, Trap> GuestMemory::write(uint32_t address, std::span<std::byte const> bytes)
{
    if (auto inRange = canonical::checkRange(address, bytes.size(), 1, size()); !inRange)
        return inRange;
    auto range = _store->coreStore().memoryRange(_memory, address, bytes.size());
    Require(range.has_value());
    std::ranges::copy(bytes, range->begin());
    return {};
}

std::expected<uint32_t, Trap> GuestMemory::reallocate(uint32_t old,
                                                      uint32_t oldSize,
                                                      uint32_t alignment,
                                                      uint32_t newSize)
{
    Require(_realloc.has_value());
    if (_store->state() == StoreState::LockedDown)
        return std::unexpected { trapOf(TrapCode::CannotEnter) };
    auto const args = std::to_array<isoline::api::Value>({ isoline::api::I32 { old },
                                                           isoline::api::I32 { oldSize },
                                                           isoline::api::I32 { alignment },
                                                           isoline::api::I32 { newSize } });
    auto result = std::array<isoline::api::Value, 1> {};
    {
        auto const cleared = MayLeaveCleared { _store->instanceState(_instance) };
        if (auto called = _store->coreStore().call(*_realloc, args, result); !called)
            return std::unexpected { asTrap(fromCallError(called.error(), _store->pendingTrap())) };
    }
    // Defence in depth, as after a callee: the realloc ran with may_leave cleared, so it reached no host.
    if (auto checked = recheck(*_store); !checked)
        return std::unexpected { asTrap(std::move(checked.error())) }; // uncovered(defence)
    return std::get<isoline::api::I32>(result[0]).bits;
}

std::expected<void, Trap> GuestMemory::poll()
{
    auto polled =
        _store->coreStore().poll(isoline::api::BulkGranule { .bytes = isoline::api::BulkPollBytes });
    if (!polled)
        return std::unexpected { asTrap(fromCallError(polled.error(), _store->pendingTrap())) };
    return {};
}

uint64_t GuestMemory::size()
{
    auto const size = _store->coreStore().memorySize(_memory);
    Require(size.has_value());
    return size->bytes;
}

std::expected<void, isoline::api::Abrupt> LoweredFunction::call(
    isoline::api::HostContext& context,
    std::span<isoline::api::Value const> args,
    std::span<isoline::api::Value> results) noexcept
{
    std::ignore = context;
    auto outcome = run(args, results);
    if (outcome)
        return {};
    _store->lockDown();
    if (auto* trap = std::get_if<Trap>(&outcome.error()))
    {
        // The trap crosses the core frames as the carrier; the call that sees it fail takes it back.
        _store->pendingTrap().raise(std::move(*trap));
        return std::unexpected { isoline::api::Abrupt { canonical::carrierTrap() } };
    }
    return std::unexpected { isoline::api::Abrupt { std::get<isoline::api::HostFailure>(outcome.error()) } };
}

std::expected<void, CallError> LoweredFunction::run(std::span<isoline::api::Value const> args,
                                                    std::span<isoline::api::Value> results)
{
    // Guest code runs only in a call or an instantiation, which a lockdown ends at once.
    if (_store->state() == StoreState::LockedDown)
        return std::unexpected { CallError { trapOf(TrapCode::CannotEnter) } }; // uncovered(defence)
    if (_store->instanceState(_caller.instance).mayLeave == MayLeave::No)
        return std::unexpected { CallError { trapOf(TrapCode::CannotLeave) } };

    auto& resources = _store->resources();
    auto const& type = (*_caller.arena)[_type];
    auto lends = SubtaskInProgress { resources.instances() };
    auto handles = ResourceOperations { resources, _caller.instance, *_caller.bindings, &lends.subtask };
    auto memory = memoryOf(*_store, _options, _caller.instance);
    auto context = canonical::LiftContext { .arena = *_caller.arena,
                                            .layouts = _caller.layouts->all(),
                                            .memory = memory ? &*memory : nullptr,
                                            .encoding = _options.encoding,
                                            .budget = _store->liveBytes(),
                                            .handles = handles };

    // Results that do not fit flat go where the last argument points (lower_flat_values's out_param).
    auto const resultsSpill =
        type.result && (*_caller.layouts)[*type.result].flat.size() > canonical::MaxFlatResults;
    auto const params = parameterTypes(type);
    auto const flatParams = args.first(args.size() - (resultsSpill ? 1 : 0));
    auto const resultPointer = resultsSpill
                                   ? std::optional<uint32_t> { std::get<isoline::api::I32>(args.back()).bits }
                                   : std::nullopt;

    auto start = [&]() -> std::expected<CallArguments, CallError> {
        lends.subtask.state = Subtask::State::Started;
        auto lifted = liftFrom(*_store, handles, [&] {
            return canonical::liftFlat(context, params, flatParams, canonical::MaxFlatParams);
        });
        if (!lifted)
            return std::unexpected { CallError { std::move(lifted.error()) } };
        return CallArguments { std::move(*lifted) };
    };
    auto resolve = [&](canonical::Values values) -> std::expected<void, CallError> {
        auto const views = rootsOf(values);
        auto lowered =
            canonical::lowerFlat(context, resultTypes(type), views, canonical::MaxFlatResults, resultPointer);
        if (!lowered)
        {
            // The owns among the results stay the callee's (hostCall() hands them back to a host).
            handles.rollBackLoweredOwns();
            return std::unexpected { CallError { std::move(lowered.error()) } };
        }
        Require(lowered->size() == results.size());
        std::ranges::copy(*lowered, results.begin());
        return {};
    };
    // callComponentFunction() re-checked the store after its callee, and a realloc of the results after
    // itself.
    if (auto called = callComponentFunction(*_store, _callee, _caller, _type, start, resolve); !called)
        return called;
    lends.subtask.state = Subtask::State::Returned;
    return {};
}

isoline::api::FuncType loweredCoreType(InstanceSide const& side, types::FunctionTypeId type)
{
    auto const signature =
        canonical::flattenFunction(*side.layouts, (*side.arena)[type], canonical::FlattenContext::Lower);
    auto result = isoline::api::FuncType {};
    for (auto const kind: signature.params)
        result.params.push_back(CoreValueTypes[static_cast<std::size_t>(kind)]);
    for (auto const kind: signature.results)
        result.results.push_back(CoreValueTypes[static_cast<std::size_t>(kind)]);
    return result;
}

CallError fromCallError(isoline::api::CallError const& error, canonical::PendingTrap& pending)
{
    // The first trap raised ends the call: one waiting in the carrier precedes whatever the core
    // call reported on its way out.
    if (auto taken = pending.take())
        return CallError { std::move(*taken) };
    if (auto const* trap = std::get_if<isoline::api::Trap>(&error.detail))
        return CallError { canonical::fromCore(*trap) };
    if (auto const* failure = std::get_if<isoline::api::HostFailure>(&error.detail))
        return CallError { *failure };
    if (std::holds_alternative<isoline::api::UncaughtException>(error.detail))
        return CallError { trapOf(TrapCode::UncaughtException) };
    // A mismatch or a foreign handle: the component layer calls only its store's functions, with values
    // of their flattened types.
    Require(false);
    return CallError { trapOf(TrapCode::CannotEnter) }; // uncovered(unreachable)
}

Trap asTrap(CallError error)
{
    if (auto* trap = std::get_if<Trap>(&error))
        return std::move(*trap);
    return canonical::fromCore(canonical::carrierTrap());
}

} // namespace isoline::component::runtime
