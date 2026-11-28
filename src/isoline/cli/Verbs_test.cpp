// SPDX-License-Identifier: Apache-2.0
#include <core/Utils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <format>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/api/Module.hpp>
#include <isoline/binary/Decoder.hpp>
#include <isoline/binary/Encoder.hpp>
#include <isoline/binary/Module.hpp>
#include <isoline/cli/Verbs.hpp>
#include <isoline/cli/testing/MemoryFiles.hpp>
#include <isoline/text/TextLimits.hpp>
#include <isoline/text/TopLevel.hpp>

using isoline::FeatureSet;
using isoline::binary::canonicalize;
using isoline::binary::decodeModule;
using isoline::binary::encodeModule;
using isoline::binary::MemoryType;
using isoline::binary::Module;
using isoline::cli::compileVerb;
using isoline::cli::disassembleCoreModule;
using isoline::cli::disassembleVerb;
using isoline::cli::ExitCode;
using isoline::cli::exitCodeOf;
using isoline::cli::FeatureChoice;
using isoline::cli::loadFailure;
using isoline::cli::Refusal;
using isoline::cli::Refusals;
using isoline::cli::VerbContext;
using isoline::cli::testing::MemoryFiles;
using isoline::text::compileText;
using isoline::text::TextLimits;

namespace
{

/// A wide-arithmetic module, which WebAssembly 3.0 does not have.
constexpr auto WideArithmetic =
    std::string_view { "(module (func (param i64 i64 i64 i64) (result i64 i64)"
                       " local.get 0 local.get 1 local.get 2 local.get 3 i64.add128))" };

/// The component preamble: the magic number, then version 0x0d and layer 1.
constexpr auto ComponentPreamble = std::string_view { "\0asm\x0d\0\x01\0", 8 };

/// A verb's world: in-memory files and the diagnostics it printed.
struct Harness
{
    MemoryFiles files;
    std::ostringstream diagnostics;
    FeatureChoice features = FeatureChoice::All;
    TextLimits limits {};

    /// The context the verbs run with.
    [[nodiscard]] VerbContext context()
    {
        return VerbContext {
            .files = files, .diagnostics = diagnostics, .features = features, .limits = limits
        };
    }

    [[nodiscard]] ExitCode compile(std::string_view input = "a.wat", std::string_view output = "a.wasm")
    {
        return compileVerb(context(), input, output);
    }

    [[nodiscard]] ExitCode disassemble(std::string_view input = "a.wasm", std::string_view output = "a.wat")
    {
        return disassembleVerb(context(), input, output);
    }
};

/// The bytes of @p source, which must compile.
std::vector<std::byte> compiled(std::string_view source)
{
    auto bytes = compileText(source, TextLimits {});
    REQUIRE(bytes.has_value());
    return std::move(*bytes);
}

/// The module of @p bytes, which must decode with every feature.
Module decoded(std::vector<std::byte> const& bytes)
{
    auto module = decodeModule(bytes, FeatureSet::all());
    REQUIRE(module.has_value());
    return std::move(*module);
}

} // namespace

TEST_CASE("Verbs.compile")
{
    auto harness = Harness {};
    harness.files.add("a.wat", "(module (func (export \"f\")))");
    CHECK(harness.compile() == ExitCode::Success);
    CHECK(harness.diagnostics.str().empty());
    REQUIRE(harness.files.files.contains("a.wasm"));
    auto const module = decoded(harness.files.files.at("a.wasm"));
    REQUIRE(module.exports.size() == 1);
    CHECK(module.exports.front().name == "f");
}

