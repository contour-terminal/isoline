// SPDX-License-Identifier: Apache-2.0
#include <core/Utils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "Comparison.hpp"
#include "Corpus.hpp"
#include "DifferentialRunner.hpp"
#include "Observation.hpp"
#include "Reach.hpp"
#include "testing/EnvironmentNumber.hpp"
#include <isoline/interpreter/ReferenceInterpreter.hpp>
#include <isoline/runtime/Abrupt.hpp>
#include <isoline/runtime/HostFunction.hpp>
#include <isoline/runtime/Instantiate.hpp>
#include <isoline/runtime/Interrupt.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/ModuleCode.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/text/TextLimits.hpp>
#include <isoline/text/TopLevel.hpp>

using namespace isoline;
using namespace isoline::differential;

namespace
{

[[nodiscard]] std::filesystem::path sourceDirectory()
{
    return std::filesystem::path { ISOLINE_SOURCE_DIR };
}

/// The binary of a module given as text.
[[nodiscard]] std::vector<std::byte> compiled(std::string_view text)
{
    auto module = text::compileText(text, text::TextLimits {});
    REQUIRE(module.has_value());
    return std::move(*module);
}

/// What a perturbing participant does after the reference interpreter ran a call: it changes one observable.
using Perturbation = std::function<void(runtime::Store& store,
                                        runtime::ICodeExecutor& inner,
                                        runtime::FuncAddr function,
                                        std::span<runtime::Value> results,
                                        std::expected<void, runtime::Abrupt>& outcome)>;

/// The reference interpreter, its every call perturbed after it ran: a planted divergence.
class PerturbingExecutor final: public runtime::ICodeExecutor
{
  public:
    explicit PerturbingExecutor(Perturbation perturbation): _perturbation { std::move(perturbation) } {}

    [[nodiscard]] runtime::ExecutionEngine engine() const noexcept override { return _inner.engine(); }

    [[nodiscard]] std::expected<void, runtime::Abrupt> invoke(runtime::Store& store,
                                                              runtime::FuncAddr function,
                                                              std::span<runtime::Value const> args,
                                                              std::span<runtime::Value> results) override
    {
        auto outcome = _inner.invoke(store, function, args, results);
        _perturbation(store, _inner, function, results, outcome);
        return outcome;
    }

