// SPDX-License-Identifier: Apache-2.0
#include <core/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "Corpus.hpp"
#include "DifferentialRunner.hpp"
#include "Observation.hpp"
#include "ProcessRunner.hpp"
#include "Reach.hpp"
#include "WasmtimeOracle.hpp"

using namespace isoline::differential;

namespace
{

/// Every OracleStride-th corpus module is compared with Wasmtime too, when it is there: a process per
/// export call is slow beside the engines.
constexpr auto OracleStride = uint64_t { 16 };

/// The corpus directory: ISOLINE_DIFFERENTIAL_CORPUS, or out/corpus/differential in the source tree, where
/// scripts/make-differential-corpus.py writes it.
[[nodiscard]] std::filesystem::path corpusDirectory()
{
    if (auto const directory = core::LiveEnvironment {}.get("ISOLINE_DIFFERENTIAL_CORPUS"))
        return *directory;
    return std::filesystem::path { ISOLINE_SOURCE_DIR } / "out" / "corpus" / "differential";
}

/// Counts what the first participant's observed run reached.
void tally(std::span<ParticipantObservation const> observations, Reach& reach)
{
    for (auto const& observation: observations)
    {
        if (observation.mode == ObservationMode::Observed && observation.observation)
        {
            reach.add(*observation.observation);
            return;
        }
    }
}

/// The divergences of @p modules on every engine in both lanes, each with its reproducer; and what the
/// first participant's observed runs reached, into @p reach.
[[nodiscard]] std::vector<std::string> divergencesOf(
    std::vector<std::pair<std::filesystem::path, uint64_t>> const& modules,
    WasmtimeOracle const* oracle,
    Reach& reach)
{
    auto divergences = std::vector<std::string> {};
    for (auto const& [path, seed]: modules)
    {
        auto const module = readModule(path);
        REQUIRE(module.has_value());
        auto const bytes = seedBytes(seed);
        for (auto const lane: EveryLane)
        {
            auto const runner =
                DifferentialRunner { everyEngine(), lane, FuzzLimits, interruptionFor(bytes) };
            auto const observations = runner.observe(*module, bytes);
            if (!observations)
                continue;
            if (lane == Lane::Deterministic)
                tally(*observations, reach);
            if (auto const outcome = runner.compare(*observations); !outcome)
                divergences.push_back(
                    std::format("{}: {} diverges in {}: expected {}, got {}\n  reproduce: "
                                "isoline-differential-replay {} {} --lane {}",
                                path.string(),
                                outcome.error().participant,
                                outcome.error().field,
                                outcome.error().expected,
                                outcome.error().actual,
                                path.string(),
                                seed,
                                lane == Lane::Deterministic ? "deterministic" : "production"));
        }
        if (oracle == nullptr || seed % OracleStride != 0)
            continue;
        for (auto const& comparison: compareWithOracle(*oracle, *module, bytes))
        {
            if (comparison.verdict == OracleVerdict::Disagrees)
                divergences.push_back(std::format("{}: Wasmtime disagrees on {}: {}\n  reproduce: "
                                                  "isoline-differential-replay {} {} --oracle <wasmtime>",
                                                  path.string(),
                                                  comparison.exportName,
                                                  comparison.detail,
                                                  path.string(),
                                                  seed));
        }
    }
    return divergences;
}

} // namespace

TEST_CASE("CorpusReplay.everyRegressionAgrees")
{
    // Every module a divergence was once found in, kept as tests/differential/regressions/<seed>.wasm.
    auto const modules = corpusModules(std::filesystem::path { ISOLINE_SOURCE_DIR } / "tests" / "differential"
                                       / "regressions");
    auto reach = Reach {};
    auto const divergences = divergencesOf(modules, nullptr, reach);
    for (auto const& divergence: divergences)
        UNSCOPED_INFO(divergence);
    CHECK(divergences.empty());
}

TEST_CASE("CorpusReplay.everyCorpusModuleAgrees", "[differential]")
{
    auto const directory = corpusDirectory();
    if (!std::filesystem::is_directory(directory))
        SKIP(std::format("no corpus at {}: run scripts/make-differential-corpus.py", directory.string()));
    auto const modules = corpusModules(directory);
    REQUIRE_FALSE(modules.empty());

    auto runner = std::optional<ProcessRunner> {};
    auto files = std::optional<DirectoryModuleFiles> {};
    auto oracle = std::optional<WasmtimeOracle> {};
    if (auto const wasmtime = core::LiveEnvironment {}.get("ISOLINE_WASMTIME"))
    {
        runner.emplace(std::filesystem::temp_directory_path());
        files.emplace(std::filesystem::temp_directory_path() / "isoline-differential-oracle");
        oracle.emplace(std::filesystem::path { *wasmtime }, *runner, *files);
    }
    auto reach = Reach {};
    auto const divergences = divergencesOf(modules, oracle ? &*oracle : nullptr, reach);
    std::println("replayed {} corpus modules, {} divergences", modules.size(), divergences.size());
    // What the generated modules reached, so that a corpus that stops reaching an edge shows.
    std::println("reached: {}", describe(reach));
    for (auto const& divergence: divergences)
        UNSCOPED_INFO(divergence);
    CHECK(divergences.empty());
}