TEST_CASE("Verbs.compileReportsValidationError")
{
    auto harness = Harness {};
    SECTION("within a body")
    {
        harness.files.add("a.wat", "(module\n  (func (i32.add)))");
        CHECK(harness.compile() == ExitCode::InputRejected);
        auto const diagnostics = harness.diagnostics.str();
        INFO(diagnostics);
        CHECK(diagnostics.starts_with("a.wat: error: function 0, instruction 0: "));
        CHECK(diagnostics.contains("type mismatch"));
    }
    SECTION("at its function and instruction")
    {
        harness.files.add("a.wat", "(module (func) (func nop nop i32.add))");
        CHECK(harness.compile() == ExitCode::InputRejected);
        CHECK(
            harness.diagnostics.str().starts_with("a.wat: error: function 1, instruction 2: type mismatch"));
    }
    SECTION("in a function, outside its instructions")
    {
        // A local of a type the module does not have fails before the body's first instruction.
        harness.files.add("a.wat", "(module (func) (func (local (ref 5))))");
        CHECK(harness.compile() == ExitCode::InputRejected);
        CHECK(harness.diagnostics.str() == "a.wat: error: function 1: unknown type 5\n");
    }
    SECTION("outside every body")
    {
        harness.files.add("a.wat", "(module (export \"f\" (func 0)))");
        CHECK(harness.compile() == ExitCode::InputRejected);
        CHECK(harness.diagnostics.str() == "a.wat: error: unknown function 0\n");
    }
    CHECK_FALSE(harness.files.files.contains("a.wasm"));
}

TEST_CASE("Verbs.compileSyntaxError")
{
    auto harness = Harness {};
    harness.files.add("a.wat", "(module (fnc))");
    CHECK(harness.compile() == ExitCode::InputRejected);
    CHECK(harness.diagnostics.str() == "a.wat:1:10: error: unknown operator fnc\n");
    CHECK_FALSE(harness.files.files.contains("a.wasm"));
}

TEST_CASE("Verbs.disassemble")
{
    auto harness = Harness {};
    auto const bytes = compiled("(module (func (export \"f\") (result i32) i32.const 42))");
    harness.files.files["a.wasm"] = bytes;
    CHECK(harness.disassemble() == ExitCode::Success);
    CHECK(harness.diagnostics.str().empty());
    auto const text = harness.files.text("a.wat");
    INFO(text);
    CHECK(text.contains("(export \"f\" (func 0))"));
    auto const canonical = canonicalize(bytes, FeatureSet::all());
    REQUIRE(canonical.has_value());
    CHECK(compiled(text) == *canonical);
}

TEST_CASE("Verbs.disassembleMalformed")
{
    auto harness = Harness {};
    // A module whose type section claims a type it does not have.
    harness.files.add("a.wasm", std::string_view { "\0asm\x01\0\0\0\x01\x01\x01", 11 });
    CHECK(harness.disassemble() == ExitCode::InputRejected);
    auto const diagnostics = harness.diagnostics.str();
    INFO(diagnostics);
    CHECK(diagnostics.starts_with("a.wasm: error at offset 0x"));
    CHECK_FALSE(harness.files.files.contains("a.wat"));
    // A binary shorter than any preamble is read as a core module, whose decoder says what is wrong.
    harness.diagnostics.str({});
    harness.files.add("b.wasm", std::string_view { "\0asm", 4 });
    CHECK(harness.disassemble("b.wasm", "b.wat") == ExitCode::InputRejected);
    CHECK(harness.diagnostics.str().starts_with("b.wasm: error at offset 0x"));
}

TEST_CASE("Verbs.componentRefused")
{
    auto harness = Harness {};
    SECTION("text")
    {
        harness.files.add("a.wat", "(component)");
        CHECK(harness.compile() == ExitCode::InputRejected);
        CHECK(harness.diagnostics.str() == "a.wat:1:1: error: component text needs the component model\n");
    }
    SECTION("binary")
    {
        harness.files.add("a.wasm", ComponentPreamble);
        CHECK(harness.disassemble() == ExitCode::InputRejected);
        CHECK(harness.diagnostics.str() == "a.wasm: error: component text needs the component model\n");
        CHECK_FALSE(harness.files.files.contains("a.wat"));
    }
}

TEST_CASE("Verbs.featuresGate")
{
    auto harness = Harness {};
    SECTION("compile")
    {
        harness.files.add("a.wat", WideArithmetic);
        CHECK(harness.compile() == ExitCode::Success);
        harness.files.files.erase("a.wasm");
        harness.features = FeatureChoice::Wasm3;
        CHECK(harness.compile() == ExitCode::InputRejected);
        auto const diagnostics = harness.diagnostics.str();
        INFO(diagnostics);
        CHECK(diagnostics.starts_with("a.wat: error: "));
        CHECK(diagnostics.contains("wide-arithmetic"));
        CHECK_FALSE(harness.files.files.contains("a.wasm"));
    }
    SECTION("disassemble")
    {
        harness.files.files["a.wasm"] = compiled(WideArithmetic);
        CHECK(harness.disassemble() == ExitCode::Success);
        harness.files.files.erase("a.wat");
        harness.features = FeatureChoice::Wasm3;
        CHECK(harness.disassemble() == ExitCode::InputRejected);
        CHECK(harness.diagnostics.str().starts_with("a.wasm: error at offset 0x"));
        CHECK_FALSE(harness.files.files.contains("a.wat"));
    }
}

