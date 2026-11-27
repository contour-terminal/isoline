// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string_view>
#include <vector>

#include <isoline/text/Keywords.hpp>
#include <isoline/text/ModuleReader.hpp>
#include <isoline/text/SExpression.hpp>
#include <isoline/text/TextError.hpp>
#include <isoline/text/TextLimits.hpp>
#include <isoline/text/testing/ReadModule.hpp>

using isoline::text::isKnownKeyword;
using isoline::text::NodeId;
using isoline::text::parseSExpressions;
using isoline::text::specMessage;
using isoline::text::TextErrorCode;
using isoline::text::TextLimits;
using isoline::text::unexpectedKeyword;

TEST_CASE("Keywords.unknownVersusUnexpected")
{
    auto const read = [](std::string_view source) {
        return isoline::text::testing::readModuleText(source, TextLimits {});
    };

    auto const unknown = read("(module (fnc))");
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().code == TextErrorCode::UnknownOperator);
    CHECK(unknown.error().subject == "fnc");
    CHECK(unknown.error().position.line == 1);
    CHECK(unknown.error().position.column == 10);

    auto const obsolete = read("(module (anyfunc))");
    REQUIRE_FALSE(obsolete.has_value());
    CHECK(specMessage(obsolete.error()) == "unknown operator anyfunc");

    auto const misplaced = read("(module (i32))");
    REQUIRE_FALSE(misplaced.has_value());
    CHECK(misplaced.error().code == TextErrorCode::UnexpectedToken);
    CHECK(misplaced.error().position.column == 10);
}

TEST_CASE("Keywords.vocabulary")
{
    CHECK(isKnownKeyword("module"));
    CHECK(isKnownKeyword("pagesize"));
    CHECK(isKnownKeyword("i32.add")); // An instruction's name.
    CHECK_FALSE(isKnownKeyword("anyfunc"));
    CHECK_FALSE(isKnownKeyword("struct")); // GC's: Isoline implements no GC.
    CHECK_FALSE(isKnownKeyword("select_t"));
}

TEST_CASE("Keywords.everyKindOfAtom")
{
    // Which atoms the reference lexer refuses wherever they appear, and which only its parser does.
    struct Row
    {
        std::string_view text;
        TextErrorCode code;
    };
    auto const rows = std::vector<Row> {
        { .text = "fnc", .code = TextErrorCode::UnknownOperator },
        { .text = R"wat(x"y")wat", .code = TextErrorCode::UnknownOperator }, // A reserved token.
        { .text = "i32", .code = TextErrorCode::UnexpectedToken },
        { .text = "i32.add", .code = TextErrorCode::UnexpectedToken },
        { .text = "nan:canonical", .code = TextErrorCode::UnexpectedToken },
        { .text = "1", .code = TextErrorCode::UnexpectedToken },
        { .text = "-1.5", .code = TextErrorCode::UnexpectedToken },
        { .text = "inf", .code = TextErrorCode::UnexpectedToken },
        { .text = "nan", .code = TextErrorCode::UnexpectedToken },
        { .text = "nan:0x1", .code = TextErrorCode::UnexpectedToken },
        { .text = "offset=8", .code = TextErrorCode::UnexpectedToken },
        { .text = "$x", .code = TextErrorCode::UnexpectedToken },
        { .text = R"wat("s")wat", .code = TextErrorCode::UnexpectedToken },
        { .text = "(fnc)", .code = TextErrorCode::UnexpectedToken }, // A list is refused at its `(`.
    };
    for (auto const& row: rows)
    {
        INFO(row.text);
        auto const tree = parseSExpressions(row.text, TextLimits {});
        REQUIRE(tree.has_value());
        auto const error = unexpectedKeyword(*tree, NodeId { 0 });
        CHECK(error.code == row.code);
        CHECK(error.position.column == 1);
        if (row.code == TextErrorCode::UnknownOperator)
            CHECK(error.subject == row.text);
    }
}
