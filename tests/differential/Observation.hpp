// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/runtime/Abrupt.hpp>
#include <isoline/runtime/ExecutionMode.hpp>
#include <isoline/runtime/Instantiate.hpp>
#include <isoline/runtime/Interrupt.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/ModuleCode.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/validate/ValidatedModule.hpp>

/// The differential harness: one module run by every participant (an executor and the code it runs), each
/// observation compared exactly with the first participant's.
namespace isoline::differential
{

/// The lanes of the specification's differential testing (§11): what the code is lowered for.
///
/// Both compare every observable exactly. In P4 they are identical, since both execution modes lower every
/// float operation to the canonical-NaN kernels; they part when a compiler lowers Production otherwise.
enum class Lane : uint8_t
{
    Deterministic, ///< Lane 1: ExecutionMode::Deterministic.
    Production,    ///< Lane 2: ExecutionMode::Production.
};

/// The execution mode a lane lowers its code for.
/// @param lane The lane.
/// @return Its mode.
[[nodiscard]] constexpr runtime::ExecutionMode modeOf(Lane lane) noexcept
{
    return lane == Lane::Deterministic ? runtime::ExecutionMode::Deterministic
                                       : runtime::ExecutionMode::Production;
}

/// One engine under comparison: how to make its executor, and the code it runs for a module.
struct Participant
{
    /// Its name in a Divergence: "reference", "interpreter.switch".
    std::string_view name;
    /// Makes a fresh executor, one per store.
    std::function<std::unique_ptr<runtime::ICodeExecutor>()> makeExecutor;
    /// The code its executor runs @p module with, lowered for the mode: null for an engine that runs the
    /// validated module itself (the reference), and null for an engine that lowers when it refused to, which
    /// the runner reports as a divergence of the field "lowering".
    std::function<std::shared_ptr<runtime::ModuleCode const>(std::shared_ptr<validate::ValidatedModule const>,
                                                             runtime::ExecutionMode)>
        lowerModule;
};

/// The limits every participant's store gets, pinned so that a run is the same on every host and every
/// engine: small enough that a generated module reaches each of them (the hard edges), large enough that
/// most calls end on their own.
inline constexpr auto FuzzLimits = runtime::StoreLimits {
    .maxCallDepth = 1'000,
    .maxStackValues = uint64_t { 1 } << 20,
    .maxNestedInvocations = 8,
    .maxMemoryBytes = uint64_t { 64 } << 20,
    .maxTableElements = uint64_t { 1 } << 20,
    .maxMemories = 16,
    .maxTables = 16,
    .maxInstances = 4,
    .maxExceptions = 64,
    .maxExceptionValues = 4'096,
    .maxTypes = 100'000,
    .maxTags = 1'000,
    .maxSteps = 100'000,
};

/// How a call ended: its results, or how it ended abruptly (a refused call too: the arguments do not fit).
using CallOutcome = std::variant<std::vector<runtime::Value>, runtime::Abrupt>;

/// The exception a call threw out of the guest: its tag and its payload, which its address alone does not
/// show.
struct ThrownException
{
    runtime::TagAddr tag = 0;            ///< Its tag's store address.
    std::vector<runtime::Value> payload; ///< Its payload.
    /// Whether both are the same exception.
    [[nodiscard]] bool operator==(ThrownException const&) const = default;
};

/// An exception the store holds after a call: its address, its tag and its payload.
struct LiveException
{
    runtime::ExnAddr address = 0;        ///< Its store address.
    runtime::TagAddr tag = 0;            ///< Its tag's store address.
    std::vector<runtime::Value> payload; ///< Its payload.
    /// Whether both are the same exception.
    [[nodiscard]] bool operator==(LiveException const&) const = default;
};

/// Whether a passive segment can still be read.
enum class SegmentState : uint8_t
{
    Live,    ///< Not dropped: memory.init and table.init read it.
    Dropped, ///< Dropped by data.drop or elem.drop, or an active segment after instantiation.
};

/// The state of an instance's segments: what data.drop and elem.drop change.
struct SegmentStates
{
    std::vector<SegmentState> data;     ///< Each data segment, by segment index.
    std::vector<SegmentState> elements; ///< Each element segment, by segment index.
    /// Whether both are in the same state.
    [[nodiscard]] bool operator==(SegmentStates const&) const = default;
};

/// What one participant did in one export call, or in the instantiation: everything compared, each field
/// exactly (Comparison.hpp's ObservationFields, one row per field).
struct Observation
{
    /// The export called; "(instantiation)" for the instantiation's own observation.
    std::string exportName;
    /// Its results, or how it ended. The instantiation's: no values, or how its start function ended.
    CallOutcome outcome = std::vector<runtime::Value> {};
    /// The steps the invocation used (Store::used(Limit::Steps)).
    uint64_t steps = 0;
    /// The polls of each site during the call, by PollSite; all zero in an unobserved run.
    std::array<uint64_t, std::to_underlying(runtime::LastPollSite) + 1> polls {};
    /// A digest of each memory of the instance (memoryDigest()), by memory index: its bytes and size.
    std::vector<uint64_t> memoryHashes;
    /// Each global of the instance, by global index.
    std::vector<runtime::Value> globals;
    /// Each table of the instance, by table index: every element.
    std::vector<std::vector<runtime::Ref>> tables;
    /// The first refused memory.grow and table.grow of the call, and why.
    runtime::GrowthRecord growth;
    /// The exceptions the store holds after the call, by ascending address.
    std::vector<LiveException> exceptions;
    /// The instance's data and element segments: which a call dropped.
    SegmentStates segments;
    /// The stack values still charged after the call: zero, unless a frame leaked its charge.
    uint64_t chargedStackValues = 0;
    /// The calls of the recording host's functions during the call, in order (RecordingHost).
    std::vector<std::string> hostTrace;
    /// The exception the call threw, when it threw one.
    std::optional<ThrownException> thrown;
};

/// Everything one participant did with one module, in one lane and one observation mode.
struct ModuleObservation
{
    /// Whether the module instantiated, and if not, why: the error as text (describe()), empty on success.
    std::string instantiation;
    /// What instantiating did: the start function, the data and element copies, the imports.
    Observation instantiated;
    /// Each export call, in the order the module exports its functions.
    std::vector<Observation> calls;
};

/// Where a participant observed otherwise than the first one.
struct Divergence
{
    std::string participant; ///< The participant that diverged, and its observation mode.
    std::string
        field; ///< The field that differs: a row of ObservationFields, or "instantiation", "lowering".
    std::string expected; ///< The first participant's value, and the call it was observed in.
    std::string actual;   ///< The diverging participant's.
};

/// Which call an observer interrupts: derived from the argument seed by a fuzzer.
///
/// The interrupt is level-triggered and never lowered (runtime::Interrupter), so the interrupted call is one
/// more call after every complete one: those keep their polls, and the interrupted one is the last the store
/// runs.
struct InterruptionPlan
{
    /// The poll of the interrupted call, counted from 1, at which the store's interrupt is raised; none (no
    /// interrupted call) by default.
    std::optional<uint64_t> interruptAtPoll;
    /// Which exported function the interrupted call calls: this, modulo the function exports, in export
    /// order.
    uint64_t exportPick = 0;
};

} // namespace isoline::differential
