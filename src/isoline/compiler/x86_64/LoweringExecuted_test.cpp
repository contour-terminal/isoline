// SPDX-License-Identifier: Apache-2.0
// The x86-64 lowerings run, not read: modules compiled through every stage and called through the entry
// trampoline (execution/testing/CompileOne.hpp), their results compared bit for bit with numerics::, the
// kernels both interpreters run, and their traps and steps with the reference interpreter's.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/compiler/machine/TargetDescription.hpp>
#include <isoline/compiler/x86_64/Registers.hpp>
#include <isoline/execution/CpuFeatureProbe.hpp>
#include <isoline/execution/StackBounds.hpp>
#include <isoline/execution/testing/CompileOne.hpp>
#include <isoline/numerics/Float.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/runtime/testing/SmallStackThread.hpp>
#include <isoline/runtime/testing/StackAddress.hpp>

using isoline::compiler::machine::CpuFeature;
using isoline::compiler::machine::CpuFeatures;
using isoline::execution::testing::CompiledInstance;
using isoline::execution::testing::hostConfig;
using isoline::execution::testing::hostRunsCompiledCode;
using isoline::execution::testing::InstanceOptions;
using isoline::execution::testing::Interruption;
using isoline::execution::testing::moduleOf;
using isoline::execution::testing::runReference;
using isoline::runtime::Abrupt;
using isoline::runtime::F32;
using isoline::runtime::F64;
using isoline::runtime::I32;
using isoline::runtime::I64;
using isoline::runtime::Trap;
using isoline::runtime::TrapCode;
using isoline::runtime::Value;

