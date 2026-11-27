// SPDX-License-Identifier: Apache-2.0
#include <isoline/text/Lexer.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>

#include <isoline/text/Digits.hpp>
#include <isoline/text/TextError.hpp>
#include <isoline/text/TextLimits.hpp>

namespace isoline::text
{

namespace
{

    /// What a byte is to the lexer, at the start of a token (spec §6.2).
    enum class CharacterClass : uint8_t
    {
        Control,     ///< A control character, or DEL: misplaced outside a string.
        Space,       ///< Space, tab, LF or CR.
        IdChar,      ///< A letter, digit, `_` or symbol: the characters of keywords, numbers and identifiers.
        Quote,       ///< `"`, which opens a string.
        LeftParen,   ///< `(`.
        RightParen,  ///< `)`.
        Punctuation, ///< `,` `;` `[` `]` `{` `}`, each a reserved token of its own.
        NonAscii,    ///< A byte of 0x80 or above, which starts a UTF-8 sequence or is malformed.
    };

    /// The symbols that are identifier characters besides letters, digits and `_` (spec §6.2.2).
    constexpr auto IdSymbols = std::string_view { "+-*/\\^~=<>!?@#$%&|:`.'" };

    /// The bytes that are reserved tokens of their own.
    constexpr auto PunctuationBytes = std::string_view { ",;[]{}" };

