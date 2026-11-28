// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <memory_resource>
#include <new>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/binary/BodyUnits.hpp>
#include <isoline/compiler/CompileConfig.hpp>
#include <isoline/compiler/CompileError.hpp>
#include <isoline/compiler/Compiler.hpp>
#include <isoline/compiler/machine/TargetDescription.hpp>
#include <isoline/compiler/testing/CodeDigest.hpp>
#include <isoline/compiler/x86_64/Target.hpp>
#include <isoline/execution/testing/CompileOne.hpp>
#include <isoline/runtime/CountingArena.hpp>
#include <isoline/runtime/ExecutionMode.hpp>
#include <isoline/runtime/testing/FloatEnvironment.hpp>

using isoline::compiler::CompileCancellation;
using isoline::compiler::CompileConfig;
using isoline::compiler::CompiledCode;
using isoline::compiler::CompileError;
using isoline::compiler::CompileErrorCode;
using isoline::compiler::Compiler;
using isoline::compiler::machine::OperatingSystemAbi;
using isoline::execution::testing::hostConfig;
using isoline::execution::testing::moduleOf;
using isoline::runtime::CountingArena;
using isoline::runtime::ExecutionMode;

namespace
{

/// A budget no test module reaches.
constexpr auto LargeBudget = uint64_t { 1 } << 32;

/// A module of several functions over most of what the compiler compiles: calls, loops, memory, tables,
/// globals, float folding and a host import.
constexpr auto Mixed = std::string_view { R"((module
  (import "host" "f" (func $host (param i32) (result i32)))
  (memory 1) (table 2 funcref) (global $g (mut i64) (i64.const 7))
  (type $t (func (param i32) (result i32)))
  (elem (i32.const 0) $host $square)
  (func $square (type $t) (i32.mul (local.get 0) (local.get 0)))
  (func $sum (param i32) (result i32) (local i32)
    (loop $l
      (local.set 1 (i32.add (local.get 1) (call_indirect (type $t) (local.get 0) (i32.rem_u (local.get 0) (i32.const 2)))))
      (br_if $l (local.tee 0 (i32.sub (local.get 0) (i32.const 1)))))
    (i32.store (i32.const 8) (local.get 1))
    (global.set $g (i64.extend_i32_u (local.get 1)))
    (i32.load (i32.const 8)))
  (func $floats (result f64)
    (f64.add (f64.mul (f64.const 0x1p-1022) (f64.const 0.5))
             (f64.promote_f32 (f32.div (f32.const 1) (f32.const 3)))))
  (func $switch (param i32) (result i32)
    (block $c (block $b (block $a (br_table $a $b $c (local.get 0)))
      (return (i32.const 10))) (return (i32.const 20))) (i32.const 30)))
)" };

/// A compile of @p wat for @p config on @p arena (cancelled through @p cancellation).
[[nodiscard]] std::expected<CompiledCode, CompileError> compileOf(std::string_view wat,
                                                                  CompileConfig const& config,
                                                                  isoline::runtime::ICountingArena& arena,
                                                                  CompileCancellation const& cancellation)
{
    auto const target = isoline::compiler::x86_64::X64Target::create(config.target);
    REQUIRE(target.has_value());
    return Compiler { config, *target, arena, cancellation }.compile(*moduleOf(wat));
}

/// A compile of @p wat for @p config on a large budget.
[[nodiscard]] std::expected<CompiledCode, CompileError> compileOf(std::string_view wat,
                                                                  CompileConfig const& config)
{
    auto arena = CountingArena { LargeBudget, *std::pmr::new_delete_resource() };
    auto const cancellation = CompileCancellation {};
    return compileOf(wat, config, arena, cancellation);
}

/// A counting arena that cancels a compile at its @p at-th allocation.
class CancellingArena final: public isoline::runtime::ICountingArena
{
  public:
    CancellingArena(CompileCancellation& cancellation, uint64_t at) noexcept:
        _arena { LargeBudget, *std::pmr::new_delete_resource() }, _cancellation { &cancellation }, _at { at }
    {
    }

