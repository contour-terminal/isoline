// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <ranges>
#include <string_view>
#include <vector>

#include <isoline/compiler/ContextLayout.hpp>
#include <isoline/compiler/machine/FrameRule.hpp>
#include <isoline/execution/RuntimeStubs.hpp>
#include <isoline/execution/StackBounds.hpp>
#include <isoline/execution/testing/CompileOne.hpp>
#include <isoline/runtime/ExecutionMode.hpp>
#include <isoline/runtime/Interrupt.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/testing/SmallStackThread.hpp>
#include <isoline/runtime/testing/StackAddress.hpp>

using isoline::compiler::FloatRoundOperation;
using isoline::compiler::RuntimeStub;
using isoline::compiler::machine::FrameRule;
using isoline::compiler::machine::StubStackBudgetBytes;
using isoline::execution::nativeStackLimitOf;
using isoline::execution::requiredNativeStack;
using isoline::execution::rowOf;
using isoline::execution::StubReserve;
using isoline::execution::ThreadStackBounds;
using isoline::execution::testing::callStub;
using isoline::execution::testing::CompiledInstance;
using isoline::execution::testing::hostConfig;
using isoline::execution::testing::moduleOf;
using isoline::runtime::StoreLimits;
using isoline::runtime::testing::stackAddress;

namespace
{

/// The bytes a stack painting covers below where it starts.
constexpr auto PaintBytes = std::size_t { 64 } * 1024;

/// The bytes left unpainted below the measuring frame: what the painting's own calls may take there.
constexpr auto PaintMargin = std::uintptr_t { 1024 };

/// How deep the work that calibrates the painting recurses: at 16 bytes a frame at the least (a return
/// address and the level it keeps), 8 KiB, far past PaintMargin wherever its first frame starts.
constexpr auto CalibrationLevels = std::uint32_t { 512 };

/// The pattern a painting fills the stack with.
constexpr auto Paint = std::byte { 0xa5 };

/// The calibration's recursion. Each level calls the next through a volatile pointer, which no compiler can
/// turn into a loop or inline, and keeps its level across the call: a frame on the real stack for every level
/// (it holds no addressable local, which ASan would move to its fake stack).
struct Calibration
{
    std::uint32_t (*volatile level)(Calibration const&, std::uint32_t); ///< The next level.
};

/// One level of @p calibration, with @p levels more below it.
std::uint32_t calibrationLevel(Calibration const& calibration, std::uint32_t levels)
{
    if (levels == 0)
        return 0;
    return calibration.level(calibration, levels - 1) ^ levels;
}

/// The stack @p work takes below the caller of this function, measured by painting: PaintBytes below this
/// frame are filled with Paint, top down (so that every guard page is met in order), @p work runs, and the
/// lowest byte it changed tells how deep it went. Frames of std::function and of this function's calls count
/// as the work's, and so does the unpainted margin, so the measure errs high, never low.
/// @return The bytes.
std::size_t stackDepthOf(std::function<void()> const& work)
{
    auto const top = stackAddress() - PaintMargin;
    auto const bottom = top - PaintBytes;
    // Below the stack pointer, written through an address no object of the program's has: the painting
    // touches nothing a frame holds, and no instrumentation tracks it.
    auto* const stack = reinterpret_cast<std::byte volatile*>(bottom);
    for (auto const offset: std::views::iota(std::size_t { 0 }, PaintBytes) | std::views::reverse)
        stack[offset] = Paint;
    work();
    auto lowest = PaintBytes;
    for (auto const offset: std::views::iota(std::size_t { 0 }, PaintBytes))
        if (stack[offset] != Paint)
        {
            lowest = offset;
            break;
        }
    // The margin is not painted, so the work may have used all of it: counted in, the measure is a bound.
    return PaintBytes - lowest + PaintMargin;
}

/// One stub call measured: the stub and its operand words.
struct PaintedStub
{
    RuntimeStub stub;
    std::vector<uint64_t> operands;
};

/// A store with a memory of 2 pages, a table of 4 slots, a passive data and a passive element segment: what
/// every stub but CallHost works on.
constexpr auto StubModule = std::string_view { R"((module
  (memory 2)
  (table 4 funcref)
  (data "abcdefgh")
  (elem func $f)
  (func $f))
)" };

} // namespace

