// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <span>
#include <string>
#include <vector>

#include "SpecEngine.hpp"
#include <isoline/Features.hpp>
#include <isoline/fastinterpreter/Dispatch.hpp>
#include <isoline/runtime/ExecutionMode.hpp>

using isoline::FeatureSet;
using isoline::fastinterpreter::availableDispatches;
using isoline::fastinterpreter::bestDispatch;
using isoline::fastinterpreter::Dispatch;
using isoline::runtime::ExecutionEngine;
using isoline::spectest::parseOptions;
using isoline::spectest::SpecEngine;

namespace
{

/// The options of a command line, as main() hands them over.
[[nodiscard]] auto parsed(std::vector<char const*> const& arguments)
{
    return parseOptions(std::span { arguments });
}

} // namespace

TEST_CASE("SpecEngine.optionsNameTheEngine")
{
    auto const reference = parsed({ "--features", "wasm3" });
    REQUIRE(reference.has_value());
    CHECK(reference->features == FeatureSet::wasm3());
    CHECK(reference->engine == SpecEngine {});
    auto const named = parsed({ "--engine", "reference", "--features", "all" });
    REQUIRE(named.has_value());
    CHECK(named->features == FeatureSet::all());
    CHECK(named->engine == SpecEngine {});
    auto const fast = parsed({ "--features", "wasm3", "--engine", "fast" });
    REQUIRE(fast.has_value());
    CHECK(fast->engine == SpecEngine { .engine = ExecutionEngine::Fast, .dispatch = bestDispatch() });
    for (auto const dispatch: availableDispatches())
    {
        auto const name = dispatch == Dispatch::Switch ? "switch" : "tailcall";
        auto const chosen = parsed({ "--dispatch", name, "--engine", "fast", "--features", "wasm3" });
        REQUIRE(chosen.has_value());
        CHECK(chosen->engine == SpecEngine { .engine = ExecutionEngine::Fast, .dispatch = dispatch });
    }
}

TEST_CASE("SpecEngine.refusesWhatNamesNoRun")
{
    for (auto const& arguments: std::to_array<std::vector<char const*>>({
             {},
             { "--engine", "fast" },
             { "--features" },
             { "--features", "wasm4" },
             { "--features", "wasm3", "--engine", "jit" },
             { "--features", "wasm3", "--dispatch", "computed" },
             { "--features", "wasm3", "--dispatch", "switch" },
             { "--features", "wasm3", "--engine", "reference", "--dispatch", "switch" },
             { "--features", "wasm3", "--features", "all" },
             { "--features", "wasm3", "--engine", "fast", "--engine", "fast" },
             { "--features", "wasm3", "--engine", "fast", "--dispatch", "switch", "--dispatch", "switch" },
             { "--features", "wasm3", "--fast" },
         }))
    {
        INFO(arguments.size() << " arguments");
        CHECK_FALSE(parsed(arguments).has_value());
    }
}
