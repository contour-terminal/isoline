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
#include <utility>
#include <vector>

#include "DifferentialRunner.hpp"
#include "Observation.hpp"
#include "Reach.hpp"
#include "WasmtimeOracle.hpp"

namespace isoline::differential
{

/// The argument seed of corpus module @p seed (`<seed>.wasm`): its number's eight bytes, little-endian.
/// @param seed The module's seed.
/// @return The bytes.
[[nodiscard]] std::array<std::byte, 8> seedBytes(uint64_t seed) noexcept;

/// The interruption plan a seed asks for: after every complete call, one more call of an export the seed
/// picks, interrupted at a poll from 1 to MaxInterruptPoll, so that partial calls (bulk writes, sweeps,
/// unwinding) are compared as well as complete ones.
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

/// How Isoline's call of an export ended, for the oracle.
enum class CallEnding : uint8_t
{
    NotRun,      ///< Not called: the store's limits refused the instance.
    Abruptly,    ///< A trap, an exception or a host failure.
    WithResults, ///< It returned.
};

/// One export call compared with the oracle.
struct OracleComparison
{
    std::string exportName;                             ///< The export.
    OracleVerdict verdict = OracleVerdict::NotCompared; ///< The verdict.
    std::string detail;                                 ///< Both outcomes, or why it was not compared.
    /// Why the oracle had no answer, when it was asked and had none.
    std::optional<OracleErrorKind> error;
    /// Whether Isoline's call returned results: with OracleErrorKind::Refused, what a replay counts apart.
    CallEnding isoline = CallEnding::NotRun;
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

/// A corpus module: its file and its seed.
using CorpusModule = std::pair<std::filesystem::path, uint64_t>;

/// The modules of shard @p shard of @p shards: a contiguous run of @p modules, the shards together covering
/// each module once.
/// @param modules The corpus, by ascending seed.
/// @param shard The shard, from 0.
/// @param shards How many shards. Precondition: @p shard is below it.
/// @return Its modules.
[[nodiscard]] std::span<CorpusModule const> shardOf(std::span<CorpusModule const> modules,
                                                    std::size_t shard,
                                                    std::size_t shards) noexcept;

/// How much of a corpus a replay takes: lane 1 every module; lane 2 and the oracle the seeds divisible by
/// the sample, so that a pull request replays an eighth of them and the nightly run all.
struct ReplayScope
{
    uint64_t sample = 1; ///< Lane 2 and the oracle replay the seeds divisible by this; 1 for all of them.
};

/// Whether lane 2 and the oracle replay @p seed under @p scope.
/// @param seed The module's seed.
/// @param scope The scope.
/// @return Whether it is in the sample.
[[nodiscard]] constexpr bool isSampled(uint64_t seed, ReplayScope const& scope) noexcept
{
    return scope.sample <= 1 || seed % scope.sample == 0;
}

/// How many Applicability values there are.
inline constexpr auto ApplicabilityCount =
    std::size_t { std::to_underlying(Applicability::UnsupportedImport) + 1 };
/// How many OracleVerdict values there are.
inline constexpr auto OracleVerdictCount = std::size_t { std::to_underlying(OracleVerdict::NotCompared) + 1 };

/// What a replay of corpus modules counted: its modules by Applicability, the export calls of the compared
/// ones, the oracle's verdicts and why it had no answer, what the observed runs reached, and every
/// divergence with its reproducer.
struct ReplayTally
{
    std::array<uint64_t, ApplicabilityCount> modules {};          ///< The modules, by Applicability.
    uint64_t calls = 0;                                           ///< Lane 1's export calls, unobserved.
    uint64_t sampled = 0;                                         ///< The compared modules in the sample.
    std::array<uint64_t, OracleVerdictCount> verdicts {};         ///< The oracle's calls, by verdict.
    std::array<uint64_t, OracleErrorRows.size()> oracleErrors {}; ///< Its unanswered calls, by kind.
    /// The calls Wasmtime refused for a limit of its own (OracleErrorKind::Refused) while Isoline returned
    /// results: no failure, but a count a replay shows.
    uint64_t refusedWhileIsolineReturned = 0;
    Reach reach;                          ///< What lane 1's observed runs of the first participant reached.
    std::vector<std::string> divergences; ///< Each divergence, and how to reproduce it.

    /// The oracle's unanswered calls that are the harness's fault (OracleFault::HarnessFault).
    [[nodiscard]] uint64_t harnessFaults() const noexcept;
};

/// Replays @p modules on every engine: lane 1 every module, lane 2 the sample (the reference observed once,
/// in lane 1, and reused), and, given an oracle, the sample's oracle-eligible exports with Wasmtime.
/// @param modules The modules.
/// @param scope How much of them lane 2 and the oracle take.
/// @param oracle Wasmtime, or null to leave it out.
/// @return What it counted.
[[nodiscard]] ReplayTally replayCorpus(std::span<CorpusModule const> modules,
                                       ReplayScope const& scope,
                                       WasmtimeOracle const* oracle);

/// A replay's counts as text, one line per kind of count.
/// @param tally The counts.
/// @return The text.
[[nodiscard]] std::string describe(ReplayTally const& tally);

/// A verdict as text.
/// @param verdict The verdict.
/// @return The text.
[[nodiscard]] std::string_view describe(OracleVerdict verdict) noexcept;

} // namespace isoline::differential
