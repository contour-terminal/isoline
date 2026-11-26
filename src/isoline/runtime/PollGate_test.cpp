// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <ranges>
#include <vector>

#include <isoline/binary/Types.hpp>
#include <isoline/runtime/Interrupt.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/LinearMemory.hpp>
#include <isoline/runtime/PollGate.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/testing/PollCounter.hpp>

using isoline::runtime::HeapMemoryAllocator;
using isoline::runtime::InvocationScope;
using isoline::runtime::IPollObserver;
using isoline::runtime::Limit;
using isoline::runtime::PollGate;
using isoline::runtime::PollSite;
using isoline::runtime::PollSiteTable;
using isoline::runtime::Store;
using isoline::runtime::StoreLimits;
using isoline::runtime::StoreServices;
using isoline::runtime::Trap;
using isoline::runtime::TrapCode;
using isoline::runtime::testing::PollCounter;
using isoline::runtime::testing::PollTrigger;

namespace
{

/// A store with its own allocator, in an invocation, so that its step budget is fresh.
struct InvokedStore
{
    /// @param limits The store's limits.
    /// @param observer Who observes its polls; none when null.
    explicit InvokedStore(StoreLimits limits, IPollObserver* observer = nullptr):
        store { limits, StoreServices { .allocator = allocator, .pollObserver = observer } }
    {
    }

    HeapMemoryAllocator allocator;
    Store store;
    InvocationScope invocation { store };
};

} // namespace

