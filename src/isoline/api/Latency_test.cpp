// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iostream>
#include <optional>
#include <ranges>
#include <thread>
#include <vector>

#include <isoline/api/CallError.hpp>
#include <isoline/api/Engine.hpp>
#include <isoline/api/Imports.hpp>
#include <isoline/api/Module.hpp>
#include <isoline/api/Store.hpp>
#include <isoline/api/Types.hpp>
#include <isoline/api/testing/Contract.hpp>
#include <isoline/binary/testing/Code.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>

using isoline::api::CallFailure;
using isoline::api::Engine;
using isoline::api::EngineConfig;
using isoline::api::FuncType;
using isoline::api::HeapMemoryAllocator;
using isoline::api::Imports;
using isoline::api::IPollObserver;
using isoline::api::PollSite;
using isoline::api::Store;
using isoline::api::testing::contract::JoiningThread;
using isoline::api::testing::contract::ThreadDeadline;
using isoline::binary::testing::br;
using isoline::binary::testing::loop;
using isoline::binary::testing::ModuleBuilder;

namespace
{

using Clock = std::chrono::steady_clock;

/// The trials measured: enough that the 99th percentile is not the slowest sample.
constexpr auto Trials = std::size_t { 200 };

/// How long the call spins before the watchdog interrupts it, so that the interrupt lands in the
/// middle of the loop rather than at the call's first poll.
constexpr auto SpinBeforeInterrupt = std::chrono::microseconds { 500 };

/// The bound the specification sets (§7): p99 interrupt-to-return within 10 ms, a fifth of the grace
/// Contour gives a guest before it kills the helper.
constexpr auto LatencyBound = std::chrono::milliseconds { 10 };

/// A backstop for a broken interrupt: the steps a spinning call may take, far beyond what a working
/// interrupt lets it, so that such a call ends as StepLimitExceeded rather than spinning on; CTest's
/// timeout for isoline_interrupt_latency (300 s) ends the run first where 2^34 back-edges take longer.
constexpr auto BackstopSteps = uint64_t { 1 } << 34;

/// Tells the watchdog that the guest runs: the first poll of the call releases running. Every later
/// poll costs one test of a member only the executing thread touches.
class FirstPoll final: public IPollObserver
{
  public:
    void onPoll(PollSite /*site*/) noexcept override
    {
        if (_polled)
            return;
        _polled = true;
        _running.store(true, std::memory_order_release);
    }

    /// Waits, at most ThreadDeadline, for the call's first poll.
    /// @return Whether the call polled before the deadline.
    [[nodiscard]] bool waitUntilRunning() const noexcept
    {
        auto const deadline = Clock::now() + ThreadDeadline;
        while (!_running.load(std::memory_order_acquire))
        {
            if (Clock::now() >= deadline)
                return false;
            std::this_thread::yield();
        }
        return true;
    }

  private:
    bool _polled = false;                 ///< Read and written on the executing thread only.
    std::atomic<bool> _running { false }; ///< Released at the first poll.
};

/// The @p percent-th percentile of @p samples, by the nearest-rank method.
/// @param samples The samples (not empty).
/// @param percent The percentile, 1 to 100.
/// @return The smallest sample at least @p percent percent of the samples do not exceed.
Clock::duration percentile(std::vector<Clock::duration> samples, std::size_t percent)
{
    std::ranges::sort(samples);
    auto const rank = ((percent * samples.size()) + 99) / 100;
    return samples[rank - 1];
}

/// The duration @p duration in microseconds, for the report.
double microseconds(Clock::duration duration)
{
    return std::chrono::duration<double, std::micro> { duration }.count();
}

} // namespace

TEST_CASE("Latency.interruptToReturnP99", "[.perf]")
{
    // Each trial spins in a fresh store (the interrupt is final), on the test thread; a watchdog waits
    // for the guest's first poll, lets it spin a while, stamps t0 and interrupts; t1 is stamped when
    // the call returns. The spin is the shortest loop: every iteration a back-edge poll.
    auto config = EngineConfig {};
    config.store.maxSteps = BackstopSteps;
    auto const engine = Engine { config };
    auto builder = ModuleBuilder {};
    builder.type(FuncType {}).function(0, {}, loop(br(0))).exportFunction("spin", 0);
    auto const module = engine.load(builder.build());
    REQUIRE(module.has_value());
    auto allocator = HeapMemoryAllocator {};
    auto const imports = Imports {};

    auto samples = std::vector<Clock::duration> {};
    samples.reserve(Trials);
    for ([[maybe_unused]] auto const trial: std::views::iota(std::size_t { 0 }, Trials))
    {
        auto observer = FirstPoll {};
        auto store = Store { engine, allocator, &observer };
        auto const instance = store.instantiate(*module, imports);
        REQUIRE(instance.has_value());
        auto const spin = instance->function("spin");
        REQUIRE(spin.has_value());

        auto interruptedAt = std::optional<Clock::time_point> {};
        auto watchdog = JoiningThread { [&observer, &interruptedAt, interrupter = store.interrupter()] {
            if (!observer.waitUntilRunning())
                return;
            std::this_thread::sleep_for(SpinBeforeInterrupt);
            interruptedAt = Clock::now();
            interrupter->interrupt();
        } };
        auto const called = instance->call(*spin, {}, {});
        auto const returnedAt = Clock::now();
        watchdog.join();

        REQUIRE_FALSE(called.has_value());
        REQUIRE(called.error().failure() == CallFailure::Interrupted);
        REQUIRE(interruptedAt.has_value());
        samples.push_back(returnedAt - *interruptedAt);
    }

    auto const p99 = percentile(samples, 99);
    std::cout << std::format(
        "interrupt-to-return over {} trials: p50 {:.1f} us, p99 {:.1f} us, max {:.1f} us\n",
        Trials,
        microseconds(percentile(samples, 50)),
        microseconds(p99),
        microseconds(percentile(samples, 100)));
    CHECK(p99 <= LatencyBound);
}