  private:
    interpreter::ReferenceInterpreter _inner;
    Perturbation _perturbation;
};

[[nodiscard]] Participant perturbed(std::string_view name, Perturbation perturbation)
{
    return Participant {
        .name = name,
        .makeExecutor = [perturbation = std::move(
                             perturbation)] { return std::make_unique<PerturbingExecutor>(perturbation); },
        .lowerModule = referenceParticipant().lowerModule,
    };
}

/// The instance a module function belongs to.
[[nodiscard]] runtime::ModuleInstance const& instanceOf(runtime::Store const& store,
                                                        runtime::FuncAddr function)
{
    return store.moduleInstance(store.function(function).moduleInstance);
}

/// The module every perturbation is planted in: it calls a host import, holds a memory, a table, a mutable
/// global, a tag and a passive data and element segment, and has one export that returns, one that throws
/// and one that drops both segments.
constexpr auto PerturbedModule = std::string_view { R"(
(module
  (import "env" "host" (func $host (param i32) (result i32)))
  (memory 1)
  (table 2 funcref)
  (global (mut i32) (i32.const 7))
  (tag $tag (param i32))
  (data $passive "isoline")
  (elem $elements func $drop)
  (func (export "run") (param i32) (result i32) (call $host (local.get 0)))
  (func (export "throw") (param i32) (throw $tag (local.get 0)))
  (func $drop (export "drop") (data.drop $passive) (elem.drop $elements))
))" };

/// One perturbation per row of ObservationFields: each changes only its field.
struct PerturbationRow
{
    std::string_view field;
    Perturbation perturbation;
};

[[nodiscard]] std::vector<PerturbationRow> perturbations()
{
    return {
        { .field = "thrown",
          .perturbation =
              [](runtime::Store& store,
                 runtime::ICodeExecutor& /*inner*/,
                 runtime::FuncAddr /*function*/,
                 std::span<runtime::Value> /*results*/,
                 std::expected<void, runtime::Abrupt>& outcome) {
                  // Another exception of the same tag in its place, with another payload.
                  if (outcome || !std::holds_alternative<runtime::Thrown>(outcome.error()))
                      return;
                  auto const thrown = std::get<runtime::Thrown>(outcome.error());
                  auto const tag = store.exception(thrown.exception).tag;
                  auto const replaced = store.addException(tag, { runtime::I32 { 0xBAD } });
                  REQUIRE(replaced.has_value());
                  outcome = std::unexpected { runtime::Abrupt { runtime::Thrown { *replaced } } };
              } },
        { .field = "outcome",
          .perturbation =
              [](runtime::Store& /*store*/,
                 runtime::ICodeExecutor& /*inner*/,
                 runtime::FuncAddr /*function*/,
                 std::span<runtime::Value> results,
                 std::expected<void, runtime::Abrupt>& outcome) {
                  if (outcome && !results.empty())
                      std::get<runtime::I32>(results.front()).bits ^= 1U;
              } },
        { .field = "steps",
          .perturbation =
              [](runtime::Store& store,
                 runtime::ICodeExecutor& /*inner*/,
                 runtime::FuncAddr /*function*/,
                 std::span<runtime::Value> /*results*/,
                 std::expected<void, runtime::Abrupt>& /*outcome*/) {
                  // One step more than the reference charged: a FunctionEntry poll of its own.
                  [[maybe_unused]] auto const polled = store.poll(runtime::PollSite::FunctionEntry);
              } },
        { .field = "polls",
          .perturbation =
              [](runtime::Store& store,
                 runtime::ICodeExecutor& /*inner*/,
                 runtime::FuncAddr /*function*/,
                 std::span<runtime::Value> /*results*/,
                 std::expected<void, runtime::Abrupt>& /*outcome*/) {
                  // Unwind charges no step: only the observer sees it.
                  [[maybe_unused]] auto const polled = store.poll(runtime::PollSite::Unwind);
              } },
        { .field = "memoryHashes",
          .perturbation =
              [](runtime::Store& store,
                 runtime::ICodeExecutor& /*inner*/,
                 runtime::FuncAddr function,
                 std::span<runtime::Value> /*results*/,
                 std::expected<void, runtime::Abrupt>& /*outcome*/) {
                  auto const memory = instanceOf(store, function).memories.front();
                  store.memory(memory).bytes().back() ^= std::byte { 1 };
              } },
        { .field = "globals",
          .perturbation =
              [](runtime::Store& store,
                 runtime::ICodeExecutor& /*inner*/,
                 runtime::FuncAddr function,
                 std::span<runtime::Value> /*results*/,
                 std::expected<void, runtime::Abrupt>& /*outcome*/) {
                  auto& global = store.global(instanceOf(store, function).globals.front());
                  global.assign(runtime::I32 { std::get<runtime::I32>(global.value()).bits + 1 });
              } },
        { .field = "tables",
          .perturbation =
              [](runtime::Store& store,
                 runtime::ICodeExecutor& /*inner*/,
                 runtime::FuncAddr function,
                 std::span<runtime::Value> /*results*/,
                 std::expected<void, runtime::Abrupt>& /*outcome*/) {
                  auto& table = store.table(instanceOf(store, function).tables.front());
                  table.elements().front() = runtime::Ref { .kind = runtime::RefKind::Func,
                                                            .heap = table.elements().front().heap,
                                                            .address = function };
              } },
        { .field = "growth",
          .perturbation =
              [](runtime::Store& store,
                 runtime::ICodeExecutor& /*inner*/,
                 runtime::FuncAddr function,
                 std::span<runtime::Value> /*results*/,
                 std::expected<void, runtime::Abrupt>& /*outcome*/) {
                  // Past the store's limit: refused, and recorded in the call's growth record.
                  [[maybe_unused]] auto const grown =
                      store.growMemory(instanceOf(store, function).memories.front(), uint64_t { 1 } << 20);
              } },
        { .field = "exceptions",
          .perturbation =
              [](runtime::Store& store,
                 runtime::ICodeExecutor& /*inner*/,
                 runtime::FuncAddr function,
                 std::span<runtime::Value> /*results*/,
                 std::expected<void, runtime::Abrupt>& /*outcome*/) {
                  if (!store.function(function).isHost())
                  {
                      [[maybe_unused]] auto const added = store.addException(
                          instanceOf(store, function).tags.front(), { runtime::I32 { 1 } });
                  }
              } },
        { .field = "segments",
          .perturbation =
              [](runtime::Store& store,
                 runtime::ICodeExecutor& /*inner*/,
                 runtime::FuncAddr function,
                 std::span<runtime::Value> /*results*/,
                 std::expected<void, runtime::Abrupt>& /*outcome*/) {
                  // The passive data segment dropped behind the guest's back, as a lost data.drop would
                  // leave it live.
                  if (!store.function(function).isHost())
                      store.moduleInstance(store.function(function).moduleInstance).data.back().reset();
              } },
        { .field = "chargedStackValues",
          .perturbation =
              [](runtime::Store& store,
                 runtime::ICodeExecutor& /*inner*/,
                 runtime::FuncAddr /*function*/,
                 std::span<runtime::Value> /*results*/,
                 std::expected<void, runtime::Abrupt>& /*outcome*/) {
                  [[maybe_unused]] auto const charged = store.chargeFrame(1);
              } },
        { .field = "hostTrace",
          .perturbation =
              [](runtime::Store& store,
                 runtime::ICodeExecutor& inner,
                 runtime::FuncAddr function,
                 std::span<runtime::Value> /*results*/,
                 std::expected<void, runtime::Abrupt>& /*outcome*/) {
                  // The imported host function, called once more past the executor: no poll, no step.
                  auto const host = instanceOf(store, function).functions.front();
                  auto context = runtime::HostContext { store, inner, std::nullopt };
                  auto const args = std::vector<runtime::Value> { runtime::I32 { 99 } };
                  auto results = std::vector<runtime::Value>(1);
                  [[maybe_unused]] auto const called =
                      store.function(host).host->call(context, args, results);
              } },
    };
}

/// FuzzLimits with a tenth of its steps: what the edge modules run with in the unit run, where a sanitizer
/// makes each step slow. The call depth (1'000 frames) is still reached well within it.
constexpr auto EdgeLimits = [] {
    auto limits = FuzzLimits;
    limits.maxSteps = 10'000;
    return limits;
}();

/// Every committed spec binary: what agreesOnTheSpecSelftests runs.
[[nodiscard]] std::vector<std::filesystem::path> specBinaries()
{
    auto binaries = std::vector<std::filesystem::path> {};
    for (auto const& entry:
         std::filesystem::recursive_directory_iterator { sourceDirectory() / "tests" / "spec" })
    {
        if (entry.is_regular_file() && entry.path().extension() == ".wasm")
            binaries.push_back(entry.path());
    }
    std::ranges::sort(binaries);
    return binaries;
}

/// Runs @p module on every engine in both lanes, and fails naming the divergence.
void requireAgreement(std::span<std::byte const> module,
                      std::span<std::byte const> seed,
                      InterruptionPlan plan,
                      std::string const& what,
                      runtime::StoreLimits const& limits = FuzzLimits)
{
    for (auto const lane: EveryLane)
    {
        auto const runner = DifferentialRunner { everyEngine(), lane, limits, plan };
        auto const outcome = runner.run(module, seed);
        if (!outcome)
            FAIL(std::format("{}: {} diverges in {}: expected {}, got {}",
                             what,
                             outcome.error().participant,
                             outcome.error().field,
                             outcome.error().expected,
                             outcome.error().actual));
    }
}

} // namespace

