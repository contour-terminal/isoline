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
#include <isoline/fastinterpreter/Dispatch.hpp>
#include <isoline/fastinterpreter/FastInterpreter.hpp>
#include <isoline/fastinterpreter/Lowering.hpp>
#include <isoline/interpreter/ReferenceInterpreter.hpp>
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
    /// The name a fast participant has under each dispatch: string literals, which a Participant's name
    /// views.
    struct FastParticipantName
    {
        fastinterpreter::Dispatch dispatch; ///< The dispatch.
        std::string_view name;              ///< The participant's name.
    };
    constexpr auto FastParticipantNames = std::to_array<FastParticipantName>({
        { .dispatch = fastinterpreter::Dispatch::Switch, .name = "fast.switch" },
        { .dispatch = fastinterpreter::Dispatch::TailCall, .name = "fast.tailcall" },
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

    /// Counts the polls of each call, and raises the store's interrupt at the planned poll of each.
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
        void beginCall() noexcept
        {
            _polls = {};
            _pollsOfCall = 0;
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
            if (_plan.interruptAtPoll && *_plan.interruptAtPoll == _pollsOfCall && _interrupter)
                _interrupter->interrupt();
        }

      private:
        InterruptionPlan _plan;
        std::shared_ptr<runtime::Interrupter> _interrupter;
        std::array<uint64_t, runtime::PollSiteTable.size()> _polls {};
        uint64_t _pollsOfCall = 0;
    };

    /// Reads what an Observation holds of the store after a call: the instance's memories, globals and
    /// tables (when there is an instance), and what the store charges.
    void readState(runtime::Store const& store,
                   std::optional<uint32_t> instance,
                   runtime::GrowthRecord const& growth,
                   Observation& observation)
    {
        observation.steps = store.used(runtime::Limit::Steps);
        observation.growth = growth;
        observation.liveExceptions = store.liveExceptions();
        observation.chargedStackValues = store.chargedStackValues();
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
    }

    /// The size of the instance's memory 0, which the arguments aim near.
    [[nodiscard]] ArgumentContext contextOf(runtime::Store const& store, uint32_t instance)
    {
        auto const& module = store.moduleInstance(instance);
        if (module.memories.empty())
            return {};
        return ArgumentContext { .memoryBytes = store.memory(module.memories.front()).bytes().size() };
    }

    /// Everything one participant does with one module in one lane and observation mode.
    [[nodiscard]] std::optional<ModuleObservation> observeParticipant(
        Participant const& participant,
        std::shared_ptr<validate::ValidatedModule const> const& validated,
        Lane lane,
        ObservationMode mode,
        runtime::StoreLimits const& limits,
        InterruptionPlan const& interruptions,
        std::span<std::byte const> argumentSeed)
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
        observer.beginCall();
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
        if (!instance)
            return result;

        auto ordinal = uint64_t { 0 };
        for ([[maybe_unused]] auto const round:
             std::views::iota(uint32_t { 0 }, DifferentialRunner::CallRounds))
        {
            for (auto const& [name, value]: store.moduleInstance(*instance).exports)
            {
                if (value.kind != binary::ExternKind::Func)
                    continue;
                auto stream = ArgumentStream { argumentSeed, ordinal++ };
                auto const& type = store.functionType(value.address);
                auto const args = argumentsFor(stream, type.params, contextOf(store, *instance));
                auto results = std::vector<runtime::Value>(type.results.size());
                auto& call = result.calls.emplace_back();
                call.exportName = name;
                observer.beginCall();
                auto const invoked = runtime::invokeChecked(store, *executor, value.address, args, results);
                call.hostTrace = host.takeTrace();
                call.polls = observer.polls();
                if (invoked.outcome)
                    call.outcome = std::move(results);
                else
                    call.outcome = invoked.outcome.error();
                readState(store, instance, invoked.refusedGrowth, call);
            }
        }
        return result;
    }

    [[nodiscard]] std::shared_ptr<runtime::ModuleCode const> lowerForFast(
        fastinterpreter::Dispatch dispatch,
        std::shared_ptr<validate::ValidatedModule const> const& module,
        runtime::ExecutionMode mode)
    {
        auto lowered = fastinterpreter::lower(
            *module,
            fastinterpreter::LoweringConfig { .mode = mode, .dispatch = dispatch },
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
        .makeExecutor = [] { return std::make_unique<interpreter::ReferenceInterpreter>(); },
        .lowerModule =
            [](std::shared_ptr<validate::ValidatedModule const> const& /*module*/,
               runtime::ExecutionMode /*mode*/) { return std::shared_ptr<runtime::ModuleCode const> {}; },
    };
}

Participant fastParticipant(fastinterpreter::Dispatch dispatch)
{
    auto const row = std::ranges::find(FastParticipantNames, dispatch, &FastParticipantName::dispatch);
    return Participant {
        .name = row->name,
        .makeExecutor = [dispatch] { return std::make_unique<fastinterpreter::FastInterpreter>(dispatch); },
        .lowerModule =
            [dispatch](std::shared_ptr<validate::ValidatedModule const> const& module,
                       runtime::ExecutionMode mode) { return lowerForFast(dispatch, module, mode); },
    };
}

std::vector<Participant> everyEngine()
{
    auto participants = std::vector<Participant> { referenceParticipant() };
    for (auto const dispatch: fastinterpreter::availableDispatches())
        participants.push_back(fastParticipant(dispatch));
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
    auto const validated = loadForComparison(module);
    if (!validated)
        return std::unexpected { validated.error() };
    auto observations = std::vector<ParticipantObservation> {};
    for (auto const mode: { ObservationMode::Unobserved, ObservationMode::Observed })
    {
        for (auto const& participant: _participants)
        {
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
    return compare(*observations);
}

std::expected<void, Divergence> DifferentialRunner::compare(
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
        if (auto difference = differential::compare(*expected->observation, *actual.observation))
            return std::unexpected { Divergence { .participant = who,
                                                  .field = std::move(difference->field),
                                                  .expected = std::move(difference->expected),
                                                  .actual = std::move(difference->actual) } };
    }
    return {};
}

} // namespace isoline::differential
