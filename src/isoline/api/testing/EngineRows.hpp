// SPDX-License-Identifier: Apache-2.0
//
// Test-only: the engines every engine-parametrised api case runs on, one row each, so that a case is
// written once and an engine is added as a row (GENERATE(from_range(engineRows()))). A case names its
// engine through a row, never through EngineConfig's default engine.
#pragma once

#include <catch2/catch_tostring.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <isoline/api/Engine.hpp>
#include <isoline/api/EngineIdentity.hpp>
#include <isoline/api/ExecutionEngines.hpp>
#include <isoline/api/Types.hpp>
#include <isoline/api/testing/ContractLimits.hpp>

namespace isoline::api::testing
{

/// An engine an api case runs on: its name, the execution engine, its dispatch and the features it is
/// configured with.
struct EngineRow
{
    std::string_view name;                               ///< How a case's report names the row.
    ExecutionEngine engine = ExecutionEngine::Reference; ///< The engine that runs the code.
    Dispatch dispatch = Dispatch::Switch;      ///< The fast engine's dispatch; the reference ignores it.
    FeatureSet features = FeatureSet::wasm3(); ///< The features modules load with.
};

/// Every engine row, in the order the cases run them: the reference engine, then the fast one under each
/// dispatch; a build runs those it compiled (engineRows()).
inline constexpr auto EngineRowTable = std::to_array<EngineRow>({
    { .name = "reference", .engine = ExecutionEngine::Reference, .features = FeatureSet::wasm3() },
    { .name = "fast-switch",
      .engine = ExecutionEngine::Fast,
      .dispatch = Dispatch::Switch,
      .features = FeatureSet::wasm3() },
    { .name = "fast-tailcall",
      .engine = ExecutionEngine::Fast,
      .dispatch = Dispatch::TailCall,
      .features = FeatureSet::wasm3() },
});

/// Whether this build runs @p row: the reference engine always, the fast one under a dispatch it compiled.
/// @param row The row.
/// @return Whether the row's engine can be constructed.
[[nodiscard]] inline bool isAvailable(EngineRow const& row) noexcept
{
    return engines::rowOf(row.engine)->dispatch == engines::DispatchUse::None
           || fastinterpreter::isAvailable(row.dispatch);
}

/// Every engine row this build runs (EngineRowTable, isAvailable()), as GENERATE(from_range(…)) takes it.
/// @return The rows.
[[nodiscard]] inline std::span<EngineRow const> engineRows()
{
    static auto const rows = [] {
        auto available = std::vector<EngineRow> {};
        std::ranges::copy_if(EngineRowTable, std::back_inserter(available), &isAvailable);
        return available;
    }();
    return rows;
}

namespace contract
{
    /// An engine configuration of @p row's engine and features, under @p limits; @p maxSteps bounds a
    /// call that should have been interrupted.
    /// @param limits The store caps.
    /// @param row The engine and its features.
    /// @param maxSteps The steps a call may take; a backstop only, for the cases that spin.
    /// @return The configuration.
    [[nodiscard]] inline EngineConfig configOf(ContractLimits limits,
                                               EngineRow const& row,
                                               uint64_t maxSteps = std::numeric_limits<uint64_t>::max())
    {
        auto config = EngineConfig {};
        config.features = row.features;
        config.engine = row.engine;
        config.dispatch = row.dispatch;
        config.store.maxMemoryBytes = limits.memoryBytes;
        config.store.maxTableElements = limits.tableElements;
        config.store.maxSteps = maxSteps;
        return config;
    }

    /// An engine configuration of @p row's engine and features, under the whole of @p limits.
    /// @param limits The store limits, every one of them.
    /// @param row The engine and its features.
    /// @return The configuration.
    [[nodiscard]] inline EngineConfig configOf(StoreLimits const& limits, EngineRow const& row)
    {
        auto config = EngineConfig {};
        config.features = row.features;
        config.engine = row.engine;
        config.dispatch = row.dispatch;
        config.store = limits;
        return config;
    }
} // namespace contract

} // namespace isoline::api::testing

/// How Catch2 reports an engine row: by its name, so that a failing GENERATE case names its engine.
template <>
struct Catch::StringMaker<isoline::api::testing::EngineRow>
{
    /// The row's name.
    /// @param row The row.
    /// @return Its name.
    static std::string convert(isoline::api::testing::EngineRow const& row)
    {
        return std::string { row.name };
    }
};