TEST_CASE("Verbs.ioFailure")
{
    auto harness = Harness {};
    SECTION("a missing input")
    {
        CHECK(harness.compile() == ExitCode::UsageOrIo);
        CHECK(harness.diagnostics.str() == "a.wat: error: cannot read: No such file or directory\n");
        CHECK(harness.disassemble() == ExitCode::UsageOrIo);
    }
    SECTION("an output that cannot be written")
    {
        harness.files.add("a.wat", "(module)");
        harness.files.unwritable.insert("a.wasm");
        CHECK(harness.compile() == ExitCode::UsageOrIo);
        CHECK(harness.diagnostics.str() == "a.wasm: error: cannot write: Permission denied\n");
        harness.files.files["b.wasm"] = compiled("(module)");
        harness.files.unwritable.insert("a.wat");
        CHECK(harness.disassemble("b.wasm", "a.wat") == ExitCode::UsageOrIo);
    }
    SECTION("the host out of memory")
    {
        harness.files.exhausting.insert("a.wat");
        CHECK(harness.compile() == ExitCode::UsageOrIo);
        CHECK(harness.diagnostics.str() == "a.wat: error: out of memory\n");
    }
}

TEST_CASE("Verbs.inputTooLarge")
{
    auto harness = Harness {};
    SECTION("compile: a text past maxSourceBytes")
    {
        harness.limits.maxSourceBytes = 8;
        harness.files.add("a.wat", "(module)");
        CHECK(harness.compile() == ExitCode::Success);
        harness.files.add("a.wat", "(module) ");
        CHECK(harness.compile("a.wat", "b.wasm") == ExitCode::InputRejected);
        CHECK(harness.diagnostics.str() == "a.wat: error: larger than the limit of 8 bytes\n");
        CHECK_FALSE(harness.files.files.contains("b.wasm"));
    }
    SECTION("disassemble: a binary past maxModuleBytes")
    {
        harness.limits.maxModuleBytes = 8;
        // The empty module is the 8 bytes of its preamble.
        harness.files.files["a.wasm"] = compiled("(module)");
        CHECK(harness.disassemble() == ExitCode::Success);
        harness.files.files["a.wasm"].push_back(std::byte { 0 });
        CHECK(harness.disassemble("a.wasm", "b.wat") == ExitCode::InputRejected);
        CHECK(harness.diagnostics.str() == "a.wasm: error: larger than the limit of 8 bytes\n");
        CHECK_FALSE(harness.files.files.contains("b.wat"));
    }
}

TEST_CASE("Verbs.writerRefusals")
{
    auto harness = Harness {};
    SECTION("text too large")
    {
        harness.files.files["a.wasm"] = compiled("(module (func (export \"a long export name\")))");
        harness.limits.maxSourceBytes = 32;
        CHECK(harness.disassemble() == ExitCode::InputRejected);
        CHECK(harness.diagnostics.str()
              == "a.wasm: error: its text would be larger than the limit of 32 bytes\n");
        auto const refused =
            disassembleCoreModule(harness.files.files.at("a.wasm"), harness.context(), "a.wasm");
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().refusal == Refusal::TextTooLarge);
    }
    SECTION("a page size the text format cannot say")
    {
        auto module = Module {};
        module.memories.push_back(MemoryType { .limits = {}, .pageSizeLog2 = 32 });
        harness.files.files["a.wasm"] = encodeModule(module);
        CHECK(harness.disassemble() == ExitCode::InputRejected);
        CHECK(harness.diagnostics.str()
              == "a.wasm: error: memory page size 2^32 or more cannot be written as text\n");
        auto const refused =
            disassembleCoreModule(harness.files.files.at("a.wasm"), harness.context(), "a.wasm");
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().refusal == Refusal::PageSizeNotWritable);
    }
    CHECK_FALSE(harness.files.files.contains("a.wat"));
}

