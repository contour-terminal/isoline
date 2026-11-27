// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <isoline/text/Lexer.hpp>
#include <isoline/text/TextError.hpp>
#include <isoline/text/TextLimits.hpp>

using isoline::text::decodeName;
using isoline::text::decodeString;
using isoline::text::identifierName;
using isoline::text::Lexer;
using isoline::text::positionAt;
using isoline::text::specMessage;
using isoline::text::TextError;
using isoline::text::TextErrorCode;
using isoline::text::TextLimits;
using isoline::text::textOf;
using isoline::text::Token;
using isoline::text::TokenKind;

namespace
{

/// One token as kind and text, for comparing a whole stream at once.
using Lexed = std::pair<TokenKind, std::string>;

/// Lexes @p source to its end.
/// @param source The text to lex.
/// @return Every token up to and including End; the test fails if the lexer reports an error.
std::vector<Lexed> lexAll(std::string_view source)
{
    auto lexer = Lexer::create(source, TextLimits {});
    REQUIRE(lexer.has_value());
    auto tokens = std::vector<Lexed> {};
    while (true)
    {
        auto const token = lexer->next();
        REQUIRE(token.has_value());
        tokens.emplace_back(token->kind, std::string { textOf(source, *token) });
        if (token->kind == TokenKind::End)
            return tokens;
    }
}

/// Lexes @p source until it fails.
/// @param source The text to lex, which must not lex to its end.
/// @return The code of the first error.
TextErrorCode lexError(std::string_view source)
{
    auto lexer = Lexer::create(source, TextLimits {});
    REQUIRE(lexer.has_value());
    while (true)
    {
        auto const token = lexer->next();
        if (!token)
            return token.error().code;
        REQUIRE(token->kind != TokenKind::End);
    }
}

/// The error of a result that must be one.
/// @param result A result the test expects to have failed.
/// @return Its error's code; the test fails if it holds a value.
template <typename T>
TextErrorCode errorOf(std::expected<T, TextError> const& result)
{
    REQUIRE_FALSE(result.has_value());
    return result.error().code;
}

/// A token spanning the whole of @p source, of kind @p kind.
Token wholeToken(std::string_view source, TokenKind kind)
{
    return Token { .kind = kind, .offset = 0, .length = static_cast<uint32_t>(source.size()) };
}

} // namespace

TEST_CASE("Lexer.tokens", "[Lexer]")
{
    using enum TokenKind;
    CHECK(lexAll("(func $f (@name \"x\") i32.add 0x1p3)")
          == std::vector<Lexed> { { LeftParen, "(" },
                                  { Keyword, "func" },
                                  { Identifier, "$f" },
                                  { AnnotationStart, "(@name" },
                                  { String, "\"x\"" },
                                  { RightParen, ")" },
                                  { Keyword, "i32.add" },
                                  { Keyword, "0x1p3" },
                                  { RightParen, ")" },
                                  { End, "" } });
    // A quoted annotation id.
    CHECK(lexAll("(@\"a\")")
          == std::vector<Lexed> { { AnnotationStart, "(@\"a\"" }, { RightParen, ")" }, { End, "" } });
    // Reserved tokens are valid inside an annotation, and its nested lists end it only when balanced.
    CHECK(lexAll("(@a , ; ] [ }})")
          == std::vector<Lexed> { { AnnotationStart, "(@a" },
                                  { Reserved, "," },
                                  { Reserved, ";" },
                                  { Reserved, "]" },
                                  { Reserved, "[" },
                                  { Reserved, "}" },
                                  { Reserved, "}" },
                                  { RightParen, ")" },
                                  { End, "" } });
    // annotations.wast:16: inside an annotation, `(@` without an id is a plain `(` and an `@`.
    CHECK(lexAll("(@a @ @x (@x) (@x y) (@) (@ x) (@(@(@(@)))))")
          == std::vector<Lexed> { { AnnotationStart, "(@a" }, { Reserved, "@" },   { Reserved, "@x" },
                                  { AnnotationStart, "(@x" }, { RightParen, ")" }, { AnnotationStart, "(@x" },
                                  { Reserved, "y" },          { RightParen, ")" }, { LeftParen, "(" },
                                  { Reserved, "@" },          { RightParen, ")" }, { LeftParen, "(" },
                                  { Reserved, "@" },          { Reserved, "x" },   { RightParen, ")" },
                                  { LeftParen, "(" },         { Reserved, "@" },   { LeftParen, "(" },
                                  { Reserved, "@" },          { LeftParen, "(" },  { Reserved, "@" },
                                  { LeftParen, "(" },         { Reserved, "@" },   { RightParen, ")" },
                                  { RightParen, ")" },        { RightParen, ")" }, { RightParen, ")" },
                                  { RightParen, ")" },        { End, "" } });
    // A malformed quoted id inside an annotation leaves a plain `(`, and the string is lexed on its own.
    CHECK(lexError("(@a (@\"\n\"))") == TextErrorCode::UnclosedString);
    // A well-formed one must still be a non-empty name.
    CHECK(lexError("(@a (@\"\"))") == TextErrorCode::EmptyAnnotationId);
    CHECK(lexAll("(@a (bc (@c d)) e) f")
          == std::vector<Lexed> { { AnnotationStart, "(@a" },
                                  { LeftParen, "(" },
                                  { Keyword, "bc" },
                                  { AnnotationStart, "(@c" },
                                  { Reserved, "d" },
                                  { RightParen, ")" },
                                  { RightParen, ")" },
                                  { Reserved, "e" },
                                  { RightParen, ")" },
                                  { Reserved, "f" },
                                  { End, "" } });
}

