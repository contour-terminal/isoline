// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <isoline/meta/EnumRows.hpp>

namespace isoline::text
{

/// Which assertion of a spec script a text error answers: the reference interpreter separates a
/// custom annotation's syntax errors from all others.
enum class TextErrorStage : uint8_t
{
    Syntax, ///< Malformed text, as `assert_malformed` expects.
    Custom, ///< A malformed custom annotation, as `assert_malformed_custom` expects.
};

/// Where a text error's message puts its subject, when it has one.
enum class TextErrorDetail : uint8_t
{
    None,   ///< Nowhere: the message is the row's text.
    Suffix, ///< After the row's text, separated by a space: "unknown operator foo".
    Prefix, ///< Before the row's text, separated by a space: "i8 constant out of range".
};

/// Why reading text failed.
///
/// The enumerators are the rows of TextErrorTable, in the same order; adding an error is adding an
/// enumerator here and a row there.
enum class TextErrorCode : uint8_t
{
    IllegalCharacter,
    MisplacedControlCharacter,
    MisplacedUnicodeCharacter,
    IllegalControlInString,
    IllegalEscape,
    UnclosedString,
    UnclosedComment,
    MalformedUtf8,
    EmptyIdentifier,
    EmptyAnnotationId,
    UnclosedAnnotation,
    UnexpectedToken,
    AlignmentNotPowerOfTwo,
    InlineFunctionType,
    UnknownLabel,
    MismatchingLabel,
    MultipleStart,
    WrongLaneCount,
    InvalidLaneLength,
    IdentifierNotAllowed,
    InvalidCustomPageSize,
    NestingLimitExceeded,
    SourceTooLarge,
    OutputTooLarge,
    NumberTooLong,
    UnsupportedComponent,
    UnknownOperator,
    UnknownIdentifier,
    DuplicateIdentifier,
    ImportAfterDefinition,
    ConstantOutOfRange,
    CustomMissingName,
    CustomMalformedUtf8,
    CustomUnexpectedToken,
    CustomMalformedSectionKind,
    CustomMalformedPlacement,
    MisplacedAnnotation,
    MisplacedCustom,
    MisplacedName,
    NameMultipleModule,
    NameMultipleFunctionNames,
    NameMultipleLocalNames,
    NameStringExpected,
    NameUnexpectedToken,
    NameMalformedUtf8,
    BranchHintDuplicate,
    BranchHintNotInFunction,
    BranchHintInvalidPlacement,
    BranchHintInvalidValue,
    BranchHintUnexpectedToken,
    ComponentBinaryMalformed,
};

/// The last TextErrorCode enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastTextErrorCode = TextErrorCode::ComponentBinaryMalformed;

/// One row of TextErrorTable: an error, the assertion it answers, and the message the spec test
/// suite expects for it.
struct TextErrorRow
{
    TextErrorCode code;   ///< The error this row describes; equal to the row's index.
    TextErrorStage stage; ///< The assertion it answers.
    std::string_view
        specMessage; ///< The reference interpreter's message, which spec tests match as a prefix.
    TextErrorDetail detail = TextErrorDetail::None; ///< Where the message puts TextError::subject.
};

/// Every text error, one row per TextErrorCode enumerator, in enumerator order.
///
/// The messages are those of the spec reference interpreter (`interpreter/text/lexer.mll`,
/// `parser.mly` and `interpreter/custom/*`), since the spec suite's `assert_malformed` and
/// `assert_malformed_custom` match their expected text as a prefix of the reported one.
inline constexpr auto TextErrorTable = std::to_array<TextErrorRow>({
    // A character an annotation may not hold; outside one, the two Misplaced rows report it.
    { .code = TextErrorCode::IllegalCharacter,
      .stage = TextErrorStage::Syntax,
      .specMessage = "illegal character" },
    { .code = TextErrorCode::MisplacedControlCharacter,
      .stage = TextErrorStage::Syntax,
      .specMessage = "misplaced control character" },
    { .code = TextErrorCode::MisplacedUnicodeCharacter,
      .stage = TextErrorStage::Syntax,
      .specMessage = "misplaced unicode character" },
    { .code = TextErrorCode::IllegalControlInString,
      .stage = TextErrorStage::Syntax,
      .specMessage = "illegal control character in string literal" },
    { .code = TextErrorCode::IllegalEscape,
      .stage = TextErrorStage::Syntax,
      .specMessage = "illegal escape" },
    { .code = TextErrorCode::UnclosedString,
      .stage = TextErrorStage::Syntax,
      .specMessage = "unclosed string literal" },
    { .code = TextErrorCode::UnclosedComment,
      .stage = TextErrorStage::Syntax,
      .specMessage = "unclosed comment" },
    { .code = TextErrorCode::MalformedUtf8,
      .stage = TextErrorStage::Syntax,
      .specMessage = "malformed UTF-8 encoding" },
    { .code = TextErrorCode::EmptyIdentifier,
      .stage = TextErrorStage::Syntax,
      .specMessage = "empty identifier" },
    { .code = TextErrorCode::EmptyAnnotationId,
      .stage = TextErrorStage::Syntax,
      .specMessage = "empty annotation id" },
    { .code = TextErrorCode::UnclosedAnnotation,
      .stage = TextErrorStage::Syntax,
      .specMessage = "unclosed annotation" },
    // What the reference's generated parser reports for a token its grammar does not admit there.
    { .code = TextErrorCode::UnexpectedToken,
      .stage = TextErrorStage::Syntax,
      .specMessage = "unexpected token" },
    { .code = TextErrorCode::AlignmentNotPowerOfTwo,
      .stage = TextErrorStage::Syntax,
      .specMessage = "alignment must be a power of two" },
    { .code = TextErrorCode::InlineFunctionType,
      .stage = TextErrorStage::Syntax,
      .specMessage = "inline function type does not match explicit type" },
    { .code = TextErrorCode::UnknownLabel, .stage = TextErrorStage::Syntax, .specMessage = "unknown label" },
    { .code = TextErrorCode::MismatchingLabel,
      .stage = TextErrorStage::Syntax,
      .specMessage = "mismatching label" },
    { .code = TextErrorCode::MultipleStart,
      .stage = TextErrorStage::Syntax,
      .specMessage = "multiple start sections" },
    { .code = TextErrorCode::WrongLaneCount,
      .stage = TextErrorStage::Syntax,
      .specMessage = "wrong number of lane literals" },
    { .code = TextErrorCode::InvalidLaneLength,
      .stage = TextErrorStage::Syntax,
      .specMessage = "invalid lane length" },
    { .code = TextErrorCode::IdentifierNotAllowed,
      .stage = TextErrorStage::Syntax,
      .specMessage = "identifier not allowed" },
    { .code = TextErrorCode::InvalidCustomPageSize,
      .stage = TextErrorStage::Syntax,
      .specMessage = "invalid custom page size" },
    // Isoline's own: the reference has no limits of its own.
    { .code = TextErrorCode::NestingLimitExceeded,
      .stage = TextErrorStage::Syntax,
      .specMessage = "nesting limit exceeded" },
    { .code = TextErrorCode::SourceTooLarge,
      .stage = TextErrorStage::Syntax,
      .specMessage = "source too large" },
    { .code = TextErrorCode::OutputTooLarge,
      .stage = TextErrorStage::Syntax,
      .specMessage = "output too large" },
    { .code = TextErrorCode::NumberTooLong,
      .stage = TextErrorStage::Syntax,
      .specMessage = "number too long" },
    { .code = TextErrorCode::UnsupportedComponent,
      .stage = TextErrorStage::Syntax,
      .specMessage = "component text needs the component model" },
    // The subject is the token, as the reference lexer's `unknown` appends its lexeme.
    { .code = TextErrorCode::UnknownOperator,
      .stage = TextErrorStage::Syntax,
      .specMessage = "unknown operator",
      .detail = TextErrorDetail::Suffix },
    // The subject is the category and the reference, "type 3" or "func $f", as parser.mly's lookup.
    { .code = TextErrorCode::UnknownIdentifier,
      .stage = TextErrorStage::Syntax,
      .specMessage = "unknown",
      .detail = TextErrorDetail::Suffix },
    { .code = TextErrorCode::DuplicateIdentifier,
      .stage = TextErrorStage::Syntax,
      .specMessage = "duplicate",
      .detail = TextErrorDetail::Suffix },
    // The subject is the kind of the earlier definition, "function definition" and so on.
    { .code = TextErrorCode::ImportAfterDefinition,
      .stage = TextErrorStage::Syntax,
      .specMessage = "import after",
      .detail = TextErrorDetail::Suffix },
    // The subject is the type where the reference names one, "i8" or "i64".
    { .code = TextErrorCode::ConstantOutOfRange,
      .stage = TextErrorStage::Syntax,
      .specMessage = "constant out of range",
      .detail = TextErrorDetail::Prefix },
    { .code = TextErrorCode::CustomMissingName,
      .stage = TextErrorStage::Custom,
      .specMessage = "@custom annotation: missing section name" },
    { .code = TextErrorCode::CustomMalformedUtf8,
      .stage = TextErrorStage::Custom,
      .specMessage = "@custom annotation: malformed UTF-8 encoding" },
    { .code = TextErrorCode::CustomUnexpectedToken,
      .stage = TextErrorStage::Custom,
      .specMessage = "@custom annotation: unexpected token" },
    { .code = TextErrorCode::CustomMalformedSectionKind,
      .stage = TextErrorStage::Custom,
      .specMessage = "@custom annotation: malformed section kind" },
    { .code = TextErrorCode::CustomMalformedPlacement,
      .stage = TextErrorStage::Custom,
      .specMessage = "@custom annotation: malformed placement" },
    // parse.ml's message for a known annotation outside every module of the text.
    { .code = TextErrorCode::MisplacedAnnotation,
      .stage = TextErrorStage::Custom,
      .specMessage = "misplaced annotation" },
    { .code = TextErrorCode::MisplacedCustom,
      .stage = TextErrorStage::Custom,
      .specMessage = "misplaced @custom annotation" },
    { .code = TextErrorCode::MisplacedName,
      .stage = TextErrorStage::Custom,
      .specMessage = "misplaced @name annotation" },
    { .code = TextErrorCode::NameMultipleModule,
      .stage = TextErrorStage::Custom,
      .specMessage = "@name annotation: multiple module names" },
    // handler_name.ml's message for any direct name map: functions, types, tags and the rest alike.
    { .code = TextErrorCode::NameMultipleFunctionNames,
      .stage = TextErrorStage::Custom,
      .specMessage = "@name annotation: multiple function names" },
    // handler_name.ml's message for any indirect name map: parameters, locals and labels.
    { .code = TextErrorCode::NameMultipleLocalNames,
      .stage = TextErrorStage::Custom,
      .specMessage = "@name annotation: multiple local names" },
    { .code = TextErrorCode::NameStringExpected,
      .stage = TextErrorStage::Custom,
      .specMessage = "@name annotation: string expected" },
    { .code = TextErrorCode::NameUnexpectedToken,
      .stage = TextErrorStage::Custom,
      .specMessage = "@name annotation: unexpected token" },
    // handler_name.ml reports a name that is no UTF-8 without its annotation's prefix.
    { .code = TextErrorCode::NameMalformedUtf8,
      .stage = TextErrorStage::Custom,
      .specMessage = "malformed UTF-8 encoding" },
    { .code = TextErrorCode::BranchHintDuplicate,
      .stage = TextErrorStage::Custom,
      .specMessage = "@metadata.code.branch_hint annotation: duplicate annotation" },
    { .code = TextErrorCode::BranchHintNotInFunction,
      .stage = TextErrorStage::Custom,
      .specMessage = "@metadata.code.branch_hint annotation: not in a function" },
    { .code = TextErrorCode::BranchHintInvalidPlacement,
      .stage = TextErrorStage::Custom,
      .specMessage = "@metadata.code.branch_hint annotation: invalid placement" },
    { .code = TextErrorCode::BranchHintInvalidValue,
      .stage = TextErrorStage::Custom,
      .specMessage = "@metadata.code.branch_hint annotation: invalid hint value" },
    { .code = TextErrorCode::BranchHintUnexpectedToken,
      .stage = TextErrorStage::Custom,
      .specMessage = "@metadata.code.branch_hint annotation: unexpected token" },
    // A `(component binary …)` whose bytes do not decode; the subject is the decoder's own message.
    { .code = TextErrorCode::ComponentBinaryMalformed,
      .stage = TextErrorStage::Syntax,
      .specMessage = "malformed component binary:",
      .detail = TextErrorDetail::Suffix },
});

static_assert(meta::firstMisplacedRow<LastTextErrorCode>(TextErrorTable, &TextErrorRow::code)
                  == meta::NoMisplacedRow,
              "TextErrorTable must hold one row per TextErrorCode, at the index of its enumerator");

/// A place in a source text.
struct SourcePosition
{
    uint32_t line = 1;      ///< The line, counted from 1.
    uint32_t column = 1;    ///< The byte in the line, counted from 1.
    std::size_t offset = 0; ///< The byte offset in the source, counted from 0.
};

/// A failure to read text: what went wrong, where, and what it names.
struct TextError
{
    /// Builds an error; the code and position are required, so one that forgets its code does not
    /// compile.
    /// @param code What went wrong.
    /// @param position Where it is reported.
    /// @param subject What it names, as its row's detail places it; empty when it names nothing.
    TextError(TextErrorCode code, SourcePosition position, std::string subject = {}):
        code { code }, position { position }, subject { std::move(subject) }
    {
    }