TEST_CASE("Differential.agreesOnTheSpecSelftests")
{
    // R15: the corpus is every binary of the committed spec suite, whatever its fixture format; a build whose
    // ISOLINE_DIFFERENTIAL_SPEC_SAMPLE is above 1 (ThreadSanitizer's) compares the binaries whose index it
    // divides.
    auto const binaries = specBinaries();
    REQUIRE(binaries.size() > 1'000);
    auto const scope =
        ReplayScope { .sample = testing::numberFromEnvironment("ISOLINE_DIFFERENTIAL_SPEC_SAMPLE", 1) };
    auto compared = std::size_t { 0 };
    for (auto const [index, path]: core::views::enumerate(binaries))
    {
        if (!isSampled(index, scope))
            continue;
        auto const module = readModule(path);
        REQUIRE(module.has_value());
        if (loadForComparison(*module))
            ++compared;
        auto const seed = seedBytes(compared);
        requireAgreement(*module, seed, interruptionFor(seed), path.string());
    }
    // Most are invalid or malformed on purpose; the valid ones are compared.
    CHECK(compared * std::max(scope.sample, uint64_t { 1 }) > 1'000);
}

TEST_CASE("Differential.reportsAPlantedDivergence")
{
    // A participant whose every call spends one step more: what a budget of maxSteps − 1 compares as.
    auto const module = compiled(R"(
        (module (func (export "count") (param i32) (result i32) (local i32)
          (loop $again
            (local.set 1 (i32.add (local.get 1) (i32.const 1)))
            (br_if $again (i32.lt_u (local.get 1) (i32.const 10))))
          (local.get 1)))
    )");
    auto planted = perturbed("planted",
                             [](runtime::Store& store,
                                runtime::ICodeExecutor& /*inner*/,
                                runtime::FuncAddr /*function*/,
                                std::span<runtime::Value> /*results*/,
                                std::expected<void, runtime::Abrupt>& /*outcome*/) {
                                 // One step more than the reference charged: a FunctionEntry poll of its own.
                                 [[maybe_unused]] auto const polled =
                                     store.poll(runtime::PollSite::FunctionEntry);
                             });
    auto const runner = DifferentialRunner {
        { referenceParticipant(), std::move(planted) }, Lane::Deterministic, FuzzLimits, InterruptionPlan {}
    };
    auto const outcome = runner.run(module, {});
    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().field == "steps");
    CHECK(outcome.error().participant.starts_with("planted"));
    CHECK(outcome.error().expected != outcome.error().actual);
}

TEST_CASE("Differential.comparesEveryObservable")
{
    auto const module = compiled(PerturbedModule);
    auto const rows = perturbations();
    // One perturbation per field, and a field per perturbation.
    REQUIRE(rows.size() == ObservationFields.size());
    for (auto const& field: ObservationFields)
    {
        auto const row = std::ranges::find(rows, field.name, &PerturbationRow::field);
        REQUIRE(row != rows.end());
        INFO("field " << field.name);
        auto const runner =
            DifferentialRunner { { referenceParticipant(), perturbed("perturbed", row->perturbation) },
                                 Lane::Deterministic,
                                 FuzzLimits,
                                 InterruptionPlan {} };
        auto const outcome = runner.run(module, seedBytes(1));
        REQUIRE_FALSE(outcome.has_value());
        CHECK(outcome.error().field == field.name);
        CHECK(outcome.error().expected != outcome.error().actual);
    }
}

TEST_CASE("Differential.agreesWithItself")
{
    // The perturbed module, unperturbed: the reference twice, and every engine.
    auto const module = compiled(PerturbedModule);
    auto const twice = DifferentialRunner { { referenceParticipant(), referenceParticipant() },
                                            Lane::Deterministic,
                                            FuzzLimits,
                                            InterruptionPlan {} };
    CHECK(twice.run(module, seedBytes(1)).has_value());
    requireAgreement(module, seedBytes(1), InterruptionPlan {}, "the perturbed module");
}

TEST_CASE("Differential.reportsARefusedLowering")
{
    auto const module = compiled(R"((module (func (export "f"))))");
    auto refusing = fastParticipant(fastinterpreter::Dispatch::Switch);
    refusing.lowerModule = [](std::shared_ptr<validate::ValidatedModule const> const& /*module*/,
                              runtime::ExecutionMode /*mode*/) {
        return std::shared_ptr<runtime::ModuleCode const> {};
    };
    auto const runner = DifferentialRunner {
        { referenceParticipant(), std::move(refusing) }, Lane::Production, FuzzLimits, InterruptionPlan {}
    };
    auto const outcome = runner.run(module, {});
    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().field == "lowering");
    CHECK(outcome.error().actual == "refused to lower");
}

