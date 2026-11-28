// SPDX-License-Identifier: Apache-2.0
#include "DifferentialRunner.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <memory_resource>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "Arguments.hpp"
#include "Comparison.hpp"
#include "Observation.hpp"
#include "RecordingHost.hpp"
#include <isoline/Features.hpp>
#include <isoline/binary/DecodeLimits.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/interpreter/Dispatch.hpp>
#include <isoline/interpreter/Interpreter.hpp>
#include <isoline/interpreter/Lowering.hpp>
#include <isoline/reference/ReferenceInterpreter.hpp>
#include <isoline/runtime/CountingArena.hpp>
#include <isoline/runtime/ExecutionMode.hpp>
#include <isoline/runtime/HostFunction.hpp>
#include <isoline/runtime/Instantiate.hpp>
#include <isoline/runtime/Interrupt.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/LinearMemory.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/validate/ValidatedModule.hpp>

namespace isoline::differential
{

namespace
{
    /// The name an interpreter participant has under each dispatch: string literals, which a Participant's
    /// name views.
    struct InterpreterParticipantName
    {
        interpreter::Dispatch dispatch; ///< The dispatch.
        std::string_view name;          ///< The participant's name.
    };
    constexpr auto InterpreterParticipantNames = std::to_array<InterpreterParticipantName>({
        { .dispatch = interpreter::Dispatch::Switch, .name = "interpreter.switch" },
        { .dispatch = interpreter::Dispatch::TailCall, .name = "interpreter.tailcall" },
    });

    /// The export name of the instantiation's own observation.
    constexpr auto InstantiationName = std::string_view { "(instantiation)" };

    [[nodiscard]] std::string_view nameOf(Lane lane) noexcept
    {
        return lane == Lane::Deterministic ? "deterministic" : "production";
    }

    [[nodiscard]] std::string_view nameOf(ObservationMode mode) noexcept
    {
        return mode == ObservationMode::Observed ? "observed" : "unobserved";
    }

    /// Whether a call is the one the InterruptionPlan interrupts.
    enum class CallKind : uint8_t
    {
        Complete,    ///< Runs as far as it goes on its own.
        Interrupted, ///< The planned call: interrupted at the plan's poll.
    };

    /// Counts the polls of each call, and raises the store's interrupt at the planned poll of the planned
    /// call.
    class CallObserver final: public runtime::IPollObserver
    {
      public:
        explicit CallObserver(InterruptionPlan plan) noexcept: _plan { plan } {}

        /// Wires the store's interrupter, which exists only once the store does: the one call after
        /// construction that the store and its observer, which know each other, need.
        void attach(std::shared_ptr<runtime::Interrupter> interrupter) noexcept
        {
            _interrupter = std::move(interrupter);
        }

        /// Starts counting a call afresh.
        /// @param kind Whether it is the call to interrupt.
        void beginCall(CallKind kind) noexcept
        {
            _polls = {};
            _pollsOfCall = 0;
            _kind = kind;
        }

        /// The polls of each site since beginCall().
        [[nodiscard]] std::array<uint64_t, runtime::PollSiteTable.size()> const& polls() const noexcept
        {
            return _polls;
        }

        void onPoll(runtime::PollSite site) noexcept override
        {
            ++_polls[std::to_underlying(site)];
            ++_pollsOfCall;
            if (_kind == CallKind::Interrupted && _plan.interruptAtPoll == _pollsOfCall && _interrupter)
                _interrupter->interrupt();
        }

      private:
        InterruptionPlan _plan;
        std::shared_ptr<runtime::Interrupter> _interrupter;
        std::array<uint64_t, runtime::PollSiteTable.size()> _polls {};
        uint64_t _pollsOfCall = 0;
        CallKind _kind = CallKind::Complete;
    };

    /// Each memory's bytes after one call: what a memory divergence is diffed from.
    using MemoryBytes = std::vector<std::vector<std::byte>>;

