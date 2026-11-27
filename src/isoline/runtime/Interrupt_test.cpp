// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <format>
#include <memory>
#include <thread>

#include <isoline/runtime/Interrupt.hpp>
#include <isoline/runtime/LinearMemory.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/testing/PollCounter.hpp>

using isoline::runtime::HeapMemoryAllocator;
using isoline::runtime::Interrupter;
using isoline::runtime::InterruptState;
using isoline::runtime::PollSite;
using isoline::runtime::PollSiteTable;
using isoline::runtime::StepCharge;
using isoline::runtime::Store;
using isoline::runtime::StoreLimits;
using isoline::runtime::StoreServices;
using isoline::runtime::Trap;
using isoline::runtime::TrapCode;
using isoline::runtime::testing::PollCounter;

TEST_CASE("Interrupter.isWaitFreeAndSticky")
{
    auto interrupter = Interrupter {};
    STATIC_CHECK(noexcept(interrupter.interrupt()));
    STATIC_CHECK(noexcept(interrupter.state()));
    CHECK(interrupter.state() == InterruptState::Running);
    interrupter.interrupt();
    interrupter.interrupt();
    CHECK(interrupter.state() == InterruptState::Interrupted);
}

TEST_CASE("Interrupt.pollSitesPublishTheirRules")
{
    for (auto const& row: PollSiteTable)
    {
        CHECK_FALSE(row.name.empty());
        CHECK_FALSE(row.rule.empty());
        // Unwinding charges no step: how many clauses an engine scans is its own business.
        CHECK(row.charge == (row.site == PollSite::Unwind ? StepCharge::None : StepCharge::OneStep));
    }
    // The rules state the constants executors poll by.
    auto const ruleOf = [](PollSite site) {
        return PollSiteTable[static_cast<std::size_t>(site)].rule;
    };
    CHECK(ruleOf(PollSite::BulkGranule)
              .contains(std::format("every {} bytes", isoline::runtime::BulkPollBytes)));
    CHECK(ruleOf(PollSite::BulkGranule)
              .contains(std::format("every {} elements", isoline::runtime::BulkPollElements)));
    CHECK(
        ruleOf(PollSite::Unwind).contains(std::format("every {} labels", isoline::runtime::UnwindPollItems)));
    STATIC_CHECK(isoline::runtime::BulkPollBytes == 65'536);
    STATIC_CHECK(isoline::runtime::BulkPollElements
                 == isoline::runtime::BulkPollBytes / sizeof(isoline::runtime::Ref));
    STATIC_CHECK(isoline::runtime::UnwindPollItems == 4'096);
    CHECK(ruleOf(PollSite::Sweep).contains(std::format("every {} items", isoline::runtime::SweepPollItems)));
    STATIC_CHECK(isoline::runtime::SweepPollItems == 4'096);
}

TEST_CASE("Store.pollNotifiesThenChecksTheInterrupt")
{
    auto allocator = HeapMemoryAllocator {};
    auto counter = PollCounter {};
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator, .pollObserver = &counter } };
    REQUIRE(store.interrupter() != nullptr);
    CHECK(store.poll(PollSite::FunctionEntry).has_value());
    store.interrupter()->interrupt();
    // Interrupted, and the observer still sees the poll.
    CHECK(store.poll(PollSite::LoopBackEdge).error() == Trap { TrapCode::Interrupted });
    CHECK(store.poll(PollSite::LoopBackEdge).error() == Trap { TrapCode::Interrupted });
    CHECK(counter.count(PollSite::FunctionEntry) == 1);
    CHECK(counter.count(PollSite::LoopBackEdge) == 2);
}

TEST_CASE("Store.interrupterOutlivesTheStore")
{
    auto allocator = HeapMemoryAllocator {};
    auto interrupter = std::shared_ptr<Interrupter> {};
    {
        auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
        interrupter = store.interrupter();
    }
    interrupter->interrupt();
    CHECK(interrupter->state() == InterruptState::Interrupted);
}

TEST_CASE("Interrupter.isSeenAcrossThreads", "[threading]")
{
    // A poll on one thread sees an interrupt raised on another, with no lock between them.
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { StoreLimits {}, StoreServices { .allocator = allocator } };
    auto const interrupter = store.interrupter();
    // A std::thread joined before the assertions: Apple's libc++ has no std::jthread.
    auto watchdog = std::thread { [interrupter] { interrupter->interrupt(); } };
    // Polls until the interrupt is seen, or the deadline passes, which fails the test.
    auto const deadline = std::chrono::steady_clock::now() + isoline::runtime::testing::WatchdogDeadline;
    auto polled = store.poll(PollSite::LoopBackEdge);
    while (polled && std::chrono::steady_clock::now() < deadline)
        polled = store.poll(PollSite::LoopBackEdge);
    watchdog.join();
    REQUIRE_FALSE(polled.has_value());
    CHECK(polled.error() == Trap { TrapCode::Interrupted });
    CHECK(store.poll(PollSite::FunctionEntry).error() == Trap { TrapCode::Interrupted });
}
