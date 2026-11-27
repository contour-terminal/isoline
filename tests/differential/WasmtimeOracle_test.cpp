// SPDX-License-Identifier: Apache-2.0
#include <core/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "Corpus.hpp"
#include "Observation.hpp"
#include "ProcessRunner.hpp"
#include "TrapMapping.hpp"
#include "WasmtimeOracle.hpp"
#include "testing/RecordedRunner.hpp"
#include <isoline/runtime/Abrupt.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/text/TextLimits.hpp>
#include <isoline/text/TopLevel.hpp>

using namespace isoline;
using namespace isoline::differential;
using isoline::differential::testing::NamedModuleFiles;
using isoline::differential::testing::RecordedRunner;

namespace
{

[[nodiscard]] std::vector<std::byte> compiled(std::string_view text)
{
    auto module = text::compileText(text, text::TextLimits {});
    REQUIRE(module.has_value());
    return std::move(*module);
}

/// Wasmtime 50.0.3's standard error for a trap with @p message, as it printed it (its backtrace aside).
[[nodiscard]] std::string recordedTrap(std::string_view message)
{
    return std::format(
        "warning: using `--invoke` with a function that returns values is experimental and may "
        "break in the future\nError: failed to run main module `module.wasm`\n\nCaused by:\n    "
        "0: failed to invoke `f`\n    1: error while executing at wasm backtrace:\n    0:     "
        "0xad - <unknown>!<wasm function 0>\n    2: wasm trap: {}\n",
        message);
}

/// A module whose export f takes an i32, an i64, an f32 and an f64, and returns an i32.
constexpr auto ScalarModule =
    std::string_view { R"((module (func (export "f") (param i32 i64 f32 f64) (result i32) (i32.const 0))))" };

[[nodiscard]] std::expected<OracleResult, OracleError> callRecorded(
    std::expected<ProcessResult, ProcessError> recorded,
    std::string_view module = ScalarModule,
    std::vector<runtime::Value> args = { runtime::I32 {}, runtime::I64 {}, runtime::F32 {}, runtime::F64 {} })
{
    auto runner = RecordedRunner { std::move(recorded) };
    auto files = NamedModuleFiles {};
    auto const oracle = WasmtimeOracle { "/tools/wasmtime", runner, files };
    return oracle.call(compiled(module), "f", args);
}

} // namespace

TEST_CASE("WasmtimeOracle.mapsEveryTrapRow")
{
    for (auto const& row: TrapMappingRows)
    {
        INFO(row.message);
        auto const result = callRecorded(ProcessResult {
            .exitCode = 134, .standardOutput = {}, .standardError = recordedTrap(row.message) });
        REQUIRE(result.has_value());
        auto const* trap = std::get_if<OracleTrap>(&*result);
        REQUIRE(trap != nullptr);
        CHECK(trap->message == row.message);
        CHECK(std::ranges::equal(trap->codes, row.agreeing()));
        // Every code of the row agrees with Isoline's trap of that code, and no other code does.
        for (auto const& trapRow: runtime::TrapTable)
        {
            auto const isoline = CallOutcome { runtime::Abrupt { runtime::Trap { trapRow.code } } };
            CHECK(agreesWithOracle(*result, isoline)
                  == (std::ranges::find(row.agreeing(), trapRow.code) != row.agreeing().end()));
        }
    }
}

TEST_CASE("WasmtimeOracle.everyRowIsADistinctMessage")
{
    for (auto const& row: TrapMappingRows)
    {
        CHECK(trapMappingOf(row.message) == &row);
        CHECK_FALSE(row.agreeing().empty());
    }
    CHECK(trapMappingOf("no such trap") == nullptr);
}

