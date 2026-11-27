// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <thread>
#include <type_traits>

#include <isoline/compiler/CompileConfig.hpp>
#include <isoline/compiler/CompileLimits.hpp>

using isoline::compiler::CancellationState;
using isoline::compiler::CompileCancellation;
using isoline::compiler::CompileConfig;
using isoline::compiler::CompileLimits;
using isoline::compiler::SpectreGuard;
using isoline::compiler::machine::TargetDescription;
using isoline::runtime::ExecutionMode;

// The target and the mode are required: a configuration names both, or does not compile.
static_assert(!std::is_default_constructible_v<CompileConfig>);
static_assert(!std::is_constructible_v<CompileConfig, TargetDescription>);
static_assert(std::is_constructible_v<CompileConfig, TargetDescription, ExecutionMode>);

TEST_CASE("CompileConfig.defaults")
{
    // The plan's published caps (Decisions, "Compile caps"), and the Spectre guard on.
    auto const limits = CompileLimits {};
    CHECK(limits.maxFunctionBodyUnits == 7'654'321);
    CHECK(limits.maxFunctionInstructions == 4'000'000);
    CHECK(limits.maxBlockParameters == 100'000);
    CHECK(limits.maxTotalBlockParameters == 4'000'000);
    CHECK(limits.maxPoolEntries == 4'294'967'295);
    CHECK(limits.artifactBytesPerModuleByte == 4);
    CHECK(limits.artifactBytesFloor == uint64_t { 64 } * 1024);
    auto const config = CompileConfig { TargetDescription {}, ExecutionMode::Production };
    CHECK(config.spectre == SpectreGuard::Enabled);
    CHECK(config.limits == limits);
    static_assert(SpectreGuard {} == SpectreGuard::Disabled, "the zero value is the negative case");
}

TEST_CASE("CompileConfig.equalityCoversEveryField")
{
    auto const base = CompileConfig { TargetDescription {}, ExecutionMode::Production };
    CHECK(base == CompileConfig { TargetDescription {}, ExecutionMode::Production });
    auto spectre = base;
    spectre.spectre = SpectreGuard::Disabled;
    CHECK(spectre != base);
    auto mode = base;
    mode.mode = ExecutionMode::Deterministic;
    CHECK(mode != base);
    auto target = base;
    target.target.abi = isoline::compiler::machine::OperatingSystemAbi::Windows;
    CHECK(target != base);
    auto limits = base;
    limits.limits.artifactBytesFloor = 1;
    CHECK(limits != base);
}

TEST_CASE("CompileCancellation.startsRunningAndStaysCancelled")
{
    auto cancellation = CompileCancellation {};
    CHECK(cancellation.state() == CancellationState::Running);
    cancellation.cancel();
    CHECK(cancellation.state() == CancellationState::Cancelled);
    cancellation.cancel();
    CHECK(cancellation.state() == CancellationState::Cancelled);
}

TEST_CASE("CompileCancellation.fromAnotherThread", "[threading]")
{
    auto cancellation = CompileCancellation {};
    auto canceller = std::thread { [&cancellation] { cancellation.cancel(); } };
    canceller.join();
    CHECK(cancellation.state() == CancellationState::Cancelled);
}
