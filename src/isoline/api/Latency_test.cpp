// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iostream>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include <isoline/api/CallError.hpp>
#include <isoline/api/Engine.hpp>
#include <isoline/api/Imports.hpp>
#include <isoline/api/Module.hpp>
#include <isoline/api/Store.hpp>
#include <isoline/api/Types.hpp>
#include <isoline/api/testing/Contract.hpp>
#include <isoline/api/testing/EngineRows.hpp>
#include <isoline/binary/testing/Code.hpp>
#include <isoline/binary/testing/ModuleBuilder.hpp>

using isoline::api::CallFailure;
using isoline::api::Engine;
using isoline::api::EngineConfig;
using isoline::api::FuncType;
using isoline::api::HeapMemoryAllocator;
using isoline::api::I32;
using isoline::api::Imports;
using isoline::api::IPollObserver;
using isoline::api::PollSite;
using isoline::api::Store;
using isoline::api::StoreOptions;
using isoline::api::Value;
using isoline::api::testing::engineRows;
using isoline::api::testing::contract::addModule;
using isoline::api::testing::contract::configOf;
using isoline::api::testing::contract::ContractLimits;
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

/// The calls one batch of the call-latency benchmark times together: enough that the clock's
/// resolution and the loop around them vanish in the batch's duration.
constexpr auto CallsPerBatch = std::size_t { 100'000 };

/// The batches the call-latency benchmark times, of which it reports the median and the fastest.
constexpr auto CallBatches = std::size_t { 31 };

/// What the call-latency benchmark measured of one entry: the duration of a call, per batch.
struct CallTimes
{
    double medianNanoseconds = 0;  ///< The median batch's duration, per call.
    double fastestNanoseconds = 0; ///< The fastest batch's duration, per call.
    std::size_t failures = 0;      ///< The calls that failed or returned a wrong sum: 0 in a valid run.
};

/// Times one batch of CallsPerBatch calls of @p call, which adds its two arguments.
/// @param call Calls the guest's add with the arguments and the result slot; returns whether it did.
/// @param failures Counts the calls that failed or returned a wrong sum.
/// @return The batch's duration.
template <typename Call>
Clock::duration timeBatch(Call const& call, std::size_t& failures)
{
    auto const startedAt = Clock::now();
    for (auto const index: std::views::iota(std::size_t { 0 }, CallsPerBatch))
    {
        auto const args = std::array<Value, 2> { I32 { static_cast<uint32_t>(index) }, I32 { 1 } };
        auto results = std::array<Value, 1> {};
        if (!call(std::span<Value const> { args }, std::span<Value> { results })
            || std::get<I32>(results[0]).bits != static_cast<uint32_t>(index) + 1)
            ++failures;
    }
    return Clock::now() - startedAt;
}

/// What the batches of one entry measured.
/// @param perBatch The batches' durations (not empty).
/// @param failures The calls among them that failed.
CallTimes callTimesOf(std::vector<Clock::duration> const& perBatch, std::size_t failures)
{
    auto const perCall = [](Clock::duration batch) {
        return std::chrono::duration<double, std::nano> { batch }.count() / double { CallsPerBatch };
    };
    return CallTimes { .medianNanoseconds = perCall(percentile(perBatch, 50)),
                       .fastestNanoseconds = perCall(percentile(perBatch, 1)),
                       .failures = failures };
}

/// Times CallBatches batches of each of @p first and @p second, alternating which of the two runs first
/// from batch to batch, so that neither is always timed on the cache and clock state the other left.
/// @return What each measured, @p first's first.
template <typename First, typename Second>
std::array<CallTimes, 2> timeAlternating(First const& first, Second const& second)
{
    auto firstBatches = std::vector<Clock::duration> {};
    auto secondBatches = std::vector<Clock::duration> {};
    firstBatches.reserve(CallBatches);
    secondBatches.reserve(CallBatches);
    auto firstFailures = std::size_t { 0 };
    auto secondFailures = std::size_t { 0 };
    for (auto const batch: std::views::iota(std::size_t { 0 }, CallBatches))
    {
        if (batch % 2 == 0)
        {
            firstBatches.push_back(timeBatch(first, firstFailures));
            secondBatches.push_back(timeBatch(second, secondFailures));
        }
        else
        {
            secondBatches.push_back(timeBatch(second, secondFailures));
            firstBatches.push_back(timeBatch(first, firstFailures));
        }
    }
    return { callTimesOf(firstBatches, firstFailures), callTimesOf(secondBatches, secondFailures) };
}

/// Reports what @p times measured of the entry @p entry.
void report(std::string_view entry, CallTimes const& times)
{
    std::cout << std::format("{} over {} batches of {} calls: median {:.1f} ns, fastest {:.1f} ns\n",
                             entry,
                             CallBatches,
                             CallsPerBatch,
                             times.medianNanoseconds,
                             times.fastestNanoseconds);
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
        auto store =
            Store { engine, allocator, StoreOptions { .limits = std::nullopt, .observer = &observer } };
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

TEST_CASE("Latency.callRoundTrip", "[.perf]")
{
    // The cost of one call into the guest and back, the checks of every call (the alive-token, the
    // handle's store, the arguments) included: the guest's add of two i32s, so that the entry, not the
    // guest, is what is timed. Instance::call() checks the alive-token, then enters as Store::call()
    // does; the two are timed batch for batch, alternating which goes first.
    auto const row = GENERATE(from_range(engineRows()));
    CAPTURE(row);
    auto const engine = Engine { configOf(ContractLimits {}, row) };
    auto const module = engine.load(addModule().build());
    REQUIRE(module.has_value());
    auto allocator = HeapMemoryAllocator {};
    auto store = Store { engine, allocator };
    auto const instance = store.instantiate(*module, Imports {});
    REQUIRE(instance.has_value());
    auto const add = instance->function("add");
    REQUIRE(add.has_value());

    auto const [viaInstance, viaStore] = timeAlternating(
        [&](std::span<Value const> args, std::span<Value> results) {
            return instance->call(*add, args, results).has_value();
        },
        [&](std::span<Value const> args, std::span<Value> results) {
            return store.call(*add, args, results).has_value();
        });
    std::cout << std::format("engine {}:\n", row.name);
    report("Instance::call", viaInstance);
    report("Store::call", viaStore);
    CHECK(viaInstance.failures == 0);
    CHECK(viaStore.failures == 0);
}