TEST_CASE("WasmtimeOracle.reportsWhatItCannotMap")
{
    SECTION("an unknown trap")
    {
        auto const result =
            callRecorded(ProcessResult { .exitCode = 134,
                                         .standardOutput = {},
                                         .standardError = recordedTrap("misaligned memory access") });
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().kind == OracleErrorKind::UnmappedTrap);
        CHECK(result.error().detail == "misaligned memory access");
    }
    SECTION("its own timeout")
    {
        auto const result = callRecorded(ProcessResult {
            .exitCode = 134, .standardOutput = {}, .standardError = recordedTrap("interrupt") });
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().kind == OracleErrorKind::Exhausted);
    }
    SECTION("a limit of its own, which a WasmtimeLimitRows row names")
    {
        // Wasmtime 50.0.3's error for a table of 10^11 elements, as it printed it.
        auto const result = callRecorded(ProcessResult {
            .exitCode = 1,
            .standardOutput = {},
            .standardError = "Error: out of memory (failed to allocate 800000000000 bytes)\n" });
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().kind == OracleErrorKind::Refused);
    }
    SECTION("an option it does not know: a fault of the harness, never a limit")
    {
        auto const result =
            callRecorded(ProcessResult { .exitCode = 2,
                                         .standardOutput = {},
                                         .standardError = "error: unexpected argument '--frobnicate' "
                                                          "found\n\nUsage: wasmtime run [OPTIONS] "
                                                          "<WASM>...\n" });
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().kind == OracleErrorKind::Rejected);
        CHECK(OracleErrorRows[std::to_underlying(result.error().kind)].fault == OracleFault::HarnessFault);
    }
    SECTION("a feature it refuses to validate")
    {
        auto const result = callRecorded(ProcessResult {
            .exitCode = 1,
            .standardOutput = {},
            .standardError =
                "Error: failed to compile\n\nCaused by:\n    0: WebAssembly translation error\n    1: "
                "Invalid input WebAssembly code at offset 9: exceptions support is not enabled\n" });
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().kind == OracleErrorKind::Rejected);
    }
    SECTION("results it cannot read")
    {
        auto const result =
            callRecorded(ProcessResult { .exitCode = 0, .standardOutput = "three\n", .standardError = {} });
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().kind == OracleErrorKind::Unparsed);
    }
    SECTION("too few results")
    {
        auto const result =
            callRecorded(ProcessResult { .exitCode = 0, .standardOutput = "", .standardError = {} });
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().kind == OracleErrorKind::Unparsed);
    }
}

TEST_CASE("WasmtimeOracle.reportsAProcessThatFailed")
{
    // Each a fault of the harness, never of the guest: the oracle could not ask Wasmtime at all.
    SECTION("it does not start")
    {
        auto const result = callRecorded(std::unexpected {
            ProcessError { .kind = ProcessErrorKind::NotStarted, .detail = "no such file" } });
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().kind == OracleErrorKind::Process);
    }
    SECTION("it runs past the deadline")
    {
        auto const result = callRecorded(
            std::unexpected { ProcessError { .kind = ProcessErrorKind::TimedOut, .detail = "killed" } });
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().kind == OracleErrorKind::Process);
    }
    SECTION("it crashes")
    {
        auto const result = callRecorded(
            std::unexpected { ProcessError { .kind = ProcessErrorKind::Killed, .detail = "signal 11" } });
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().kind == OracleErrorKind::Process);
    }
    SECTION("a shell's cannot-execute status")
    {
        auto const result = callRecorded(
            ProcessResult { .exitCode = 127, .standardOutput = {}, .standardError = "cannot execute\n" });
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().kind == OracleErrorKind::Process);
    }
    SECTION("a failure that says nothing")
    {
        auto const result =
            callRecorded(ProcessResult { .exitCode = 1, .standardOutput = {}, .standardError = {} });
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().kind == OracleErrorKind::Process);
    }
}

TEST_CASE("WasmtimeOracle.givesWasmtimeADeadlinePastItsOwnTimeout")
{
    auto runner =
        RecordedRunner { ProcessResult { .exitCode = 0, .standardOutput = "0\n", .standardError = {} } };
    auto files = NamedModuleFiles {};
    auto const oracle = WasmtimeOracle { "/tools/wasmtime", runner, files, std::chrono::seconds { 4 } };
    auto const result =
        oracle.call(compiled(ScalarModule),
                    "f",
                    { { runtime::I32 {}, runtime::I64 {}, runtime::F32 {}, runtime::F64 {} } });
    REQUIRE(result.has_value());
    CHECK(runner.timeout() > std::chrono::seconds { 4 });
}

TEST_CASE("WasmtimeOracle.namesEveryFaultOfTheHarness")
{
    // One row per OracleErrorKind, in order; the harness's own faults are the ones a replay fails on.
    REQUIRE(OracleErrorRows.size() == std::to_underlying(LastOracleErrorKind) + 1);
    for (auto const index: std::views::iota(std::size_t { 0 }, OracleErrorRows.size()))
        CHECK(std::to_underlying(OracleErrorRows[index].kind) == index);
    auto const faults = std::vector<OracleErrorKind> { OracleErrorKind::ModuleFile,
                                                       OracleErrorKind::Process,
                                                       OracleErrorKind::Rejected,
                                                       OracleErrorKind::UnmappedTrap,
                                                       OracleErrorKind::Unparsed };
    for (auto const& row: OracleErrorRows)
    {
        INFO(row.name);
        CHECK((row.fault == OracleFault::HarnessFault) == std::ranges::contains(faults, row.kind));
    }
}

