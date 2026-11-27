// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <ranges>
#include <tuple>
#include <vector>

#include <isoline/binary/Types.hpp>
#include <isoline/runtime/Interrupt.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/LinearMemory.hpp>
#include <isoline/runtime/PollGate.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/testing/ContractViolation.hpp>
#include <isoline/runtime/testing/PollCounter.hpp>

using isoline::runtime::BudgetState;
using isoline::runtime::HeapMemoryAllocator;
using isoline::runtime::InvocationScope;
using isoline::runtime::IPollObserver;
using isoline::runtime::Lease;
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

TEST_CASE("PollGate.lentStepsSettleExactly")
{
    // Generated code counts the lent steps down itself; settling writes back what it took, absolutely.
    auto invoked = InvokedStore { StoreLimits { .maxStackValues = 50, .maxSteps = 100 } };
    auto& store = invoked.store;
    auto gate = PollGate { store };
    REQUIRE(store.budgetState() == BudgetState::Settled);
    auto const lent = gate.lend();
    CHECK(store.budgetState() == BudgetState::Lent);
    CHECK(lent.steps == 100);
    CHECK(lent.stackValues == 50);
    auto left = lent;
    left.steps -= 30;
    gate.settle(lent, left);
    CHECK(store.budgetState() == BudgetState::Settled);
    CHECK(store.used(Limit::Steps) == 30);
    CHECK(store.chargedStackValues() == 0);

    // The next lease lends what is left, and settling it untouched takes nothing more.
    auto const again = gate.lend();
    CHECK(again.steps == 70);
    gate.settle(again, again);
    CHECK(store.used(Limit::Steps) == 30);
    CHECK(store.budgetState() == BudgetState::Settled);
}

TEST_CASE("PollGate.observerLendsNoSteps")
{
    // Under an observer every poll must reach Store::poll(): the lease lends no step, so generated
    // code takes its slow path at every poll, and settling leaves the store's own count alone.
    auto counter = PollCounter {};
    auto invoked = InvokedStore { StoreLimits { .maxStackValues = 50, .maxSteps = 10 }, &counter };
    auto& store = invoked.store;
    REQUIRE(store.poll(PollSite::FunctionEntry).has_value());
    REQUIRE(store.used(Limit::Steps) == 1);
    auto gate = PollGate { store };
    auto const lent = gate.lend();
    CHECK(lent.steps == 0);
    CHECK(lent.stackValues == 50);
    gate.settle(lent, lent);
    CHECK(store.used(Limit::Steps) == 1);
    CHECK(counter.count(PollSite::FunctionEntry) == 1);
}

TEST_CASE("PollGate.chargeInOneLeaseReleaseInAnother")
{
    // A frame charged under one lease and released under a later one balances, and a frame the store
    // charged before either lease stays charged throughout: stack values settle absolutely.
    auto invoked = InvokedStore { StoreLimits { .maxStackValues = 100 } };
    auto& store = invoked.store;
    REQUIRE(store.chargeFrame(5).has_value());
    auto gate = PollGate { store };

    auto const first = gate.lend();
    CHECK(first.stackValues == 95);
    auto left = first;
    left.stackValues -= 12; // a callee's frame, charged inline
    gate.settle(first, left);
    CHECK(store.chargedStackValues() == 17);

    auto const second = gate.lend();
    CHECK(second.stackValues == 83);
    left = second;
    left.stackValues += 12; // the same frame, released under this lease
    gate.settle(second, left);
    CHECK(store.chargedStackValues() == 5);
    store.releaseFrame(5);
    CHECK(store.chargedStackValues() == 0);
}