    [[nodiscard]] uint64_t budget() const noexcept override { return _arena.budget(); }
    [[nodiscard]] uint64_t charged() const noexcept override { return _arena.charged(); }
    [[nodiscard]] uint64_t reserved() const noexcept override { return _arena.reserved(); }
    [[nodiscard]] uint64_t peak() const noexcept override { return _arena.peak(); }
    [[nodiscard]] std::expected<void, isoline::runtime::ArenaBudgetExceeded> reserve(
        uint64_t bytes) noexcept override
    {
        return _arena.reserve(bytes);
    }
    void release(uint64_t bytes) noexcept override { _arena.release(bytes); }
    [[nodiscard]] std::optional<isoline::runtime::ArenaRefusal> lastRefusal() const noexcept override
    {
        return _arena.lastRefusal();
    }

    /// The allocations made so far.
    [[nodiscard]] uint64_t allocations() const noexcept { return _allocations; }

  private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override
    {
        if (++_allocations == _at)
            _cancellation->cancel();
        return _arena.allocate(bytes, alignment);
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override
    {
        _arena.deallocate(pointer, bytes, alignment);
    }
    [[nodiscard]] bool do_is_equal(std::pmr::memory_resource const& other) const noexcept override
    {
        return this == &other;
    }

    CountingArena _arena;
    CompileCancellation* _cancellation;
    uint64_t _at;
    uint64_t _allocations = 0;
};

/// The text of @p count copies of @p text.
[[nodiscard]] std::string repeated(std::string_view text, std::size_t count)
{
    auto result = std::string {};
    for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, count))
        result.append(text);
    return result;
}

/// A function long enough that translation, the passes and register allocation each poll the cancellation
/// several times: a chain of 40'000 dependent additions.
[[nodiscard]] std::string longChain()
{
    return "(module (func (param i32) (result i32)"
           + repeated(" (local.set 0 (i32.add (local.get 0) (i32.const 3)))", 40'000) + " (local.get 0)))";
}

} // namespace

