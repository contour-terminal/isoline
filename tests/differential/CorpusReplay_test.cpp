// SPDX-License-Identifier: Apache-2.0
#include <core/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "Corpus.hpp"
#include "DifferentialRunner.hpp"
#include "Observation.hpp"
#include "ProcessRunner.hpp"
#include "WasmtimeOracle.hpp"
#include "testing/EnvironmentNumber.hpp"
#include "testing/RecordedRunner.hpp"
#include <isoline/text/TextLimits.hpp>
#include <isoline/text/TopLevel.hpp>

using namespace isoline;
using namespace isoline::differential;

namespace
{

/// What a replay of the generated corpus must reach, per module of a shard, so that a generator or a loader
/// that quietly stops producing comparable modules fails the replay. Measured on the 10'000-module corpus of
/// scripts/make-differential-corpus.py (wasm-tools 1.263.0) in 8 shards: every module compared, 7.8 to 8.7
/// export calls per module, and 1.10 to 1.69 calls Wasmtime agreed on per sampled module.
constexpr auto MinComparedShare = 0.95;
constexpr auto MinCallsPerModule = 6.0;
constexpr auto MinAgreesPerSampledModule = 0.8;

/// The corpus directory: ISOLINE_DIFFERENTIAL_CORPUS, or out/corpus/differential in the source tree, where
/// scripts/make-differential-corpus.py writes it.
[[nodiscard]] std::filesystem::path corpusDirectory()
{
    if (auto const directory = core::LiveEnvironment {}.get("ISOLINE_DIFFERENTIAL_CORPUS"))
        return *directory;
    return std::filesystem::path { ISOLINE_SOURCE_DIR } / "out" / "corpus" / "differential";
}

/// A directory of the build's own for the oracle's modules and outputs, removed with its scope.
class ScratchDirectory
{
  public:
    explicit ScratchDirectory(std::filesystem::path path): _path { std::move(path) }
    {
        std::filesystem::create_directories(_path);
    }
    ~ScratchDirectory()
    {
        auto ignored = std::error_code {};
        std::filesystem::remove_all(_path, ignored);
    }
    ScratchDirectory(ScratchDirectory const&) = delete;
    ScratchDirectory(ScratchDirectory&&) = delete;
    ScratchDirectory& operator=(ScratchDirectory const&) = delete;
    ScratchDirectory& operator=(ScratchDirectory&&) = delete;

    [[nodiscard]] std::filesystem::path const& path() const noexcept { return _path; }

  private:
    std::filesystem::path _path;
};

void writeFile(std::filesystem::path const& path, std::span<std::byte const> bytes)
{
    auto stream = std::ofstream { path, std::ios::binary | std::ios::trunc };
    stream.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(stream.good());
}

[[nodiscard]] std::vector<std::byte> compiled(std::string_view text)
{
    auto module = text::compileText(text, text::TextLimits {});
    REQUIRE(module.has_value());
    return std::move(*module);
}

} // namespace

TEST_CASE("CorpusReplay.countsWhatItCannotCompare")
{
    // A module it compares, one that does not decode, and one whose import the recording host cannot bind.
    auto const scratch = ScratchDirectory { std::filesystem::current_path() / "corpus-replay-count" };
    writeFile(scratch.path() / "0.wasm",
              compiled(R"((module (func (export "f") (param i32) (result i32) (local.get 0))))"));
    auto const garbage = std::vector<std::byte> { std::byte { 0 }, std::byte { 'a' }, std::byte { 's' } };
    writeFile(scratch.path() / "1.wasm", garbage);
    writeFile(scratch.path() / "2.wasm",
              compiled(R"((module (type $t (func)) (import "env" "f" (func (param (ref null $t))))))"));
    auto const modules = corpusModules(scratch.path());
    REQUIRE(modules.size() == 3);
    auto const tally = replayCorpus(modules, ReplayScope {}, nullptr);
    INFO(describe(tally));
    CHECK(tally.modules[std::to_underlying(Applicability::Compared)] == 1);
    CHECK(tally.modules[std::to_underlying(Applicability::Invalid)] == 1);
    CHECK(tally.modules[std::to_underlying(Applicability::UnsupportedImport)] == 1);
    CHECK(tally.calls == DifferentialRunner::CallRounds);
    CHECK(tally.sampled == 1);
    CHECK(tally.divergences.empty());
}

TEST_CASE("CorpusReplay.countsWhatWasmtimeRefusesOrRejects")
{
    // One module whose export returns: Wasmtime answering with a limit of its own is counted beside
    // Isoline's results, and is no failure; answering with an option it does not know is a harness fault.
    auto const scratch = ScratchDirectory { std::filesystem::current_path() / "corpus-replay-oracle" };
    writeFile(scratch.path() / "0.wasm",
              compiled(R"((module (func (export "f") (param i32) (result i32) (local.get 0))))"));
    auto const modules = corpusModules(scratch.path());
    REQUIRE(modules.size() == 1);
    auto files = testing::NamedModuleFiles {};

    auto refusing = testing::RecordedRunner { ProcessResult {
        .exitCode = 1,
        .standardOutput = {},
        .standardError = "Error: out of memory (failed to allocate 8 bytes)\n" } };
    auto const refusingOracle = WasmtimeOracle { "/tools/wasmtime", refusing, files };
    auto const refused = replayCorpus(modules, ReplayScope {}, &refusingOracle);
    INFO(describe(refused));
    CHECK(refused.oracleErrors[std::to_underlying(OracleErrorKind::Refused)] == 1);
    CHECK(refused.refusedWhileIsolineReturned == 1);
    CHECK(refused.harnessFaults() == 0);
    CHECK(refused.divergences.empty());
    CHECK(describe(refused).contains("refused while Isoline returned results 1"));

    auto rejecting = testing::RecordedRunner { ProcessResult {
        .exitCode = 2,
        .standardOutput = {},
        .standardError = "error: unexpected argument '--frobnicate' found\n" } };
    auto const rejectingOracle = WasmtimeOracle { "/tools/wasmtime", rejecting, files };
    auto const rejected = replayCorpus(modules, ReplayScope {}, &rejectingOracle);
    INFO(describe(rejected));
    CHECK(rejected.oracleErrors[std::to_underlying(OracleErrorKind::Rejected)] == 1);
    CHECK(rejected.harnessFaults() == 1);
    CHECK(rejected.divergences.size() == 1);
}

