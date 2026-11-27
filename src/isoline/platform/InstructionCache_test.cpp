// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include <isoline/platform/InstructionCache.hpp>
#include <isoline/platform/JitMemory.hpp>
#include <isoline/platform/Membarrier.hpp>
#include <isoline/platform/Platform.hpp>
#include <isoline/platform/PlatformTable.hpp>
#include <isoline/platform/ThisPlatform.hpp>
#include <isoline/platform/VirtualMemory.hpp>
#include <isoline/platform/testing/ExecutableCode.hpp>
#include <isoline/platform/testing/RecordingInstructionCache.hpp>

using namespace isoline::platform;
using isoline::platform::testing::ExecutableCode;
using isoline::platform::testing::ExecutableCodeServices;
using isoline::platform::testing::FirstFunctionOffset;
using isoline::platform::testing::InstructionCacheCall;
using isoline::platform::testing::RecordingInstructionCache;
using isoline::platform::testing::returnConstantCode;
using isoline::platform::testing::SynchronizeRefusal;

namespace
{

/// One call the fakes below saw, on one log, so that their order across both is checked.
enum class Event : uint8_t
{
    Invalidate,          ///< IInstructionCacheMaintenance::invalidate().
    SynchronizeContext,  ///< IInstructionCacheMaintenance::synchronizeContext().
    RegisterSynchronize, ///< IMembarrier::registerSynchronizeCore().
    SynchronizeCore,     ///< IMembarrier::synchronizeCore().
};

/// The calls both fakes saw, in order, and the code invalidate() was given.
struct EventLog
{
    std::vector<Event> events;                           ///< Every call.
    std::vector<std::span<std::byte const>> invalidated; ///< Every invalidate()'s code.
};

/// Cache maintenance that records its calls and refuses invalidate() with a chosen code.
class RecordingMaintenance final: public IInstructionCacheMaintenance
{
  public:
    RecordingMaintenance(EventLog& log, std::optional<int> invalidateRefusal) noexcept:
        _log { &log }, _invalidateRefusal { invalidateRefusal }
    {
    }

    [[nodiscard]] std::expected<void, int> invalidate(std::span<std::byte const> code) noexcept override
    {
        _log->events.push_back(Event::Invalidate);
        _log->invalidated.push_back(code);
        if (_invalidateRefusal)
            return std::unexpected { *_invalidateRefusal };
        return {};
    }

    void synchronizeContext() noexcept override { _log->events.push_back(Event::SynchronizeContext); }

  private:
    EventLog* _log;
    std::optional<int> _invalidateRefusal;
};

/// The refusals a FakeMembarrier answers with: errno per call, none for success.
struct MembarrierRefusals
{
    std::optional<int> registration; ///< registerSynchronizeCore()'s.
    std::optional<int> synchronize;  ///< synchronizeCore()'s.
};

/// A membarrier that records its calls and refuses as configured.
class FakeMembarrier final: public IMembarrier
{
  public:
    FakeMembarrier(EventLog& log, MembarrierRefusals refusals) noexcept: _log { &log }, _refusals { refusals }
    {
    }

    [[nodiscard]] std::expected<void, int> registerSynchronizeCore() noexcept override
    {
        _log->events.push_back(Event::RegisterSynchronize);
        if (_refusals.registration)
            return std::unexpected { *_refusals.registration };
        return {};
    }

    [[nodiscard]] std::expected<void, int> synchronizeCore() noexcept override
    {
        _log->events.push_back(Event::SynchronizeCore);
        if (_refusals.synchronize)
            return std::unexpected { *_refusals.synchronize };
        return {};
    }

  private:
    EventLog* _log;
    MembarrierRefusals _refusals;
};

/// The cache of @p kind over the fakes, which must be built.
[[nodiscard]] std::unique_ptr<IInstructionCache> fakeCache(
    InstructionCacheKind kind,
    EventLog& log,
    FakeMembarrier& membarrier,
    std::optional<int> invalidateRefusal = std::nullopt)
{
    auto cache = makeInstructionCache(
        kind, std::make_unique<RecordingMaintenance>(log, invalidateRefusal), membarrier);
    REQUIRE(cache.has_value());
    return std::move(*cache);
}

/// Some code to synchronize.
constexpr auto SomeCode = std::array<std::byte, 8> {};

/// How long a test that waits on other threads waits at most.
constexpr auto TestDeadline = std::chrono::seconds { 30 };

/// Every arm64 kind.
constexpr auto Arm64Kinds = std::to_array({ InstructionCacheKind::LinuxArm64,
                                            InstructionCacheKind::FreeBSDArm64,
                                            InstructionCacheKind::MacOSArm64,
                                            InstructionCacheKind::WindowsArm64 });

} // namespace

