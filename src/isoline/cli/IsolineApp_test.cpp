// SPDX-License-Identifier: Apache-2.0
#include <core/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <new>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <isoline/cli/IsolineApp.hpp>
#include <isoline/cli/Verbs.hpp>
#include <isoline/cli/testing/MemoryFiles.hpp>
#include <isoline/text/TextLimits.hpp>

using isoline::cli::ExitCode;
using isoline::cli::IsolineApp;
using isoline::cli::VerbContext;
using isoline::cli::VerbRow;
using isoline::cli::Verbs;
using isoline::cli::testing::MemoryFiles;
using isoline::text::TextLimits;

namespace
{

/// An environment that holds no variable.
class EmptyEnvironment final: public core::Environment
{
  public:
    [[nodiscard]] std::optional<std::string> get(std::string_view) const override { return std::nullopt; }
};

/// The tool over in-memory files, run with argument lists.
struct Tool
{
    EmptyEnvironment environment;
    MemoryFiles files;
    std::ostringstream diagnostics;

    /// Runs `isoline` with @p arguments.
    /// @return Its exit code.
    [[nodiscard]] ExitCode run(std::vector<char const*> arguments, std::span<VerbRow const> verbs = Verbs)
    {
        arguments.insert(arguments.begin(), "isoline");
        auto app = IsolineApp { environment, files, diagnostics, TextLimits {}, verbs };
        return static_cast<ExitCode>(app.run(static_cast<int>(arguments.size()), arguments.data()));
    }
};

} // namespace

TEST_CASE("IsolineApp.optionsAroundTheInput")
{
    auto tool = Tool {};
    tool.files.add("a.wat", "(module)");
    SECTION("after it")
    {
        CHECK(tool.run({ "compile", "a.wat", "-o", "a.wasm" }) == ExitCode::Success);
    }
    SECTION("before it")
    {
        CHECK(tool.run({ "compile", "--output", "a.wasm", "a.wat" }) == ExitCode::Success);
    }
    SECTION("on both sides")
    {
        CHECK(tool.run({ "compile", "--features", "wasm3", "a.wat", "--output=a.wasm" })
              == ExitCode::Success);
    }
    CHECK(tool.diagnostics.str().empty());
    CHECK(tool.files.files.contains("a.wasm"));
}

TEST_CASE("IsolineApp.features")
{
    auto tool = Tool {};
    tool.files.add("a.wat",
                   "(module (func (param i64 i64 i64 i64) (result i64 i64)"
                   " local.get 0 local.get 1 local.get 2 local.get 3 i64.add128))");
    CHECK(tool.run({ "compile", "a.wat", "-o", "a.wasm" }) == ExitCode::Success);
    CHECK(tool.run({ "compile", "a.wat", "-o", "b.wasm", "--features", "all" }) == ExitCode::Success);
    // The option after the input wins over the one before it.
    CHECK(tool.run({ "compile", "--features", "all", "a.wat", "-o", "c.wasm", "--features", "wasm3" })
          == ExitCode::InputRejected);
    CHECK(tool.run({ "compile", "--features", "wasm3", "a.wat", "-o", "c.wasm" }) == ExitCode::InputRejected);
    CHECK_FALSE(tool.files.files.contains("c.wasm"));
    CHECK(tool.run({ "disassemble", "a.wasm", "-o", "a.wat", "--features", "wasm3" })
          == ExitCode::InputRejected);
}

TEST_CASE("IsolineApp.commandLineRefused")
{
    auto tool = Tool {};
    tool.files.add("a.wat", "(module)");
    auto const refused = [&](std::vector<char const*> arguments, std::string_view message) {
        tool.diagnostics.str({});
        auto const code = tool.run(std::move(arguments));
        CHECK(code == ExitCode::UsageOrIo);
        auto const diagnostics = tool.diagnostics.str();
        INFO(diagnostics);
        CHECK(diagnostics.starts_with(std::string { "isoline: " } + std::string { message }));
        CHECK(diagnostics.contains("compile"));
        CHECK(diagnostics.contains("disassemble"));
    };
    refused({}, "no verb given");
    refused({ "assemble", "a.wat" }, "Unexpected argument");
    refused({ "compile", "a.wat" }, "no output given (-o, --output)");
    refused({ "compile", "-o", "a.wasm" }, "no input given");
    refused({ "compile", "a.wat", "b.wat", "-o", "a.wasm" }, "Unexpected argument");
    refused({ "compile", "--unknown", "a.wat", "-o", "a.wasm" }, "Unknown option");
    refused({ "compile", "a.wat", "-o", "a.wasm", "--unknown" }, "Unknown option");
    refused({ "compile", "a.wat", "-o", "a.wasm", "--features", "wasm9" },
            "unknown feature set 'wasm9' (all, wasm3)");
    refused({ "compile", "a.wat", "--output=" }, "the output is empty (-o, --output)");
    refused({ "compile", "a.wat", "-o", "a.wasm", "--features=" }, "unknown feature set '' (all, wasm3)");
    refused({ "compile", "-o", "a.wasm", "--", "a.wat", "--features", "wasm3" }, "Unexpected argument");
    CHECK_FALSE(tool.files.files.contains("a.wasm"));
}

namespace
{

/// A verb that runs out of memory.
ExitCode exhaust(VerbContext const&, std::string_view, std::string_view)
{
    throw std::bad_alloc {};
}

/// A verb that fails in a way the tool has no row for.
ExitCode breakDown(VerbContext const&, std::string_view, std::string_view)
{
    throw std::runtime_error { "broken" };
}

/// A verb that throws what is no exception.
ExitCode throwNumber(VerbContext const&, std::string_view, std::string_view)
{
    throw 42;
}

constexpr auto ThrowingVerbs = std::to_array<VerbRow>({
    { .name = "exhaust", .help = "Runs out of memory.", .run = &exhaust },
    { .name = "break", .help = "Fails.", .run = &breakDown },
    { .name = "throw", .help = "Throws a number.", .run = &throwNumber },
});

} // namespace

TEST_CASE("IsolineApp.exceptionsAreReported")
{
    auto tool = Tool {};
    auto const run = [&](char const* verb) {
        tool.diagnostics.str({});
        return tool.run({ verb, "a.wat", "-o", "a.wasm" }, ThrowingVerbs);
    };
    CHECK(run("exhaust") == ExitCode::UsageOrIo);
    CHECK(tool.diagnostics.str() == "isoline: error: out of memory\n");
    CHECK(run("break") == ExitCode::InternalError);
    CHECK(tool.diagnostics.str() == "isoline: error: internal error: broken\n");
    CHECK(run("throw") == ExitCode::InternalError);
    CHECK(tool.diagnostics.str() == "isoline: error: internal error: an unknown exception\n");
}

TEST_CASE("IsolineApp.coreCommands")
{
    // help, version and license are core-cpp's, run by App::run.
    auto tool = Tool {};
    CHECK(tool.run({ "version" }) == ExitCode::Success);
    CHECK(tool.diagnostics.str().empty());
}
