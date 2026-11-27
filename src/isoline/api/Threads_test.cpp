// SPDX-License-Identifier: Apache-2.0
//
// The api across threads: a module shared by stores on many threads at once, one store moving
// between threads, and a watchdog that interrupts a store from another thread and outlives it.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <future>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <vector>

#include <isoline/api/CallError.hpp>
#include <isoline/api/Engine.hpp>
#include <isoline/api/Imports.hpp>
#include <isoline/api/Module.hpp>
#include <isoline/api/Store.hpp>
#include <isoline/api/Types.hpp>
#include <isoline/api/testing/Contract.hpp>

using isoline::api::CallError;
using isoline::api::CallFailure;
using isoline::api::Engine;
using isoline::api::HeapMemoryAllocator;
using isoline::api::I32;
using isoline::api::Imports;
using isoline::api::Instance;
using isoline::api::InterruptState;
using isoline::api::Module;
using isoline::api::Store;
using isoline::api::Value;
using isoline::api::testing::contract::addModule;
using isoline::api::testing::contract::callI32;
using isoline::api::testing::contract::configOf;
using isoline::api::testing::contract::functionOf;
using isoline::api::testing::contract::GateOutcome;
using isoline::api::testing::contract::Handover;
using isoline::api::testing::contract::HandoverInterrupt;
using isoline::api::testing::contract::HostAnswer;
using isoline::api::testing::contract::instanceModule;
using isoline::api::testing::contract::JoiningThread;
using isoline::api::testing::contract::onPreparedThread;
using isoline::api::testing::contract::Runtime;
using isoline::api::testing::contract::SpinBackstopSteps;
using isoline::api::testing::contract::StartGate;
using isoline::api::testing::contract::ThreadDeadline;
using isoline::api::testing::contract::Worker;

TEST_CASE("Threads.instantiatesConcurrently", "[threading]")
{
    // One Module, shared by 8 threads, each with a store of its own: every round instantiates it and
    // calls add(1, 2).
    constexpr auto ThreadCount = 8;
    constexpr auto Rounds = 1'000;
    auto const engine = Engine { configOf({}) };
    auto const loaded = engine.load(addModule().build());
    REQUIRE(loaded.has_value());
    auto const& module = *loaded;
    auto const imports = Imports {};
    auto gate = StartGate { ThreadCount };
    /// What one thread saw: how its start went, and each round's sum.
    struct Outcomes
    {
        GateOutcome gate = GateOutcome::TimedOut;
        std::vector<std::optional<Value>> sums;
    };
    auto const instantiateMany = [&] {
        auto outcomes = Outcomes {};
        auto const preparation = engine.prepareThread();
        auto allocator = HeapMemoryAllocator {};
        auto store = Store { engine, allocator };
        outcomes.gate = gate.arriveAndWait();
        if (!preparation.has_value())
            return outcomes;
        for ([[maybe_unused]] auto const round: std::views::iota(0, Rounds))
        {
            auto const instance = store.instantiate(module, imports);
            auto const add = instance.has_value() ? instance->function("add") : std::nullopt;
            auto const args = std::array<Value, 2> { I32 { 1 }, I32 { 2 } };
            auto results = std::array<Value, 1> {};
            auto const called = add.has_value() && instance->call(*add, args, results).has_value();
            outcomes.sums.push_back(called ? std::optional { results[0] } : std::nullopt);
        }
        return outcomes;
    };
    // Threads of their own (Worker), so that all of them reach the gate however few cores there are.
    auto workers = std::deque<Worker<Outcomes>> {};
    for ([[maybe_unused]] auto const thread: std::views::iota(0, ThreadCount))
        workers.emplace_back(instantiateMany);
    auto gates = std::vector<GateOutcome> {};
    auto sums = std::vector<std::optional<Value>> {};
    for (auto& worker: workers)
    {
        auto outcomes = worker.result("an instantiating thread");
        gates.push_back(outcomes.gate);
        std::ranges::move(outcomes.sums, std::back_inserter(sums));
    }
    CHECK(std::ranges::all_of(gates,
                              [](GateOutcome gateOutcome) { return gateOutcome == GateOutcome::Opened; }));
    CHECK(sums.size() == std::size_t { ThreadCount } * Rounds);
    CHECK(std::ranges::all_of(sums, [](auto const& sum) { return sum == Value { I32 { 3 } }; }));
}

TEST_CASE("Threads.instanceMovesBetweenThreads", "[threading]")
{
    // Loaded on thread A, its store made and the module instantiated on B, called on C, one after the
    // other, and the store destroyed on the test thread: no state is the thread's own.
    auto const engine = Engine { configOf({}) };
    auto allocator = HeapMemoryAllocator {};
    auto const loaded = onPreparedThread(engine, [&] { return engine.load(addModule().build()); });
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->has_value());
    auto store = std::unique_ptr<Store> {};
    auto const created = onPreparedThread(engine, [&] {
        store = std::make_unique<Store>(engine, allocator);
        return store->instantiate(**loaded, Imports {});
    });
    REQUIRE(created.has_value());
    REQUIRE(created->has_value());
    auto const instance = **created;
    auto const add = functionOf(instance, "add");
    auto const sum = onPreparedThread(engine, [&] { return callI32(instance, add, std::array { 1U, 2U }); });
    REQUIRE(sum.has_value());
    CHECK(*sum == std::vector { 3U });
    store.reset();
}

TEST_CASE("Threads.watchdogInterruptsASpinningCall", "[threading]")
{
    // A watchdog thread interrupts the spinning call (at a fixed back-edge, where the call waits for it:
    // see HandoverInterrupt), and keeps the store's interrupter past the store:
    // interrupting it then is harmless, and it still reads Interrupted.
    auto handover = HandoverInterrupt {};
    auto handedOver = Handover::TimedOut;
    auto afterStoreGone = std::optional<InterruptState> {};
    auto spun = std::optional<std::expected<std::vector<uint32_t>, CallError>> {};
    auto watchdog = std::optional<JoiningThread> {};
    // After the watchdog: on any way out it is destroyed first, which wakes the watchdog as a broken
    // promise before the watchdog is joined.
    auto storeGone = std::promise<void> {};
    {
        auto runtime = Runtime { configOf({}, SpinBackstopSteps), nullptr, HostAnswer::Return, &handover };
        auto const instance = runtime.instantiate(runtime.load(instanceModule()));
        watchdog.emplace([&handover,
                          &handedOver,
                          &afterStoreGone,
                          interrupter = runtime.store().interrupter(),
                          gone = storeGone.get_future()] {
            handedOver = handover.interruptAtHandover(*interrupter);
            if (gone.wait_for(ThreadDeadline) != std::future_status::ready)
                return;
            interrupter->interrupt();
            afterStoreGone = interrupter->state();
        });
        spun = callI32(instance, "spin");
        handover.finish();
    }
    storeGone.set_value();
    watchdog.reset();
    CHECK(handedOver == Handover::Interrupted);
    REQUIRE(spun.has_value());
    REQUIRE_FALSE(spun->has_value());
    CHECK(spun->error().failure() == CallFailure::Interrupted);
    CHECK(afterStoreGone == InterruptState::Interrupted);
}
