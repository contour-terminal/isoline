// SPDX-License-Identifier: Apache-2.0
#include <isoline/component/api/ComponentStore.hpp>

#include <core/Assert.hpp>
#include <core/Utils.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/component/api/Linker.hpp>
#include <isoline/component/runtime/Calls.hpp>
#include <isoline/component/validate/Subtyping.hpp>

namespace isoline::component::api
{

namespace
{

    /// The text of a host failure's kind.
    [[nodiscard]] std::string_view hostFailureText(isoline::api::HostFailureKind kind) noexcept
    {
        switch (kind)
        {
            case isoline::api::HostFailureKind::Embedder: return "embedder failure";
            case isoline::api::HostFailureKind::ResultMismatch: return "results not of the function's type";
            case isoline::api::HostFailureKind::ArgumentMismatch: return "arguments not of their types";
        }
        return "host failure"; // uncovered(exhaustive-switch)
    }

    /// The call error a runtime call failed with: a trap (Interrupted for the interrupter, R13) or a host
    /// failure.
    [[nodiscard]] ComponentCallError fromRuntime(runtime::CallError error)
    {
        return std::visit(core::Overloaded {
                              [](canonical::Trap trap) {
                                  auto const failure = trap.code == canonical::TrapCode::Interrupted
                                                           ? ComponentCallFailure::Interrupted
                                                           : ComponentCallFailure::Trap;
                                  return callError(failure, std::move(trap));
                              },
                              [](isoline::api::HostFailure failure) {
                                  return callError(ComponentCallFailure::HostFailure, failure);
                              },
                          },
                          std::move(error));
    }

    [[nodiscard]] ComponentCallError argumentMismatch(canonical::ValueErrorCode code,
                                                      std::string path,
                                                      std::string detail)
    {
        return callError(
            ComponentCallFailure::ArgumentMismatch,
            canonical::ValueError { .code = code, .path = std::move(path), .detail = std::move(detail) });
    }

    /// Process-unique store serials, as TypeArena ids are: never reused, so a handle of a store gone
    /// never matches a store made later.
    [[nodiscard]] uint64_t nextSerial() noexcept
    {
        static auto next = std::atomic<uint64_t> { 1 };
        return next.fetch_add(1, std::memory_order_relaxed);
    }

    using HostOwnKey = std::pair<uint32_t, uint32_t>;

    /// The host holds @p owns now: each is counted in the store's table.
    void hold(StoreState& state, std::span<ResourceValue const> owns)
    {
        for (auto const& own: owns)
            ++state.hostOwns[HostOwnKey { own.type.value, own.rep }];
    }

    /// The host gives up @p owns: each must be held (as often as it occurs), else nothing is taken.
    [[nodiscard]] std::expected<void, canonical::ValueError> release(StoreState& state,
                                                                     std::span<ResourceValue const> owns)
    {
        auto wanted = std::map<HostOwnKey, uint32_t> {};
        for (auto const& own: owns)
            ++wanted[HostOwnKey { own.type.value, own.rep }];
        for (auto const& [key, count]: wanted)
            if (auto const held = state.hostOwns.find(key);
                held == state.hostOwns.end() || held->second < count)
                return std::unexpected { canonical::ValueError {
                    .code = canonical::ValueErrorCode::TypeMismatch,
                    .path = {},
                    .detail = std::format("an own of resource type {} rep {} the host does not hold",
                                          key.first,
                                          key.second) } };
        for (auto const& [key, count]: wanted)
        {
            auto const held = state.hostOwns.find(key);
            held->second -= count;
            if (held->second == 0)
                state.hostOwns.erase(held);
        }
        return {};
    }

    /// The runtime's view of a linker's host function in one store.
    class LinkedFunction final: public runtime::IHostCallee
    {
      public:
        LinkedFunction(StoreState& state, std::shared_ptr<IHostComponentFunction> function) noexcept:
            _state { &state }, _function { std::move(function) }
        {
        }

        [[nodiscard]] std::expected<void, isoline::api::HostFailure> call(
            runtime::ComponentStore&,
            canonical::Values const& args,
            canonical::ValueBuilder& results) noexcept override
        {
            // The store's ComponentStore cannot move while a call runs (checked there). The owns lifted
            // into the arguments left the guest's table: the host holds them (R12).
            hold(*_state, runtime::ownsOf(args));
            auto context = HostComponentContext { *_state->owner };
            return _function->call(context, args, results);
        }

