// SPDX-License-Identifier: Apache-2.0
//
// Test-only: the engines every engine-parametrised api case runs on, one row each, so that a case is
// written once and an engine is added as a row (GENERATE(from_range(engineRows()))). A case names its
// engine through a row, never through EngineConfig's default engine.
#pragma once

#include <catch2/catch_tostring.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>

#include <isoline/api/Engine.hpp>
#include <isoline/api/EngineIdentity.hpp>
#include <isoline/api/Types.hpp>
#include <isoline/api/testing/ContractLimits.hpp>

namespace isoline::api::testing
{

/// An engine an api case runs on: its name, the execution engine and the features it is configured with.
struct EngineRow
{
    std::string_view name;                               ///< How a case's report names the row.
    ExecutionEngine engine = ExecutionEngine::Reference; ///< The engine that runs the code.
    FeatureSet features = FeatureSet::wasm3();           ///< The features modules load with.
};

/// Every engine row, in the order the cases run them; the first is the default engine of the cases
/// that name none (contract::configOf(ContractLimits, uint64_t)).
inline constexpr auto EngineRowTable = std::to_array<EngineRow>({
    { .name = "reference", .engine = ExecutionEngine::Reference, .features = FeatureSet::wasm3() },
});

/// Every engine row (EngineRowTable), as GENERATE(from_range(…)) takes it.
/// @return The rows.
[[nodiscard]] inline std::span<EngineRow const> engineRows() noexcept
{
    return EngineRowTable;
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
        config.store = limits;
        return config;
    }

    /// An engine configuration of the first engine row (EngineRowTable), under @p limits.
    /// @param limits The store caps.
    /// @param maxSteps The steps a call may take; a backstop only, for the cases that spin.
    /// @return The configuration.
    [[nodiscard]] inline EngineConfig configOf(ContractLimits limits,
                                               uint64_t maxSteps = std::numeric_limits<uint64_t>::max())
    {
        return configOf(limits, EngineRowTable.front(), maxSteps);
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