TEST_CASE("Verbs.refusalExitCodes")
{
    CHECK(exitCodeOf(Refusal::CommandLine) == ExitCode::UsageOrIo);
    CHECK(exitCodeOf(Refusal::Unreadable) == ExitCode::UsageOrIo);
    CHECK(exitCodeOf(Refusal::TextMalformed) == ExitCode::InputRejected);
    CHECK(exitCodeOf(Refusal::OutOfMemory) == ExitCode::UsageOrIo);
    CHECK(exitCodeOf(Refusal::InternalError) == ExitCode::InternalError);
    for (auto const& row: Refusals)
    {
        CHECK(row.exitCode != ExitCode::Success);
        CHECK_FALSE(row.meaning.empty());
    }
}

TEST_CASE("Verbs.outputChecks")
{
    auto harness = Harness {};
    harness.files.add("a.wat", "(module)");
    SECTION("an output that names the input is refused before the input is read")
    {
        harness.files.exhausting.insert("a.wat");
        CHECK(harness.compile("a.wat", "a.wat") == ExitCode::UsageOrIo);
        CHECK(harness.diagnostics.str()
              == "a.wat: error: the output names the input, which it would overwrite\n");
        harness.files.files["a.wasm"] = compiled("(module)");
        CHECK(harness.disassemble("a.wasm", "a.wasm") == ExitCode::UsageOrIo);
        CHECK(harness.files.files.at("a.wasm") == compiled("(module)"));
    }
    SECTION("a binary is not written to a terminal; text is")
    {
        harness.files.terminals.insert("-");
        CHECK(harness.compile("a.wat", "-") == ExitCode::UsageOrIo);
        CHECK(harness.diagnostics.str().starts_with("-: error: a binary is not written to a terminal"));
        CHECK_FALSE(harness.files.files.contains("-"));
        harness.files.files["a.wasm"] = compiled("(module)");
        CHECK(harness.disassemble("a.wasm", "-") == ExitCode::Success);
        CHECK(harness.files.text("-").starts_with("(module"));
    }
}

TEST_CASE("Verbs.loadFailure")
{
    auto const tooLarge = loadFailure(isoline::api::ModuleTooLarge { .bytes = 9, .limit = 8 }, "a.wat");
    CHECK(tooLarge.refusal == Refusal::InputTooLarge);
    CHECK(tooLarge.message == "a.wat: error: its module of 9 bytes is larger than the limit of 8 bytes");
    auto const exhausted = loadFailure(isoline::api::HostMemoryExhausted {}, "a.wat");
    CHECK(exhausted.refusal == Refusal::OutOfMemory);
    CHECK(exhausted.message == "a.wat: error: out of memory");
    auto const uncompiled = loadFailure(
        isoline::api::CompileLimitExceeded { .codeIndex = 3, .requested = 200, .limit = 128 }, "a.wat");
    CHECK(uncompiled.refusal == Refusal::InputTooLarge);
    CHECK(uncompiled.message
          == "a.wat: error: lowering function 3 needs 200 bytes, past the compile budget of 128 bytes");
    auto const tooManySlots =
        loadFailure(isoline::api::CompileLimitExceeded { .codeIndex = isoline::api::ModuleLevel,
                                                         .requested = 5'000'000'000,
                                                         .limit = 4'294'967'295,
                                                         .kind = isoline::api::CompileLimit::FrameSlots },
                    "a.wat");
    CHECK(
        tooManySlots.message
        == "a.wat: error: lowering the module-level records needs 5000000000 slots, past the frame slots of "
           "4294967295 slots");
}

TEST_CASE("Verbs.readmeListsEveryRefusal")
{
    // README.md's exit status table is Refusals, row for row.
    auto const text = core::readFileAsString(std::filesystem::path { ISOLINE_SOURCE_DIR } / "README.md");
    REQUIRE_FALSE(text.empty());
    auto table =
        std::string { "| Status | Refusal |\n| --- | --- |\n| 0 | none: the output was written |\n" };
    for (auto const& row: Refusals)
        table += std::format("| {} | {} |\n", static_cast<int>(row.exitCode), row.meaning);
    CHECK(text.contains(table + "\n"));
}