    TextErrorCode code;      ///< What went wrong.
    SourcePosition position; ///< Where it is reported.
    std::string subject;     ///< What it names; empty when it names nothing.
};

/// The assertion an error answers.
/// @param code The error.
/// @return Its row's stage in TextErrorTable.
[[nodiscard]] constexpr TextErrorStage stageOf(TextErrorCode code) noexcept
{
    return TextErrorTable[static_cast<std::size_t>(code)].stage;
}

/// The message the reference interpreter reports for @p error: its row's message, with the subject
/// placed as the row's detail says when the error has one.
/// @param error The error to describe.
/// @return The message the spec suite's assertions match as a prefix.
[[nodiscard]] inline std::string specMessage(TextError const& error)
{
    auto const& row = TextErrorTable[static_cast<std::size_t>(error.code)];
    auto message = std::string { row.specMessage };
    if (error.subject.empty())
        return message;
    switch (row.detail)
    {
        case TextErrorDetail::None: break;
        case TextErrorDetail::Suffix: return message + ' ' + error.subject;
        case TextErrorDetail::Prefix: return error.subject + ' ' + message;
    }
    return message;
}

/// The line and column of a byte offset, computed on demand, so that tokens and nodes keep offsets
/// only. A line ends at LF, CR LF or a lone CR.
/// @param source The text the offset is into.
/// @param offset The byte offset; one past the end of @p source is the end.
/// @return The position of @p offset, clamped to the end of @p source.
[[nodiscard]] constexpr SourcePosition positionAt(std::string_view source, std::size_t offset) noexcept
{
    auto const end = offset < source.size() ? offset : source.size();
    auto position = SourcePosition { .line = 1, .column = 1, .offset = end };
    auto previous = '\0';
    for (auto const character: source.substr(0, end))
    {
        if (character == '\n' && previous == '\r')
            position.column = 1; // The LF of a CR LF, whose CR already ended the line.
        else if (character == '\n' || character == '\r')
        {
            ++position.line;
            position.column = 1;
        }
        else
            ++position.column;
        previous = character;
    }
    return position;
}

} // namespace isoline::text
