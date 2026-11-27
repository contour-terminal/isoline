// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "DifferentialRunner.hpp"
#include "Observation.hpp"
#include "WasmtimeOracle.hpp"

namespace isoline::differential
{

/// The argument seed of corpus module @p seed (`<seed>.wasm`): its number's eight bytes, little-endian.
/// @param seed The module's seed.
/// @return The bytes.
[[nodiscard]] std::array<std::byte, 8> seedBytes(uint64_t seed) noexcept;

/// The interruption plan a seed asks for: none for half the seeds, and for the other half an interrupt at
/// a poll from 1 to MaxInterruptPoll, so that both complete calls and partial ones (bulk writes, sweeps,
/// unwinding) are compared.
/// @param seed The argument seed.
/// @return The plan.
[[nodiscard]] InterruptionPlan interruptionFor(std::span<std::byte const> seed) noexcept;

/// The latest poll interruptionFor() interrupts at.
inline constexpr auto MaxInterruptPoll = uint64_t { 1'024 };

/// The lane a command line names: deterministic or production.
/// @param name The name.
/// @return The lane, or none.
[[nodiscard]] std::optional<Lane> laneNamed(std::string_view name) noexcept;

/// Every lane, in order.
inline constexpr auto EveryLane = std::to_array<Lane>({ Lane::Deterministic, Lane::Production });

/// Why a module is not compared, as text.
/// @param applicability The reason.
/// @return The text.
[[nodiscard]] std::string_view describe(Applicability applicability) noexcept;

/// An observation mode as text: observed or unobserved.
/// @param mode The mode.
/// @return The text.
[[nodiscard]] std::string_view describe(ObservationMode mode) noexcept;

/// A module's bytes from a file: a binary as it is, a `.wat` file compiled from the text format.
/// @param path The file.
/// @return The binary, or why it could not be read or compiled.
[[nodiscard]] std::expected<std::vector<std::byte>, std::string> readModule(
    std::filesystem::path const& path);

/// The corpus modules of @p directory: every `<seed>.wasm`, by ascending seed.
/// @param directory The corpus directory.
/// @return Each module's path and seed.
[[nodiscard]] std::vector<std::pair<std::filesystem::path, uint64_t>> corpusModules(
    std::filesystem::path const& directory);

/// Whether a call agreed with the oracle, or why it was not compared.
enum class OracleVerdict : uint8_t
{
    Agrees,      ///< Isoline's outcome agrees with Wasmtime's.
    Disagrees,   ///< It does not: a divergence from the oracle.
    NotCompared, ///< The oracle has no answer (OracleError), or Isoline ended on one of its own limits.
};

/// One export call compared with the oracle.
struct OracleComparison
{
    std::string exportName;                             ///< The export.
    OracleVerdict verdict = OracleVerdict::NotCompared; ///< The verdict.
    std::string detail;                                 ///< Both outcomes, or why it was not compared.
};

/// Compares each eligible export of @p module (isOracleEligible()) with the oracle: each is called once on a
/// fresh instance of the reference interpreter, as Wasmtime calls it, with arguments derived from @p seed. A
/// call Isoline ends on one of its own limits (steps, depth, exceptions, a grow refused for the store's
/// limit) is not compared: Wasmtime's limits are others.
/// @param oracle The oracle.
/// @param module The module's bytes.
/// @param seed The argument seed.
/// @return One comparison per eligible export; none when the module is not compared or imports.
[[nodiscard]] std::vector<OracleComparison> compareWithOracle(WasmtimeOracle const& oracle,
                                                              std::span<std::byte const> module,
                                                              std::span<std::byte const> seed);

/// A verdict as text.
/// @param verdict The verdict.
/// @return The text.
[[nodiscard]] std::string_view describe(OracleVerdict verdict) noexcept;

} // namespace isoline::differential
