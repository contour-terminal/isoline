// SPDX-License-Identifier: Apache-2.0
#pragma once

// Test doubles of the runtime layer's resources: a built-in host over a ResourceStore whose guest
// destructors a test scripts, a recording host destructor, and a core store that makes the
// api::Function a guest destructor names. Uses Catch2's assertions.

#include <core/Assert.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <isoline/api/Engine.hpp>
#include <isoline/api/Extern.hpp>
#include <isoline/api/Store.hpp>
#include <isoline/api/Types.hpp>
#include <isoline/component/Limits.hpp>
#include <isoline/component/canonical/Trap.hpp>
#include <isoline/component/runtime/Builtins.hpp>
#include <isoline/component/runtime/Resources.hpp>

namespace isoline::component::runtime::testing
{

/// The trap @p result failed with.
template <typename T>
[[nodiscard]] Trap trapIn(std::expected<T, Trap> const& result)
{
    REQUIRE_FALSE(result.has_value());
    return result.error();
}

/// The value @p result holds.
template <typename T>
[[nodiscard]] T valueIn(std::expected<T, Trap> const& result)
{
    if (!result)
        FAIL("trapped: " << result.error().message());
    return *result;
}

/// A task of a store's TaskStack, held for a test's scope: begun when constructed, ended when the scope
/// exits, the unwinding of a failed REQUIRE included. A failed assertion mid-call then reports instead of
/// aborting the binary through ~ResourceStore's "no task in progress" check, which stays checked. Scopes
/// nest as calls do: C++ destroys them innermost-first, the order TaskStack::end() requires.
class TaskScope
{
  public:
    /// Begins a task of @p tasks.
    /// @param tasks The store's tasks; outlives the scope.
    /// @throws std::bad_alloc When the task cannot be stored.
    explicit TaskScope(TaskStack& tasks): _tasks { &tasks }, _id { tasks.begin() } {}

    TaskScope(TaskScope const&) = delete;
    TaskScope& operator=(TaskScope const&) = delete;
    TaskScope(TaskScope&&) = delete;
    TaskScope& operator=(TaskScope&&) = delete;
    ~TaskScope() { end(); }

    /// @return The task in progress (checked: not ended).
    [[nodiscard]] TaskId id() const noexcept
    {
        Require(_id.has_value());
        return *_id;
    }

    /// Ends the task now, as a call returning or trapping does; the scope's exit then ends nothing.
    /// Precondition (checked by TaskStack::end()): it is the innermost task.
    void end() noexcept
    {
        if (_id)
            _tasks->end(*std::exchange(_id, std::nullopt));
    }

    /// Ends the task and begins the next one: a store's following call.
    /// @return The new task.
    /// @throws std::bad_alloc When the task cannot be stored.
    TaskId renew()
    {
        end();
        _id = _tasks->begin();
        return *_id;
    }

  private:
    TaskStack* _tasks;
    std::optional<TaskId> _id; ///< The task in progress; empty once ended.
};

/// A host destructor that records every representation it destroys.
class RecordingDestructor final: public IHostResourceDestructor
{
  public:
    void destroy(uint32_t rep) noexcept override { destroyed.push_back(rep); }

    std::vector<uint32_t> destroyed {}; ///< Each representation destroyed, in order.
};

/// A call of a guest destructor IBuiltinHost::callGuestDestructor() saw.
struct DestructorCall
{
    uint32_t implementor = 0; ///< The implementing instance.
    uint32_t caller = 0;      ///< The instance that dropped the handle.
    uint32_t rep = 0;         ///< The representation.

    bool operator==(DestructorCall const&) const = default;
};

/// A built-in host over its own ResourceStore: a guest destructor call is recorded, then runs the
/// test's script (which may re-enter the store, as a destructor re-entering its component does).
class FakeBuiltinHost final: public IBuiltinHost
{
  public:
    explicit FakeBuiltinHost(ComponentLimits const& limits = {}, uint32_t maxIndex = HandleTableMaxLength):
        _resources { limits, maxIndex }
    {
    }

    [[nodiscard]] ResourceStore& resources() noexcept override { return _resources; }
    [[nodiscard]] canonical::PendingTrap& pendingTrap() noexcept override { return _pending; }

    [[nodiscard]] std::expected<void, Trap> callGuestDestructor(GuestImplementor const& implementor,
                                                                uint32_t callerInstance,
                                                                uint32_t rep) override
    {
        destructorCalls.push_back(DestructorCall {
            .implementor = implementor.componentInstance, .caller = callerInstance, .rep = rep });
        if (onDestructor)
            return onDestructor(rep);
        return {};
    }

    /// What a guest destructor does after it is recorded; nothing when empty.
    std::function<std::expected<void, Trap>(uint32_t rep)> onDestructor {};
    std::vector<DestructorCall> destructorCalls {}; ///< Every guest destructor call, in order.

  private:
    ResourceStore _resources;
    canonical::PendingTrap _pending {};
};

/// A core store, for the api::Function a guest destructor names and for calling built-ins' core
/// functions as a core instance would.
struct CoreStore
{
    isoline::api::Engine engine { isoline::api::EngineConfig {} };
    isoline::api::HeapMemoryAllocator allocator {};
    isoline::api::Store store { engine, allocator };

    /// A core function (i32) -> () that does nothing: a guest destructor's stand-in.
    [[nodiscard]] isoline::api::Function destructor()
    {
        auto const type = isoline::api::FuncType {
            .params = { isoline::api::ValueType { .kind = isoline::api::ValueKind::I32 } }, .results = {}
        };
        auto defined = store.define(
            type,
            isoline::api::makeHostFunction([](isoline::api::HostContext&,
                                              std::span<isoline::api::Value const>,
                                              std::span<isoline::api::Value>) noexcept
                                               -> std::expected<void, isoline::api::Abrupt> { return {}; }));
        REQUIRE(defined.has_value());
        return *defined;
    }
};

/// A guest implementor: @p instance, with a destructor when @p destructor is given.
[[nodiscard]] inline ResourceImplementor guestImplementor(
    uint32_t instance, std::optional<isoline::api::Function> destructor = {})
{
    return GuestImplementor { .componentInstance = instance, .destructor = std::move(destructor) };
}

/// A host implementor named @p name, destroyed by @p destructor.
[[nodiscard]] inline ResourceImplementor hostImplementor(std::string name,
                                                         std::shared_ptr<IHostResourceDestructor> destructor)
{
    return HostImplementor { .identity = std::make_shared<HostResourceIdentity const>(
                                 HostResourceIdentity { .name = std::move(name) }),
                             .destructor = std::move(destructor) };
}

} // namespace isoline::component::runtime::testing
