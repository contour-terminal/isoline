// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <thread>

#include <isoline/compiler/CompileConfig.hpp>
#include <isoline/compiler/CompileLimits.hpp>

using isoline::compiler::CancellationState;
using isoline::compiler::CompileCancellation;
using isoline::compiler::CompileConfig;
using isoline::compiler::CompileLimits;
using isoline::compiler::SpectreGuard;

TEST_CASE("CompileConfig.defaults")
{
    // The plan's published caps (Decisions, "Compile caps"), and the Spectre guard on.
    auto const limits = CompileLimits {};
    CHECK(limits.maxFunctionBodyUnits == 7'654'321);
    CHECK(limits.maxFunctionInstructions == 4'000'000);
    CHECK(limits.maxBlockParameters == 100'000);
    CHECK(limits.maxTotalBlockParameters == 4'000'000);
    CHECK(limits.artifactBytesPerModuleByte == 4);
    CHECK(limits.artifactBytesFloor == uint64_t { 64 } * 1024);
    auto const config = CompileConfig {};
    CHECK(config.spectre == SpectreGuard::Enabled);
    CHECK(config.limits == limits);
    static_assert(SpectreGuard {} == SpectreGuard::Disabled, "the zero value is the negative case");
}

TEST_CASE("CompileConfig.equalityCoversEveryField")
{
    auto const base = CompileConfig {};
    CHECK(base == CompileConfig {});
    auto spectre = base;
    spectre.spectre = SpectreGuard::Disabled;
    CHECK(spectre != base);
    auto mode = base;
    mode.mode = isoline::runtime::ExecutionMode::Deterministic;
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