        [[nodiscard]] std::expected<void, isoline::api::HostFailure> checkResults(
            runtime::ComponentStore&, canonical::Values const& results) noexcept override
        {
            // The owns returned go to the guest: the host must hold each, and holds it no more.
            if (!release(*_state, runtime::ownsOf(results)))
                return std::unexpected { isoline::api::HostFailure {
                    .kind = isoline::api::HostFailureKind::ArgumentMismatch,
                    .code = 0,
                    .mismatch = std::nullopt } };
            return {};
        }

        void resultsUndelivered(runtime::ComponentStore&,
                                std::span<ResourceValue const> owns) noexcept override
        {
            hold(*_state, owns);
        }

      private:
        StoreState* _state;
        std::shared_ptr<IHostComponentFunction> _function;
    };

    /// Whether every handle of @p args is of the runtime resource type its type names in @p side: what
    /// lowering would trap on (WrongResource) otherwise. Values whose types hold no handle are skipped.
    [[nodiscard]] std::expected<void, ComponentCallError> checkHandles(runtime::ComponentStore& store,
                                                                       runtime::InstanceSide const& side,
                                                                       canonical::Values const& args)
    {
        auto const& arena = *side.arena;
        auto pending = std::vector<std::pair<std::size_t, canonical::ValueView>> {};
        for (auto const root: std::views::iota(std::size_t { 0 }, args.rootCount()))
            pending.emplace_back(root, args.root(root));
        while (!pending.empty())
        {
            auto const [root, view] = pending.back();
            pending.pop_back();
            auto const& type = arena[view.type()];
            if (type.handles == types::HandleContent::None)
                continue;
            if (type.kind == types::ValueKind::Own || type.kind == types::ValueKind::Borrow)
            {
                auto const value = view.resource();
                auto const expected = (*side.bindings)[*type.resource];
                if (!store.resources().types().contains(value.type) || value.type != expected)
                    return std::unexpected { argumentMismatch(
                        canonical::ValueErrorCode::TypeMismatch,
                        std::format("[{}]", root),
                        std::format("a handle of resource type {}, where {} is expected",
                                    value.type.value,
                                    expected.value)) };
                continue;
            }
            for (auto const child: view.children())
                pending.emplace_back(root, child);
        }
        return {};
    }