TEST_CASE("StackBounds.reportsTheThreadsStack")
{
    constexpr auto SmallStack = std::size_t { 512 } * 1024;
    auto const bounds = ThreadStackBounds {};
    SECTION("a small thread: lowest below the stack in use and within the thread's stack")
    {
        auto reported = std::expected<std::uintptr_t, isoline::execution::StackBoundsError> {};
        auto here = std::uintptr_t { 0 };
        auto const ran = isoline::runtime::testing::runOnSmallStack(SmallStack, [&] {
            reported = bounds.lowestUsableAddress();
            here = stackAddress();
        });
        REQUIRE(ran.has_value());
        REQUIRE(reported.has_value());
        auto const lowest = *reported;
        CHECK(lowest < here);
        // A system may reserve more than asked: Windows (under Wine, at least) reserves a thread's stack in
        // whole MiB, its smallest reservation.
        CHECK(here - lowest < std::max(SmallStack, std::size_t { 1 } << 20U));
    }
    SECTION("the test's own thread: lowest below the stack in use")
    {
        auto const lowest = bounds.lowestUsableAddress();
        REQUIRE(lowest.has_value());
        CHECK(*lowest < stackAddress());
    }
}

TEST_CASE("StackBounds.nativeStackFollowsTheLimits")
{
    constexpr auto Rule = FrameRule { .bytesPerStackValue = 32, .frameOverhead = 256 };
    SECTION("what an invocation needs: every term of the published sum")
    {
        auto const limits =
            StoreLimits { .maxCallDepth = 100, .maxStackValues = 1'000, .maxNestedInvocations = 2 };
        CHECK(requiredNativeStack(Rule, limits)
              == (32 * 1'000) + (256 * 100) + (2 * isoline::execution::NestedInvocationBytes) + StubReserve
                     + isoline::execution::EntryOverheadBytes);
        CHECK(
            requiredNativeStack(Rule, StoreLimits { .maxStackValues = std::numeric_limits<uint64_t>::max() })
            == std::numeric_limits<uint64_t>::max());
    }
    SECTION("the limit: the grant below the entry, but never past the thread's end")
    {
        constexpr auto Entry = std::uintptr_t { 0x10'0000 };
        CHECK(nativeStackLimitOf(Entry, 0x1000, 0) == Entry - 0x1000);
        CHECK(nativeStackLimitOf(Entry, 0x8'0000, 0x9'0000) == 0x9'0000 + StubReserve);
        CHECK(nativeStackLimitOf(Entry, std::numeric_limits<uint64_t>::max(), 0) == StubReserve);
    }
    SECTION("the reserve holds every stub's budget")
    {
        STATIC_CHECK(StubStackBudgetBytes <= StubReserve);
    }
}

TEST_CASE("StackBounds.stubReserveCoversEveryStub")
{
    // Every stub but CallHost (a host function gets whatever stack is left) runs on the reserve the
    // prologue's native check keeps free, and so must stay within StubStackBudgetBytes on every build
    // flavour.
    auto created = CompiledInstance::create(moduleOf(StubModule),
                                            hostConfig(isoline::runtime::ExecutionMode::Deterministic, {}));
    REQUIRE(created.has_value());
    auto& instance = **created;
    auto const two = std::bit_cast<uint64_t>(2.5);
    auto const painted = std::to_array<PaintedStub>({
        { RuntimeStub::PollSlow, { std::to_underlying(isoline::runtime::PollSite::FunctionEntry) } },
        { RuntimeStub::MemoryGrow, { 0, 1 } },
        { RuntimeStub::MemoryFill, { 0, 0, 7, uint64_t { 3 } * 65'536 } },
        { RuntimeStub::MemoryCopy, { 0, 0, 100, 0, 65'536 } },
        { RuntimeStub::MemoryInit, { 0, 0, 16, 0, 8 } },
        { RuntimeStub::DataDrop, { 0 } },
        { RuntimeStub::TableGrow, { 0, 0, 4 } },
        { RuntimeStub::TableFill, { 0, 0, 0, 8 } },
        { RuntimeStub::TableCopy, { 0, 0, 1, 0, 4 } },
        { RuntimeStub::TableInit, { 0, 0, 2, 0, 1 } },
        { RuntimeStub::ElementDrop, { 0 } },
        { RuntimeStub::TableGet, { 0, 1 } },
        { RuntimeStub::TableSet, { 0, 1, 0 } },
        { RuntimeStub::FloatRound, { std::to_underlying(FloatRoundOperation::F64Nearest), two } },
    });
    auto measured = std::vector<RuntimeStub> {};
    auto deepest = std::size_t { 0 };
    for (auto const& [stub, operands]: painted)
    {
        auto status = uint64_t { 0 };
        auto depth = std::size_t { 0 };
        instance.lent([&](isoline::compiler::ExecutionState& state,
                          isoline::compiler::InstanceContext const& context) {
            depth =
                stackDepthOf([&] { status = callStub(rowOf(stub).entry, state, context, operands).status; });
        });
        INFO(rowOf(stub).name << " took " << depth << " bytes");
        CHECK(status == isoline::compiler::StubSucceeded);
        deepest = std::max(deepest, depth);
        CHECK(depth <= StubStackBudgetBytes);
        measured.push_back(stub);
    }
    // The painting sees frames below its margin: a recursion known to go far past it measures past it. (A
    // stub need not: an optimized arm64 build runs every one within the margin.) Not 16 bytes a level: GCC
    // devirtualizes the call speculatively and inlines a level into its caller, so gcc-release on x86-64
    // takes 4'040 bytes for the 512 levels.
    auto const calibration = Calibration { .level = &calibrationLevel };
    auto result = std::uint32_t { 0 };
    auto const calibrated = stackDepthOf([&] { result = calibration.level(calibration, CalibrationLevels); });
    INFO("the deepest stub took " << deepest << " bytes, " << CalibrationLevels << " levels " << calibrated
                                  << " (" << result << ")");
    CHECK(calibrated > PaintMargin);
    // Every stub but CallHost, the one a host function's own stack follows.
    for (auto const& row: isoline::execution::RuntimeStubTable)
        CHECK((row.stub == RuntimeStub::CallHost || std::ranges::contains(measured, row.stub)));
}

TEST_CASE("StackBounds.unknownBoundsRefuseTheEntry")
{
    // A thread whose stack the system does not report (a sandbox without /proc) gets no native limit, so its
    // entry into compiled code is refused with an error, before any poll, never an abort.
    class UnknownStack final: public isoline::execution::IStackBounds
    {
      public:
        [[nodiscard]] std::expected<std::uintptr_t, isoline::execution::StackBoundsError>
        lowestUsableAddress() const noexcept override
        {
            return std::unexpected { isoline::execution::StackBoundsError::Unavailable };
        }
    };
    auto const unknown = UnknownStack {};
    auto created =
        CompiledInstance::create(moduleOf("(module (func))"),
                                 hostConfig(isoline::runtime::ExecutionMode::Deterministic, {}),
                                 isoline::execution::testing::InstanceOptions { .bounds = &unknown });
    REQUIRE(created.has_value());
    auto& instance = **created;
    auto const called = instance.call(0, {});
    REQUIRE_FALSE(called.has_value());
    CHECK(called.error()
          == isoline::runtime::Abrupt {
              isoline::runtime::Trap { isoline::runtime::TrapCode::CallStackExhausted } });
    CHECK(instance.store().used(isoline::runtime::Limit::Steps) == 0);
    CHECK(instance.store().budgetState() == isoline::runtime::BudgetState::Settled);
}
