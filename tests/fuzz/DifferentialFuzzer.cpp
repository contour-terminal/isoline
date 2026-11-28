// SPDX-License-Identifier: Apache-2.0
// isoline_differential_fuzzer: generates a module from each input (ModuleGenerator.hpp) and runs it on every
// engine of the build, in every active lane (FuzzTargets.hpp's DifferentialLanes) or the one a non-empty
// ISOLINE_FUZZ_LANE names (deterministic, production), and traps after printing the first divergence. A
// finding's module is `isoline-fuzz-module <input> <directory>`.
#include <core/Environment.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <print>
#include <span>
#include <string_view>

#include "FuzzTargets.hpp"
#include <Corpus.hpp>
#include <Observation.hpp>

namespace
{

/// The lanes every input runs in: set once, at initialization.
auto lanes = isoline::fuzz::LaneSelection::Active;

} // namespace

extern "C" int LLVMFuzzerInitialize(int* /*argc*/, char*** /*argv*/)
{
    // The fuzzer's main is the composition root: the one place that reads the environment.
    auto const name = core::LiveEnvironment {}.get("ISOLINE_FUZZ_LANE");
    if (!name || name->empty())
        return 0;
    auto const lane = isoline::differential::laneNamed(*name);
    if (!lane)
    {
        std::println(stderr, "isoline_differential_fuzzer: ISOLINE_FUZZ_LANE names no lane: '{}'", *name);
        std::abort();
    }
    lanes = *lane == isoline::differential::Lane::Deterministic ? isoline::fuzz::LaneSelection::Deterministic
                                                                : isoline::fuzz::LaneSelection::Production;
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, std::size_t size)
{
    auto const input = std::as_bytes(std::span { data, size });
    auto const outcome = isoline::fuzz::runDifferential(input, lanes);
    if (!outcome)
    {
        std::println(stderr,
                     "divergence: {} in {}\n  expected {}\n  actual   {}",
                     outcome.error().participant,
                     outcome.error().field,
                     outcome.error().expected,
                     outcome.error().actual);
        __builtin_trap();
    }
    return 0;
}