    /// Reads what an Observation holds of the store after a call: the instance's memories, globals, tables
    /// and segments (when there is an instance), and what the store charges and holds.
    void readState(runtime::Store const& store,
                   std::optional<uint32_t> instance,
                   runtime::GrowthRecord const& growth,
                   Observation& observation)
    {
        observation.steps = store.used(runtime::Limit::Steps);
        observation.growth = growth;
        observation.chargedStackValues = store.chargedStackValues();
        // Every address an exception can have is below the store's limit of them.
        for (auto const address: std::views::iota(
                 runtime::ExnAddr { 0 }, static_cast<runtime::ExnAddr>(store.limits().maxExceptions)))
        {
            if (!store.isLiveException(address))
                continue;
            auto const& exception = store.exception(address);
            observation.exceptions.push_back(
                LiveException { .address = address, .tag = exception.tag, .payload = exception.payload });
        }
        if (auto const* abrupt = std::get_if<runtime::Abrupt>(&observation.outcome))
        {
            if (auto const* thrown = std::get_if<runtime::Thrown>(abrupt))
            {
                auto const& exception = store.exception(thrown->exception);
                observation.thrown = ThrownException { .tag = exception.tag, .payload = exception.payload };
            }
        }
        if (!instance)
            return;
        auto const& module = store.moduleInstance(*instance);
        for (auto const memory: module.memories)
            observation.memoryHashes.push_back(memoryDigest(store.memory(memory).bytes()));
        for (auto const global: module.globals)
            observation.globals.push_back(store.global(global).value());
        for (auto const table: module.tables)
            observation.tables.push_back(store.table(table).elements);
        auto const stateOf = [](auto const& segment) {
            return segment.has_value() ? SegmentState::Live : SegmentState::Dropped;
        };
        for (auto const& segment: module.data)
            observation.segments.data.push_back(stateOf(segment));
        for (auto const& segment: module.elements)
            observation.segments.elements.push_back(stateOf(segment));
    }

    /// Each memory of the instance, copied.
    [[nodiscard]] MemoryBytes memoryBytesOf(runtime::Store const& store, std::optional<uint32_t> instance)
    {
        auto bytes = MemoryBytes {};
        if (!instance)
            return bytes;
        for (auto const memory: store.moduleInstance(*instance).memories)
        {
            auto const view = store.memory(memory).bytes();
            bytes.emplace_back(view.begin(), view.end());
        }
        return bytes;
    }

    /// The size of the instance's memory 0, which the arguments aim near.
    [[nodiscard]] ArgumentContext contextOf(runtime::Store const& store, uint32_t instance)
    {
        auto const& module = store.moduleInstance(instance);
        if (module.memories.empty())
            return {};
        return ArgumentContext { .memoryBytes = store.memory(module.memories.front()).bytes().size() };
    }

    /// What observing a participant may also keep: the memories after one call (FieldDifference::call's
    /// numbering), for diffing a memory divergence.
    struct Capture
    {
        std::size_t call = 0; ///< The call.
        MemoryBytes bytes;    ///< Its memories, once observed.
    };

    /// Everything one participant does with one module in one lane and observation mode.
    [[nodiscard]] std::optional<ModuleObservation> observeParticipant(
        Participant const& participant,
        std::shared_ptr<validate::ValidatedModule const> const& validated,
        Lane lane,
        ObservationMode mode,
        runtime::StoreLimits const& limits,
        InterruptionPlan const& interruptions,
        std::span<std::byte const> argumentSeed,
        Capture* capture = nullptr)
    {
        auto executor = participant.makeExecutor();
        auto const code = participant.lowerModule(validated, modeOf(lane));
        if (executor->engine() != runtime::ExecutionEngine::Reference && code == nullptr)
            return std::nullopt;

        auto allocator = runtime::HeapMemoryAllocator {};
        auto observer = CallObserver { interruptions };
        auto store = runtime::Store {
            limits,
            runtime::StoreServices { .allocator = allocator,
                                     .pollObserver = mode == ObservationMode::Observed ? &observer : nullptr }
        };
        observer.attach(store.interrupter());
        auto host = RecordingHost {};
        auto result = ModuleObservation {};
        result.instantiated.exportName = InstantiationName;

        auto const imports = host.bind(store, validated->module());
        if (!imports)
        {
            result.instantiation = std::visit(
                [](auto const& refusal) {
                    return "imports: " + describe(runtime::InstantiationError { refusal });
                },
                imports.error());
            return result;
        }
        observer.beginCall(CallKind::Complete);
        auto const instantiated = runtime::instantiateReporting(store, *executor, validated, code, *imports);
        result.instantiated.hostTrace = host.takeTrace();
        result.instantiated.polls = observer.polls();
        auto instance = std::optional<uint32_t> {};
        if (instantiated.instance)
            instance = *instantiated.instance;
        else
        {
            result.instantiation = describe(instantiated.instance.error());
            std::visit(
                [&result]<typename T>(T const& error) {
                    if constexpr (std::is_constructible_v<runtime::Abrupt, T>)
                        result.instantiated.outcome = runtime::Abrupt { error };
                },
                instantiated.instance.error());
        }
        readState(store, instance, instantiated.refusedGrowth, result.instantiated);
        if (capture != nullptr && capture->call == 0)
            capture->bytes = memoryBytesOf(store, instance);
        if (!instance)
            return result;

        auto functions = std::vector<std::pair<std::string_view, runtime::FuncAddr>> {};
        for (auto const& [name, value]: store.moduleInstance(*instance).exports)
        {
            if (value.kind == binary::ExternKind::Func)
                functions.emplace_back(name, value.address);
        }
        // Every function export CallRounds times, then the planned call, when it is observed and there is
        // one.
        auto schedule = std::vector<std::pair<std::size_t, CallKind>> {};
        for ([[maybe_unused]] auto const round:
             std::views::iota(uint32_t { 0 }, DifferentialRunner::CallRounds))
        {
            for (auto const index: std::views::iota(std::size_t { 0 }, functions.size()))
                schedule.emplace_back(index, CallKind::Complete);
        }
        if (mode == ObservationMode::Observed && interruptions.interruptAtPoll && !functions.empty())
            schedule.emplace_back(interruptions.exportPick % functions.size(), CallKind::Interrupted);

        auto ordinal = uint64_t { 0 };
        for (auto const& [index, kind]: schedule)
        {
            auto const& [name, function] = functions[index];
            auto stream = ArgumentStream { argumentSeed, ordinal++ };
            auto const& type = store.functionType(function);
            auto const args = argumentsFor(stream, type.params, contextOf(store, *instance));
            auto results = std::vector<runtime::Value>(type.results.size());
            auto& call = result.calls.emplace_back();
            call.exportName = name;
            observer.beginCall(kind);
            auto const invoked = runtime::invokeChecked(store, *executor, function, args, results);
            call.hostTrace = host.takeTrace();
            call.polls = observer.polls();
            if (invoked.outcome)
                call.outcome = std::move(results);
            else
                call.outcome = invoked.outcome.error();
            readState(store, instance, invoked.refusedGrowth, call);
            if (capture != nullptr && capture->call == result.calls.size())
                capture->bytes = memoryBytesOf(store, instance);
        }
        return result;
    }

