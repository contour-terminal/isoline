// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>

#include <isoline/text/TextError.hpp>
#include <isoline/text/TextLimits.hpp>
#include <isoline/text/TopLevel.hpp>

using isoline::text::compileText;
using isoline::text::positionAt;
using isoline::text::SourcePosition;
using isoline::text::specMessage;
using isoline::text::stageOf;
using isoline::text::TextError;
using isoline::text::TextErrorCode;
using isoline::text::TextErrorStage;
using isoline::text::TextErrorTable;
using isoline::text::TextLimits;

TEST_CASE("TextError.everyRowInOrder", "[TextError]")
{
    auto index = std::size_t { 0 };
    for (auto const& row: TextErrorTable)
    {
        CHECK(static_cast<std::size_t>(row.code) == index);
        CHECK_FALSE(row.specMessage.empty());
        ++index;
    }
    CHECK(index == static_cast<std::size_t>(isoline::text::LastTextErrorCode) + 1);
}

namespace
{

/// A text that compiling refuses with one error.
struct Reachable
{
    TextErrorCode code;      ///< The error.
    std::string_view source; ///< A text compileText() refuses with it.
    TextLimits limits {};    ///< The limits it is compiled under.
};

/// One text per TextErrorTable row, in the table's order. Adding a row is adding a text here, or
/// TextError.everyRowReachable fails: a row nothing produces is a message no test can see.
constexpr auto ReachableRows = std::to_array<Reachable>({
    { .code = TextErrorCode::IllegalCharacter, .source = "(module (@x \x01))" },
    { .code = TextErrorCode::MisplacedControlCharacter, .source = "(module \x01)" },
    { .code = TextErrorCode::MisplacedUnicodeCharacter, .source = "(module λ)" },
    { .code = TextErrorCode::IllegalControlInString, .source = "(module (func (export \"a\x01\")))" },
    { .code = TextErrorCode::IllegalEscape, .source = R"((module (func (export "\q"))))" },
    { .code = TextErrorCode::UnclosedString, .source = R"((module (func (export "a)" },
    { .code = TextErrorCode::UnclosedComment, .source = "(module (; x" },
    { .code = TextErrorCode::MalformedUtf8, .source = R"((module $"\ef"))" },
    { .code = TextErrorCode::EmptyIdentifier, .source = R"((module $""))" },
    { .code = TextErrorCode::EmptyAnnotationId, .source = R"((module (@a (@""))))" },
    { .code = TextErrorCode::UnclosedAnnotation, .source = "(module (@x" },
    { .code = TextErrorCode::UnexpectedToken, .source = "(module" },
    { .code = TextErrorCode::AlignmentNotPowerOfTwo,
      .source = "(module (memory 1) (func (drop (i32.load align=3 (i32.const 0)))))" },
    { .code = TextErrorCode::InlineFunctionType,
      .source = "(module (type $t (func)) (func (type $t) (param i32)))" },
    { .code = TextErrorCode::UnknownLabel, .source = "(module (func (br $l)))" },
    { .code = TextErrorCode::MismatchingLabel, .source = "(module (func block $a end $b))" },
    { .code = TextErrorCode::MultipleStart, .source = "(module (func) (start 0) (start 0))" },
    { .code = TextErrorCode::WrongLaneCount, .source = "(module (func v128.const i32x4 0 0 0 drop))" },
    { .code = TextErrorCode::InvalidLaneLength,
      .source = "(module (func i8x16.shuffle 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0))" },
    { .code = TextErrorCode::IdentifierNotAllowed,
      .source = R"((module (import "t" (item "a") (item "b") (func $f))))" },
    { .code = TextErrorCode::InvalidCustomPageSize, .source = "(module (memory (pagesize 0) (data)))" },
    { .code = TextErrorCode::NestingLimitExceeded,
      .source = "(module (func (block (block))))",
      .limits = TextLimits { .maxNesting = 3 } },
    { .code = TextErrorCode::SourceTooLarge,
      .source = "(module)",
      .limits = TextLimits { .maxSourceBytes = 4 } },
    { .code = TextErrorCode::OutputTooLarge,
      .source = "(module)",
      .limits = TextLimits { .maxModuleBytes = 4 } },
    { .code = TextErrorCode::NumberTooLong,
      .source = "(module (func (drop (i32.const 12345))))",
      .limits = TextLimits { .maxNumberLength = 3 } },
    { .code = TextErrorCode::UnsupportedComponent, .source = "(component)" },
    { .code = TextErrorCode::UnknownOperator, .source = "(module (func fnc))" },
    { .code = TextErrorCode::UnknownIdentifier, .source = "(module (func (call $f)))" },
    { .code = TextErrorCode::DuplicateIdentifier, .source = "(module (func $f) (func $f))" },
    { .code = TextErrorCode::ImportAfterDefinition, .source = R"((module (func) (import "a" "b" (func))))" },
    { .code = TextErrorCode::ConstantOutOfRange,
      .source = "(module (func (drop (i32.const 0x1_0000_0000))))" },
    { .code = TextErrorCode::CustomMissingName, .source = "(module (@custom))" },
    { .code = TextErrorCode::CustomMalformedUtf8, .source = R"((module (@custom "\df")))" },
    { .code = TextErrorCode::CustomUnexpectedToken, .source = R"((module (@custom "bla" here)))" },
    { .code = TextErrorCode::CustomMalformedSectionKind, .source = R"((module (@custom "bla" (after))))" },
    { .code = TextErrorCode::CustomMalformedPlacement, .source = R"((module (@custom "bla" (type))))" },
    { .code = TextErrorCode::MisplacedAnnotation, .source = R"((module) (@custom "x"))" },
    { .code = TextErrorCode::MisplacedCustom, .source = R"((module (func (@custom "b"))))" },
    { .code = TextErrorCode::MisplacedName, .source = R"((module (func) (@name "M")))" },
    { .code = TextErrorCode::NameMultipleModule, .source = R"((module (@name "M1") (@name "M2")))" },
    { .code = TextErrorCode::NameMultipleFunctionNames,
      .source = R"((module (func (@name "a") (@name "b"))))" },
    { .code = TextErrorCode::NameMultipleLocalNames,
      .source = R"((module (func (param $x (@name "a") (@name "b") i32))))" },
    { .code = TextErrorCode::NameStringExpected, .source = "(module (func (@name)))" },
    { .code = TextErrorCode::NameUnexpectedToken, .source = R"((module (func (@name "a" "b"))))" },
    { .code = TextErrorCode::NameMalformedUtf8, .source = R"((module (func (@name "\ff"))))" },
    { .code = TextErrorCode::BranchHintDuplicate,
      .source =
          R"((module (func (@metadata.code.branch_hint "\01") (@metadata.code.branch_hint "\01") nop)))" },
    { .code = TextErrorCode::BranchHintNotInFunction,
      .source = R"((module (@metadata.code.branch_hint "\01") (func nop)))" },
    { .code = TextErrorCode::BranchHintInvalidPlacement,
      .source = R"((module (func (@metadata.code.branch_hint "\01"))))" },
    { .code = TextErrorCode::BranchHintInvalidValue,
      .source = R"((module (func (@metadata.code.branch_hint "\02") nop)))" },
    { .code = TextErrorCode::BranchHintUnexpectedToken,
      .source = R"((module (func (@metadata.code.branch_hint (x)) nop)))" },
});

} // namespace