TEST_CASE("InstructionCache.rowsPerPlatform")
{
    // Coherent: nothing to invalidate, nobody to synchronize, and it says so by calling nothing.
    auto const& coherent = instructionCacheRow(InstructionCacheKind::Coherent);
    CHECK(coherent.invalidation == InstructionCacheInvalidation::None);
    CHECK(coherent.otherThreads == OtherThreadSynchronization::None);
    CHECK(coherent.thisThread == ThisThreadSynchronization::None);
    {
        auto log = EventLog {};
        auto membarrier = FakeMembarrier { log, {} };
        auto const cache = fakeCache(InstructionCacheKind::Coherent, log, membarrier);
        CHECK(cache->synchronize(SomeCode).has_value());
        cache->synchronizeThisThread();
        CHECK(log.events.empty());
    }

    // Each arm64 row names its operating system's call, and an isb for the calling thread.
    CHECK(instructionCacheRow(InstructionCacheKind::LinuxArm64).invalidation
          == InstructionCacheInvalidation::CompilerClearCache);
    CHECK(instructionCacheRow(InstructionCacheKind::LinuxArm64).otherThreads
          == OtherThreadSynchronization::MembarrierSynchronizeCore);
    CHECK(instructionCacheRow(InstructionCacheKind::FreeBSDArm64).invalidation
          == InstructionCacheInvalidation::CompilerClearCache);
    CHECK(instructionCacheRow(InstructionCacheKind::MacOSArm64).invalidation
          == InstructionCacheInvalidation::AppleInvalidate);
    CHECK(instructionCacheRow(InstructionCacheKind::WindowsArm64).invalidation
          == InstructionCacheInvalidation::WindowsFlush);
    for (auto const kind: Arm64Kinds)
    {
        INFO("kind " << static_cast<int>(kind));
        auto const& row = instructionCacheRow(kind);
        CHECK(row.thisThread == ThisThreadSynchronization::InstructionSynchronizationBarrier);
        // Only Linux needs membarrier: the others' invalidation covers every processor.
        CHECK((row.otherThreads == OtherThreadSynchronization::MembarrierSynchronizeCore)
              == (kind == InstructionCacheKind::LinuxArm64));

        auto log = EventLog {};
        auto membarrier = FakeMembarrier { log, {} };
        auto const cache = fakeCache(kind, log, membarrier);
        log.events.clear();
        REQUIRE(cache->synchronize(SomeCode).has_value());
        REQUIRE_FALSE(log.events.empty());
        CHECK(log.events.front() == Event::Invalidate);
        REQUIRE(log.invalidated.size() == 1);
        CHECK(log.invalidated.front().data() == SomeCode.data());
        CHECK(log.invalidated.front().size() == SomeCode.size());
        log.events.clear();
        cache->synchronizeThisThread();
        CHECK(log.events == std::vector { Event::SynchronizeContext });
    }

    // Every platform row's kind has its row, and the x86-64 rows need none of it.
    for (auto const& platform: PlatformTable)
    {
        CHECK(instructionCacheRow(platform.instructionCache).kind == platform.instructionCache);
        if (platform.architecture == Architecture::X64)
            CHECK(platform.instructionCache == InstructionCacheKind::Coherent);
    }
}

TEST_CASE("InstructionCache.linuxArm64RegistersMembarrierOnce")
{
    auto log = EventLog {};
    auto membarrier = FakeMembarrier { log, {} };
    auto const cache = fakeCache(InstructionCacheKind::LinuxArm64, log, membarrier);
    CHECK(log.events == std::vector { Event::RegisterSynchronize });

    for ([[maybe_unused]] auto const round: std::views::iota(0, 3))
        REQUIRE(cache->synchronize(SomeCode).has_value());
    // Each synchronize invalidates first, then has every other thread synchronize: three SYNC_CORE
    // commands and still the one registration.
    CHECK(log.events
          == std::vector { Event::RegisterSynchronize,
                           Event::Invalidate,
                           Event::SynchronizeCore,
                           Event::Invalidate,
                           Event::SynchronizeCore,
                           Event::Invalidate,
                           Event::SynchronizeCore });
}

