// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <isoline/cli/VerbArguments.hpp>

using isoline::cli::parseVerbArguments;
using isoline::cli::VerbArguments;
using isoline::cli::verbOperand;
using isoline::cli::VerbOption;

namespace
{

/// The arguments of `compile <tokens>`, which must parse.
VerbArguments parsed(std::vector<std::string_view> const& tokens)
{
    auto arguments = parseVerbArguments("compile", tokens);
    INFO((arguments.has_value() ? std::string {} : arguments.error()));
    REQUIRE(arguments.has_value());
    return std::move(*arguments);
}

/// What is wrong with `compile <tokens>`, which must not parse.
std::string refused(std::vector<std::string_view> const& tokens)
{
    auto const arguments = parseVerbArguments("compile", tokens);
    REQUIRE_FALSE(arguments.has_value());
    return arguments.error();
}

} // namespace

TEST_CASE("VerbArguments.optionsAroundTheInput")
{
    auto const before = parsed({ "-o", "a.wasm", "a.wat" });
    CHECK(before.input == "a.wat");
    CHECK(before.option(VerbOption::Output) == "a.wasm");
    CHECK_FALSE(before.option(VerbOption::Features).has_value());
    auto const after = parsed({ "a.wat", "--output=a.wasm", "--features", "wasm3" });
    CHECK(after.input == "a.wat");
    CHECK(after.option(VerbOption::Output) == "a.wasm");
    CHECK(after.option(VerbOption::Features) == "wasm3");
    // The later wins.
    CHECK(parsed({ "--features", "all", "a.wat", "--features", "wasm3" }).option(VerbOption::Features)
          == "wasm3");
    // An option given before the input is kept when the part after gives another one.
    CHECK(parsed({ "--features", "all", "a.wat", "-o", "x" }).option(VerbOption::Features) == "all");
}

TEST_CASE("VerbArguments.emptyIsGiven")
{
    // An empty value is given, not "not given": --output= and --features= are refused by the verb.
    auto const empty = parsed({ "a.wat", "--output=", "--features=" });
    CHECK(empty.option(VerbOption::Output) == "");
    CHECK(empty.option(VerbOption::Features) == "");
    CHECK(parsed({ "--output=", "a.wat" }).option(VerbOption::Output) == "");
    CHECK_FALSE(parsed({ "a.wat" }).option(VerbOption::Output).has_value());
}

TEST_CASE("VerbArguments.endOfOptions")
{
    SECTION("-- before the input stops the options")
    {
        CHECK(refused({ "-o", "x", "--", "a.wat", "--features", "wasm3" })
              == "Unexpected argument \"--features\" after the input.");
        CHECK(parsed({ "-o", "x", "--", "a.wat" }).input == "a.wat");
    }
    SECTION("an input named like an option follows --")
    {
        // ::core::cli reads a bare option name as the option (contour-terminal/core-cpp#106).
        for (auto const name: { "o", "output", "features" })
        {
            INFO(name);
            auto const arguments = parsed({ "-o", "x", "--", name });
            CHECK(arguments.input == name);
            CHECK(arguments.option(VerbOption::Output) == "x");
        }
        // Without --, a bare `o` is -o, and takes the next token as its value.
        auto const bare = parsed({ "o", "a.wasm", "a.wat" });
        CHECK(bare.input == "a.wat");
        CHECK(bare.option(VerbOption::Output) == "a.wasm");
        CHECK(verbOperand().helpText.contains("(o, output, features) follows --"));
    }
    SECTION("a -- that is an option's value ends nothing")
    {
        auto const arguments = parsed({ "-o", "--", "a.wat", "--features", "wasm3" });
        CHECK(arguments.option(VerbOption::Output) == "--");
        CHECK(arguments.input == "a.wat");
        CHECK(arguments.option(VerbOption::Features) == "wasm3");
        // And one after such a value still ends the options.
        CHECK(parsed({ "-o", "--", "--", "a.wat" }).input == "a.wat");
    }
}

TEST_CASE("VerbArguments.refused")
{
    CHECK(refused({}) == "no input given");
    CHECK(refused({ "-o", "x" }) == "no input given");
    CHECK(refused({ "--unknown", "a.wat" }).starts_with("Unknown option"));
    CHECK(refused({ "a.wat", "--unknown" }).starts_with("Unknown option"));
    CHECK(refused({ "a.wat", "b.wat" }).starts_with("Unexpected argument \"b.wat\""));
    CHECK(refused({ "a.wat", "-o" }).starts_with("Option \"output\" expects a value"));
}
