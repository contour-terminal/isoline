// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <stdexcept>
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
#include <isoline/platform/testing/JitWriteScopeKeys.hpp>

using namespace isoline::platform;
using isoline::platform::testing::ExecutableCode;
using isoline::platform::testing::ExecutableCodeServices;
using isoline::platform::testing::FirstFunctionOffset;
using isoline::platform::testing::JitWriteScopeKeys;
using isoline::platform::testing::returnConstantCode;

namespace
{

/// JIT memory that records the calling thread's access changes and maps nothing.
class RecordingJitMemory final: public IJitMemory
{
  public:
    [[nodiscard]] std::expected<Region, VirtualMemoryFailure> mapJit(uint64_t /*bytes*/,
                                                                     JitMapping /*mapping*/) noexcept override
    {
        return std::unexpected { VirtualMemoryFailure { .error = VirtualMemoryError::ReserveRefused } };
    }

    void setThreadAccess(JitAccess access) noexcept override { accesses.push_back(access); }

    [[nodiscard]] std::expected<void, VirtualMemoryFailure> release(Region /*region*/) noexcept override
    {
        return std::unexpected { VirtualMemoryFailure { .error = VirtualMemoryError::ReleaseRefused } };
    }

    std::vector<JitAccess> accesses; ///< Every setThreadAccess(), in order.
};

/// How long a test that waits on other threads waits at most.
constexpr auto TestDeadline = std::chrono::seconds { 30 };

/// A copy that fails part-way, as a seal's copy may.
struct CopyFailed: std::runtime_error
{
    CopyFailed(): std::runtime_error { "copy failed" } {}
};

} // namespace

TEST_CASE("JitMemory.scopeRestoresExecuteOnEveryExit")
{
    auto memory = RecordingJitMemory {};
    SECTION("normal exit")
    {
        {
            auto const scope = JitWriteScope { memory, JitWriteScopeKeys::key() };
            CHECK(memory.accesses == std::vector { JitAccess::Write });
        }
        CHECK(memory.accesses == std::vector { JitAccess::Write, JitAccess::Execute });
    }
    SECTION("an exception's unwinding")
    {
        auto const writeThenThrow = [&memory] {
            auto const scope = JitWriteScope { memory, JitWriteScopeKeys::key() };
            throw CopyFailed {};
        };
        CHECK_THROWS_AS(writeThenThrow(), CopyFailed);
        CHECK(memory.accesses == std::vector { JitAccess::Write, JitAccess::Execute });
    }
}

TEST_CASE("JitMemory.mappingsPerCodeMemoryKind")
{
    // MAP_JIT exists on the macOS rows alone; an executable mapping only behind the per-thread toggle,
    // which is the one writable-and-executable mapping anywhere (macOS arm64).
    CHECK_FALSE(mapsJit(CodeMemoryKind::ReserveWriteThenExecute));
    CHECK(mapsJit(CodeMemoryKind::MapJitWithProtection));
    CHECK(mapsJit(CodeMemoryKind::MapJitWithThreadToggle));
    CHECK_FALSE(mapsJit(CodeMemoryKind::Unavailable));
    CHECK_FALSE(togglesJitPerThread(CodeMemoryKind::MapJitWithProtection));
    CHECK(togglesJitPerThread(CodeMemoryKind::MapJitWithThreadToggle));

    CHECK(allowsJitMapping(CodeMemoryKind::MapJitWithProtection, JitMapping::ReadWrite));
    CHECK_FALSE(allowsJitMapping(CodeMemoryKind::MapJitWithProtection, JitMapping::ReadWriteExecute));
    CHECK(allowsJitMapping(CodeMemoryKind::MapJitWithThreadToggle, JitMapping::ReadWrite));
    CHECK(allowsJitMapping(CodeMemoryKind::MapJitWithThreadToggle, JitMapping::ReadWriteExecute));
    for (auto const kind: { CodeMemoryKind::ReserveWriteThenExecute, CodeMemoryKind::Unavailable })
        for (auto const& row: JitMappingTable)
            CHECK_FALSE(allowsJitMapping(kind, row.mapping));

    // The checks of IJitMemory's preconditions let every allowed call through (the refused ones end the
    // process: the canary's jit-* cases).
    detail::requireJitMapping(CodeMemoryKind::MapJitWithThreadToggle, JitMapping::ReadWriteExecute);
    detail::requireJitMapping(CodeMemoryKind::MapJitWithProtection, JitMapping::ReadWrite);
    detail::requireJitThreadToggle(CodeMemoryKind::MapJitWithThreadToggle);

    // The rows: macOS arm64 toggles, macOS x86-64 maps read-write and protects, nobody else maps JIT.
    for (auto const& platform: PlatformTable)
    {
        auto const expectedJit =
            platform.system == OperatingSystem::MacOS && platform.architecture != Architecture::Other;
        CHECK(mapsJit(platform.codeMemory) == expectedJit);
        CHECK(togglesJitPerThread(platform.codeMemory)
              == (expectedJit && platform.architecture == Architecture::Arm64));
    }
}