TEST_CASE("WasmtimeOracle.everyLimitRowIsRefused")
{
    // Each row of Wasmtime's own limits, as the last line of its error: Refused, and no other kind.
    for (auto const& row: WasmtimeLimitRows)
    {
        INFO(row.message);
        auto const result =
            callRecorded(ProcessResult { .exitCode = 1,
                                         .standardOutput = {},
                                         .standardError = std::format("Error: {} (detail)\n", row.message) });
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().kind == OracleErrorKind::Refused);
    }
}

TEST_CASE("WasmtimeOracle.readsAnEscapedException")
{
    auto const result = callRecorded(ProcessResult {
        .exitCode = 1,
        .standardOutput = {},
        .standardError =
            "Error: failed to run main module `module.wasm`\n\nCaused by:\n    0: failed to invoke `f`\n    "
            "1: error while executing at wasm backtrace:\n    2: thrown Wasm exception\n" });
    REQUIRE(result.has_value());
    CHECK(std::holds_alternative<OracleThrown>(*result));
    CHECK(agreesWithOracle(*result, CallOutcome { runtime::Abrupt { runtime::Thrown { 3 } } }));
    CHECK_FALSE(
        agreesWithOracle(*result, CallOutcome { std::vector<runtime::Value> { runtime::I32 { 0 } } }));
}