TEST_CASE("TextError.everyRowReachable", "[TextError]")
{
    // Through compileText, the entry every caller uses, so a row the reader's layers produce but the top
    // level maps to another would fail too.
    REQUIRE(ReachableRows.size() == TextErrorTable.size());
    for (auto const& row: TextErrorTable)
    {
        auto const reachable = std::ranges::find(ReachableRows, row.code, &Reachable::code);
        INFO(row.specMessage);
        REQUIRE(reachable != ReachableRows.end());
        INFO(reachable->source);
        auto const compiled = compileText(reachable->source, reachable->limits);
        REQUIRE_FALSE(compiled.has_value());
        CHECK(compiled.error().code == row.code);
        CHECK(specMessage(compiled.error()).starts_with(row.specMessage));
    }
}

TEST_CASE("TextError.prefixAndSuffix", "[TextError]")
{
    CHECK(specMessage(TextError { TextErrorCode::UnknownOperator, {}, "get_local" })
          == "unknown operator get_local");
    CHECK(specMessage(TextError { TextErrorCode::ConstantOutOfRange, {}, "i8" })
          == "i8 constant out of range");
    CHECK(specMessage(TextError { TextErrorCode::UnknownIdentifier, {}, "type 3" }) == "unknown type 3");
    // Without a subject, a detailed row reads as its message alone.
    CHECK(specMessage(TextError { TextErrorCode::ConstantOutOfRange, {} }) == "constant out of range");
    CHECK(specMessage(TextError { TextErrorCode::DuplicateIdentifier, {} }) == "duplicate");
    // A row without a detail ignores a subject.
    CHECK(specMessage(TextError { TextErrorCode::UnclosedComment, {}, "ignored" }) == "unclosed comment");
}

TEST_CASE("TextError.stages", "[TextError]")
{
    CHECK(stageOf(TextErrorCode::UnexpectedToken) == TextErrorStage::Syntax);
    CHECK(stageOf(TextErrorCode::ConstantOutOfRange) == TextErrorStage::Syntax);
    CHECK(stageOf(TextErrorCode::MisplacedCustom) == TextErrorStage::Custom);
    CHECK(stageOf(TextErrorCode::BranchHintUnexpectedToken) == TextErrorStage::Custom);
}

TEST_CASE("TextError.positionAt", "[TextError]")
{
    auto const position = positionAt("ab\ncd", 4);
    CHECK(position.line == 2);
    CHECK(position.column == 2);
    CHECK(position.offset == 4);

    auto const start = positionAt("ab", 0);
    CHECK(start.line == 1);
    CHECK(start.column == 1);

    // CR LF is one line break, a lone CR another.
    CHECK(positionAt("a\r\nb\rc", 6).line == 3);
    CHECK(positionAt("a\r\nb\rc", 6).column == 2);
    CHECK(positionAt("a\r\nb\rc", 3).line == 2);

    // An offset past the end is the end.
    auto const end = positionAt("ab", 9);
    CHECK(end.offset == 2);
    CHECK(end.column == 3);
}

TEST_CASE("TextError.constructor", "[TextError]")
{
    auto const error = TextError { TextErrorCode::UnknownLabel,
                                   SourcePosition { .line = 3, .column = 4, .offset = 7 },
                                   "$l" };
    CHECK(error.code == TextErrorCode::UnknownLabel);
    CHECK(error.position.line == 3);
    CHECK(error.position.column == 4);
    CHECK(error.position.offset == 7);
    CHECK(error.subject == "$l");
}
