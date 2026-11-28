// SPDX-License-Identifier: Apache-2.0
#include "FuzzTargets.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <memory_resource>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <Corpus.hpp>
#include <DifferentialRunner.hpp>
#include <Observation.hpp>
#include <isoline/Features.hpp>
#include <isoline/binary/DecodeLimits.hpp>
#include <isoline/interpreter/Code.hpp>
#include <isoline/interpreter/Dispatch.hpp>
#include <isoline/interpreter/Lowering.hpp>
#include <isoline/runtime/CountingArena.hpp>
#include <isoline/runtime/ExecutionMode.hpp>
#include <isoline/text/TextLimits.hpp>
#include <isoline/text/TopLevel.hpp>
#include <isoline/validate/ValidatedModule.hpp>

namespace isoline::fuzz
{

namespace
{

    /// A generated module the harness does not compare: the generator's fault, which the target reports as
    /// a divergence of its own.
    [[nodiscard]] differential::Divergence generatorDivergence(differential::Applicability applicability)
    {
        return differential::Divergence { .participant = "generator",
                                          .field = "validation",
                                          .expected = "a module every engine compares",
                                          .actual = std::string { differential::describe(applicability) } };
    }

    /// Observes @p module in @p lane and compares, reusing the reference's observations of @p first.
    [[nodiscard]] std::expected<std::vector<differential::ParticipantObservation>, differential::Divergence>
    compareInLane(differential::Lane lane,
                  std::span<std::byte const> module,
                  std::span<std::byte const> seed,
                  std::span<differential::ParticipantObservation const> first)
    {
        auto const runner = differential::DifferentialRunner {
            differential::everyEngine(), lane, differential::FuzzLimits, differential::interruptionFor(seed)
        };
        auto observations = runner.observe(module, seed, first);
        if (!observations)
            return std::unexpected { generatorDivergence(observations.error()) };
        if (auto const outcome = runner.compare(module, seed, *observations); !outcome)
            return std::unexpected { outcome.error() };
        return std::move(*observations);
    }

} // namespace

DifferentialInput splitDifferentialInput(std::span<std::byte const> input) noexcept
{
    auto const head = input.first(std::min(input.size(), ArgumentSeedBytes));
    auto seed = uint64_t { 0 };
    for (auto const index: std::views::iota(std::size_t { 0 }, head.size()))
        seed |= std::to_integer<uint64_t>(head[index]) << (8 * index);
    return DifferentialInput { .seed = seed, .moduleEntropy = input.subspan(head.size()) };
}

std::expected<void, differential::Divergence> runDifferential(std::span<std::byte const> input,
                                                              LaneSelection lanes)
{
    auto const split = splitDifferentialInput(input);
    auto const module = generateModule(split.moduleEntropy, DifferentialGeneratorLimits);
    auto const seed = differential::seedBytes(split.seed);
    auto const isSelected = [&](DifferentialLaneRow const& row) {
        switch (lanes)
        {
            case LaneSelection::Active: return row.activity == LaneActivity::Active;
            case LaneSelection::Deterministic: return row.lane == differential::Lane::Deterministic;
            case LaneSelection::Production: return row.lane == differential::Lane::Production;
        }
        return false;
    };
    // The reference runs the validated module itself, the same in every lane: the first lane's observations
    // of it are reused by the next.
    auto first = std::vector<differential::ParticipantObservation> {};
    for (auto const& row: DifferentialLanes)
    {
        if (!isSelected(row))
            continue;
        auto observed = compareInLane(row.lane, module, seed, first);
        if (!observed)
            return std::unexpected { observed.error() };
        if (first.empty())
            first = std::move(*observed);
    }
    return {};
}

std::expected<LoadVerdict, std::string> judgeLowering(LoweringMeasures const& measures)
{
    if (measures.peak > measures.budget)
        return std::unexpected { std::format(
            "the arena's peak {} passed its budget {}", measures.peak, measures.budget) };
    if (measures.refusal)
    {
        if (*measures.refusal == interpreter::LoweringFailure::Budget)
            return LoadVerdict::OverBudget;
        return std::unexpected { std::format(
            "lowering refused the module for another reason than the budget ({})",
            std::to_underlying(*measures.refusal)) };
    }
    for (auto const index: std::views::iota(std::size_t { 0 }, measures.functions.size()))
    {
        auto const& function = measures.functions[index];
        if (function.words > function.bound)
            return std::unexpected { std::format("function {} lowered to {} code words, past its bound of {}",
                                                 index,
                                                 function.words,
                                                 function.bound) };
    }
    if (measures.work > interpreter::WorkPerBodyUnit * measures.bodyUnits)
        return std::unexpected { std::format("lowering did {} work for {} body units, past {} per unit",
                                             measures.work,
                                             measures.bodyUnits,
                                             interpreter::WorkPerBodyUnit) };
    return LoadVerdict::Lowered;
}

std::expected<LoadVerdict, std::string> runLoad(std::span<std::byte const> bytes, uint64_t compileBudget)
{
    auto const loaded = validate::ValidatedModule::load(bytes, FeatureSet::all(), binary::DecodeLimits {});
    if (!loaded)
        return LoadVerdict::Refused;
    auto const& module = (*loaded)->module();
    auto const arena =
        std::make_shared<runtime::CountingArena>(compileBudget, *std::pmr::new_delete_resource());
    auto const lowered =
        interpreter::lower(**loaded,
                           interpreter::LoweringConfig { .mode = runtime::ExecutionMode::Deterministic,
                                                         .dispatch = interpreter::Dispatch::Switch },
                           arena);
    auto measures = LoweringMeasures { .peak = arena->peak(),
                                       .budget = arena->budget(),
                                       .refusal = std::nullopt,
                                       .functions = {},
                                       .work = 0,
                                       .bodyUnits = 0 };
    if (!lowered)
    {
        measures.refusal = lowered.error().reason;
        return judgeLowering(measures);
    }
    auto const functions = (*lowered)->functions();
    for (auto const index: std::views::iota(std::size_t { 0 }, functions.size()))
        measures.functions.push_back(
            FunctionWords { .words = functions[index].code.size(),
                            .bound = interpreter::codeBoundWords(module.code[index]) });
    measures.work = (*lowered)->stats().work;
    measures.bodyUnits = (*lowered)->stats().bodyUnits;
    return judgeLowering(measures);
}

std::expected<LoadVerdict, std::string> runText(std::span<std::byte const> bytes)
{
    auto source = std::string(bytes.size(), '\0');
    std::ranges::transform(bytes, source.begin(), [](std::byte byte) { return static_cast<char>(byte); });
    auto limits = text::TextLimits {};
    limits.maxModuleBytes = TextModuleBytes;
    auto const compiled = text::compileText(source, limits);
    if (!compiled)
        return LoadVerdict::Refused;
    return runLoad(*compiled, LoadCompileBudget);
}

} // namespace isoline::fuzz