TEST_CASE("InstructionCache.membarrierRefusalIsAnError")
{
    SECTION("a refused registration builds no cache")
    {
        // ENOSYS: no membarrier(2); EINVAL: no SYNC_CORE on this kernel or architecture; EPERM: a sandbox.
        auto const code = GENERATE(ENOSYS, EINVAL, EPERM);
        auto log = EventLog {};
        auto membarrier = FakeMembarrier { log, { .registration = code, .synchronize = std::nullopt } };
        auto const cache = makeInstructionCache(InstructionCacheKind::LinuxArm64,
                                                std::make_unique<RecordingMaintenance>(log, std::nullopt),
                                                membarrier);
        REQUIRE_FALSE(cache.has_value());
        CHECK(cache.error()
              == InstructionCacheFailure { .error = InstructionCacheError::RegistrationRefused,
                                           .systemCode = code });
    }
    SECTION("a refused synchronization is never success")
    {
        auto const code = GENERATE(EPERM, EINVAL);
        auto log = EventLog {};
        auto membarrier = FakeMembarrier { log, { .registration = std::nullopt, .synchronize = code } };
        auto const cache = fakeCache(InstructionCacheKind::LinuxArm64, log, membarrier);
        auto const synchronized = cache->synchronize(SomeCode);
        REQUIRE_FALSE(synchronized.has_value());
        CHECK(synchronized.error()
              == InstructionCacheFailure { .error = InstructionCacheError::SynchronizationRefused,
                                           .systemCode = code });
    }
    SECTION("a refused invalidation is never success, and synchronizes nobody")
    {
        // FlushInstructionCache can fail (Windows); every row's invalidation is checked the same way.
        auto const kind = GENERATE(Catch::Generators::from_range(Arm64Kinds));
        auto log = EventLog {};
        auto membarrier = FakeMembarrier { log, {} };
        auto const cache = fakeCache(kind, log, membarrier, EINVAL);
        log.events.clear();
        auto const synchronized = cache->synchronize(SomeCode);
        REQUIRE_FALSE(synchronized.has_value());
        CHECK(synchronized.error()
              == InstructionCacheFailure { .error = InstructionCacheError::SynchronizationRefused,
                                           .systemCode = EINVAL });
        CHECK(log.events == std::vector { Event::Invalidate });
    }
}

TEST_CASE("InstructionCache.freeBSDArm64NeedsNoMembarrier")
{
    // A membarrier that refuses everything: the row must never ask it.
    auto log = EventLog {};
    auto membarrier = FakeMembarrier { log, { .registration = ENOSYS, .synchronize = ENOSYS } };
    auto const cache = fakeCache(InstructionCacheKind::FreeBSDArm64, log, membarrier);
    for ([[maybe_unused]] auto const round: std::views::iota(0, 3))
        REQUIRE(cache->synchronize(SomeCode).has_value());
    CHECK(log.events == std::vector { Event::Invalidate, Event::Invalidate, Event::Invalidate });
}

TEST_CASE("InstructionCache.thisSystemsRow")
{
    // The system's maintenance and membarrier, as this build's row uses them.
    auto const maintenance = makeSystemInstructionCacheMaintenance();
    REQUIRE(maintenance != nullptr);
    auto code = std::array<std::byte, 64> {};
    CHECK(maintenance->invalidate(code).has_value());
    maintenance->synchronizeContext();

    auto const membarrier = makeSystemMembarrier();
    REQUIRE(membarrier != nullptr);
    if (ThisPlatform.system == OperatingSystem::Linux)
    {
        // Every kernel CI and the gates run has SYNC_CORE (Linux 4.16 and later, on x86-64 and arm64 alike).
        CHECK(membarrier->registerSynchronizeCore().has_value());
        CHECK(membarrier->synchronizeCore().has_value());
    }
    else
    {
        CHECK(membarrier->registerSynchronizeCore() == std::unexpected { ENOSYS });
        CHECK(membarrier->synchronizeCore() == std::unexpected { ENOSYS });
    }

    auto const cache = makeSystemInstructionCache(*membarrier);
    REQUIRE(cache.has_value());
    CHECK((*cache)->synchronize(code).has_value());
    (*cache)->synchronizeThisThread();
}