    /// The class of every byte, indexed by its value.
    constexpr auto CharacterClasses = [] {
        auto table = std::array<CharacterClass, 256> {};
        for (auto const byte: std::views::iota(0, 256))
        {
            auto const character = static_cast<char>(byte);
            auto const isLetter = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
            auto const isDigit = byte >= '0' && byte <= '9';
            auto& entry = table[static_cast<std::size_t>(byte)];
            if (byte >= 0x80)
                entry = CharacterClass::NonAscii;
            else if (byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r')
                entry = CharacterClass::Space;
            else if (isLetter || isDigit || byte == '_' || IdSymbols.contains(character))
                entry = CharacterClass::IdChar;
            else if (byte == '"')
                entry = CharacterClass::Quote;
            else if (byte == '(')
                entry = CharacterClass::LeftParen;
            else if (byte == ')')
                entry = CharacterClass::RightParen;
            else if (PunctuationBytes.contains(character))
                entry = CharacterClass::Punctuation;
            else
                entry = CharacterClass::Control;
        }
        return table;
    }();

    /// The class of a byte.
    constexpr CharacterClass classOf(char character) noexcept
    {
        return CharacterClasses[static_cast<uint8_t>(character)];
    }

    /// One row of Utf8Sequences: the lead bytes of a UTF-8 sequence length, and the range its second
    /// byte must fall in to be neither overlong nor a surrogate nor above U+10FFFF.
    struct Utf8SequenceRow
    {
        uint8_t leadFirst;   ///< The lowest lead byte of the row.
        uint8_t leadLast;    ///< The highest lead byte of the row.
        uint8_t secondFirst; ///< The lowest second byte.
        uint8_t secondLast;  ///< The highest second byte.
        uint8_t length;      ///< The length of the sequence in bytes.
    };

    /// The well-formed multi-byte UTF-8 sequences (Unicode table 3-7, the reference lexer's `utf8enc`).
    /// Every byte after the second is a continuation byte, 0x80 to 0xBF.
    constexpr auto Utf8Sequences = std::to_array<Utf8SequenceRow>({
        { .leadFirst = 0xC2, .leadLast = 0xDF, .secondFirst = 0x80, .secondLast = 0xBF, .length = 2 },
        { .leadFirst = 0xE0, .leadLast = 0xE0, .secondFirst = 0xA0, .secondLast = 0xBF, .length = 3 },
        { .leadFirst = 0xE1, .leadLast = 0xEC, .secondFirst = 0x80, .secondLast = 0xBF, .length = 3 },
        { .leadFirst = 0xED, .leadLast = 0xED, .secondFirst = 0x80, .secondLast = 0x9F, .length = 3 },
        { .leadFirst = 0xEE, .leadLast = 0xEF, .secondFirst = 0x80, .secondLast = 0xBF, .length = 3 },
        { .leadFirst = 0xF0, .leadLast = 0xF0, .secondFirst = 0x90, .secondLast = 0xBF, .length = 4 },
        { .leadFirst = 0xF1, .leadLast = 0xF3, .secondFirst = 0x80, .secondLast = 0xBF, .length = 4 },
        { .leadFirst = 0xF4, .leadLast = 0xF4, .secondFirst = 0x80, .secondLast = 0x8F, .length = 4 },
    });

    /// The length of the UTF-8 sequence starting at a non-ASCII byte.
    /// @param text The text.
    /// @param offset The offset of a byte of 0x80 or above.
    /// @return The length of the well-formed sequence there, or 0 when it is malformed.
    constexpr std::size_t utf8SequenceLength(std::string_view text, std::size_t offset) noexcept
    {
        auto const byteAt = [&](std::size_t index) {
            return static_cast<uint8_t>(text[index]);
        };
        auto const lead = byteAt(offset);
        auto const row = std::ranges::find_if(Utf8Sequences, [lead](Utf8SequenceRow const& entry) {
            return lead >= entry.leadFirst && lead <= entry.leadLast;
        });
        if (row == Utf8Sequences.end() || offset + row->length > text.size())
            return 0;
        auto const second = byteAt(offset + 1);
        if (second < row->secondFirst || second > row->secondLast)
            return 0;
        auto const continuationsValid =
            std::ranges::all_of(std::views::iota(offset + 2, offset + row->length),
                                [&](std::size_t index) { return (byteAt(index) & 0xC0U) == 0x80U; });
        return continuationsValid ? row->length : 0;
    }

    /// Whether text is well-formed UTF-8.
    constexpr bool isUtf8(std::string_view text) noexcept
    {
        auto offset = std::size_t { 0 };
        while (offset < text.size())
        {
            if (static_cast<uint8_t>(text[offset]) < 0x80)
            {
                ++offset;
                continue;
            }
            auto const length = utf8SequenceLength(text, offset);
            if (length == 0)
                return false;
            offset += length;
        }
        return true;
    }

    /// Appends the UTF-8 encoding of a Unicode scalar value.
    void appendUtf8(std::string& output, uint32_t scalar)
    {
        auto const append = [&](uint32_t byte) {
            output.push_back(static_cast<char>(byte));
        };
        if (scalar < 0x80)
            append(scalar);
        else if (scalar < 0x800)
        {
            append(0xC0U | (scalar >> 6U));
            append(0x80U | (scalar & 0x3FU));
        }
        else if (scalar < 0x10000)
        {
            append(0xE0U | (scalar >> 12U));
            append(0x80U | ((scalar >> 6U) & 0x3FU));
            append(0x80U | (scalar & 0x3FU));
        }
        else
        {
            append(0xF0U | (scalar >> 18U));
            append(0x80U | ((scalar >> 12U) & 0x3FU));
            append(0x80U | ((scalar >> 6U) & 0x3FU));
            append(0x80U | (scalar & 0x3FU));
        }
    }

    /// Reads digits without looking at their values.
    /// @return The offset after the digits, or nothing when none start at @p offset.
    constexpr std::optional<std::size_t> skipDigits(std::string_view text, std::size_t offset, Radix radix)
    {
        return readDigits(text, offset, radix, [](uint32_t) {});
    }

    /// Whether digits of @p radix run from @p offset to the end of @p text.
    constexpr bool digitsToEnd(std::string_view text, std::size_t offset, Radix radix)
    {
        return skipDigits(text, offset, radix) == text.size();
    }

    /// Whether text is the spec's `nat`: decimal or `0x` hexadecimal digits.
    constexpr bool isNat(std::string_view text)
    {
        auto const radix = text.starts_with("0x") ? Radix::Hexadecimal : Radix::Decimal;
        return digitsToEnd(text, radix == Radix::Hexadecimal ? 2U : 0U, radix);
    }

    /// Whether text is a number: the spec's `nat`, `int` or `float` (spec §6.3.1, §6.3.2).
    constexpr bool isNumber(std::string_view text)
    {
        if (text.starts_with('+') || text.starts_with('-'))
            text.remove_prefix(1);
        if (text == "inf" || text == "nan")
            return true;
        if (text.starts_with("nan:0x"))
            return digitsToEnd(text, 6, Radix::Hexadecimal);
        auto const radix = text.starts_with("0x") ? Radix::Hexadecimal : Radix::Decimal;
        auto const integral = skipDigits(text, radix == Radix::Hexadecimal ? 2U : 0U, radix);
        if (!integral)
            return false;
        auto offset = *integral;
        if (offset < text.size() && text[offset] == '.')
        {
            ++offset;
            // The fraction's digits are optional.
            offset = skipDigits(text, offset, radix).value_or(offset);
        }
        auto const exponentMarkers =
            radix == Radix::Hexadecimal ? std::string_view { "pP" } : std::string_view { "eE" };
        if (offset < text.size() && exponentMarkers.contains(text[offset]))
        {
            ++offset;
            if (offset < text.size() && (text[offset] == '+' || text[offset] == '-'))
                ++offset;
            return digitsToEnd(text, offset, Radix::Decimal);
        }
        return offset == text.size();
    }

    /// Whether text is a keyword: a lowercase letter, then one or more letters, digits, `_`, `.` or `:`
    /// (the reference lexer's `keyword`).
    constexpr bool isKeyword(std::string_view text) noexcept
    {
        auto const isKeywordCharacter = [](char character) {
            return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
                   || (character >= '0' && character <= '9') || character == '_' || character == '.'
                   || character == ':';
        };
        return text.size() >= 2 && text.front() >= 'a' && text.front() <= 'z'
               && std::ranges::all_of(text.substr(1), isKeywordCharacter);
    }

    /// The memory-argument keywords, each followed by a `nat` (spec §6.5.5).
    constexpr auto MemoryArgumentPrefixes = std::to_array<std::string_view>({ "offset=", "align=" });

    /// Whether text is a memory-argument keyword: `offset=` or `align=` and a `nat`.
    constexpr bool isMemoryArgument(std::string_view text)
    {
        return std::ranges::any_of(MemoryArgumentPrefixes, [text](std::string_view prefix) {
            return text.starts_with(prefix) && isNat(text.substr(prefix.size()));
        });
    }

    /// One simple string escape: the character after the backslash and the byte it denotes.
    struct SimpleEscape
    {
        char name;  ///< The character after the backslash.
        char value; ///< The byte it denotes.
    };

    /// The escapes of one character (spec §6.3.3).
    constexpr auto SimpleEscapes = std::to_array<SimpleEscape>({
        { .name = 'n', .value = '\n' },
        { .name = 'r', .value = '\r' },
        { .name = 't', .value = '\t' },
        { .name = '\\', .value = '\\' },
        { .name = '\'', .value = '\'' },
        { .name = '"', .value = '"' },
    });

    /// The largest Unicode scalar value.
    constexpr auto MaxScalar = uint32_t { 0x10'FFFF };

    /// Whether a code point is a Unicode scalar value: no surrogate, and at most U+10FFFF.
    constexpr bool isScalar(uint32_t codePoint) noexcept
    {
        return codePoint <= MaxScalar && (codePoint < 0xD800 || codePoint >= 0xE000);
    }

    /// The error at a byte offset of a source.
    TextError errorAt(std::string_view source, TextErrorCode code, std::size_t offset)
    {
        return TextError { code, positionAt(source, offset) };
    }

    /// Reads the escape `\u{…}` whose `u` is at an offset.
    /// @param source The source.
    /// @param offset The offset of the `u`; advanced past the `}` on success.
    /// @return The scalar value, or nothing when the escape is malformed or names no scalar value.
    std::optional<uint32_t> readUnicodeEscape(std::string_view source, std::size_t& offset)
    {
        auto position = offset + 1;
        if (position >= source.size() || source[position] != '{')
            return std::nullopt;
        ++position;
        // Saturates above MaxScalar, so that any number of digits is read without overflow.
        auto value = uint32_t { 0 };
        auto const accumulate = [&](uint32_t digit) {
            value = std::min((value * 16) + digit, MaxScalar + 1);
        };
        auto const digitsEnd = readDigits(source, position, Radix::Hexadecimal, accumulate);
        if (!digitsEnd || *digitsEnd >= source.size() || source[*digitsEnd] != '}' || !isScalar(value))
            return std::nullopt;
        offset = *digitsEnd + 1;
        return value;
    }

    /// Reads a string literal: validates it, and decodes it when asked to.
    /// @param source The source.
    /// @param offset The offset of the opening `"`.
    /// @param output Receives the bytes the literal denotes, or nothing when null.
    /// @return The offset after the closing `"`, or the error of a malformed literal.
    std::expected<std::size_t, TextError> scanString(std::string_view source,
                                                     std::size_t offset,
                                                     std::string* output)
    {
        auto const append = [output](std::string_view bytes) {
            if (output)
                output->append(bytes);
        };
        auto position = offset + 1;
        while (true)
        {
            if (position >= source.size())
                return std::unexpected { errorAt(source, TextErrorCode::UnclosedString, offset) };
            auto const character = source[position];
            auto const byte = static_cast<uint8_t>(character);
            if (character == '"')
                return position + 1;
            if (character == '\n' || character == '\r')
                return std::unexpected { errorAt(source, TextErrorCode::UnclosedString, offset) };
            if (character == '\\')
            {
                if (position + 1 >= source.size())
                    return std::unexpected { errorAt(source, TextErrorCode::UnclosedString, offset) };
                auto const name = source[position + 1];
                auto const simple = std::ranges::find_if(
                    SimpleEscapes, [name](SimpleEscape const& escape) { return escape.name == name; });
                if (simple != SimpleEscapes.end())
                {
                    append(std::string_view { &simple->value, 1 });
                    position += 2;
                    continue;
                }
                if (name == 'u')
                {
                    auto escapeEnd = position + 1;
                    auto const scalar = readUnicodeEscape(source, escapeEnd);
                    if (!scalar)
                        return std::unexpected { errorAt(source, TextErrorCode::IllegalEscape, position) };
                    if (output)
                        appendUtf8(*output, *scalar);
                    position = escapeEnd;
                    continue;
                }
                auto const high = digitValue(name, Radix::Hexadecimal);
                auto const low = position + 2 < source.size()
                                     ? digitValue(source[position + 2], Radix::Hexadecimal)
                                     : std::nullopt;
                if (!high || !low)
                    return std::unexpected { errorAt(source, TextErrorCode::IllegalEscape, position) };
                auto const value = static_cast<char>((*high << 4U) | *low);
                append(std::string_view { &value, 1 });
                position += 3;
                continue;
            }
            if (byte < 0x20 || byte == 0x7F)
                return std::unexpected { errorAt(source, TextErrorCode::IllegalControlInString, position) };
            auto const length = byte < 0x80 ? std::size_t { 1 } : utf8SequenceLength(source, position);
            if (length == 0)
                return std::unexpected { errorAt(source, TextErrorCode::MalformedUtf8, position) };
            append(source.substr(position, length));
            position += length;
        }
    }

    /// Skips the UTF-8 characters of a comment up to a stop condition.
    /// @param source The source.
    /// @param offset The offset to start at; advanced to the first byte that @p stop accepts, or the end.
    /// @param stop Whether the comment's scan stops at an offset, before looking at its character.
    /// @return Nothing, or MalformedUtf8 at a byte that is no UTF-8.
    std::expected<void, TextError> skipCommentText(std::string_view source, std::size_t& offset, auto&& stop)
    {
        while (offset < source.size() && !stop(offset))
        {
            auto const length = static_cast<uint8_t>(source[offset]) < 0x80
                                    ? std::size_t { 1 }
                                    : utf8SequenceLength(source, offset);
            if (length == 0)
                return std::unexpected { errorAt(source, TextErrorCode::MalformedUtf8, offset) };
            offset += length;
        }
        return {};
    }

} // namespace

std::expected<Lexer, TextError> Lexer::create(std::string_view source, TextLimits limits)
{
    // Tokens keep 32-bit offsets, so no limit admits a source of 2^32 bytes or more.
    auto const maxBytes = std::min<std::size_t>(limits.maxSourceBytes, std::numeric_limits<uint32_t>::max());
    if (source.size() > maxBytes)
        return std::unexpected { TextError { TextErrorCode::SourceTooLarge, SourcePosition {} } };
    return Lexer { source };
}

TextError Lexer::errorAt(TextErrorCode code, std::size_t offset) const
{
    return text::errorAt(_source, code, offset);
}

bool Lexer::at(std::string_view text) const noexcept
{
    return _source.substr(_offset).starts_with(text);
}

std::expected<Token, TextError> Lexer::next()
{
    auto const single = [this](TokenKind kind) {
        auto const token = Token { .kind = kind, .offset = static_cast<uint32_t>(_offset), .length = 1 };
        ++_offset;
        return token;
    };
    while (_offset < _source.size())
    {
        auto const insideAnnotation = _annotationDepth > 0;
        switch (classOf(_source[_offset]))
        {
            case CharacterClass::Space: ++_offset; break;
            case CharacterClass::LeftParen:
                if (at("(;"))
                {
                    if (auto skipped = skipBlockComment(); !skipped)
                        return std::unexpected { skipped.error() };
                    break;
                }
                if (at("(@"))
                    return readAnnotationStart();
                if (insideAnnotation)
                    ++_annotationDepth;
                return single(TokenKind::LeftParen);
            case CharacterClass::RightParen:
                if (insideAnnotation)
                    --_annotationDepth;
                return single(TokenKind::RightParen);
            case CharacterClass::Punctuation:
                if (at(";;"))
                {
                    if (auto skipped = skipLineComment(); !skipped)
                        return std::unexpected { skipped.error() };
                    break;
                }
                return single(TokenKind::Reserved);
            case CharacterClass::IdChar:
            case CharacterClass::Quote: return readRun();
            case CharacterClass::NonAscii:
                if (utf8SequenceLength(_source, _offset) == 0)
                    return std::unexpected { errorAt(TextErrorCode::MalformedUtf8, _offset) };
                return std::unexpected { errorAt(insideAnnotation ? TextErrorCode::IllegalCharacter
                                                                  : TextErrorCode::MisplacedUnicodeCharacter,
                                                 _offset) };
            case CharacterClass::Control:
                return std::unexpected { errorAt(insideAnnotation ? TextErrorCode::IllegalCharacter
                                                                  : TextErrorCode::MisplacedControlCharacter,
                                                 _offset) };
        }
    }
    if (_annotationDepth > 0)
        return std::unexpected { errorAt(TextErrorCode::UnclosedAnnotation, _annotationStart) };
    return Token { .kind = TokenKind::End, .offset = static_cast<uint32_t>(_offset), .length = 0 };
}

std::expected<void, TextError> Lexer::skipLineComment()
{
    _offset += 2;
    return skipCommentText(_source, _offset, [this](std::size_t offset) {
        return _source[offset] == '\n' || _source[offset] == '\r';
    });
}

std::expected<void, TextError> Lexer::skipBlockComment()
{
    // Block comments nest; the depth is counted, so any depth costs no stack.
    auto const start = _offset;
    auto depth = std::size_t { 1 };
    _offset += 2;
    auto const delimiter = [this](std::size_t offset) {
        auto const rest = _source.substr(offset);
        return rest.starts_with("(;") || rest.starts_with(";)");
    };
    while (depth > 0)
    {
        if (auto skipped = skipCommentText(_source, _offset, delimiter); !skipped)
            return skipped;
        if (_offset >= _source.size())
            return std::unexpected { errorAt(TextErrorCode::UnclosedComment, start) };
        depth = at("(;") ? depth + 1 : depth - 1;
        _offset += 2;
    }
    return {};
}

std::expected<Token, TextError> Lexer::readAnnotationStart()
{
    auto const start = _offset;
    auto end = start + 2;
    // Outside an annotation, `(@` needs an id. Inside one, the reference's `annot` rule has no such
    // error: `(@` without an id is a plain `(`, and the `@` the start of the next token.
    auto const withoutId = [&]() -> std::expected<Token, TextError> {
        if (_annotationDepth == 0)
            return std::unexpected { errorAt(TextErrorCode::EmptyAnnotationId, start) };
        ++_annotationDepth;
        _offset = start + 1;
        return Token { .kind = TokenKind::LeftParen, .offset = static_cast<uint32_t>(start), .length = 1 };
    };
    if (end < _source.size() && classOf(_source[end]) == CharacterClass::IdChar)
    {
        while (end < _source.size() && classOf(_source[end]) == CharacterClass::IdChar)
            ++end;
    }
    else if (end < _source.size() && classOf(_source[end]) == CharacterClass::Quote)
    {
        // A malformed string leaves `(@` without an id; a well-formed one must be a non-empty name,
        // inside an annotation too (the reference's `annot_id`).
        auto name = std::string {};
        auto const stringEnd = scanString(_source, end, &name);
        if (!stringEnd)
            return withoutId();
        if (name.empty())
            return std::unexpected { errorAt(TextErrorCode::EmptyAnnotationId, start) };
        if (!isUtf8(name))
            return std::unexpected { errorAt(TextErrorCode::MalformedUtf8, start) };
        end = *stringEnd;
    }
    else
        return withoutId();

    if (_annotationDepth == 0)
        _annotationStart = start;
    ++_annotationDepth;
    _offset = end;
    return Token { .kind = TokenKind::AnnotationStart,
                   .offset = static_cast<uint32_t>(start),
                   .length = static_cast<uint32_t>(end - start) };
}

std::expected<Token, TextError> Lexer::readRun()
{
    // The reference lexer matches the longest token, so a run of identifier characters and strings is
    // one token, which is reserved unless it is exactly a keyword, number, identifier or string.
    auto const start = _offset;
    auto end = start;
    auto strings = std::size_t { 0 };
    auto firstStringStart = std::size_t { 0 };
    auto firstStringEnd = std::size_t { 0 };
    while (end < _source.size())
    {
        auto const characterClass = classOf(_source[end]);
        if (characterClass == CharacterClass::IdChar)
        {
            ++end;
            continue;
        }
        if (characterClass != CharacterClass::Quote)
            break;
        auto const stringEnd = scanString(_source, end, nullptr);
        if (!stringEnd)
        {
            // A malformed string ends the run before it, or is the error when it starts the token.
            if (end == start)
                return std::unexpected { stringEnd.error() };
            break;
        }
        if (strings++ == 0)
        {
            firstStringStart = end;
            firstStringEnd = *stringEnd;
        }
        end = *stringEnd;
    }
    _offset = end;

    auto const text = _source.substr(start, end - start);
    auto const isOneString = [&](std::size_t stringStart) {
        return strings == 1 && firstStringStart == stringStart && firstStringEnd == end;
    };
    auto const token = [&](TokenKind kind) {
        return Token { .kind = kind,
                       .offset = static_cast<uint32_t>(start),
                       .length = static_cast<uint32_t>(text.size()) };
    };
    if (text.front() == '$')
    {
        // `$` alone, or `$""`, names nothing; the reference reports it at once.
        if (text.size() == 1 || (isOneString(start + 1) && text.size() == 3))
            return std::unexpected { errorAt(TextErrorCode::EmptyIdentifier, start) };
        if (strings == 0 || isOneString(start + 1))
            return token(TokenKind::Identifier);
        return token(TokenKind::Reserved);
    }
    if (isOneString(start))
        return token(TokenKind::String);
    if (strings == 0 && (isNumber(text) || isKeyword(text) || isMemoryArgument(text)))
        return token(TokenKind::Keyword);
    return token(TokenKind::Reserved);
}

std::expected<std::string, TextError> decodeString(std::string_view source, Token token)
{
    Require(token.length > 0 && token.offset + token.length <= source.size() && source[token.offset] == '"');
    auto bytes = std::string {};
    return scanString(source.substr(0, token.offset + token.length), token.offset, &bytes)
        .transform([&](std::size_t) { return std::move(bytes); });
}

std::expected<std::string, TextError> decodeName(std::string_view source, Token token)
{
    return decodeString(source, token)
        .and_then([&](std::string name) -> std::expected<std::string, TextError> {
            if (!isUtf8(name))
                return std::unexpected { errorAt(source, TextErrorCode::MalformedUtf8, token.offset) };
            return name;
        });
}

std::expected<std::string, TextError> identifierName(std::string_view source, Token token)
{
    Require(token.length > 0 && token.offset + token.length <= source.size() && source[token.offset] == '$');
    auto const text = textOf(source, token).substr(1);
    // Built only when reported: a position costs a scan of the source up to it.
    auto const empty = [&] {
        return std::unexpected { errorAt(source, TextErrorCode::EmptyIdentifier, token.offset) };
    };
    if (text.empty())
        return empty();
    if (text.front() != '"')
        return std::string { text };
    auto const quoted =
        Token { .kind = TokenKind::String, .offset = token.offset + 1, .length = token.length - 1 };
    // A quoted identifier is a name, so it must be UTF-8 (spec: id ::= '$' name).
    return decodeName(source, quoted)
        .and_then([&](std::string name) -> std::expected<std::string, TextError> {
            if (name.empty())
                return empty();
            return name;
        });
}

bool isNumberLiteral(std::string_view text)
{
    return isNumber(text);
}

bool isIdentifierCharacter(char character) noexcept
{
    return classOf(character) == CharacterClass::IdChar;
}

} // namespace isoline::text