    /// Where two memories' bytes first differ, as text for both sides; empty when they do not.
    [[nodiscard]] std::pair<std::string, std::string> firstDifference(MemoryBytes const& expected,
                                                                      MemoryBytes const& actual)
    {
        for (auto const memory: std::views::iota(std::size_t { 0 }, std::min(expected.size(), actual.size())))
        {
            auto const& left = expected[memory];
            auto const& right = actual[memory];
            auto const [at, other] = std::ranges::mismatch(left, right);
            if (at == left.end() && other == right.end())
                continue;
            auto const offset = static_cast<std::size_t>(at - left.begin());
            auto const show = [memory, offset](std::vector<std::byte> const& bytes) {
                if (offset >= bytes.size())
                    return std::format(
                        "memory {} offset {:#x}: past its end ({} bytes)", memory, offset, bytes.size());
                return std::format("memory {} offset {:#x}: {:#04x}",
                                   memory,
                                   offset,
                                   std::to_integer<unsigned>(bytes[offset]));
            };
            return { show(left), show(right) };
        }
        return {};
    }

    [[nodiscard]] std::shared_ptr<runtime::ModuleCode const> lowerForInterpreter(
        interpreter::Dispatch dispatch,
        std::shared_ptr<validate::ValidatedModule const> const& module,
        runtime::ExecutionMode mode)
    {
        auto lowered = interpreter::lower(
            *module,
            interpreter::LoweringConfig { .mode = mode, .dispatch = dispatch },
            std::make_shared<runtime::CountingArena>(LoweringBudget, *std::pmr::new_delete_resource()));
        if (!lowered)
            return nullptr;
        return *lowered;
    }
} // namespace

Participant referenceParticipant()
{
    return Participant {
        .name = "reference",
        .makeExecutor = [] { return std::make_unique<reference::ReferenceInterpreter>(); },
        .lowerModule =
            [](std::shared_ptr<validate::ValidatedModule const> const& /*module*/,
               runtime::ExecutionMode /*mode*/) { return std::shared_ptr<runtime::ModuleCode const> {}; },
    };
}

Participant interpreterParticipant(interpreter::Dispatch dispatch)
{
    auto const row =
        std::ranges::find(InterpreterParticipantNames, dispatch, &InterpreterParticipantName::dispatch);
    return Participant {
        .name = row->name,
        .makeExecutor = [dispatch] { return std::make_unique<interpreter::Interpreter>(dispatch); },
        .lowerModule =
            [dispatch](std::shared_ptr<validate::ValidatedModule const> const& module,
                       runtime::ExecutionMode mode) { return lowerForInterpreter(dispatch, module, mode); },
    };
}

std::vector<Participant> everyEngine()
{
    auto participants = std::vector<Participant> { referenceParticipant() };
    for (auto const dispatch: interpreter::availableDispatches())
        participants.push_back(interpreterParticipant(dispatch));
    return participants;
}

std::expected<std::shared_ptr<validate::ValidatedModule const>, Applicability> loadForComparison(
    std::span<std::byte const> module)
{
    auto loaded = validate::ValidatedModule::load(module, FeatureSet::all(), binary::DecodeLimits {});
    if (!loaded)
        return std::unexpected { Applicability::Invalid };
    if (!checkImports((*loaded)->module()))
        return std::unexpected { Applicability::UnsupportedImport };
    return *loaded;
}

DifferentialRunner::DifferentialRunner(std::vector<Participant> participants,
                                       Lane lane,
                                       runtime::StoreLimits limits,
                                       InterruptionPlan interruptions):
    _participants { std::move(participants) },
    _lane { lane },
    _limits { limits },
    _interruptions { interruptions }
{
}

std::expected<std::vector<ParticipantObservation>, Applicability> DifferentialRunner::observe(
    std::span<std::byte const> module, std::span<std::byte const> argumentSeed) const
{
    return observe(module, argumentSeed, {});
}

std::expected<std::vector<ParticipantObservation>, Applicability> DifferentialRunner::observe(
    std::span<std::byte const> module,
    std::span<std::byte const> argumentSeed,
    std::span<ParticipantObservation const> first) const
{
    auto const validated = loadForComparison(module);
    if (!validated)
        return std::unexpected { validated.error() };
    auto observations = std::vector<ParticipantObservation> {};
    for (auto const mode: { ObservationMode::Unobserved, ObservationMode::Observed })
    {
        for (auto const& participant: _participants)
        {
            auto const reused = std::ranges::find_if(first, [&](ParticipantObservation const& observation) {
                return observation.mode == mode && observation.participant == participant.name;
            });
            if (&participant == &_participants.front() && reused != first.end())
            {
                observations.push_back(*reused);
                continue;
            }
            observations.push_back(ParticipantObservation {
                .participant = participant.name,
                .mode = mode,
                .observation = observeParticipant(
                    participant, *validated, _lane, mode, _limits, _interruptions, argumentSeed) });
        }
    }
    return observations;
}

std::expected<void, Divergence> DifferentialRunner::run(std::span<std::byte const> module,
                                                        std::span<std::byte const> argumentSeed) const
{
    auto const observations = observe(module, argumentSeed);
    if (!observations)
        return {};
    return compare(module, argumentSeed, *observations);
}

std::expected<void, Divergence> DifferentialRunner::compare(
    std::span<std::byte const> module,
    std::span<std::byte const> argumentSeed,
    std::span<ParticipantObservation const> observations) const
{
    auto const* expected = static_cast<ParticipantObservation const*>(nullptr);
    for (auto const& actual: observations)
    {
        if (expected == nullptr || expected->mode != actual.mode)
        {
            expected = &actual;
            continue;
        }
        auto const who =
            std::format("{} ({} lane, {})", actual.participant, nameOf(_lane), nameOf(actual.mode));
        if (expected->observation.has_value() != actual.observation.has_value())
        {
            auto const shown = [](ParticipantObservation const& observation) {
                return observation.observation ? std::string { "lowered" }
                                               : std::string { "refused to lower" };
            };
            return std::unexpected { Divergence { .participant = who,
                                                  .field = "lowering",
                                                  .expected = shown(*expected),
                                                  .actual = shown(actual) } };
        }
        if (!actual.observation)
            continue;
        auto difference = differential::compare(*expected->observation, *actual.observation);
        if (!difference)
            continue;
        if (difference->field == "memoryHashes")
        {
            // A digest says that a memory differs, not where: both participants once more, keeping the
            // memories of that call.
            auto const participantNamed = [this](std::string_view name) {
                return std::ranges::find(_participants, name, &Participant::name);
            };
            auto const validated = loadForComparison(module);
            auto expectedCapture = Capture { .call = difference->call, .bytes = {} };
            auto actualCapture = Capture { .call = difference->call, .bytes = {} };
            if (validated)
            {
                [[maybe_unused]] auto const expectedAgain =
                    observeParticipant(*participantNamed(expected->participant),
                                       *validated,
                                       _lane,
                                       actual.mode,
                                       _limits,
                                       _interruptions,
                                       argumentSeed,
                                       &expectedCapture);
                [[maybe_unused]] auto const actualAgain =
                    observeParticipant(*participantNamed(actual.participant),
                                       *validated,
                                       _lane,
                                       actual.mode,
                                       _limits,
                                       _interruptions,
                                       argumentSeed,
                                       &actualCapture);
            }
            auto [expectedAt, actualAt] = firstDifference(expectedCapture.bytes, actualCapture.bytes);
            if (!expectedAt.empty())
            {
                difference->expected += std::format(" ({})", expectedAt);
                difference->actual += std::format(" ({})", actualAt);
            }
        }
        return std::unexpected { Divergence { .participant = who,
                                              .field = std::move(difference->field),
                                              .expected = std::move(difference->expected),
                                              .actual = std::move(difference->actual) } };
    }
    return {};
}

} // namespace isoline::differential
