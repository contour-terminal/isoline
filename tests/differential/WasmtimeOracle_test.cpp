// SPDX-License-Identifier: Apache-2.0
#include <core/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "Corpus.hpp"
#include "Observation.hpp"
#include "ProcessRunner.hpp"
#include "TrapMapping.hpp"
#include "WasmtimeOracle.hpp"
#include <isoline/runtime/Abrupt.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/text/TextLimits.hpp>
#include <isoline/text/TopLevel.hpp>

using namespace isoline;
using namespace isoline::differential;

namespace
{

/// Replays a recorded run of Wasmtime, and keeps the arguments it was asked to run with.
class RecordedRunner final: public IProcessRunner
{
  public:
    explicit RecordedRunner(ProcessResult recorded): _recorded { std::move(recorded) } {}

    [[nodiscard]] std::expected<ProcessResult, std::string> run(
        std::filesystem::path const& program, std::span<std::string const> arguments) override
    {
        _program = program;
        _arguments.assign(arguments.begin(), arguments.end());
        return _recorded;
    }

    [[nodiscard]] std::filesystem::path const& program() const noexcept { return _program; }
    [[nodiscard]] std::vector<std::string> const& arguments() const noexcept { return _arguments; }

  private:
    ProcessResult _recorded;
    std::filesystem::path _program;
    std::vector<std::string> _arguments;
};

/// Module files that are never written: each module is named /modules/module.wasm.
class NamedModuleFiles final: public IModuleFiles
{
  public:
    [[nodiscard]] std::expected<std::filesystem::path, std::string> write(
        std::span<std::byte const> /*module*/) override
    {
        return std::filesystem::path { "/modules/module.wasm" };
    }
};

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
    ProcessResult recorded,
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
    SECTION("a failure that is no trap")
    {
        auto const result = callRecorded(ProcessResult {
            .exitCode = 1, .standardOutput = {}, .standardError = "Error: unknown option\n" });
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().kind == OracleErrorKind::Unparsed);
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
                    { "--invoke", "f", "/modules/module.wasm", "-1", "-9223372036854775808", "nan", "-inf" });
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
    auto runner = ProcessRunner { std::filesystem::temp_directory_path() };
    auto files =
        DirectoryModuleFiles { std::filesystem::temp_directory_path() / "isoline-differential-oracle" };
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
}