TEST_CASE("PollGate.agreesWithStorePollOnEverySequence")
{
    // A fixed-seed xorshift64, so that every run and every platform polls the same sequences.
    auto state = uint64_t { 0x9E37'79B9'7F4A'7C15 };
    auto const draw = [&state](uint64_t bound) {
        state ^= state << 13U;
        state ^= state >> 7U;
        state ^= state << 17U;
        return state % bound;
    };
    constexpr auto StepBudgets = std::array<uint64_t, 4> { 0, 1, 7, 1'000 };
    constexpr auto MaxPolls = uint64_t { 48 };
    for (auto const sequence: std::views::iota(0, 2'000))
    {
        auto const maxSteps = StepBudgets[static_cast<std::size_t>(sequence) % StepBudgets.size()];
        auto const polls = 1 + draw(MaxPolls);
        // An index past the last poll never interrupts.
        auto const interruptAt = draw(polls + 1);
        auto reference = InvokedStore { StoreLimits { .maxSteps = maxSteps } };
        auto twin = InvokedStore { StoreLimits { .maxSteps = maxSteps } };
        auto gate = PollGate { twin.store };
        auto stepsLeft = gate.reload();
        for (auto const index: std::views::iota(uint64_t { 0 }, polls))
        {
            if (index == interruptAt)
            {
                reference.store.interrupter()->interrupt();
                twin.store.interrupter()->interrupt();
            }
            auto const site = PollSiteTable[draw(PollSiteTable.size())].site;
            INFO(std::format("sequence {}, poll {}, site {}, maxSteps {}",
                             sequence,
                             index,
                             PollSiteTable[static_cast<std::size_t>(site)].name,
                             maxSteps));
            auto const expected = reference.store.poll(site);
            auto const actual = gate.poll(site, stepsLeft);
            REQUIRE(actual.has_value() == expected.has_value());
            if (!expected.has_value())
                REQUIRE(actual.error() == expected.error());
        }
        gate.commit(stepsLeft);
        REQUIRE(twin.store.used(Limit::Steps) == reference.store.used(Limit::Steps));
    }
}

TEST_CASE("PollGate.interruptBeatsStepLimit")
{
    auto invoked = InvokedStore { StoreLimits { .maxSteps = 0 } };
    invoked.store.interrupter()->interrupt();
    auto gate = PollGate { invoked.store };
    auto stepsLeft = gate.reload();
    REQUIRE(stepsLeft == 0);
    auto const polled = gate.poll(PollSite::LoopBackEdge, stepsLeft);
    REQUIRE_FALSE(polled.has_value());
    CHECK(polled.error().code == TrapCode::Interrupted);
}

TEST_CASE("PollGate.observerSeesEveryPoll")
{
    SECTION("each poll, Unwind and Sweep included, charged as the store charges it")
    {
        auto counter = PollCounter {};
        auto invoked = InvokedStore { StoreLimits { .maxSteps = 10 }, &counter };
        auto gate = PollGate { invoked.store };
        auto stepsLeft = gate.reload();
        for (auto const site: { PollSite::LoopBackEdge,
                                PollSite::Unwind,
                                PollSite::LoopBackEdge,
                                PollSite::Sweep,
                                PollSite::Unwind })
            REQUIRE(gate.poll(site, stepsLeft).has_value());
        CHECK(counter.count(PollSite::LoopBackEdge) == 2);
        CHECK(counter.count(PollSite::Unwind) == 2);
        CHECK(counter.count(PollSite::Sweep) == 1);
        // The slow path keeps the countdown and the store in step: 3 charged, Unwind free.
        CHECK(stepsLeft == 7);
        CHECK(invoked.store.used(Limit::Steps) == 3);
    }

    SECTION("before the interrupt is checked")
    {
        // The observer interrupts at the second back-edge: that very poll ends the call.
        auto counter = PollCounter { PollTrigger { .site = PollSite::LoopBackEdge, .ordinal = 2 } };
        auto invoked = InvokedStore { StoreLimits {}, &counter };
        counter.interruptAtTrigger(invoked.store.interrupter());
        auto gate = PollGate { invoked.store };
        auto stepsLeft = gate.reload();
        REQUIRE(gate.poll(PollSite::LoopBackEdge, stepsLeft).has_value());
        auto const polled = gate.poll(PollSite::LoopBackEdge, stepsLeft);
        REQUIRE_FALSE(polled.has_value());
        CHECK(polled.error() == Trap { TrapCode::Interrupted });
        CHECK(counter.count(PollSite::LoopBackEdge) == 2);
    }
}

TEST_CASE("PollGate.commitSharesTheBudgetWithStorePolls")
{
    // A nested invocation's entry poll and an exception sweep's poll: two polls through the store,
    // between the gate's own, all against one budget.
    auto invoked = InvokedStore { StoreLimits { .maxExceptions = 1, .maxSteps = 6 } };
    auto& store = invoked.store;
    auto const tag = store.addTag(store.internType(isoline::binary::FuncType {})).value();
    REQUIRE(store.addException(tag, {}).has_value());
    auto gate = PollGate { store };
    auto stepsLeft = gate.reload();
    for ([[maybe_unused]] auto const poll: std::views::iota(0, 3))
        REQUIRE(gate.poll(PollSite::LoopBackEdge, stepsLeft).has_value());
    gate.commit(stepsLeft);
    {
        auto const nested = InvocationScope { store };
        REQUIRE(store.poll(PollSite::FunctionEntry).has_value());
        // The arena is full: the allocation sweeps, one item, one poll at Sweep.
        REQUIRE(store.addException(tag, {}).has_value());
    }
    REQUIRE(store.used(Limit::Steps) == 5);
    stepsLeft = gate.reload();
    CHECK(stepsLeft == 1);
    REQUIRE(gate.poll(PollSite::LoopBackEdge, stepsLeft).has_value());
    gate.commit(stepsLeft);
    CHECK(store.used(Limit::Steps) == 6);
    // The budget is spent: the next poll, through the gate or the store, is refused.
    auto const refused = gate.poll(PollSite::LoopBackEdge, stepsLeft);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == Trap { TrapCode::StepLimitExceeded });
    gate.commit(stepsLeft);
    CHECK(store.used(Limit::Steps) == 6);
}

TEST_CASE("PollGate.unchargedSitesCostNothing")
{
    auto invoked = InvokedStore { StoreLimits { .maxSteps = 1 } };
    auto gate = PollGate { invoked.store };
    auto stepsLeft = gate.reload();
    for ([[maybe_unused]] auto const poll: std::views::iota(0, 10))
        REQUIRE(gate.poll(PollSite::Unwind, stepsLeft).has_value());
    CHECK(stepsLeft == 1);
    REQUIRE(gate.poll(PollSite::LoopBackEdge, stepsLeft).has_value());
    CHECK(stepsLeft == 0);
    auto const refused = gate.poll(PollSite::LoopBackEdge, stepsLeft);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == Trap { TrapCode::StepLimitExceeded });
    // No step charged by the refused poll.
    CHECK(stepsLeft == 0);
    gate.commit(stepsLeft);
    CHECK(invoked.store.used(Limit::Steps) == 1);
}