TEST_CASE("WasmtimeOracle.readsResultsAsWasmtimePrintsThem")
{
    // Recorded from Wasmtime 50.0.3: integers signed, floats as Rust's shortest decimal without an exponent,
    // NaN without its bits, and a vector as one unsigned 128-bit decimal.
    auto const module = std::string_view { R"((module (func (export "f")
        (result i32 i64 f32 f64 f32 f64 v128)
        (unreachable))))" };
    auto const result = callRecorded(
        ProcessResult { .exitCode = 0,
                        .standardOutput =
                            "-1\n-5\n0.000000000000000000000000000000000000000000001\n-0\nNaN\ninf\n"
                            "316912650112397582603894390785\n",
                        .standardError = {} },
        module,
        {});
    REQUIRE(result.has_value());
    auto const* values = std::get_if<std::vector<runtime::Value>>(&*result);
    REQUIRE(values != nullptr);
    auto const expected = std::vector<runtime::Value> {
        runtime::I32 { 0xFFFF'FFFF },
        runtime::I64 { 0xFFFF'FFFF'FFFF'FFFB },
        runtime::F32 { 0x0000'0001 },
        runtime::F64 { 0x8000'0000'0000'0000 },
        runtime::F32 { 0x7FC0'0000 },
        runtime::F64 { 0x7FF0'0000'0000'0000 },
        runtime::V128 { .lanes = { 0x0000'0002'0000'0001, 0x0000'0004'0000'0003 } },
    };
    CHECK(*values == expected);
    // A NaN agrees with any NaN of Isoline's, whose bits Wasmtime does not print; other values exactly.
    auto isoline = expected;
    isoline[4] = runtime::F32 { 0xFFC0'0001 };
    CHECK(agreesWithOracle(*result, CallOutcome { isoline }));
    isoline[3] = runtime::F64 { 0 };
    CHECK_FALSE(agreesWithOracle(*result, CallOutcome { isoline }));
    CHECK_FALSE(agreesWithOracle(
        *result, CallOutcome { runtime::Abrupt { runtime::Trap { runtime::TrapCode::Unreachable } } }));
}

TEST_CASE("WasmtimeOracle.passesArgumentsAsWasmtimeTakesThem")
{
    auto runner =
        RecordedRunner { ProcessResult { .exitCode = 0, .standardOutput = "0\n", .standardError = {} } };
    auto files = NamedModuleFiles {};
    auto const oracle = WasmtimeOracle { "/tools/wasmtime", runner, files, std::chrono::seconds { 7 } };
    auto const args = std::vector<runtime::Value> { runtime::I32 { 0xFFFF'FFFF },
                                                    runtime::I64 { 0x8000'0000'0000'0000 },
                                                    runtime::F32 { 0x7FC0'0000 },
                                                    runtime::F64 { 0xFFF0'0000'0000'0000 } };
    auto const result = oracle.call(compiled(ScalarModule), "f", args);
    REQUIRE(result.has_value());
    CHECK(runner.program() == std::filesystem::path { "/tools/wasmtime" });
    auto expected = wasmtimeFlags(std::chrono::seconds { 7 });
    expected.insert(expected.end(),
                    { "--invoke=f", "/modules/module.wasm", "-1", "-9223372036854775808", "nan", "-inf" });
    CHECK(runner.arguments() == expected);
    CHECK(std::ranges::find(expected, "cranelift-enable-nan-canonicalization=true") != expected.end());
    CHECK(expected[2].contains("relaxed-simd-deterministic=y"));
    CHECK(expected[2].ends_with("timeout=7s"));
}

TEST_CASE("WasmtimeOracle.refusesWhatItCannotRun")
{
    SECTION("a NaN with a payload, which its command line cannot spell")
    {
        auto const result =
            callRecorded(ProcessResult {},
                         ScalarModule,
                         { runtime::I32 {}, runtime::I64 {}, runtime::F32 { 0x7FA0'0001 }, runtime::F64 {} });
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().kind == OracleErrorKind::Unrepresentable);
    }
    SECTION("a module that imports")
    {
        auto const result =
            callRecorded(ProcessResult {}, R"((module (import "env" "g" (func)) (func (export "f"))))", {});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().kind == OracleErrorKind::Ineligible);
    }
    SECTION("a vector parameter")
    {
        auto const result = callRecorded(
            ProcessResult {}, R"((module (func (export "f") (param v128))))", { runtime::V128 {} });
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().kind == OracleErrorKind::Ineligible);
    }
    SECTION("an export whose name holds a NUL, which no command-line argument can")
    {
        // The argument would end at the NUL and name another export: "" here, which returns 7.
        auto runner =
            RecordedRunner { ProcessResult { .exitCode = 0, .standardOutput = "7\n", .standardError = {} } };
        auto files = NamedModuleFiles {};
        auto const oracle = WasmtimeOracle { "/tools/wasmtime", runner, files };
        auto const module = compiled(
            R"((module (func (export "\00a") (result i32) (i32.const 1)) (func (export "") (result i32) (i32.const 7))))");
        auto const result = oracle.call(module, std::string_view { "\0a", 2 }, {});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().kind == OracleErrorKind::Unrepresentable);
        CHECK(runner.arguments().empty());
    }
    SECTION("an export named like an option, which Wasmtime would read as one")
    {
        auto runner =
            RecordedRunner { ProcessResult { .exitCode = 0, .standardOutput = "1\n", .standardError = {} } };
        auto files = NamedModuleFiles {};
        auto const oracle = WasmtimeOracle { "/tools/wasmtime", runner, files };
        auto const module = compiled(R"((module (func (export "-") (result i32) (i32.const 1))))");
        auto const result = oracle.call(module, "-", {});
        REQUIRE(result.has_value());
        CHECK(std::ranges::contains(runner.arguments(), std::string { "--invoke=-" }));
    }
    SECTION("no such export")
    {
        auto const result = callRecorded(ProcessResult {}, R"((module (func (export "g"))))", {});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().kind == OracleErrorKind::Ineligible);
    }
}

TEST_CASE("WasmtimeOracle.agreesWithTheReferenceOnTheEdges", "[differential]")
{
    // The real Wasmtime, where scripts/fetch-tools.py fetched it: ISOLINE_WASMTIME names it.
    auto const wasmtime = core::LiveEnvironment {}.get("ISOLINE_WASMTIME");
    if (!wasmtime)
        SKIP("ISOLINE_WASMTIME is not set");
    if (wasmtime->empty() || !std::filesystem::is_regular_file(*wasmtime))
        FAIL(std::format("ISOLINE_WASMTIME={} names no file: the fetch failed", *wasmtime));
    // The build's own directory, removed afterwards.
    auto const scratch = std::filesystem::current_path() / "oracle-edges";
    std::filesystem::create_directories(scratch);
    auto runner = ProcessRunner { scratch };
    auto files = DirectoryModuleFiles { scratch / "modules" };
    auto const oracle = WasmtimeOracle { std::filesystem::path { *wasmtime }, runner, files };
    auto agreed = std::size_t { 0 };
    for (auto const name: { "calls.wat", "memory.wat", "bulk.wat", "exceptions.wat", "numbers.wat" })
    {
        auto const module = readModule(std::filesystem::path { ISOLINE_SOURCE_DIR } / "tests" / "differential"
                                       / "edges" / name);
        REQUIRE(module.has_value());
        for (auto const seed: std::views::iota(uint64_t { 0 }, uint64_t { 8 }))
        {
            for (auto const& comparison: compareWithOracle(oracle, *module, seedBytes(seed)))
            {
                INFO(std::format("{} seed {} {}: {}", name, seed, comparison.exportName, comparison.detail));
                CHECK(comparison.verdict != OracleVerdict::Disagrees);
                if (comparison.verdict == OracleVerdict::Agrees)
                    ++agreed;
            }
        }
    }
    CHECK(agreed > 50);
    auto ignored = std::error_code {};
    std::filesystem::remove_all(scratch, ignored);
}
