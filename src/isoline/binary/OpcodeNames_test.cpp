// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>

#include <isoline/binary/OpcodeNames.hpp>

using isoline::binary::firstMisnamedOpcode;
using isoline::binary::firstUnusedException;
using isoline::binary::namedByRule;
using isoline::binary::OpcodeNameException;

// Every opcode's name is checked by Opcode.cpp's static_asserts, which no test can restate without
// being unable to fail; what is tested here is that the rule and the checks they apply refuse what is
// wrong, on fixtures of their own, and name the first offender's index.

namespace
{
constexpr auto Names = std::to_array<std::string_view>({ "I32Add", "SelectTyped", "Select" });
constexpr auto Texts = std::to_array<std::string_view>({ "i32.add", "select_t", "select" });
constexpr auto Typed = OpcodeNameException { .name = "SelectTyped", .text = "select_t", .reason = "typed" };

constexpr auto OneException = std::to_array<OpcodeNameException>({ Typed });
constexpr auto NoException = std::array<OpcodeNameException, 0> {};
constexpr auto DuplicateException = std::to_array<OpcodeNameException>({ Typed, Typed });
constexpr auto UnusedException = std::to_array<OpcodeNameException>(
    { Typed, OpcodeNameException { .name = "Bogus", .text = "bogus", .reason = "unused" } });
constexpr auto RuleException = std::to_array<OpcodeNameException>(
    { Typed, OpcodeNameException { .name = "Select", .text = "select", .reason = "redundant" } });
constexpr auto WrongNames = std::to_array<std::string_view>({ "I32Add", "SelectTyped", "I32Sub" });
constexpr auto WrongText = std::to_array<OpcodeNameException>(
    { OpcodeNameException { .name = "SelectTyped", .text = "select", .reason = "wrong text" } });
constexpr auto Redundant = OpcodeNameException { .name = "Select", .text = "select", .reason = "redundant" };
constexpr auto RuleExceptionTwice = std::to_array<OpcodeNameException>({ Typed, Redundant, Redundant });
constexpr auto SharedNames = std::to_array<std::string_view>({ "SelectTyped", "SelectTyped", "Select" });
constexpr auto SharedTexts = std::to_array<std::string_view>({ "select_t", "select_t", "select" });
} // namespace

TEST_CASE("OpcodeNames.checksFindTheFirstOffender")
{
    // Clean: neither check finds an offender, and each says so with its end index.
    STATIC_REQUIRE(firstMisnamedOpcode(Names, Texts, OneException) == 3);
    STATIC_REQUIRE(firstUnusedException(Names, Texts, OneException) == 1);
    // A wrong name: Select's slot holds I32Sub.
    STATIC_REQUIRE(firstMisnamedOpcode(WrongNames, Texts, OneException) == 2);
    // A missing exception row: SelectTyped follows no rule and no row.
    STATIC_REQUIRE(firstMisnamedOpcode(Names, Texts, NoException) == 1);
    STATIC_REQUIRE(firstUnusedException(Names, Texts, NoException) == 0);
    // A duplicate row: SelectTyped has two; each row alone is used once.
    STATIC_REQUIRE(firstMisnamedOpcode(Names, Texts, DuplicateException) == 1);
    STATIC_REQUIRE(firstUnusedException(Names, Texts, DuplicateException) == 2);
    // An unused row: no opcode is Bogus.
    STATIC_REQUIRE(firstMisnamedOpcode(Names, Texts, UnusedException) == 3);
    STATIC_REQUIRE(firstUnusedException(Names, Texts, UnusedException) == 1);
    // A row for a name the rule already covers: Select is both.
    STATIC_REQUIRE(firstMisnamedOpcode(Names, Texts, RuleException) == 2);
    STATIC_REQUIRE(firstUnusedException(Names, Texts, RuleException) == 1);
    // ... even with two rows.
    STATIC_REQUIRE(firstMisnamedOpcode(Names, Texts, RuleExceptionTwice) == 2);
    // A row that names the opcode but not its text matches nothing.
    STATIC_REQUIRE(firstMisnamedOpcode(Names, Texts, WrongText) == 1);
    STATIC_REQUIRE(firstUnusedException(Names, Texts, WrongText) == 0);
    // A row two opcodes share: it must name exactly one.
    STATIC_REQUIRE(firstUnusedException(SharedNames, SharedTexts, OneException) == 0);
}

TEST_CASE("OpcodeNames.ruleRejectsAWrongName")
{
    STATIC_REQUIRE(namedByRule("I64ExtendI32S", "i64.extend_i32_s"));
    STATIC_REQUIRE(namedByRule("Unreachable", "unreachable"));
    STATIC_REQUIRE_FALSE(namedByRule("I32Sub", "i32.add"));
    // A part must be capitalized, and a separator dropped, not kept or doubled.
    STATIC_REQUIRE_FALSE(namedByRule("I64extendI32S", "i64.extend_i32_s"));
    STATIC_REQUIRE_FALSE(namedByRule("I64.ExtendI32S", "i64.extend_i32_s"));
    STATIC_REQUIRE_FALSE(namedByRule("I64ExtendI32", "i64.extend_i32_s"));
    STATIC_REQUIRE_FALSE(namedByRule("I64ExtendI32SS", "i64.extend_i32_s"));
    // An empty part (a doubled, a leading or a trailing separator) names nothing.
    STATIC_REQUIRE_FALSE(namedByRule("I32Add", "i32..add"));
    STATIC_REQUIRE_FALSE(namedByRule("Select", "_select"));
    STATIC_REQUIRE_FALSE(namedByRule("Select", "select_"));
    STATIC_REQUIRE_FALSE(namedByRule("", ""));
}
