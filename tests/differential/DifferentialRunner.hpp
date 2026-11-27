// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "Observation.hpp"
#include <isoline/fastinterpreter/Dispatch.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/validate/ValidatedModule.hpp>

namespace isoline::differential
{

/// The reference interpreter as a participant: it runs the validated module itself.
/// @return The participant.
[[nodiscard]] Participant referenceParticipant();

/// The fast interpreter under @p dispatch as a participant: its code lowered once per module and lane,
/// within the default compile budget (LoweringBudget). Precondition: @p dispatch is available.
/// @param dispatch The dispatch.
/// @return The participant, named "fast.<dispatch>".
[[nodiscard]] Participant fastParticipant(fastinterpreter::Dispatch dispatch);

/// Every engine of this build: the reference first, then the fast interpreter under each available dispatch.
/// @return The participants.
[[nodiscard]] std::vector<Participant> everyEngine();

/// The compile budget a fast participant lowers within: api::LoadLimits::maxCompileBytes' default.
inline constexpr auto LoweringBudget = uint64_t { 256 } << 20;

/// Whether the harness compares a module, and why not.
enum class Applicability : uint8_t
{
    Compared,          ///< It loads, and its imports can be bound alike in every store.
    Invalid,           ///< It does not decode or validate under the cut's features: no engine runs it.
    UnsupportedImport, ///< An import names a concrete heap or a non-nullable reference (checkImports()).
};

/// Whether @p module is compared, and loads it.
/// @param module The module's bytes.
/// @return The validated module, or why it is not compared.
[[nodiscard]] std::expected<std::shared_ptr<validate::ValidatedModule const>, Applicability>
loadForComparison(std::span<std::byte const> module);

/// How a run is watched: with a poll observer (which counts the polls, and interrupts as planned), or
/// without, through the executors' unobserved fast paths. The steps of both are compared.
enum class ObservationMode : uint8_t
{
    Unobserved,
    Observed,
};

/// What one participant observed of a module in one observation mode.
struct ParticipantObservation
{
    std::string_view participant;                       ///< The participant's name.
    ObservationMode mode = ObservationMode::Unobserved; ///< How it was watched.
    /// What it did, or nothing when it lowers code and refused to lower the module.
    std::optional<ModuleObservation> observation;
};

/// Runs a module on every participant and compares what each observed with the first participant's, field
/// by field (Comparison.hpp's ObservationFields), exactly.
///
/// Each participant gets a fresh store of the limits, the same RecordingHost imports, and, in the observed
/// mode, a poll observer that counts the polls of each call and interrupts the store as planned. The module
/// is instantiated once per participant and mode, then each exported function is called, in export order,
/// CallRounds times over, with arguments derived from the seed (argumentsFor()); after the instantiation and
/// after each call, everything an Observation holds is read. The unobserved runs are compared with the first
/// participant's unobserved run, the observed ones with its observed run.
class DifferentialRunner
{
  public:
    /// The rounds of calls: every export is called this many times, with fresh arguments each.
    static constexpr auto CallRounds = uint32_t { 2 };

    /// A runner comparing @p participants in @p lane.
    /// @param participants The engines; the first one's observations are the expected ones. At least two.
    /// @param lane The lane: what the participants lower their code for.
    /// @param limits The limits of every participant's store (FuzzLimits for generated modules).
    /// @param interruptions Data: for each call, the poll index k at which an observer interrupts the store
    ///                      (none by default).
    DifferentialRunner(std::vector<Participant> participants,
                       Lane lane,
                       runtime::StoreLimits limits,
                       InterruptionPlan interruptions);

    /// Runs @p module on every participant and compares.
    /// @param module The module's bytes. One the harness does not compare (loadForComparison()) agrees.
    /// @param argumentSeed What the calls' arguments are derived from.
    /// @return Nothing, or the first divergence.
    [[nodiscard]] std::expected<void, Divergence> run(std::span<std::byte const> module,
                                                      std::span<std::byte const> argumentSeed) const;

    /// What every participant observed of @p module, in each mode: what run() compares, for printing.
    /// @param module The module's bytes.
    /// @param argumentSeed What the calls' arguments are derived from.
    /// @return Each participant's observation in each mode (unobserved first), or why the module is not
    ///         compared.
    [[nodiscard]] std::expected<std::vector<ParticipantObservation>, Applicability> observe(
        std::span<std::byte const> module, std::span<std::byte const> argumentSeed) const;

    /// observe(), taking the first participant's observations from @p first instead of observing it again:
    /// what another lane's runner observed of the same module and seed. Precondition: the first participant
    /// is the same engine in both runners, and one whose code does not depend on the lane (the reference,
    /// which runs the validated module itself), so that its observations are the same in every lane.
    /// @param module The module's bytes.
    /// @param argumentSeed What the calls' arguments are derived from.
    /// @param first Observations of the first participant, in each mode; one missing is observed.
    /// @return As observe().
    [[nodiscard]] std::expected<std::vector<ParticipantObservation>, Applicability> observe(
        std::span<std::byte const> module,
        std::span<std::byte const> argumentSeed,
        std::span<ParticipantObservation const> first) const;

    /// Compares what observe() returned of @p module: each participant's observation with the first
    /// participant's of the same mode, field by field. What run() does after observing. A memory divergence
    /// names the first offset that differs, from both participants run once more.
    /// @param module The module's bytes.
    /// @param argumentSeed The seed observe() was given.
    /// @param observations observe()'s observations of the module.
    /// @return Nothing, or the first divergence.
    [[nodiscard]] std::expected<void, Divergence> compare(
        std::span<std::byte const> module,
        std::span<std::byte const> argumentSeed,
        std::span<ParticipantObservation const> observations) const;

  private:
    std::vector<Participant> _participants;
    Lane _lane;
    runtime::StoreLimits _limits;
    InterruptionPlan _interruptions;
};

} // namespace isoline::differential