TEST_CASE("CorpusReplay.shardsCoverTheCorpusOnce")
{
    auto modules = std::vector<CorpusModule> {};
    for (auto const seed: std::views::iota(uint64_t { 0 }, uint64_t { 10 }))
        modules.emplace_back(std::format("{}.wasm", seed), seed);
    for (auto const shards: { std::size_t { 1 }, std::size_t { 3 }, std::size_t { 10 }, std::size_t { 16 } })
    {
        auto seen = std::vector<uint64_t> {};
        for (auto const shard: std::views::iota(std::size_t { 0 }, shards))
        {
            for (auto const& module: shardOf(modules, shard, shards))
                seen.push_back(module.second);
        }
        CHECK(seen == std::vector<uint64_t> { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 });
    }
}

TEST_CASE("CorpusReplay.samplesAFixedEighth")
{
    auto const scope = ReplayScope { .sample = 8 };
    CHECK(isSampled(0, scope));
    CHECK_FALSE(isSampled(1, scope));
    CHECK(isSampled(16, scope));
    CHECK(isSampled(7, ReplayScope { .sample = 1 }));
}

TEST_CASE("CorpusReplay.everyRegressionAgrees")
{
    // Every module a divergence was once found in, kept as tests/differential/regressions/<seed>.wasm.
    auto const modules = corpusModules(std::filesystem::path { ISOLINE_SOURCE_DIR } / "tests" / "differential"
                                       / "regressions");
    auto const tally = replayCorpus(modules, ReplayScope {}, nullptr);
    for (auto const& divergence: tally.divergences)
        UNSCOPED_INFO(divergence);
    CHECK(tally.divergences.empty());
    CHECK(tally.modules[std::to_underlying(Applicability::Invalid)] == 0);
}

TEST_CASE("CorpusReplay.everyCorpusModuleAgrees", "[differential]")
{
    // One shard of the corpus (ISOLINE_DIFFERENTIAL_SHARD of ISOLINE_DIFFERENTIAL_SHARDS, which CMake sets
    // per test), lane 2 and Wasmtime on its sample (ISOLINE_DIFFERENTIAL_SAMPLE).
    auto const directory = corpusDirectory();
    if (!std::filesystem::is_directory(directory))
        SKIP(std::format("no corpus at {}: run scripts/make-differential-corpus.py", directory.string()));
    auto const all = corpusModules(directory);
    REQUIRE_FALSE(all.empty());
    auto const shards = testing::numberFromEnvironment("ISOLINE_DIFFERENTIAL_SHARDS", 1);
    auto const shard = testing::numberFromEnvironment("ISOLINE_DIFFERENTIAL_SHARD", 0);
    REQUIRE(shard < shards);
    auto const modules = shardOf(all, shard, shards);
    auto const scope =
        ReplayScope { .sample = testing::numberFromEnvironment("ISOLINE_DIFFERENTIAL_SAMPLE", 1) };

    // Wasmtime, where ISOLINE_WASMTIME names it: set and empty is a failed fetch, never a skipped oracle.
    auto const wasmtime = core::LiveEnvironment {}.get("ISOLINE_WASMTIME");
    if (wasmtime && (wasmtime->empty() || !std::filesystem::is_regular_file(*wasmtime)))
        FAIL(std::format("ISOLINE_WASMTIME={} names no file: the fetch failed", *wasmtime));
    auto const scratch =
        ScratchDirectory { std::filesystem::current_path() / std::format("oracle-{}-of-{}", shard, shards) };
    auto runner = std::optional<ProcessRunner> {};
    auto files = std::optional<DirectoryModuleFiles> {};
    auto oracle = std::optional<WasmtimeOracle> {};
    if (wasmtime)
    {
        runner.emplace(scratch.path());
        files.emplace(scratch.path() / "modules");
        oracle.emplace(std::filesystem::path { *wasmtime }, *runner, *files);
    }

    auto const tally = replayCorpus(modules, scope, oracle ? &*oracle : nullptr);
    std::println("shard {} of {}: {} modules, sample 1/{}\n{}",
                 shard,
                 shards,
                 modules.size(),
                 scope.sample,
                 describe(tally));
    for (auto const& divergence: tally.divergences)
        UNSCOPED_INFO(divergence);
    CHECK(tally.divergences.empty());
    // wasm-tools validated every module with the cut's features: one Isoline refuses is a validator
    // divergence.
    CHECK(tally.modules[std::to_underlying(Applicability::Invalid)] == 0);
    auto const compared = tally.modules[std::to_underlying(Applicability::Compared)];
    CHECK(static_cast<double>(compared) >= MinComparedShare * static_cast<double>(modules.size()));
    CHECK(static_cast<double>(tally.calls) >= MinCallsPerModule * static_cast<double>(compared));
    CHECK(tally.harnessFaults() == 0);
    if (oracle)
    {
        auto const agrees = tally.verdicts[std::to_underlying(OracleVerdict::Agrees)];
        CHECK(static_cast<double>(agrees) >= MinAgreesPerSampledModule * static_cast<double>(tally.sampled));
    }
}
