// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Observation.hpp"
#include <isoline/runtime/Abrupt.hpp>
#include <isoline/runtime/Instantiate.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/Value.hpp>

namespace isoline::differential
{

/// A digest of a memory: its size and every byte, so that two memories of equal digests are, but for a
/// collision of the 64-bit hash, equal.
/// @param bytes The memory's bytes.
/// @return The digest.
[[nodiscard]] uint64_t memoryDigest(std::span<std::byte const> bytes) noexcept;

/// A value as a Divergence shows it: its type and its bits, exactly (`f32:0x7fc00000`, `ref.func 3`).
/// @param value The value.
/// @return The text.
[[nodiscard]] std::string describe(runtime::Value const& value);

/// Values as a Divergence shows them: each value, in brackets.
/// @param values The values.
/// @return The text.
[[nodiscard]] std::string describe(std::span<runtime::Value const> values);

/// How a call ended abruptly, every field of it.
/// @param abrupt The trap, exception or host failure.
/// @return The text.
[[nodiscard]] std::string describe(runtime::Abrupt const& abrupt);

/// How a call ended: its results, or how it ended abruptly.
/// @param outcome The outcome.
/// @return The text.
[[nodiscard]] std::string describe(CallOutcome const& outcome);

/// Why an instantiation failed, every field of it.
/// @param error The error.
/// @return The text.
[[nodiscard]] std::string describe(runtime::InstantiationError const& error);

/// A refused growth record: the reason of each kind's first refused grow, or none.
/// @param record The record.
/// @return The text.
[[nodiscard]] std::string describe(runtime::GrowthRecord const& record);

/// The polls of each site, by PollSite, as text: `function entry 3, loop back-edge 0, …`.
/// @param polls The counts.
/// @return The text.
[[nodiscard]] std::string describePolls(std::span<uint64_t const> polls);

/// The thrown exception of a call as text, or "none".
/// @param thrown The exception.
/// @return The text.
[[nodiscard]] std::string describe(std::optional<ThrownException> const& thrown);

/// Each table's elements as text.
/// @param tables The tables.
/// @return The text.
[[nodiscard]] std::string describe(std::span<std::vector<runtime::Ref> const> tables);

/// Numbers or names as text, comma-separated in brackets.
/// @param items The numbers.
/// @return The text.
[[nodiscard]] std::string describe(std::span<uint64_t const> items);

/// Lines as text, comma-separated in brackets.
/// @param items The lines.
/// @return The text.
[[nodiscard]] std::string describe(std::span<std::string const> items);

/// One field of an Observation, as the comparison reads it: a row of ObservationFields.
struct ObservationField
{
    std::string_view name; ///< The field's name, as a Divergence names it.
    /// Whether two observations agree on it, exactly.
    bool (*agree)(Observation const& expected, Observation const& actual);
    /// The field's value as text.
    std::string (*show)(Observation const& observation);
};

/// Every field of an Observation but its export's name (the runner's, the same in every participant), in
/// the order they are compared: what the call did (the exception it threw, whose address the outcome shows
/// but not its payload, then its outcome), what it charged (steps and polls), and then the state it left.
/// Adding a field to Observation is adding a row here, and a perturbation to DifferentialRunner_test's
/// table, which checks there is one per row.
inline constexpr auto ObservationFields = std::to_array<ObservationField>({
    { .name = "thrown",
      .agree = [](Observation const& expected,
                  Observation const& actual) { return expected.thrown == actual.thrown; },
      .show = [](Observation const& observation) { return describe(observation.thrown); } },
    { .name = "outcome",
      .agree = [](Observation const& expected,
                  Observation const& actual) { return expected.outcome == actual.outcome; },
      .show = [](Observation const& observation) { return describe(observation.outcome); } },
    { .name = "steps",
      .agree = [](Observation const& expected,
                  Observation const& actual) { return expected.steps == actual.steps; },
      .show = [](Observation const& observation) { return std::to_string(observation.steps); } },
    { .name = "polls",
      .agree = [](Observation const& expected,
                  Observation const& actual) { return expected.polls == actual.polls; },
      .show = [](Observation const& observation) { return describePolls(observation.polls); } },
    { .name = "memoryHashes",
      .agree = [](Observation const& expected,
                  Observation const& actual) { return expected.memoryHashes == actual.memoryHashes; },
      .show =
          [](Observation const& observation) {
              return describe(std::span<uint64_t const> { observation.memoryHashes });
          } },
    { .name = "globals",
      .agree = [](Observation const& expected,
                  Observation const& actual) { return expected.globals == actual.globals; },
      .show =
          [](Observation const& observation) {
              return describe(std::span<runtime::Value const> { observation.globals });
          } },
    { .name = "tables",
      .agree = [](Observation const& expected,
                  Observation const& actual) { return expected.tables == actual.tables; },
      .show =
          [](Observation const& observation) {
              return describe(std::span<std::vector<runtime::Ref> const> { observation.tables });
          } },
    { .name = "growth",
      .agree = [](Observation const& expected,
                  Observation const& actual) { return expected.growth == actual.growth; },
      .show = [](Observation const& observation) { return describe(observation.growth); } },
    { .name = "liveExceptions",
      .agree = [](Observation const& expected,
                  Observation const& actual) { return expected.liveExceptions == actual.liveExceptions; },
      .show = [](Observation const& observation) { return std::to_string(observation.liveExceptions); } },
    { .name = "chargedStackValues",
      .agree =
          [](Observation const& expected, Observation const& actual) {
              return expected.chargedStackValues == actual.chargedStackValues;
          },
      .show = [](Observation const& observation) { return std::to_string(observation.chargedStackValues); } },
    { .name = "hostTrace",
      .agree = [](Observation const& expected,
                  Observation const& actual) { return expected.hostTrace == actual.hostTrace; },
      .show =
          [](Observation const& observation) {
              return describe(std::span<std::string const> { observation.hostTrace });
          } },
});

/// Where two observations of a module differ: the field, and both values with the call they came from.
struct FieldDifference
{
    std::string field;    ///< The field: a row of ObservationFields, "instantiation" or "calls".
    std::string expected; ///< The expected value, after the call's name.
    std::string actual;   ///< The actual value, after the call's name.
};

/// The first difference between two observations of a module, comparing the instantiation, then each call
/// in order, each by the rows of ObservationFields.
/// @param expected The first participant's observation.
/// @param actual Another participant's observation of the same module, lane and observation mode.
/// @return The first difference, or none when they agree on every field.
[[nodiscard]] std::optional<FieldDifference> compare(ModuleObservation const& expected,
                                                     ModuleObservation const& actual);

/// An observation as text, one field per line: what isoline-differential-replay prints.
/// @param observation The observation.
/// @return The text, each line ending with a newline.
[[nodiscard]] std::string describe(Observation const& observation);

/// A module observation as text: its instantiation, then each call.
/// @param observation The observation.
/// @return The text, each line ending with a newline.
[[nodiscard]] std::string describe(ModuleObservation const& observation);

} // namespace isoline::differential