TEST_CASE("Lexer.tokenClasses", "[Lexer]")
{
    using enum TokenKind;
    auto const kindOf = [](std::string_view source) {
        auto const tokens = lexAll(source);
        REQUIRE(tokens.size() == 2);
        CHECK(tokens[0].second == source);
        return tokens[0].first;
    };
    // Numbers are keywords, as in the spec's grammar.
    for (auto const* number: { "0",
                               "1_000",
                               "+1",
                               "-0x1F",
                               "0xa_b",
                               "1.",
                               "1.5",
                               "1e10",
                               "1.5E-3",
                               "-1e+2",
                               "0x1.",
                               "0x1.8p-3",
                               "0x1P+4",
                               "inf",
                               "-inf",
                               "nan",
                               "+nan:0x7f",
                               "1_0.2_5" })
        CHECK(kindOf(number) == Keyword);
    for (auto const* keyword:
         { "module", "i32.add", "nan:canonical", "v128.load8x8_s", "nan:0x", "offset=4", "align=0x10" })
        CHECK(kindOf(keyword) == Keyword);
    // Anything else made of identifier characters and strings is reserved.
    for (auto const* reserved:
         { "a",       "1a",      "0x",      "1__0",      "1_",      "+",      "0x1p",
           "1e",      "Module",  "offset=", "offset=-1", "align=a", "x\"y\"", R"("a""b")",
           "$a\"b\"", "$\"a\"b", "=",       ",",         "[",       "{" })
        CHECK(kindOf(reserved) == Reserved);
    for (auto const* reserved: { "_1", "offset=1x", "~ab", "a~b", "1.5e" })
        CHECK(kindOf(reserved) == Reserved);
    CHECK(kindOf("nan:0x1g") == Keyword);
    CHECK(kindOf("i32.Add") == Keyword);
    CHECK(kindOf("\"\\u{41}\"") == String);
    CHECK(kindOf("$x.y'z") == Identifier);
    CHECK(kindOf("$\"a b\"") == Identifier);
    CHECK(kindOf("\"\"") == String);
}

TEST_CASE("Lexer.comments", "[Lexer]")
{
    using enum TokenKind;
    CHECK(lexAll(";; line\nnop ;; end") == std::vector<Lexed> { { Keyword, "nop" }, { End, "" } });
    CHECK(lexAll("a;;x\rb") == std::vector<Lexed> { { Reserved, "a" }, { Reserved, "b" }, { End, "" } });
    CHECK(lexAll("(@a ;; c\n (; d ;) b)")
          == std::vector<Lexed> {
              { AnnotationStart, "(@a" }, { Reserved, "b" }, { RightParen, ")" }, { End, "" } });
    CHECK(lexError(";; \x80") == TextErrorCode::MalformedUtf8);
    CHECK(lexError("(; \xff ;)") == TextErrorCode::MalformedUtf8);
    CHECK(lexError("(@a ;; c") == TextErrorCode::UnclosedAnnotation);
}