TEST_CASE("Differential.leavesOutWhatItCannotCompare")
{
    auto const invalid = std::vector<std::byte> { std::byte { 0 }, std::byte { 'a' } };
    auto const loaded = loadForComparison(invalid);
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error() == Applicability::Invalid);
    auto const concrete = compiled(R"(
        (module (type $t (func)) (import "env" "f" (func (param (ref null $t)))))
    )");
    auto const unsupported = loadForComparison(concrete);
    REQUIRE_FALSE(unsupported.has_value());
    CHECK(unsupported.error() == Applicability::UnsupportedImport);
    // Not compared, so every participant agrees.
    auto const runner =
        DifferentialRunner { everyEngine(), Lane::Deterministic, FuzzLimits, InterruptionPlan {} };
    CHECK(runner.run(invalid, {}).has_value());
}

TEST_CASE("Differential.interruptAtKthPollAgrees", "[memory]")
{
    // 64 MiB, the whole of FuzzLimits' memory, filled by one memory.fill: 1'024 granules, each polled first.
    // Each call fills with its own number, one more than the call before. The complete calls fill all of it
    // (1, then 2); the interrupted call after them polls first at its FunctionEntry, so its 300th poll is the
    // one before granule 299, and it has written 298 granules of 3 when the interrupt ends it.
    constexpr auto Pages = 1'024;
    constexpr auto Granule = std::size_t { 65'536 };
    constexpr auto InterruptAt = uint64_t { 300 };
    auto const module = compiled(std::format(R"(
        (module (memory {}) (global $value (mut i32) (i32.const 0))
          (func (export "fill")
            (global.set $value (i32.add (global.get $value) (i32.const 1)))
            (memory.fill (i32.const 0) (global.get $value) (i32.const {}))))
    )",
                                             Pages,
                                             Pages * Granule));
    auto const plan = InterruptionPlan { .interruptAtPoll = InterruptAt };
    requireAgreement(module, {}, plan, "the 64 MiB fill");

    auto const runner = DifferentialRunner { everyEngine(), Lane::Deterministic, FuzzLimits, plan };
    auto const observations = runner.observe(module, {});
    REQUIRE(observations.has_value());
    auto expected = std::vector<std::byte>(Pages * Granule, std::byte { DifferentialRunner::CallRounds });
    std::ranges::fill(std::span { expected }.first((InterruptAt - 2) * Granule),
                      std::byte { DifferentialRunner::CallRounds + 1 });
    auto const prefix = memoryDigest(expected);
    for (auto const& observation: *observations)
    {
        if (observation.mode != ObservationMode::Observed)
            continue;
        INFO(observation.participant);
        REQUIRE(observation.observation.has_value());
        // The planned call is the last one, after every complete call.
        auto const& first = observation.observation->calls.back();
        CHECK(first.outcome
              == CallOutcome { runtime::Abrupt { runtime::Trap { runtime::TrapCode::Interrupted } } });
        CHECK(first.polls[std::to_underlying(runtime::PollSite::BulkGranule)] == InterruptAt - 1);
        REQUIRE(first.memoryHashes.size() == 1);
        CHECK(first.memoryHashes.front() == prefix);
    }
}