TEST_CASE("PollGate.pollAfterExhaustedLeaseSettlesExactly")
{
    // maxSteps 5: generated code takes 5 steps inline, then the sixth poll finds the countdown at 0
    // and takes the slow path, which settles, polls through the store and lends again.
    auto invoked = InvokedStore { StoreLimits { .maxSteps = 5 } };
    auto& store = invoked.store;
    auto gate = PollGate { store };
    auto const lent = gate.lend();
    REQUIRE(lent.steps == 5);
    auto left = lent;
    for ([[maybe_unused]] auto const poll: std::views::iota(0, 5))
        --left.steps;
    gate.settle(lent, left);
    auto const slow = store.poll(PollSite::LoopBackEdge);
    REQUIRE_FALSE(slow.has_value());
    CHECK(slow.error().code == TrapCode::StepLimitExceeded);
    CHECK(store.used(Limit::Steps) == 5);
    auto const relent = gate.lend();
    CHECK(relent.steps == 0);
    gate.settle(relent, relent);
    CHECK(store.used(Limit::Steps) == 5);
}

// The contract tests below end the process at a failed Require, so they are hidden from the unit run:
// tests/CMakeLists.txt runs each in a process of its own and expects the Require's text
// (isoline::runtime::testing::endAtContractViolation()).

TEST_CASE("Contract.PollGate.observedPollNeedsTheStoresCountdown", "[.contract]")
{
    // An executor that polled through the store and forgot to reload: on an observed store, the gate's
    // next poll catches it rather than quietly taking the store's count.
    isoline::runtime::testing::endAtContractViolation();
    auto counter = PollCounter {};
    auto invoked = InvokedStore { StoreLimits {}, &counter };
    auto gate = PollGate { invoked.store };
    auto stepsLeft = gate.reload();
    REQUIRE(invoked.store.poll(PollSite::FunctionEntry).has_value());
    std::ignore = gate.poll(PollSite::LoopBackEdge, stepsLeft);
    FAIL("SURVIVED: an observed poll accepted a countdown the store did not hold");
}

TEST_CASE("Contract.PollGate.commitWithinTheBudget", "[.contract]")
{
    // More steps left than the budget holds: a countdown that never came from reload().
    isoline::runtime::testing::endAtContractViolation();
    auto invoked = InvokedStore { StoreLimits { .maxSteps = 5 } };
    auto gate = PollGate { invoked.store };
    gate.commit(6);
    FAIL("SURVIVED: commit accepted more steps left than maxSteps");
}

TEST_CASE("Contract.PollGate.lendOnlyWhenSettled", "[.contract]")
{
    // A second lease over an unsettled one: the first lease's steps would be counted twice.
    isoline::runtime::testing::endAtContractViolation();
    auto invoked = InvokedStore { StoreLimits {} };
    auto gate = PollGate { invoked.store };
    std::ignore = gate.lend();
    std::ignore = gate.lend();
    FAIL("SURVIVED: a lease was lent over an unsettled one");
}

TEST_CASE("Contract.PollGate.settleOnlyALease", "[.contract]")
{
    isoline::runtime::testing::endAtContractViolation();
    auto invoked = InvokedStore { StoreLimits {} };
    auto gate = PollGate { invoked.store };
    gate.settle(Lease { .steps = 0, .stackValues = 0 }, Lease { .steps = 0, .stackValues = 0 });
    FAIL("SURVIVED: a settle without a lease");
}

TEST_CASE("Contract.PollGate.settleNoMoreStepsThanLent", "[.contract]")
{
    // More steps left than lent: a countdown that went up, which no poll does.
    isoline::runtime::testing::endAtContractViolation();
    auto invoked = InvokedStore { StoreLimits { .maxSteps = 10 } };
    auto& store = invoked.store;
    REQUIRE(store.poll(PollSite::LoopBackEdge).has_value());
    auto gate = PollGate { store };
    auto const lent = gate.lend();
    auto left = lent;
    ++left.steps;
    gate.settle(lent, left);
    FAIL("SURVIVED: a settle with more steps left than lent");
}

TEST_CASE("Contract.PollGate.settleWithinTheStackBudget", "[.contract]")
{
    // More stack values left than the budget holds: a release of a frame that was never charged.
    isoline::runtime::testing::endAtContractViolation();
    auto invoked = InvokedStore { StoreLimits { .maxStackValues = 10 } };
    auto gate = PollGate { invoked.store };
    auto const lent = gate.lend();
    auto left = lent;
    ++left.stackValues;
    gate.settle(lent, left);
    FAIL("SURVIVED: a settle with more stack values left than maxStackValues");
}