namespace
{

constexpr auto Deterministic = isoline::runtime::ExecutionMode::Deterministic;

/// The feature sets a lowering runs under on this CPU: the baseline, and SSE4.1 where the CPU has it, so that
/// both the inline roundss/roundsd rows and their FloatRound fallback run.
[[nodiscard]] std::vector<CpuFeatures> featureSetsOfThisCpu()
{
    auto sets = std::vector<CpuFeatures> { CpuFeatures {} };
    if (isoline::execution::HostCpuFeatureProbe {}.features().contains(CpuFeature::Sse41))
        sets.push_back(CpuFeatures {}.with(CpuFeature::Sse41));
    return sets;
}

/// The f32 edges of min and max: both zeros, both ones, both infinities, a quiet and a signalling NaN.
constexpr auto F32Edges = std::to_array<uint32_t>({ 0x0000'0000,
                                                    0x8000'0000,
                                                    0x3f80'0000,
                                                    0xbf80'0000,
                                                    0x7f80'0000,
                                                    0xff80'0000,
                                                    0x7fc0'0000,
                                                    0x7fa0'0000 });

/// @copydoc F32Edges
constexpr auto F64Edges = std::to_array<uint64_t>({ 0x0000'0000'0000'0000,
                                                    0x8000'0000'0000'0000,
                                                    0x3ff0'0000'0000'0000,
                                                    0xbff0'0000'0000'0000,
                                                    0x7ff0'0000'0000'0000,
                                                    0xfff0'0000'0000'0000,
                                                    0x7ff8'0000'0000'0000,
                                                    0x7ff4'0000'0000'0000 });

/// Every lowering Review Focus 2 names, one function each, by code index.
constexpr auto FloatModule = std::string_view { R"((module
  (func (param f32 f32) (result f32) (f32.min (local.get 0) (local.get 1)))
  (func (param f32 f32) (result f32) (f32.max (local.get 0) (local.get 1)))
  (func (param f64 f64) (result f64) (f64.min (local.get 0) (local.get 1)))
  (func (param f64 f64) (result f64) (f64.max (local.get 0) (local.get 1)))
  (func (param f32) (result f32) (f32.nearest (local.get 0)))
  (func (param f64) (result f64) (f64.nearest (local.get 0)))
  (func (param f32) (result f32) (f32.ceil (local.get 0)))
  (func (param f64) (result f64) (f64.floor (local.get 0)))
  (func (param f32) (result f32) (f32.trunc (local.get 0)))
  (func (param f32) (result i32) (i32.trunc_f32_s (local.get 0)))
  (func (param i64) (result f32) (f32.convert_i64_u (local.get 0))))
)" };

/// The result of calling @p function of @p instance with @p arguments: its one value, or its trap.
[[nodiscard]] std::expected<Value, TrapCode> resultOf(CompiledInstance& instance,
                                                      uint32_t function,
                                                      std::vector<Value> const& arguments)
{
    auto const called = instance.call(function, arguments);
    if (!called)
    {
        auto const* trap = std::get_if<Trap>(&called.error());
        REQUIRE(trap != nullptr);
        return std::unexpected { trap->code };
    }
    REQUIRE(called->size() == 1);
    return called->front();
}

/// What numerics:: gives for a trapping truncation of @p value: its value, or its trap.
[[nodiscard]] std::expected<Value, TrapCode> truncated(uint32_t value)
{
    auto const result = isoline::numerics::truncTrapping<I32, int32_t>(F32 { .bits = value });
    if (!result)
        return std::unexpected { result.error().code };
    return Value { *result };
}

} // namespace

TEST_CASE("Lowering.loweringsMatchTheReference")
{
    // Review Focus 2, executed: every float lowering whose corners differ between instruction sets, in
    // Deterministic mode, gives numerics::'s bits, with and without SSE4.1.
    if (!hostRunsCompiledCode())
        SKIP("the code is x86-64's");
    namespace numerics = isoline::numerics;
    for (auto const features: featureSetsOfThisCpu())
    {
        CAPTURE(features.contains(CpuFeature::Sse41));
        auto created = CompiledInstance::create(moduleOf(FloatModule), hostConfig(Deterministic, features));
        REQUIRE(created.has_value());
        auto& instance = **created;
        // min and max over every pair of edges.
        {
            for (auto const a: F32Edges)
                for (auto const b: F32Edges)
                {
                    CAPTURE(a, b);
                    auto const arguments = std::vector<Value> { F32 { .bits = a }, F32 { .bits = b } };
                    CHECK(resultOf(instance, 0, arguments) == Value { numerics::fmin(F32 { a }, F32 { b }) });
                    CHECK(resultOf(instance, 1, arguments) == Value { numerics::fmax(F32 { a }, F32 { b }) });
                }
            for (auto const a: F64Edges)
                for (auto const b: F64Edges)
                {
                    CAPTURE(a, b);
                    auto const arguments = std::vector<Value> { F64 { .bits = a }, F64 { .bits = b } };
                    CHECK(resultOf(instance, 2, arguments) == Value { numerics::fmin(F64 { a }, F64 { b }) });
                    CHECK(resultOf(instance, 3, arguments) == Value { numerics::fmax(F64 { a }, F64 { b }) });
                }
        }
        // Rounding: ties, both zeros, the edges.
        {
            for (auto const value:
                 { 0.5, 1.5, 2.5, -0.5, -1.5, -2.5, 0.49999997, 8388607.5, -0.0, 0.0, 1e30 })
            {
                CAPTURE(value);
                auto const single = F32 { .bits = std::bit_cast<uint32_t>(static_cast<float>(value)) };
                auto const doubled = F64 { .bits = std::bit_cast<uint64_t>(value) };
                CHECK(resultOf(instance, 4, { single }) == Value { numerics::nearest(single) });
                CHECK(resultOf(instance, 5, { doubled }) == Value { numerics::nearest(doubled) });
                CHECK(resultOf(instance, 6, { single }) == Value { numerics::ceil(single) });
                CHECK(resultOf(instance, 7, { doubled }) == Value { numerics::floor(doubled) });
                CHECK(resultOf(instance, 8, { single }) == Value { numerics::trunc(single) });
            }
            for (auto const bits: F32Edges)
            {
                CAPTURE(bits);
                CHECK(resultOf(instance, 4, { F32 { .bits = bits } })
                      == Value { numerics::nearest(F32 { bits }) });
            }
        }
        // i32.trunc_f32_s at its edges.
        {
            // 2^31 − 128 (the largest f32 below 2^31), 2^31, −2^31, −2^31 − 256, a NaN, ±0.9 and both
            // infinities.
            for (auto const bits: std::to_array<uint32_t>({ 0x4eff'ffff,
                                                            0x4f00'0000,
                                                            0xcf00'0000,
                                                            0xcf00'0001,
                                                            0x7fc0'0000,
                                                            0x3f66'6666,
                                                            0xbf66'6666,
                                                            0x7f80'0000,
                                                            0xff80'0000 }))
            {
                CAPTURE(bits);
                CHECK(resultOf(instance, 9, { F32 { .bits = bits } }) == truncated(bits));
            }
        }
        // f32.convert_i64_u past 2^63 and at 2^24 + 1.
        {
            for (auto const value: std::to_array<uint64_t>({ (uint64_t { 1 } << 63U) + 1,
                                                             ~uint64_t { 0 },
                                                             (uint64_t { 1 } << 24U) + 1,
                                                             (uint64_t { 1 } << 63U),
                                                             0 }))
            {
                CAPTURE(value);
                CHECK(resultOf(instance, 10, { I64 { .bits = value } })
                      == Value { numerics::convert<F32, uint64_t>(I64 { value }) });
            }
        }
    }
}

TEST_CASE("Lowering.entryTrapsAsTheReferenceDoes")
{
    // A function calling one of 5'000 locals under 1'000 stack values, in Deterministic mode, its native
    // stack granted exactly requiredNativeStack() of its limits: the reference interrupts at the entry's poll
    // when an interrupt is pending, and otherwise exhausts the stack values at the callee's charge, after its
    // poll took a step. The compiled prologue, run, agrees, and the static limit fires before the native one.
    if (!hostRunsCompiledCode())
        SKIP("the code is x86-64's");
    auto text = std::string { "(module\n (func $f (call $g))\n (func $g" };
    for ([[maybe_unused]] auto const local: std::views::iota(0, 5'000))
        text += " (local i32)";
    text += "))\n";
    auto const module = moduleOf(text);
    auto const limits = isoline::runtime::StoreLimits { .maxCallDepth = 1'000, .maxStackValues = 1'000 };
    auto const grant =
        isoline::execution::requiredNativeStack(isoline::compiler::x86_64::X64FrameRule, limits);
    struct Row
    {
        std::string_view name;
        Interruption interruption;
        TrapCode code;
    };
    for (auto const& [name, interruption, code]: std::to_array<Row>({
             { "no interrupt pending: the callee's stack charge, after its poll",
               Interruption::None,
               TrapCode::CallStackExhausted },
             { "an interrupt pending: the entry's poll, before any charge",
               Interruption::Pending,
               TrapCode::Interrupted },
         }))
    {
        CAPTURE(name);
        auto const options =
            InstanceOptions { .limits = limits, .nativeStackBytes = grant, .interruption = interruption };
        auto created = CompiledInstance::create(module, hostConfig(Deterministic, {}), options);
        REQUIRE(created.has_value());
        auto& instance = **created;
        auto compiled = std::optional<std::expected<std::vector<Value>, Abrupt>> {};
        auto room = uint64_t { 0 };
        // A thread with room for the whole grant, so that the limit is the grant's, not the thread's end.
        auto const ran = isoline::runtime::testing::runOnSmallStack(grant + (std::size_t { 4 } << 20U), [&] {
            auto const lowest = isoline::execution::ThreadStackBounds {}.lowestUsableAddress();
            REQUIRE(lowest.has_value());
            room = isoline::runtime::testing::stackAddress() - *lowest;
            compiled = instance.call(0, {});
        });
        REQUIRE(ran.has_value());
        CHECK(room >= grant);
        REQUIRE(compiled.has_value());
        REQUIRE_FALSE(compiled->has_value());
        CHECK(compiled->error() == Abrupt { Trap { code } });
        auto const reference = runReference(module, 0, {}, options);
        REQUIRE_FALSE(reference.outcome.has_value());
        CHECK(reference.outcome.error() == compiled->error());
        CHECK(instance.store().used(isoline::runtime::Limit::Steps) == reference.steps);
        CHECK(instance.store().chargedStackValues() == 0);
    }
}
