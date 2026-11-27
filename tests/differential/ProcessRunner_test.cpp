// SPDX-License-Identifier: Apache-2.0
#include <core/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "ProcessRunner.hpp"

using namespace isoline::differential;

namespace
{

/// The child, built beside the test: CMake names it in ISOLINE_DIFFERENTIAL_CHILD.
[[nodiscard]] std::filesystem::path child()
{
    auto const path = core::LiveEnvironment {}.get("ISOLINE_DIFFERENTIAL_CHILD");
    if (!path)
        SKIP("ISOLINE_DIFFERENTIAL_CHILD is not set");
    return std::filesystem::path { *path };
}

/// A directory of this build's own, which no other run shares: where a runner keeps its outputs.
[[nodiscard]] std::filesystem::path scratch()
{
    auto const directory = std::filesystem::current_path() / "process-runner-scratch";
    std::filesystem::create_directories(directory);
    return directory;
}

/// Long enough for any child that ends on its own, under a sanitizer or an emulator.
constexpr auto Generous = std::chrono::milliseconds { 60'000 };

} // namespace

TEST_CASE("ProcessRunner.runsAProgramToItsEnd")
{
    auto const program = child();
    auto runner = ProcessRunner { scratch() };
    // Arguments a shell or a command-line split would change: spaces, quotes, backslashes, an empty one.
    auto const arguments =
        std::vector<std::string> { "3", "plain", "two words", "a \"quoted\" one", "back\\slash\\", "" };
    auto const result = runner.run(program, arguments, Generous);
    REQUIRE(result.has_value());
    CHECK(result->exitCode == 3);
    // Windows' C runtime writes each newline of a text stream as CR LF.
    auto output = result->standardOutput;
    std::erase(output, '\r');
    CHECK(output == "plain\ntwo words\na \"quoted\" one\nback\\slash\\\n\n");
    CHECK(result->standardError == "to standard error");
    // Its output files are gone.
    CHECK(std::filesystem::is_empty(scratch()));

    auto const success = runner.run(program, std::vector<std::string> { "0" }, Generous);
    REQUIRE(success.has_value());
    CHECK(success->exitCode == 0);
    CHECK(success->standardOutput.empty());
}

TEST_CASE("ProcessRunner.reportsAProgramThatDoesNotStart")
{
    auto runner = ProcessRunner { scratch() };
    auto const result = runner.run(scratch() / "no-such-program", {}, Generous);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == ProcessErrorKind::NotStarted);
    CHECK_FALSE(result.error().detail.empty());
    CHECK(std::filesystem::is_empty(scratch()));
}

TEST_CASE("ProcessRunner.reportsAProgramThatAborts")
{
    auto const program = child();
    auto runner = ProcessRunner { scratch() };
    auto const result = runner.run(program, std::vector<std::string> { "abort" }, Generous);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == ProcessErrorKind::Killed);
    CHECK(std::filesystem::is_empty(scratch()));
}

TEST_CASE("ProcessRunner.killsAProgramPastItsDeadline")
{
    // The child sleeps a minute; the runner ends it after a tenth of a second, long before.
    auto const program = child();
    auto runner = ProcessRunner { scratch() };
    auto const started = std::chrono::steady_clock::now();
    auto const result =
        runner.run(program, std::vector<std::string> { "sleep" }, std::chrono::milliseconds { 100 });
    auto const took = std::chrono::steady_clock::now() - started;
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind == ProcessErrorKind::TimedOut);
    CHECK(took < std::chrono::seconds { 30 });
    CHECK(std::filesystem::is_empty(scratch()));
}