TEST_CASE("InstructionCache.recordingDoubleRefusesTheConfiguredSynchronize")
{
    auto log = EventLog {};
    auto membarrier = FakeMembarrier { log, {} };
    auto const inner = fakeCache(InstructionCacheKind::FreeBSDArm64, log, membarrier);
    auto const refusal = InstructionCacheFailure { .error = InstructionCacheError::SynchronizationRefused,
                                                   .systemCode = EPERM };
    auto recording =
        RecordingInstructionCache { *inner, SynchronizeRefusal { .call = 2, .failure = refusal } };
    CHECK(recording.synchronize(SomeCode).has_value());
    recording.synchronizeThisThread();
    CHECK(recording.synchronize(SomeCode) == std::unexpected { refusal });
    CHECK(recording.synchronize(SomeCode).has_value());
    CHECK(std::ranges::equal(recording.calls(),
                             std::array { InstructionCacheCall::Synchronize,
                                          InstructionCacheCall::SynchronizeThisThread,
                                          InstructionCacheCall::Synchronize,
                                          InstructionCacheCall::Synchronize }));
    CHECK(recording.synchronizedCode().size() == 3);
    // The refused call never reached the inner cache: two invalidations, one isb.
    CHECK(log.events == std::vector { Event::Invalidate, Event::SynchronizeContext, Event::Invalidate });
}

TEST_CASE("InstructionCache.sealedOnOneThreadRunsOnEight", "[arm64][threading]")
{
    // Code written and synchronized on this thread runs on eight others that were already running, and
    // rewritten in place, they run the new code: on arm64, where nothing but the seam makes either
    // true; on x86-64 as the coherent row's proof.
    if (ThisPlatform.codeMemory == CodeMemoryKind::Unavailable)
        SKIP("no code can be generated on this platform");
    auto virtualMemory = SystemVirtualMemory {};
    auto jitMemory = SystemJitMemory {};
    auto const membarrier = makeSystemMembarrier();
    auto const cache = makeSystemInstructionCache(*membarrier);
    // A refusal (a kernel without SYNC_CORE, or a tool that does not pass membarrier through) is reported
    // as one, never run past.
    if (!cache.has_value())
        FAIL("the instruction cache was refused: error " << static_cast<int>(cache.error().error)
                                                         << ", system code " << cache.error().systemCode);
    auto code = ExecutableCode::create(ThisPlatform.codeMemory,
                                       ExecutableCodeServices { .virtualMemory = virtualMemory,
                                                                .jitMemory = jitMemory,
                                                                .instructionCache = **cache });
    REQUIRE(code.has_value());

    constexpr auto ThreadCount = std::size_t { 8 };
    auto const start = std::chrono::steady_clock::now();
    auto const beforeDeadline = [start] {
        return std::chrono::steady_clock::now() - start < TestDeadline;
    };
    auto generation = std::atomic<int> { 0 };
    auto finished = std::atomic<std::size_t> { 0 };
    // Set when a write failed: the code is not there to run, and the threads end instead of waiting.
    auto abandoned = std::atomic<int> { 0 };
    auto results = std::array<std::array<std::atomic<int>, 2>, ThreadCount> {};

    auto threads = std::vector<std::thread> {};
    threads.reserve(ThreadCount);
    for (auto const index: std::views::iota(std::size_t { 0 }, ThreadCount))
    {
        threads.emplace_back([&, index] {
            for (auto const round: std::views::iota(0, 2))
            {
                while (generation.load(std::memory_order_acquire) <= round)
                {
                    if (abandoned.load(std::memory_order_acquire) != 0 || !beforeDeadline())
                        return;
                    std::this_thread::yield();
                }
                (*cache)->synchronizeThisThread();
                results[index][static_cast<std::size_t>(round)].store(code->function(FirstFunctionOffset)(),
                                                                      std::memory_order_relaxed);
                finished.fetch_add(1, std::memory_order_acq_rel);
            }
        });
    }

    auto const publish = [&](int round, uint16_t value) {
        // No thread runs the code while it is rewritten: each waits for the next generation.
        while (finished.load(std::memory_order_acquire) < ThreadCount * static_cast<std::size_t>(round)
               && beforeDeadline())
            std::this_thread::yield();
        auto const written =
            code->write(FirstFunctionOffset, returnConstantCode(ThisPlatform.architecture, value));
        if (!written)
        {
            abandoned.store(1, std::memory_order_release);
            return false;
        }
        generation.store(round + 1, std::memory_order_release);
        return true;
    };
    auto const firstWritten = publish(0, 41);
    auto const secondWritten = firstWritten && publish(1, 42);
    for (auto& thread: threads)
        thread.join();

    CHECK(firstWritten);
    CHECK(secondWritten);
    CHECK(beforeDeadline());
    for (auto const& result: results)
    {
        CHECK(result[0].load() == 41);
        CHECK(result[1].load() == 42);
    }
}