TEST_CASE("Differential.interruptsOnlyItsCall")
{
    // The planned call comes after every complete call, so the complete ones keep their polls: ten
    // iterations, nine back-edges each. Only the observed mode has an observer to interrupt with.
    auto const module = compiled(R"(
        (module (func (export "count") (param i32) (result i32) (local i32)
          (loop $again
            (local.set 1 (i32.add (local.get 1) (i32.const 1)))
            (br_if $again (i32.lt_u (local.get 1) (i32.const 10))))
          (local.get 1)))
    )");
    auto const runner = DifferentialRunner {
        { referenceParticipant() }, Lane::Deterministic, FuzzLimits, InterruptionPlan { .interruptAtPoll = 3 }
    };
    auto const observations = runner.observe(module, seedBytes(1));
    REQUIRE(observations.has_value());
    for (auto const& observation: *observations)
    {
        INFO(describe(observation.mode));
        REQUIRE(observation.observation.has_value());
        auto const& calls = observation.observation->calls;
        auto const interrupted = observation.mode == ObservationMode::Observed;
        REQUIRE(calls.size() == DifferentialRunner::CallRounds + (interrupted ? 1U : 0U));
        for (auto const& call: std::span { calls }.first(DifferentialRunner::CallRounds))
        {
            CHECK(call.outcome == CallOutcome { std::vector<runtime::Value> { runtime::I32 { 10 } } });
            if (interrupted)
                CHECK(call.polls[std::to_underlying(runtime::PollSite::LoopBackEdge)] == 9);
        }
        if (interrupted)
        {
            CHECK(calls.back().outcome
                  == CallOutcome { runtime::Abrupt { runtime::Trap { runtime::TrapCode::Interrupted } } });
            CHECK(calls.back().polls[std::to_underlying(runtime::PollSite::LoopBackEdge)] == 2);
        }
    }
}

