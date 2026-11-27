// SPDX-License-Identifier: Apache-2.0
#include <core/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "ProcessRunner.hpp"

using namespace isoline::differential;

TEST_CASE("ProcessRunner.runsAProgramToItsEnd")
{
    // The child, built beside the test: CMake names it in ISOLINE_DIFFERENTIAL_CHILD.
    auto const child = core::LiveEnvironment {}.get("ISOLINE_DIFFERENTIAL_CHILD");
    if (!child)
        SKIP("ISOLINE_DIFFERENTIAL_CHILD is not set");
    // A directory of this build's own, which no other run shares.
    auto const scratch = std::filesystem::current_path() / "process-runner-scratch";
    std::filesystem::create_directories(scratch);
    auto runner = ProcessRunner { scratch };
    // Arguments a shell or a command-line split would change: spaces, quotes, backslashes, an empty one.
    auto const arguments =
        std::vector<std::string> { "3", "plain", "two words", "a \"quoted\" one", "back\\slash\\", "" };
    auto const result = runner.run(*child, arguments);
    REQUIRE(result.has_value());
    CHECK(result->exitCode == 3);
    // Windows' C runtime writes each newline of a text stream as CR LF.
    auto output = result->standardOutput;
    std::erase(output, '\r');
    CHECK(output == "plain\ntwo words\na \"quoted\" one\nback\\slash\\\n\n");
    CHECK(result->standardError == "to standard error");
    // Its output files are gone.
    CHECK(std::filesystem::is_empty(scratch));

    auto const success = runner.run(*child, std::vector<std::string> { "0" });
    REQUIRE(success.has_value());
    CHECK(success->exitCode == 0);
    CHECK(success->standardOutput.empty());
}

TEST_CASE("ProcessRunner.reportsAProgramThatDoesNotRun")
{
    auto runner = ProcessRunner { std::filesystem::temp_directory_path() };
    auto const result = runner.run(std::filesystem::temp_directory_path() / "no-such-program", {});
    // Either it cannot be started, or (POSIX) its child cannot execute it: a shell's 127.
    if (result)
        CHECK(result->exitCode == 127);
}
