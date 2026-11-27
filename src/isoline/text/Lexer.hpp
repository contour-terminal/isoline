// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include <isoline/text/TextError.hpp>
#include <isoline/text/TextLimits.hpp>

namespace isoline::text
{

/// What a token of the text format is (spec §6.2.2).
enum class TokenKind : uint8_t
{
    LeftParen,       ///< `(`.
    RightParen,      ///< `)`.
    AnnotationStart, ///< `(@` and the annotation's id, plain or quoted.
    Keyword,         ///< A keyword or a number: the spec's grammar lexes numbers as keywords.
    Identifier,      ///< `$` and a name, plain or quoted.
    String,          ///< A string literal, quotes included.
    Reserved,        ///< Any other token; the reference reports it as "unknown operator" where it is used.
    End,             ///< The end of the source.
};

/// One token: its kind and where its text is in the source. Tokens keep offsets only; positionAt()
/// turns one into a line and column.
struct Token
{
    TokenKind kind = TokenKind::End; ///< What the token is.
    uint32_t offset = 0;             ///< The byte offset of its first character in the source.
    uint32_t length = 0;             ///< Its length in bytes.
};

/// The text of a token.
/// @param source The source the token was read from.
/// @param token The token.
/// @return Its bytes in @p source.
[[nodiscard]] inline std::string_view textOf(std::string_view source, Token token) noexcept
{
    return source.substr(token.offset, token.length);
}

/// Splits a source text into tokens, skipping white space and comments (spec §6.2).
///
/// It keeps no state but its place in the source and how deep inside an annotation that place is, so
/// nested block comments and annotations are counted, never recursed into. Inside an annotation every
/// token is valid, and the characters it may not hold are reported as "illegal character", as the
/// reference lexer does.
class Lexer
{
  public:
    /// Builds a lexer of @p source.
    /// @param source The text to lex; it must outlive the lexer.
    /// @param limits The caps on what the source may cost; of them, only the source size concerns
    ///               tokens, so the lexer checks it here and keeps none of them.
    /// @return The lexer, or SourceTooLarge when @p source exceeds TextLimits::maxSourceBytes or
    ///         2^32 - 1 bytes.
    [[nodiscard]] static std::expected<Lexer, TextError> create(std::string_view source, TextLimits limits);

    /// Reads the next token.
    /// @return The token, End once the source is read (and on every call after), or the error at the
    ///         first character that is not part of a valid token.
    [[nodiscard]] std::expected<Token, TextError> next();

  private:
    explicit Lexer(std::string_view source) noexcept: _source { source } {}

    /// Whether the source continues with @p text at the current offset.
    [[nodiscard]] bool at(std::string_view text) const noexcept;

    /// Skips a line comment, from its `;;` up to the end of its line.
    /// @return Nothing, or MalformedUtf8 at a byte of it that is no UTF-8.
    [[nodiscard]] std::expected<void, TextError> skipLineComment();

    /// Skips a block comment and the comments nested in it, from its `(;` to its `;)`.
    /// @return Nothing, or UnclosedComment or MalformedUtf8.
    [[nodiscard]] std::expected<void, TextError> skipBlockComment();

    /// Reads `(@` and the annotation id after it.
    /// @return The AnnotationStart token, or the error of a missing or malformed id.
    [[nodiscard]] std::expected<Token, TextError> readAnnotationStart();

    /// Reads a run of identifier characters and strings, and classifies it.
    /// @return The Keyword, Identifier, String or Reserved token, or the error of a malformed one.
    [[nodiscard]] std::expected<Token, TextError> readRun();

    /// The error at a byte offset of the source.
    /// @param code What went wrong.
    /// @param offset Where.
    /// @return The error, with its position computed.
    [[nodiscard]] TextError errorAt(TextErrorCode code, std::size_t offset) const;

    std::string_view _source;         ///< The text being lexed.
    std::size_t _offset = 0;          ///< The offset of the next byte to read.
    std::size_t _annotationDepth = 0; ///< How many parentheses inside an annotation the offset is; 0 outside.
    std::size_t _annotationStart = 0; ///< The offset of the outermost open annotation.
};

/// The bytes a string literal denotes, its escapes resolved (spec §6.3.3).
/// @param source The source the token was read from.
/// @param token A String token, or any range of @p source starting at a `"`.
/// @return The bytes, or the error of a malformed literal (IllegalEscape, IllegalControlInString,
///         UnclosedString or MalformedUtf8).
[[nodiscard]] std::expected<std::string, TextError> decodeString(std::string_view source, Token token);

/// The name a string literal denotes: its bytes, which must be valid UTF-8 (spec §6.3.4).
/// @param source The source the token was read from.
/// @param token A String token.
/// @return The name, or the error of a malformed literal, MalformedUtf8 when its bytes are not UTF-8.
[[nodiscard]] std::expected<std::string, TextError> decodeName(std::string_view source, Token token);

/// The name an identifier denotes: the characters after `$`, or the bytes of the string after it.
/// @param source The source the token was read from.
/// @param token An Identifier token.
/// @return The name, or EmptyIdentifier when it is empty, MalformedUtf8 when a quoted one is not
///         UTF-8, or the error of a malformed string.
[[nodiscard]] std::expected<std::string, TextError> identifierName(std::string_view source, Token token);

/// Whether a Keyword token's text is a number: the spec's `nat`, `int` or `float` (spec §6.3.1, §6.3.2),
/// as the lexer tells numbers from keywords. A number's value may still be out of every type's range.
/// @param text The token's text.
/// @return Whether it is spelled as a number.
[[nodiscard]] bool isNumberLiteral(std::string_view text);

/// Whether a byte is an identifier character (spec §6.2.2's idchar): a letter, a digit, `_`, or one of
/// the symbols ``+-*/\^~=<>!?@#$%&|:`.'``. A name made of them only is written `$name`; any other,
/// `$"name"`.
/// @param character The byte.
/// @return Whether it may appear in an unquoted identifier, keyword or number.
[[nodiscard]] bool isIdentifierCharacter(char character) noexcept;

} // namespace isoline::text
