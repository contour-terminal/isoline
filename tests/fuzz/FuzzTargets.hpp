// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ModuleGenerator.hpp"
#include <Observation.hpp>
#include <isoline/interpreter/Lowering.hpp>

// What each fuzz target does with one input, apart from libFuzzer's entry point, so that the unit tests run
// the same code on chosen inputs and the targets stay one call each (DifferentialFuzzer.cpp, LoadFuzzer.cpp,
// TextFuzzer.cpp).

namespace isoline::fuzz
{

/// The leading bytes of a differential input that are its argument seed: a u64, little-endian, as the
/// corpus names a module's seed (`<seed>.wasm`), so that a finding becomes a regression module as it is.
inline constexpr auto ArgumentSeedBytes = std::size_t { 8 };

/// A differential input, split.
struct DifferentialInput
{
    uint64_t seed = 0;                        ///< The argument seed (differential::seedBytes()).
    std::span<std::byte const> moduleEntropy; ///< What the module is generated from: the rest.
};

/// Splits a differential input: its first ArgumentSeedBytes are the seed (missing bytes zero), the rest
/// the module's entropy.
/// @param input The fuzzer's bytes.
/// @return The seed and the entropy.
[[nodiscard]] DifferentialInput splitDifferentialInput(std::span<std::byte const> input) noexcept;

/// Whether a lane is worth fuzzing yet.
enum class LaneActivity : uint8_t
{
    InactiveUntilP5, ///< Identical to an active lane until another engine lowers it otherwise.
    Active,          ///< Fuzzed: by default, in campaigns and in CI's smoke run.
};

/// One lane of the differential target: whether it is fuzzed, and why.
struct DifferentialLaneRow
{
    differential::Lane lane;
    LaneActivity activity;
    std::string_view reason;
};

/// Every lane, in Lane order. Lane 2 (Production) lowers exactly as lane 1 until P5's compiler lowers
/// Production otherwise (DifferentialRunner.hpp), so it takes no campaign hours and no CI smoke run until
/// then; P5 makes it Active.
inline constexpr auto DifferentialLanes = std::to_array<DifferentialLaneRow>({
    { .lane = differential::Lane::Deterministic,
      .activity = LaneActivity::Active,
      .reason = "lane 1: every engine of P4" },
    { .lane = differential::Lane::Production,
      .activity = LaneActivity::InactiveUntilP5,
      .reason = "lane 2 lowers as lane 1 until P5's compiler lowers Production otherwise" },
});

/// The lanes a differential run compares.
enum class LaneSelection : uint8_t
{
    Active,        ///< Every Active lane of DifferentialLanes, the first's reference observations reused.
    Deterministic, ///< Lane 1 alone.
    Production,    ///< Lane 2 alone, active or not.
};

/// The generator limits the differential target builds its modules with: the defaults.
inline constexpr auto DifferentialGeneratorLimits = GeneratorLimits {};

/// Generates a module from @p input and runs it on every engine of the build, in the selected lanes, under
/// differential::FuzzLimits, with the interruption plan the seed asks for (differential::interruptionFor()).
/// A generated module that does not validate is the generator's divergence: participant "generator", field
/// "validation".
/// @param input The fuzzer's bytes (splitDifferentialInput()).
/// @param lanes The lanes.
/// @return Nothing, or the first divergence.
[[nodiscard]] std::expected<void, differential::Divergence> runDifferential(std::span<std::byte const> input,
                                                                            LaneSelection lanes);

/// How a module fed to the load target ended.
enum class LoadVerdict : uint8_t
{
    Refused,    ///< It does not decode or validate.
    OverBudget, ///< Lowering refused it at the compile budget.
    Lowered,    ///< It lowered within the budget and every published bound.
};

/// One lowered function's code words, and its bound (interpreter::codeBoundWords()).
struct FunctionWords
{
    uint64_t words = 0;
    uint64_t bound = 0;
};

/// What a lowering measured, which judgeLowering() holds against linear lowering's published bounds.
struct LoweringMeasures
{
    uint64_t peak = 0;   ///< The arena's peak.
    uint64_t budget = 0; ///< Its budget.
    /// Why lowering refused the module, or nothing when it lowered.
    std::optional<interpreter::LoweringFailure> refusal;
    std::vector<FunctionWords> functions; ///< Each lowered function's code words.
    uint64_t work = 0;                    ///< LoweringStats::work.
    uint64_t bodyUnits = 0;               ///< LoweringStats::bodyUnits.
};

/// Holds a lowering against its published bounds (Lowering.hpp), each a row of its own: the arena's peak
/// within its budget, the compile budget the only refusal, every function's code within codeBoundWords(),
/// and the work within WorkPerBodyUnit per body unit.
/// @param measures What the lowering measured.
/// @return OverBudget or Lowered, or the bound broken, as text.
[[nodiscard]] std::expected<LoadVerdict, std::string> judgeLowering(LoweringMeasures const& measures);

/// The compile budget the load target lowers within: small, so that a lowering that is not linear in its
/// input meets it, or the fuzzer's RSS limit, well before its timeout.
inline constexpr auto LoadCompileBudget = uint64_t { 16 } << 20;

/// Loads @p bytes with every feature and lowers the module for the Switch dispatch within @p compileBudget,
/// then judges the lowering (judgeLowering()).
/// @param bytes The module's bytes.
/// @param compileBudget The arena's budget.
/// @return The verdict, or the bound the lowering broke, as text.
[[nodiscard]] std::expected<LoadVerdict, std::string> runLoad(std::span<std::byte const> bytes,
                                                              uint64_t compileBudget);

/// The largest binary the text target compiles: a source that asks for more is refused as it reaches it.
inline constexpr auto TextModuleBytes = std::size_t { 4 } << 20;

/// Compiles @p bytes as the text format, and gives a binary it compiles to the load target (runLoad()).
/// @param bytes The source.
/// @return Refused when the text does not compile, else runLoad()'s verdict or the bound broken.
[[nodiscard]] std::expected<LoadVerdict, std::string> runText(std::span<std::byte const> bytes);

} // namespace isoline::fuzz
