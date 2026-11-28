// SPDX-License-Identifier: Apache-2.0
#include <core/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <print>
#include <ranges>
#include <utility>
#include <vector>

#include "ModuleGenerator.hpp"
#include "Seeds.hpp"
#include <Corpus.hpp>
#include <ProcessRunner.hpp>
#include <WasmtimeOracle.hpp>
#include <testing/EnvironmentNumber.hpp>
#include <testing/ScratchDirectory.hpp>

using namespace isoline::differential;

// The generator's import-free modules (GeneratorMode::Oracle) against Wasmtime: the reference and the
// interpreter share their numeric kernels, so only the oracle catches a fault in those. Label oracle-nightly:
// the nightly differential run (differential-nightly.yml) runs it with ISOLINE_WASMTIME set; without it, it
// skips (exit status 4).

TEST_CASE("GeneratedOracle.agreesWithWasmtime", "[differential]")
{
    auto const wasmtime = core::LiveEnvironment {}.get("ISOLINE_WASMTIME");
    if (!wasmtime)
        SKIP("no ISOLINE_WASMTIME: run scripts/fetch-tools.py --tool wasmtime");
    if (wasmtime->empty() || !std::filesystem::is_regular_file(*wasmtime))
        FAIL(std::format("ISOLINE_WASMTIME={} names no file: the fetch failed", *wasmtime));
    auto const count = testing::numberFromEnvironment("ISOLINE_FUZZ_ORACLE_MODULES", 1'000);
    auto const scratch = testing::ScratchDirectory { std::filesystem::current_path() / "generated-oracle" };
    auto runner = ProcessRunner { scratch.path() };
    auto files = DirectoryModuleFiles { scratch.path() / "modules" };
    auto const oracle = WasmtimeOracle { std::filesystem::path { *wasmtime }, runner, files };

    auto verdicts = std::array<uint64_t, OracleVerdictCount> {};
    auto faults = uint64_t { 0 };
    auto const modules =
        isoline::fuzz::generatedSeeds(static_cast<uint32_t>(count), isoline::fuzz::GeneratorMode::Oracle);
    for (auto const index: std::views::iota(std::size_t { 0 }, modules.size()))
    {
        for (auto const& comparison: compareWithOracle(oracle, modules[index], seedBytes(index)))
        {
            ++verdicts[std::to_underlying(comparison.verdict)];
            if (comparison.verdict == OracleVerdict::Disagrees)
                UNSCOPED_INFO(
                    std::format("module {} {}: {}", index, comparison.exportName, comparison.detail));
            if (comparison.error
                && OracleErrorRows[std::to_underlying(*comparison.error)].fault == OracleFault::HarnessFault)
            {
                ++faults;
                UNSCOPED_INFO(std::format("module {} {}: harness fault {}",
                                          index,
                                          comparison.exportName,
                                          OracleErrorRows[std::to_underlying(*comparison.error)].name));
            }
        }
    }
    std::println("{} generated modules: {} calls agree, {} disagree, {} not compared, {} harness faults",
                 modules.size(),
                 verdicts[std::to_underlying(OracleVerdict::Agrees)],
                 verdicts[std::to_underlying(OracleVerdict::Disagrees)],
                 verdicts[std::to_underlying(OracleVerdict::NotCompared)],
                 faults);
    CHECK(verdicts[std::to_underlying(OracleVerdict::Disagrees)] == 0);
    CHECK(faults == 0);
    // A generator that stops making modules the oracle can run fails: most modules have an agreed call.
    CHECK(verdicts[std::to_underlying(OracleVerdict::Agrees)] >= modules.size() / 2);
}