TEST_CASE("Lexer.nestedBlockComments", "[Lexer]")
{
    using enum TokenKind;
    CHECK(lexAll("(; a (; b ;) c ;) nop") == std::vector<Lexed> { { Keyword, "nop" }, { End, "" } });
    CHECK(lexAll("(; λ\n;)") == std::vector<Lexed> { { End, "" } });
    CHECK(lexError("(; (; ;)") == TextErrorCode::UnclosedComment);

    // Deep nesting is counted, not recursed.
    auto deep = std::string {};
    for ([[maybe_unused]] auto const level: std::views::iota(0, 100'000))
        deep += "(;";
    for ([[maybe_unused]] auto const level: std::views::iota(0, 100'000))
        deep += ";)";
    CHECK(lexAll(deep) == std::vector<Lexed> { { End, "" } });
}

TEST_CASE("Lexer.positions", "[Lexer]")
{
    auto const source = std::string_view { "(module\n  (func))" };
    auto lexer = Lexer::create(source, TextLimits {});
    REQUIRE(lexer.has_value());
    REQUIRE(lexer->next().has_value());
    REQUIRE(lexer->next().has_value());
    auto const paren = lexer->next();
    REQUIRE(paren.has_value());
    CHECK(paren->kind == TokenKind::LeftParen);
    auto const position = positionAt(source, paren->offset);
    CHECK(position.line == 2);
    CHECK(position.column == 3);

    // An error is reported where it is found.
    auto failing = Lexer::create("nop\n  \x01", TextLimits {});
    REQUIRE(failing.has_value());
    REQUIRE(failing->next().has_value());
    auto const error = failing->next();
    REQUIRE_FALSE(error.has_value());
    CHECK(error.error().position.line == 2);
    CHECK(error.error().position.column == 3);
    CHECK(error.error().position.offset == 6);

    // The end stays the end.
    auto ended = Lexer::create("", TextLimits {});
    REQUIRE(ended.has_value());
    CHECK(ended->next()->kind == TokenKind::End);
    CHECK(ended->next()->kind == TokenKind::End);
}

TEST_CASE("Lexer.stringEscapes", "[Lexer]")
{
    auto const decode = [](std::string_view source) {
        return decodeString(source, wholeToken(source, TokenKind::String));
    };
    CHECK(decode("\"\\t\\n\\u{1F600}\\ff\"") == "\t\n\xF0\x9F\x98\x80\xFF");
    CHECK(decode("\"\\r\\\\\\'\\\"\\u{0}\\u{7_F}\\u{80}\\u{800}\\u{10FFFF}λ\"")
          == std::string { "\r\\'\"" } + std::string { "\0", 1 }
                 + "\x7F\xC2\x80\xE0\xA0\x80\xF4\x8F\xBF\xBFλ");
    CHECK(errorOf(decode("\"\\q\"")) == TextErrorCode::IllegalEscape);
    CHECK(errorOf(decode("\"\\u{}\"")) == TextErrorCode::IllegalEscape);
    CHECK(errorOf(decode("\"\\u{D800}\"")) == TextErrorCode::IllegalEscape);
    CHECK(errorOf(decode("\"\\u{110000}\"")) == TextErrorCode::IllegalEscape);
    CHECK(decode("\"\\u{000000000000000000041}\"") == "A");
    CHECK(errorOf(decode("\"\\u{1F600\"")) == TextErrorCode::IllegalEscape);
    CHECK(errorOf(decode("\"\\u1F\"")) == TextErrorCode::IllegalEscape);
    CHECK(errorOf(decode("\"\\f\"")) == TextErrorCode::IllegalEscape);
    CHECK(errorOf(decode("\"\\f")) == TextErrorCode::IllegalEscape);
    CHECK(errorOf(decode("\"\\u")) == TextErrorCode::IllegalEscape);
    CHECK(errorOf(decode("\"\\u{41")) == TextErrorCode::IllegalEscape);
    CHECK(errorOf(decode("\"\\u{_41}\"")) == TextErrorCode::IllegalEscape);
    CHECK(errorOf(decode("\"\\u{41_}\"")) == TextErrorCode::IllegalEscape);
    CHECK(decode("\"\\u{4_1}\"") == "A");
    CHECK(errorOf(decode("\"a\r\"")) == TextErrorCode::UnclosedString);
    CHECK(errorOf(decode("\"\xE1\x80\x41\"")) == TextErrorCode::MalformedUtf8);
    CHECK(errorOf(decode("\"a\x01\"")) == TextErrorCode::IllegalControlInString);
    CHECK(errorOf(decode("\"a\t\"")) == TextErrorCode::IllegalControlInString);
    CHECK(errorOf(decode("\"a\x7f\"")) == TextErrorCode::IllegalControlInString);
    CHECK(errorOf(decode("\"a\x80\"")) == TextErrorCode::MalformedUtf8);
    CHECK(errorOf(decode("\"a\n\"")) == TextErrorCode::UnclosedString);
    CHECK(errorOf(decode("\"a\\")) == TextErrorCode::UnclosedString);
    CHECK(errorOf(decode("\"a")) == TextErrorCode::UnclosedString);

    auto const name = [](std::string_view source) {
        return decodeName(source, wholeToken(source, TokenKind::String));
    };
    CHECK(errorOf(name("\"\\ff\"")) == TextErrorCode::MalformedUtf8);
    CHECK(name("\"\\c3\\a9\"") == "\xC3\xA9");
    CHECK(name("\"\\u{e9}\"") == "\xC3\xA9");
    CHECK(errorOf(name("\"\\q\"")) == TextErrorCode::IllegalEscape);

    // The lexer reports what decoding would.
    CHECK(lexError("\"\\q\"") == TextErrorCode::IllegalEscape);
    CHECK(lexError("\"a\x01\"") == TextErrorCode::IllegalControlInString);
}

TEST_CASE("Lexer.quotedIdentifier", "[Lexer]")
{
    auto const name = [](std::string_view source) {
        return identifierName(source, wholeToken(source, TokenKind::Identifier));
    };
    CHECK(name("$\"a b\"") == "a b");
    // id.wast:31: a quoted identifier is a name, so it must be UTF-8.
    CHECK(errorOf(name("$\"\\ef\"")) == TextErrorCode::MalformedUtf8);
    CHECK(name("$\"\\ef\\98\\9a\"") == "\xEF\x98\x9A");
    CHECK(name("$x") == "x");
    CHECK(name("$\"\\41\"") == "A");
    CHECK(errorOf(name("$\"\"")) == TextErrorCode::EmptyIdentifier);
    CHECK(errorOf(name("$")) == TextErrorCode::EmptyIdentifier);

    CHECK(lexError("$") == TextErrorCode::EmptyIdentifier);
    CHECK(lexError("$\"\"") == TextErrorCode::EmptyIdentifier);
    // A malformed string after $ leaves the $ alone, as the reference lexer reads it.
    CHECK(lexError("$\"a\n\"") == TextErrorCode::EmptyIdentifier);
    CHECK(lexError("(@ x)") == TextErrorCode::EmptyAnnotationId);
    CHECK(lexError("(@)") == TextErrorCode::EmptyAnnotationId);
    CHECK(lexError("(@") == TextErrorCode::EmptyAnnotationId);
    CHECK(lexError("(@\"\")") == TextErrorCode::EmptyAnnotationId);
    CHECK(lexError("(@\"\n\")") == TextErrorCode::EmptyAnnotationId);
    CHECK(lexError("(@\"\\ef\")") == TextErrorCode::MalformedUtf8);
}

TEST_CASE("Lexer.misplacedCharacters", "[Lexer]")
{
    CHECK(lexError("\x7f") == TextErrorCode::MisplacedControlCharacter);
    CHECK(lexError("\x01") == TextErrorCode::MisplacedControlCharacter);
    CHECK(lexError("λ") == TextErrorCode::MisplacedUnicodeCharacter);
    CHECK(lexError("\xF0\x9F\x98\x80") == TextErrorCode::MisplacedUnicodeCharacter);
    CHECK(lexError("\x80") == TextErrorCode::MalformedUtf8);
    CHECK(lexError("\xC0\x80") == TextErrorCode::MalformedUtf8);
    CHECK(lexError("\xE0\x80\x80") == TextErrorCode::MalformedUtf8);
    CHECK(lexError("\xED\xA0\x80") == TextErrorCode::MalformedUtf8);
    CHECK(lexError("\xF4\x90\x80\x80") == TextErrorCode::MalformedUtf8);
    CHECK(lexError("\xE1\x80") == TextErrorCode::MalformedUtf8);
    CHECK(lexError("(@x \x7f)") == TextErrorCode::IllegalCharacter);
    CHECK(lexError("(@x \x01)") == TextErrorCode::IllegalCharacter);
    CHECK(lexError("(@x λ)") == TextErrorCode::IllegalCharacter);
    CHECK(lexError("(@x \x80)") == TextErrorCode::MalformedUtf8);
    CHECK(lexError("\"") == TextErrorCode::UnclosedString);
    CHECK(lexError("(@x ") == TextErrorCode::UnclosedAnnotation);
    CHECK(lexError("(@x ()") == TextErrorCode::UnclosedAnnotation);
}

TEST_CASE("Lexer.sourceTooLarge", "[Lexer]")
{
    auto const lexer = Lexer::create("(module)", TextLimits { .maxSourceBytes = 4 });
    REQUIRE_FALSE(lexer.has_value());
    CHECK(lexer.error().code == TextErrorCode::SourceTooLarge);
    CHECK(Lexer::create("(module)", TextLimits { .maxSourceBytes = 8 }).has_value());
}

TEST_CASE("Lexer.malformedUtf8InStringsAndComments", "[Lexer]")
{
    // Overlong, surrogate and above U+10FFFF, as bytes in a string, a line comment and a block comment.
    for (auto const sequence: { std::string_view { "\xC0\x80" },
                                std::string_view { "\xE0\x80\x80" },
                                std::string_view { "\xED\xA0\x80" },
                                std::string_view { "\xF4\x90\x80\x80" } })
    {
        auto const inString = "\"" + std::string { sequence } + "\"";
        CHECK(lexError(inString) == TextErrorCode::MalformedUtf8);
        CHECK(lexError(";; " + std::string { sequence } + "\n") == TextErrorCode::MalformedUtf8);
        CHECK(lexError("(; " + std::string { sequence } + " ;)") == TextErrorCode::MalformedUtf8);
    }
}

TEST_CASE("Lexer.messages", "[Lexer]")
{
    // Every error the lexer reports, with the reference interpreter's message for it.
    struct Case
    {
        std::string_view source;
        std::string_view message;
    };
    auto const cases = std::to_array<Case>({
        { .source = "(@a \x01)", .message = "illegal character" },
        { .source = "\x01", .message = "misplaced control character" },
        { .source = "λ", .message = "misplaced unicode character" },
        { .source = "\"a\x01\"", .message = "illegal control character in string literal" },
        { .source = R"("\q")", .message = "illegal escape" },
        { .source = "\"a", .message = "unclosed string literal" },
        { .source = "(; a", .message = "unclosed comment" },
        { .source = "\x80", .message = "malformed UTF-8 encoding" },
        { .source = "$", .message = "empty identifier" },
        { .source = "(@ x)", .message = "empty annotation id" },
        { .source = "(@a", .message = "unclosed annotation" },
    });
    for (auto const& [source, message]: cases)
    {
        auto lexer = Lexer::create(source, TextLimits {});
        REQUIRE(lexer.has_value());
        auto result = lexer->next();
        while (result && result->kind != TokenKind::End)
            result = lexer->next();
        REQUIRE_FALSE(result.has_value());
        CHECK(specMessage(result.error()) == message);
    }
    auto const tooLarge = Lexer::create("(module)", TextLimits { .maxSourceBytes = 1 });
    REQUIRE_FALSE(tooLarge.has_value());
    CHECK(specMessage(tooLarge.error()) == "source too large");
}