TEST_CASE("Compiler.isDeterministic")
{
    // The same module and configuration give the same CompiledCode, byte for byte, whatever the heap looked
    // like (tests/determinism checks the same across two processes).
    for (auto const abi: { OperatingSystemAbi::SystemV, OperatingSystemAbi::Windows })
        for (auto const mode: { ExecutionMode::Production, ExecutionMode::Deterministic })
        {
            auto config = hostConfig(mode, {});
            config.target.abi = abi;
            auto const first = compileOf(Mixed, config);
            REQUIRE(first.has_value());
            // Churn the heap, so that a second compile's allocations land elsewhere.
            auto churn = std::vector<std::vector<std::byte>> {};
            for (auto const index: std::views::iota(std::size_t { 0 }, std::size_t { 2'000 }))
                churn.emplace_back(((index * 7919) % 4096) + 1);
            for (auto const index: std::views::iota(std::size_t { 0 }, churn.size()))
                if (index % 3 != 0)
                    churn[index] = {};
            auto const second = compileOf(Mixed, config);
            REQUIRE(second.has_value());
            CHECK(*first == *second);
            CHECK(isoline::compiler::testing::digestOf(*first)
                  == isoline::compiler::testing::digestOf(*second));
            CHECK(first->functionOffsets.size() == 4);
            CHECK(first->adapterOffsets.size() == 4);
            CHECK(first->hostScratchValues == std::vector<uint32_t> { 1 });
        }
}

TEST_CASE("Compiler.hostFloatEnvironmentDoesNotLeak")
{
    // I9: constant folding runs on host float arithmetic, under the IEEE defaults whatever the caller set:
    // rounding upward and flushing subnormals change no byte of the code. Each function folds to a value the
    // departed environment computes otherwise: a subnormal product (flushed to zero), and 1/7 (whose nearest
    // f32 lies below it).
    constexpr auto Folding = std::string_view { R"((module
  (func (result f64) (f64.mul (f64.const 0x1p-1022) (f64.const 0.5)))
  (func (result f32) (f32.div (f32.const 1) (f32.const 7))))
)" };
    for (auto const mode: { ExecutionMode::Production, ExecutionMode::Deterministic })
    {
        auto const config = hostConfig(mode, {});
        auto const expected = compileOf(Folding, config);
        REQUIRE(expected.has_value());
        auto const host = isoline::runtime::testing::HostFloatEnvironment {};
        isoline::runtime::testing::departFromIeeeDefaults();
        auto const departed = compileOf(Folding, config);
        REQUIRE(departed.has_value());
        CHECK(isoline::compiler::testing::digestOf(*departed)
              == isoline::compiler::testing::digestOf(*expected));
    }
}

TEST_CASE("Compiler.refusesDeferredFeature")
{
    auto const compiled =
        compileOf("(module (func (param v128)))", hostConfig(ExecutionMode::Production, {}));
    REQUIRE_FALSE(compiled.has_value());
    CHECK(compiled.error().code == CompileErrorCode::UnsupportedFeature);
    CHECK(compiled.error().feature == isoline::Feature::Simd);
}

TEST_CASE("Compiler.cancels")
{
    // A cancelled compile fails Cancelled wherever the cancellation lands: before the compile, in
    // translation, in the passes or in register allocation; nothing of it stays charged.
    auto const config = hostConfig(ExecutionMode::Production, {});
    SECTION("before")
    {
        auto arena = CountingArena { LargeBudget, *std::pmr::new_delete_resource() };
        auto cancellation = CompileCancellation {};
        cancellation.cancel();
        auto const compiled = compileOf(Mixed, config, arena, cancellation);
        REQUIRE_FALSE(compiled.has_value());
        CHECK(compiled.error().code == CompileErrorCode::Cancelled);
        CHECK(arena.charged() == 0);
    }
    SECTION("midway")
    {
        auto const chain = longChain();
        auto uncancelled = CompileCancellation {};
        auto counting = CancellingArena { uncancelled, 0 };
        REQUIRE(compileOf(chain, config, counting, uncancelled).has_value());
        auto const allocations = counting.allocations();
        auto cancelledRuns = 0;
        for (auto const step: std::views::iota(uint64_t { 1 }, uint64_t { 12 }))
        {
            auto cancellation = CompileCancellation {};
            auto arena = CancellingArena { cancellation, allocations * step / 12 };
            auto const compiled = compileOf(chain, config, arena, cancellation);
            INFO("cancelled at allocation " << allocations * step / 12 << " of " << allocations);
            if (!compiled)
            {
                CHECK(compiled.error().code == CompileErrorCode::Cancelled);
                ++cancelledRuns;
            }
            CHECK(arena.charged() == 0);
        }
        // Past the compile's last poll a cancellation has nothing left to stop; before it, every one does.
        CHECK(cancelledRuns >= 9);
    }
}

TEST_CASE("Compiler.refusesPastTheBudgetAtTheFirstOverrun")
{
    // A 1 MiB budget on a body of 50'000 locals, each read: refused BudgetExceeded in that function before
    // its work, the arena never past its budget, and nothing left charged.
    auto const config = hostConfig(ExecutionMode::Production, {});
    auto const budget = uint64_t { 1 } << 20;
    auto arena = CountingArena { budget, *std::pmr::new_delete_resource() };
    auto body = std::string {};
    for (auto const index: std::views::iota(1, 50'000))
        body.append(" (drop (local.get ").append(std::to_string(index)).append("))");
    auto const wat =
        "(module (func $small) (func (param i32) (local" + repeated(" i64", 50'000) + ")" + body + "))";
    auto const units = isoline::binary::bodyUnits(moduleOf(wat)->module().code[1]);
    auto const compiled = compileOf(wat, config, arena, CompileCancellation {});
    REQUIRE_FALSE(compiled.has_value());
    CHECK(compiled.error().code == CompileErrorCode::BudgetExceeded);
    CHECK(compiled.error().function == 1);
    CHECK(compiled.error().requested == isoline::compiler::compileReservationOf(units));
    CHECK(compiled.error().limit == budget);
    CHECK(arena.peak() <= budget);
    CHECK(arena.charged() == 0);
}

TEST_CASE("Compiler.mapsTheHostRunningOutOfMemory")
{
    // The host refuses the arena an allocation midway through a function: the compile fails
    // HostMemoryExhausted in that function, unwound, and nothing is left charged.
    class RefusingUpstream final: public std::pmr::memory_resource
    {
      public:
        explicit RefusingUpstream(uint64_t allowed) noexcept: _allowed { allowed } {}

      private:
        void* do_allocate(std::size_t bytes, std::size_t alignment) override
        {
            if (bytes > _allowed)
                throw std::bad_alloc {};
            _allowed -= bytes;
            return std::pmr::new_delete_resource()->allocate(bytes, alignment);
        }
        void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override
        {
            _allowed += bytes;
            std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
        }
        [[nodiscard]] bool do_is_equal(std::pmr::memory_resource const& other) const noexcept override
        {
            return this == &other;
        }

        uint64_t _allowed;
    };
    auto upstream = RefusingUpstream { uint64_t { 256 } * 1024 };
    auto arena = CountingArena { LargeBudget, upstream };
    auto const compiled =
        compileOf(longChain(), hostConfig(ExecutionMode::Production, {}), arena, CompileCancellation {});
    REQUIRE_FALSE(compiled.has_value());
    CHECK(compiled.error().code == CompileErrorCode::HostMemoryExhausted);
    CHECK(compiled.error().function == 0);
    CHECK(compiled.error().requested > 0);
    CHECK(arena.charged() == 0);
}

TEST_CASE("Compiler.checksTheArtifactPerFunction")
{
    // The artifact's cap is checked after each function: a module whose code is too large is refused at the
    // first function past the cap, not after the last.
    auto config = hostConfig(ExecutionMode::Production, {});
    config.limits.artifactBytesFloor = 1;
    config.limits.artifactBytesPerModuleByte = 0;
    auto const compiled = compileOf(Mixed, config);
    REQUIRE_FALSE(compiled.has_value());
    CHECK(compiled.error().code == CompileErrorCode::ArtifactTooLarge);
    CHECK(compiled.error().function == 0);
    CHECK(compiled.error().limit == 1);
    SECTION("the cap scales with the module")
    {
        CHECK(isoline::compiler::artifactCapOf(isoline::compiler::CompileLimits {}, 1'000)
              == uint64_t { 64 } * 1024);
        CHECK(isoline::compiler::artifactCapOf(isoline::compiler::CompileLimits {}, 100'000) == 400'000);
        CHECK(isoline::compiler::artifactCapOf(isoline::compiler::CompileLimits {}, uint64_t { 1 } << 40)
              == std::numeric_limits<uint32_t>::max());
    }
    SECTION("the trampoline and the adapters count too")
    {
        auto const fits = compileOf(Mixed, hostConfig(ExecutionMode::Production, {}));
        REQUIRE(fits.has_value());
        auto tight = hostConfig(ExecutionMode::Production, {});
        tight.limits.artifactBytesFloor = fits->trampolineOffset;
        tight.limits.artifactBytesPerModuleByte = 0;
        auto const refused = compileOf(Mixed, tight);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code == CompileErrorCode::ArtifactTooLarge);
        CHECK_FALSE(refused.error().function.has_value());
    }
}

TEST_CASE("Compiler.refusesAFunctionTooLarge")
{
    auto config = hostConfig(ExecutionMode::Production, {});
    config.limits.maxFunctionBodyUnits = 4;
    auto const compiled =
        compileOf("(module (func) (func (drop (i32.add (i32.const 1) (i32.const 2)))))", config);
    REQUIRE_FALSE(compiled.has_value());
    CHECK(compiled.error().code == CompileErrorCode::FunctionTooLarge);
    CHECK(compiled.error().function == 1);
    CHECK(compiled.error().limit == 4);
    CHECK(compiled.error().requested == 5);
    // Before its bound is reserved: a body over the cap is refused for its size, never for a budget its
    // reservation would exceed.
    auto arena =
        CountingArena { isoline::compiler::compileReservationOf(5) - 1, *std::pmr::new_delete_resource() };
    auto const tight = compileOf("(module (func (drop (i32.add (i32.const 1) (i32.const 2)))))",
                                 config,
                                 arena,
                                 CompileCancellation {});
    REQUIRE_FALSE(tight.has_value());
    CHECK(tight.error().code == CompileErrorCode::FunctionTooLarge);
}

TEST_CASE("Compiler.reservationSaturates")
{
    CHECK(isoline::compiler::compileReservationOf(0) == isoline::compiler::CompileFunctionOverheadBytes);
    CHECK(isoline::compiler::compileReservationOf(10)
          == (10 * isoline::compiler::CompileBytesPerBodyByte)
                 + isoline::compiler::CompileFunctionOverheadBytes);
    CHECK(isoline::compiler::compileReservationOf(std::numeric_limits<uint64_t>::max())
          == std::numeric_limits<uint64_t>::max());
}