    /// Whether @p args fit @p function's parameters: its arena, one root per parameter, each of the
    /// parameter's type, every handle of its resource type.
    [[nodiscard]] std::expected<void, ComponentCallError> checkArguments(
        runtime::ComponentStore& store,
        ComponentLimits const& limits,
        runtime::LiftedFunction const& function,
        canonical::Values const& args)
    {
        auto const& arena = *function.side.arena;
        auto const& params = arena[function.type].params;
        if (args.arena() != arena.id())
            return std::unexpected { argumentMismatch(
                canonical::ValueErrorCode::TypeMismatch, {}, "arguments built in another type arena") };
        if (args.rootCount() != params.size())
            return std::unexpected { argumentMismatch(
                canonical::ValueErrorCode::Unbalanced,
                {},
                std::format("expected {} arguments, given {}", params.size(), args.rootCount())) };
        for (auto const [index, param]: core::views::enumerate(params))
        {
            auto const given = args.root(index).type();
            if (given == param.second)
                continue;
            // Two definitions of one structure are two TypeIds; equality is structural.
            auto relation = validate::TypeRelation { arena, arena, validate::ResourceMapping {}, limits };
            if (auto equal = relation.isEqual(given, param.second); !equal)
                return std::unexpected { argumentMismatch(canonical::ValueErrorCode::TypeMismatch,
                                                          std::format("[{}]", index),
                                                          std::move(equal.error().message)) };
        }
        return checkHandles(store, function.side, args);
    }

} // namespace

std::string ComponentCallError::message() const
{
    auto const& row = componentCallFailureRow(failure);
    auto const detailText =
        std::visit(core::Overloaded {
                       [](std::monostate) { return std::string {}; },
                       [](canonical::Trap const& trap) { return trap.message(); },
                       [](isoline::api::HostFailure const& host) {
                           return std::format("{} {}", hostFailureText(host.kind), host.code);
                       },
                       [](canonical::ValueError const& value) { return value.message(); },
                   },
                   detail);
    if (detailText.empty())
        return std::string { row.text };
    return std::format("{}: {}", row.text, detailText);
}

ComponentCallError callError(ComponentCallFailure failure, ComponentCallDetail detail)
{
    Require(detail.index() == componentCallFailureRow(failure).detail);
    return ComponentCallError { .failure = failure, .detail = std::move(detail) };
}

HostResourceType::HostResourceType(std::string name, std::shared_ptr<IHostResourceDestructor> destructor):
    _identity { std::make_shared<HostResourceIdentity const>(
        HostResourceIdentity { .name = std::move(name) }) },
    _destructor { std::move(destructor) }
{
}

std::expected<ResourceValue, canonical::Trap> HostComponentContext::newOwn(HostResourceType const& type,
                                                                           uint32_t rep) const
{
    return _store->hostResource(type, rep, canonical::Ownership::Own);
}

std::expected<void, ComponentCallError> HostComponentContext::drop(ResourceValue value) const noexcept
{
    return _store->drop(value);
}

types::FunctionTypeInfo const* ComponentFunction::type() const noexcept
{
    if (_alive.expired())
        return nullptr;
    return &(*_arena)[_type];
}

ComponentInstance::ComponentInstance(StoreState& state,
                                     uint64_t serial,
                                     uint32_t instance,
                                     Component const& component) noexcept:
    _state { &state },
    _alive { state.alive },
    _serial { serial },
    _instance { instance },
    _component { component }
{
}

std::expected<std::optional<ComponentFunction>, ComponentCallError> ComponentInstance::function(
    std::string_view path) const
{
    auto* const state = live();
    if (state == nullptr)
        return std::unexpected { callError(ComponentCallFailure::StoreDeleted) };
    auto& store = state->runtime;
    auto exports = store.exportsOf(_instance);
    auto value = std::optional<runtime::SlotValue> {};
    for (auto const segment: std::views::split(path, PathSeparator))
    {
        auto const name = std::string_view { segment.begin(), segment.end() };
        if (value)
        {
            auto const* instance = std::get_if<runtime::InstanceRef>(&*value);
            if (instance == nullptr)
                return std::optional<ComponentFunction> {};
            exports = store.exportsOf(*instance);
        }
        auto const found = std::ranges::find(exports, name, &runtime::NamedValue::name);
        if (found == exports.end())
            return std::optional<ComponentFunction> {};
        value = found->value;
    }
    if (!value)
        return std::optional<ComponentFunction> {}; // uncovered(split-yields-a-segment)
    auto const* function = std::get_if<runtime::ComponentFunction>(&*value);
    if (function == nullptr)
        return std::optional<ComponentFunction> {};
    auto const* lifted = std::get_if<runtime::LiftedRef>(function);
    if (lifted == nullptr)
        return std::optional<ComponentFunction> {};
    auto const& definition = store.lifted(*lifted);
    return ComponentFunction { _serial, _alive, *lifted, *definition.side.arena, definition.type };
}

std::expected<canonical::Values, ComponentCallError> ComponentInstance::call(
    ComponentFunction const& function, canonical::Values const& args) noexcept
{
    auto* const state = live();
    if (state == nullptr)
        return std::unexpected { callError(ComponentCallFailure::StoreDeleted) };
    if (function._serial != _serial)
        return std::unexpected { callError(ComponentCallFailure::ForeignFunction) };
    auto& store = state->runtime;
    if (store.state() == runtime::StoreState::LockedDown)
        return std::unexpected { callError(ComponentCallFailure::LockedDown) };
    auto const& lifted = store.lifted(function._lifted);
    if (auto checked = checkArguments(store, state->limits, lifted, args); !checked)
        return std::unexpected { std::move(checked.error()) };
    // The owns passed go to the guest's table: the host must hold each, and holds it no more.
    auto const owns = runtime::ownsOf(args);
    if (auto released = release(*state, owns); !released)
        return std::unexpected { callError(ComponentCallFailure::ArgumentMismatch,
                                           std::move(released.error())) };
    auto results = std::optional<canonical::Values> {};
    auto start = [&]() -> std::expected<runtime::CallArguments, runtime::CallError> {
        return runtime::CallArguments { std::cref(args) };
    };
    auto resolve = [&](canonical::Values values) -> std::expected<void, runtime::CallError> {
        results.emplace(std::move(values));
        return {};
    };
    // The owns were taken before the call: a lowering that fails passed none, so the host keeps them.
    auto lowered = false;
    auto onLowered = [&] {
        lowered = true;
    };
    if (auto called = runtime::canonLift(store, lifted, start, onLowered, resolve); !called)
    {
        if (!lowered)
            hold(*state, owns);
        return std::unexpected { fromRuntime(std::move(called.error())) };
    }
    // The owns returned left the guest's table: the host holds them (R12).
    hold(*state, runtime::ownsOf(*results));
    return std::move(*results);
}

std::expected<canonical::ValueBuilder, ComponentCallError> ComponentInstance::argumentsFor(
    ComponentFunction const& function) const
{
    auto* const state = live();
    if (state == nullptr)
        return std::unexpected { callError(ComponentCallFailure::StoreDeleted) };
    if (function._serial != _serial)
        return std::unexpected { callError(ComponentCallFailure::ForeignFunction) };
    auto params = std::vector<types::TypeId> {};
    for (auto const& param: (*function._arena)[function._type].params)
        params.push_back(param.second);
    return std::expected<canonical::ValueBuilder, ComponentCallError> {
        std::in_place, *function._arena, params, state->runtime.liveBytes()
    };
}

bool ComponentInstance::belongsTo(ComponentStore const& store) const noexcept
{
    return store._state != nullptr && store._state->serial == _serial;
}

StoreState::StoreState(isoline::api::Engine const& engine,
                       isoline::api::IMemoryAllocator& allocator,
                       ComponentConfig const& config,
                       isoline::api::IPollObserver* observer):
    runtime { engine, allocator, config.limits, observer },
    serial { nextSerial() },
    engine { engine.config().identity() },
    limits { config.limits }
{
}

ComponentStore::ComponentStore(isoline::api::Engine const& engine,
                               isoline::api::IMemoryAllocator& allocator,
                               ComponentConfig const& config,
                               isoline::api::IPollObserver* observer):
    _state { std::make_unique<StoreState>(engine, allocator, config, observer) }
{
    _state->owner = this;
}

namespace
{
    /// Whether a call or an instantiation of @p state runs: what forbids moving its store.
    [[nodiscard]] bool isBusyState(StoreState const& state) noexcept
    {
        return state.runtime.activeCalls() != 0
               || state.runtime.state() == runtime::StoreState::Instantiating;
    }
} // namespace

ComponentStore::ComponentStore(ComponentStore&& other) noexcept
{
    Require(other._state == nullptr || !isBusyState(*other._state));
    _state = std::move(other._state);
    if (_state != nullptr)
        _state->owner = this;
}

ComponentStore& ComponentStore::operator=(ComponentStore&& other) noexcept
{
    if (this == &other)
        return *this;
    Require(other._state == nullptr || !isBusyState(*other._state));
    Require(_state == nullptr || !isBusyState(*_state));
    _state = std::move(other._state);
    if (_state != nullptr)
        _state->owner = this;
    return *this;
}

ComponentStore::~ComponentStore() = default;

std::shared_ptr<isoline::api::Interrupter> const& ComponentStore::interrupter() const noexcept
{
    return _state->runtime.coreStore().interrupter();
}

std::expected<ResourceValue, canonical::Trap> ComponentStore::hostResource(HostResourceType const& type,
                                                                           uint32_t rep,
                                                                           canonical::Ownership ownership)
{
    auto const bound = _state->runtime.hostResourceTypeOf(*type.identity());
    if (!bound)
        return std::unexpected { canonical::trapOf(canonical::TrapCode::WrongResource, rep) };
    auto const value = ResourceValue { .type = *bound, .rep = rep, .ownership = ownership };
    if (ownership == canonical::Ownership::Own)
        hold(*_state, std::span { &value, 1 });
    return value;
}

bool ComponentStore::isBusy() const noexcept
{
    return _state != nullptr && isBusyState(*_state);
}

bool ComponentStore::isLockedDown() const noexcept
{
    return _state->runtime.state() == runtime::StoreState::LockedDown;
}

std::shared_ptr<runtime::IHostCallee> ComponentStore::hostCallee(
    StoreState& state, std::shared_ptr<IHostComponentFunction> function)
{
    return std::make_shared<LinkedFunction>(state, std::move(function));
}

std::expected<ComponentInstance, InstantiateComponentError> ComponentStore::instantiate(
    Component const& component, Linker const& linker) noexcept
{
    // The API boundary: resolving and instantiating allocate, and the host running out of memory is a
    // refusal, never an exception in the embedder's lap.
    try
    {
        if (component.engine() != _state->engine)
            return std::unexpected { InstantiateComponentError { isoline::api::EngineMismatch {} } };
        auto resolution = linker.resolveFor(component, _state.get());
        if (resolution.refusal)
            return std::unexpected { std::move(*resolution.refusal) };
        auto root = _state->runtime.instantiate(component.code(), resolution.imports);
        if (!root)
            return std::unexpected { InstantiateComponentError { std::move(root.error()) } };
        return ComponentInstance { *_state, _state->serial, *root, component };
    }
    catch (std::bad_alloc const&)
    {
        // uncovered-begin(host-memory)
        return std::unexpected { InstantiateComponentError { runtime::InstantiationFailure {
            .kind = runtime::InstantiationFailureKind::HostMemoryExhausted } } };
    }
    // uncovered-end(host-memory)
    catch (std::length_error const&)
    {
        // uncovered-begin(host-memory)
        return std::unexpected { InstantiateComponentError { runtime::InstantiationFailure {
            .kind = runtime::InstantiationFailureKind::HostMemoryExhausted } } };
    }
    // uncovered-end(host-memory)
}

std::expected<void, ComponentCallError> ComponentStore::drop(ResourceValue value) noexcept
{
    if (_state == nullptr)
        return std::unexpected { callError(ComponentCallFailure::StoreDeleted) };
    auto& store = _state->runtime;
    if (store.state() == runtime::StoreState::Instantiating)
        return std::unexpected { callError(ComponentCallFailure::Instantiating) };
    if (value.ownership != canonical::Ownership::Own)
        return std::unexpected { argumentMismatch(
            canonical::ValueErrorCode::TypeMismatch, {}, "a borrow, which only its lender drops") };
    auto const& types = store.resources().types();
    if (!types.contains(value.type))
        return std::unexpected { argumentMismatch(
            canonical::ValueErrorCode::TypeMismatch,
            {},
            std::format("resource type {} is not of the store", value.type.value)) };
    auto const* const guestType = std::get_if<runtime::GuestImplementor>(&types[value.type]);
    if (guestType != nullptr && store.state() == runtime::StoreState::LockedDown)
        return std::unexpected { callError(ComponentCallFailure::LockedDown) };
    if (auto released = release(*_state, std::span { &value, 1 }); !released)
        return std::unexpected { callError(ComponentCallFailure::ArgumentMismatch,
                                           std::move(released.error())) };
    return std::visit(
        core::Overloaded {
            [&](runtime::HostImplementor const& host) -> std::expected<void, ComponentCallError> {
                // Host code only: a host resource is destroyed even in a locked-down store.
                if (host.destructor != nullptr)
                    host.destructor->destroy(value.rep);
                return {};
            },
            [&](runtime::GuestImplementor const& guest) -> std::expected<void, ComponentCallError> {
                if (!guest.destructor)
                    return {};
                auto destroyed = store.callGuestDestructor(guest, guest.componentInstance, value.rep);
                if (!destroyed)
                {
                    // A trap in guest code locks the store down (D6), as `resource.drop`'s would: as its
                    // failure's row says.
                    auto error = fromRuntime(runtime::CallError { std::move(destroyed.error()) });
                    if (componentCallFailureRow(error.failure).effect == StoreEffect::LocksDown)
                        store.lockDown();
                    return std::unexpected { std::move(error) };
                }
                return {};
            },
        },
        types[value.type]);
}

} // namespace isoline::component::api
