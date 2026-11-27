// SPDX-License-Identifier: Apache-2.0
// isoline-differential-replay <module.wasm|module.wat> <seed> [--lane deterministic|production]
//                             [--interrupt-at <poll>] [--oracle <wasmtime>]
//
// Replays one module of the differential corpus (or a regression) on every engine of this build, prints
// what each observed and the fast interpreter's lowered listing, and the first divergence. Exits with 0
// when every participant agrees (or the module is not compared), 1 on a divergence, which is what
// tests/fuzz/shrink-predicate.py asks of it, and 2 on a usage or input error.
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "Comparison.hpp"
#include "Corpus.hpp"
#include "DifferentialRunner.hpp"
#include "Observation.hpp"
#include "ProcessRunner.hpp"
#include "WasmtimeOracle.hpp"
#include <isoline/fastinterpreter/Dispatch.hpp>
#include <isoline/fastinterpreter/Listing.hpp>
#include <isoline/fastinterpreter/Lowering.hpp>
#include <isoline/runtime/CountingArena.hpp>
#include <isoline/runtime/Limits.hpp>

namespace
{

using namespace isoline::differential;

constexpr auto ExitAgrees = 0;
constexpr auto ExitDiverges = 1;
constexpr auto ExitUsage = 2;

struct Options
{
    std::filesystem::path module;
    uint64_t seed = 0;
    Lane lane = Lane::Deterministic;
    InterruptionPlan interruptions;
    std::optional<std::filesystem::path> oracle;
};

[[nodiscard]] std::optional<uint64_t> numberOf(std::string_view text) noexcept
{
    auto value = uint64_t {};
    auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc {} || end != text.data() + text.size())
        return std::nullopt;
    return value;
}

[[nodiscard]] std::expected<Options, std::string> parse(std::span<char const* const> arguments)
{
    if (arguments.size() < 2)
        return std::unexpected { std::string { "a module and a seed are required" } };
    auto options = Options { .module = arguments[0],
                             .seed = 0,
                             .lane = Lane::Deterministic,
                             .interruptions = {},
                             .oracle = std::nullopt };
    auto const seed = numberOf(arguments[1]);
    if (!seed)
        return std::unexpected { std::format("the seed '{}' is no number", arguments[1]) };
    options.seed = *seed;
    options.interruptions = interruptionFor(seedBytes(*seed));
    auto const rest = arguments.subspan(2);
    if (rest.size() % 2 != 0)
        return std::unexpected { std::string { "every option takes one value" } };
    for (auto const pair: std::views::iota(std::size_t { 0 }, rest.size() / 2))
    {
        auto const name = std::string_view { rest[2 * pair] };
        auto const value = std::string_view { rest[(2 * pair) + 1] };
        if (name == "--lane")
        {
            auto const lane = laneNamed(value);
            if (!lane)
                return std::unexpected { std::format("no lane is named '{}'", value) };
            options.lane = *lane;
        }
        else if (name == "--interrupt-at")
        {
            auto const poll = numberOf(value);
            if (!poll)
                return std::unexpected { std::format("the poll '{}' is no number", value) };
            // 0 asks for no interrupt.
            options.interruptions.interruptAtPoll = poll;
            if (*poll == 0)
                options.interruptions.interruptAtPoll.reset();
        }
        else if (name == "--oracle")
            options.oracle = std::filesystem::path { value };
        else
            return std::unexpected { std::format("unknown option '{}'", name) };
    }
    return options;
}

void printListings(std::span<std::byte const> module, Lane lane)
{
    auto const validated = loadForComparison(module);
    if (!validated)
        return;
    for (auto const dispatch: isoline::fastinterpreter::availableDispatches())
    {
        auto const lowered = isoline::fastinterpreter::lower(
            **validated,
            isoline::fastinterpreter::LoweringConfig { .mode = modeOf(lane), .dispatch = dispatch },
            std::make_shared<isoline::runtime::CountingArena>(LoweringBudget,
                                                              *std::pmr::new_delete_resource()));
        if (!lowered)
        {
            std::println("lowering ({}): refused", std::to_underlying(dispatch));
            continue;
        }
        for (auto const& function: (*lowered)->functions())
            std::print("lowered function {} ({}):\n{}",
                       function.codeIndex,
                       fastParticipant(dispatch).name,
                       isoline::fastinterpreter::describe(function));
    }
}

void consultOracle(std::filesystem::path const& wasmtime, std::span<std::byte const> module, uint64_t seed)
{
    // A directory of this run's own, removed when it is done.
    auto const scratch = std::filesystem::temp_directory_path()
                         / std::format("isoline-differential-replay-{:016x}", memoryDigest(module) ^ seed);
    std::filesystem::create_directories(scratch);
    {
        auto runner = ProcessRunner { scratch };
        auto files = DirectoryModuleFiles { scratch / "modules" };
        auto const oracle = WasmtimeOracle { wasmtime, runner, files };
        for (auto const& comparison: compareWithOracle(oracle, module, seedBytes(seed)))
            std::println(
                "oracle {}: {}, {}", comparison.exportName, describe(comparison.verdict), comparison.detail);
    }
    auto ignored = std::error_code {};
    std::filesystem::remove_all(scratch, ignored);
}

} // namespace

int main(int argc, char const* argv[])
{
    auto const arguments = std::span<char const* const> { argv, static_cast<std::size_t>(argc) }.subspan(1);
    auto const options = parse(arguments);
    if (!options)
    {
        std::println(stderr, "isoline-differential-replay: {}", options.error());
        std::println(stderr,
                     "usage: isoline-differential-replay <module.wasm|module.wat> <seed> [--lane "
                     "deterministic|production] [--interrupt-at <poll>] [--oracle <wasmtime>]");
        return ExitUsage;
    }
    auto const module = readModule(options->module);
    if (!module)
    {
        std::println(stderr, "isoline-differential-replay: {}", module.error());
        return ExitUsage;
    }
    auto const seed = seedBytes(options->seed);
    auto const runner =
        DifferentialRunner { everyEngine(), options->lane, FuzzLimits, options->interruptions };
    auto const observations = runner.observe(*module, seed);
    if (!observations)
    {
        std::println("not compared: {}", describe(observations.error()));
        return ExitAgrees;
    }
    for (auto const& observation: *observations)
    {
        std::println("== {} ({})", observation.participant, describe(observation.mode));
        std::print("{}",
                   observation.observation ? describe(*observation.observation)
                                           : std::string { "refused to lower\n" });
    }
    printListings(*module, options->lane);
    if (options->oracle)
        consultOracle(*options->oracle, *module, options->seed);
    auto const outcome = runner.compare(*module, seed, *observations);
    if (outcome)
    {
        std::println("every participant agrees");
        return ExitAgrees;
    }
    std::println("divergence: {} in {}\n  expected {}\n  actual   {}",
                 outcome.error().participant,
                 outcome.error().field,
                 outcome.error().expected,
                 outcome.error().actual);
    return ExitDiverges;
}