TEST_CASE("JitMemory.pageSizeIsTheSystems")
{
    // Read once, at construction: the system's where there is MAP_JIT, 0 where there is none.
    auto const expected =
        mapsJit(ThisPlatform.codeMemory) ? SystemVirtualMemory {}.pageSize() : uint64_t { 0 };
    CHECK(SystemJitMemory {}.pageSize() == expected);
}

TEST_CASE("JitMemory.macOSTogglesPerThread", "[macos-arm64][threading]")
{
    // While this thread writes, another that never toggles keeps executing the same region: its view
    // stays executable, never writable. Were the toggle the process's, it would fault mid-run.
    if (!togglesJitPerThread(ThisPlatform.codeMemory))
        SKIP("this platform has no per-thread MAP_JIT toggle");
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
    constexpr auto SecondFunction = FirstFunctionOffset + 64;
    REQUIRE(code->write(FirstFunctionOffset, returnConstantCode(ThisPlatform.architecture, 7)).has_value());

    auto const start = std::chrono::steady_clock::now();
    auto const beforeDeadline = [start] {
        return std::chrono::steady_clock::now() - start < TestDeadline;
    };
    auto runs = std::atomic<int> { 0 };
    auto wrongResults = std::atomic<int> { 0 };
    auto switched = std::atomic<int> { 0 };
    auto secondResult = std::atomic<int> { 0 };
    auto executor = std::thread { [&] {
        (*cache)->synchronizeThisThread();
        while (switched.load(std::memory_order_acquire) == 0 && beforeDeadline())
        {
            if (code->function(FirstFunctionOffset)() != 7)
                wrongResults.fetch_add(1, std::memory_order_relaxed);
            runs.fetch_add(1, std::memory_order_release);
        }
        (*cache)->synchronizeThisThread();
        secondResult.store(code->function(SecondFunction)(), std::memory_order_release);
    } };

    auto const waitForRuns = [&](int count) {
        while (runs.load(std::memory_order_acquire) < count && beforeDeadline())
            std::this_thread::yield();
    };
    waitForRuns(1);
    auto const second = returnConstantCode(ThisPlatform.architecture, 9);
    auto const secondBytes = code->bytes().subspan(SecondFunction, second.size());
    auto runsWhileWriting = 0;
    {
        // This thread's view is writable now, and it writes into the page the executor runs from; the
        // executor's view must stay executable throughout.
        auto const scope = JitWriteScope { jitMemory, JitWriteScopeKeys::key() };
        auto const before = runs.load(std::memory_order_acquire);
        while (runs.load(std::memory_order_acquire) < before + 1000 && beforeDeadline())
            std::memcpy(secondBytes.data(), second.data(), second.size());
        runsWhileWriting = runs.load(std::memory_order_acquire) - before;
    }
    auto const synchronized = (*cache)->synchronize(secondBytes);
    switched.store(1, std::memory_order_release);
    executor.join();

    CHECK(synchronized.has_value());
    CHECK(runsWhileWriting >= 1000);
    CHECK(wrongResults.load() == 0);
    CHECK(secondResult.load() == 9);
}