TEST_CASE("Differential.reportsTheFirstDifferingOffset")
{
    // One byte of memory 0 changed, 0x1234 into it: the divergence names the memory, the offset and both
    // bytes, which a digest alone does not show.
    auto const module = compiled(PerturbedModule);
    auto planted = perturbed("planted",
                             [](runtime::Store& store,
                                runtime::ICodeExecutor& /*inner*/,
                                runtime::FuncAddr function,
                                std::span<runtime::Value> /*results*/,
                                std::expected<void, runtime::Abrupt>& /*outcome*/) {
                                 auto const memory = instanceOf(store, function).memories.front();
                                 store.memory(memory).bytes()[0x1234] = std::byte { 0x5a };
                             });
    auto const runner = DifferentialRunner {
        { referenceParticipant(), std::move(planted) }, Lane::Deterministic, FuzzLimits, InterruptionPlan {}
    };
    auto const outcome = runner.run(module, seedBytes(1));
    REQUIRE_FALSE(outcome.has_value());
    CHECK(outcome.error().field == "memoryHashes");
    CHECK(outcome.error().expected.contains("memory 0 offset 0x1234: 0x00"));
    CHECK(outcome.error().actual.contains("memory 0 offset 0x1234: 0x5a"));
}

TEST_CASE("Differential.agreesOnTheEdges")
{
    // The hard edges, written by hand: calls to the depth limit, accesses at the bounds, bulk operations of
    // many granules, exceptions through many frames and sweeps, host imports, numbers at their edges, and
    // every branch to a loop. Each with seeds that pick other arguments, and an interrupted call after the
    // rest.
    constexpr auto Seeds = uint64_t { 8 };
    auto edges = std::vector<std::filesystem::path> {};
    for (auto const& entry:
         std::filesystem::directory_iterator { sourceDirectory() / "tests" / "differential" / "edges" })
    {
        if (entry.path().extension() == ".wat")
            edges.push_back(entry.path());
    }
    std::ranges::sort(edges);
    REQUIRE(edges.size() >= 8);
    for (auto const& path: edges)
    {
        auto const module = readModule(path);
        REQUIRE(module.has_value());
        REQUIRE(loadForComparison(*module).has_value());
        for (auto const seed: std::views::iota(uint64_t { 0 }, Seeds))
        {
            auto const bytes = seedBytes(seed);
            requireAgreement(*module,
                             bytes,
                             interruptionFor(bytes),
                             std::format("{} seed {}", path.string(), seed),
                             EdgeLimits);
        }
    }
}

TEST_CASE("Differential.reachesTheEdges")
{
    // The edge modules' arguments reach what they are written for: the call depth, the step limit, a refused
    // grow, a trap at a bound, a sweep, an interrupt, host re-entry to the nesting limit, and host writes.
    // Counted over the reference's observed runs.
    auto reach = Reach {};
    for (auto const& entry:
         std::filesystem::directory_iterator { sourceDirectory() / "tests" / "differential" / "edges" })
    {
        if (entry.path().extension() != ".wat")
            continue;
        auto const module = readModule(entry.path());
        REQUIRE(module.has_value());
        for (auto const seed: std::views::iota(uint64_t { 0 }, uint64_t { 8 }))
        {
            auto const bytes = seedBytes(seed);
            auto const runner = DifferentialRunner {
                { referenceParticipant() }, Lane::Deterministic, EdgeLimits, interruptionFor(bytes)
            };
            auto const observations = runner.observe(*module, bytes);
            REQUIRE(observations.has_value());
            for (auto const& observation: *observations)
            {
                if (observation.observation)
                    reach.add(*observation.observation);
            }
        }
    }
    INFO(describe(reach));
    for (auto const code: { runtime::TrapCode::CallStackExhausted,
                            runtime::TrapCode::StepLimitExceeded,
                            runtime::TrapCode::OutOfBoundsMemoryAccess,
                            runtime::TrapCode::OutOfBoundsTableAccess,
                            runtime::TrapCode::UndefinedElement,
                            runtime::TrapCode::UninitializedElement,
                            runtime::TrapCode::IntegerDivideByZero,
                            runtime::TrapCode::InvalidConversionToInteger,
                            runtime::TrapCode::NullExceptionReference,
                            runtime::TrapCode::Interrupted })
    {
        INFO("trap " << runtime::specMessage(code));
        CHECK(reach.traps[std::to_underlying(code)] > 0);
    }
    CHECK(reach.refusedGrowth > 0);
    CHECK(reach.thrown > 0);
    CHECK(reach.hostFailures > 0);
    CHECK(reach.bulkGranules > 0);
    CHECK(reach.unwinds > 0);
    CHECK(reach.sweeps > 0);
    // The host re-enters the guest, as deep as the store's nested invocations go, and writes the caller's
    // memory between the guest's own accesses.
    CHECK(reach.reentries > 0);
    CHECK(reach.nestingExhausted > 0);
    CHECK(reach.hostWrites > 0);
}
