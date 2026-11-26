// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace isoline::validate
{

/// Why the validator rejected a module.
///
/// The enumerators are the rows of ValidationErrorTable, in the same order; adding an error is adding
/// an enumerator here and a row there.
enum class ValidationErrorCode : uint8_t
{
    TypeMismatch,
    UnknownLocal,
    UnknownGlobal,
    UnknownFunction,
    UnknownTable,
    UnknownMemory,
    UnknownType,
    UnknownLabel,
    UnknownElemSegment,
    UnknownDataSegment,
    InvalidResultArity,
    GlobalIsImmutable,
    ConstantExpressionRequired,
    DuplicateExportName,
    MultipleMemories,
    MultipleTables,
    AlignmentMustNotBeLarger,
    MemorySizeMustBeAtMost4GiB,
    SizeMinimumMustNotBeGreaterThanMaximum,
    StartFunction,
    UndeclaredFunctionReference,
    InvalidLaneIndex,
    UnknownTag,
};

/// The last ValidationErrorCode enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastValidationErrorCode = ValidationErrorCode::UnknownTag;

/// What the reference interpreter appends to an error's message.
enum class MessageDetail : uint8_t
{
    None,  ///< Nothing: the message is the row's text.
    Index, ///< The index the error names (ValidationError::subject), after a space.
};

/// One row of ValidationErrorTable: an error and the message the spec test suite expects for it.
struct ValidationErrorRow
{
    ValidationErrorCode code; ///< The error this row describes; equal to the row's index.
    std::string_view
        specMessage; ///< The reference interpreter's message, which spec tests match as a prefix.
    MessageDetail detail = MessageDetail::None; ///< What the interpreter appends to it.
};

/// Every validation error, one row per ValidationErrorCode enumerator, in enumerator order.
///
/// The messages are those of the WebAssembly 2.0 reference interpreter (`interpreter/valid/valid.ml`),
/// since the spec suite's `assert_invalid` matches its expected text as a prefix of the reported
/// one; the interpreter appends details: the types involved, which the prefix leaves out, and the index an
/// unknown or undeclared entity has, which the suite asserts and the Index rows reproduce.
inline constexpr auto ValidationErrorTable = std::to_array<ValidationErrorRow>({
    { .code = ValidationErrorCode::TypeMismatch, .specMessage = "type mismatch" },
    { .code = ValidationErrorCode::UnknownLocal,
      .specMessage = "unknown local",
      .detail = MessageDetail::Index },
    { .code = ValidationErrorCode::UnknownGlobal,
      .specMessage = "unknown global",
      .detail = MessageDetail::Index },
    { .code = ValidationErrorCode::UnknownFunction,
      .specMessage = "unknown function",
      .detail = MessageDetail::Index },
    { .code = ValidationErrorCode::UnknownTable,
      .specMessage = "unknown table",
      .detail = MessageDetail::Index },
    { .code = ValidationErrorCode::UnknownMemory,
      .specMessage = "unknown memory",
      .detail = MessageDetail::Index },
    { .code = ValidationErrorCode::UnknownType,
      .specMessage = "unknown type",
      .detail = MessageDetail::Index },
    { .code = ValidationErrorCode::UnknownLabel,
      .specMessage = "unknown label",
      .detail = MessageDetail::Index },
    { .code = ValidationErrorCode::UnknownElemSegment,
      .specMessage = "unknown elem segment",
      .detail = MessageDetail::Index },
    { .code = ValidationErrorCode::UnknownDataSegment,
      .specMessage = "unknown data segment",
      .detail = MessageDetail::Index },
    { .code = ValidationErrorCode::InvalidResultArity, .specMessage = "invalid result arity" },
    { .code = ValidationErrorCode::GlobalIsImmutable, .specMessage = "global is immutable" },
    { .code = ValidationErrorCode::ConstantExpressionRequired,
      .specMessage = "constant expression required" },
    { .code = ValidationErrorCode::DuplicateExportName, .specMessage = "duplicate export name" },
    { .code = ValidationErrorCode::MultipleMemories, .specMessage = "multiple memories" },
    // The MVP's rule; WebAssembly 2.0 (reference types) allows any number of tables.
    { .code = ValidationErrorCode::MultipleTables, .specMessage = "multiple tables" },
    { .code = ValidationErrorCode::AlignmentMustNotBeLarger,
      .specMessage = "alignment must not be larger than natural" },
    { .code = ValidationErrorCode::MemorySizeMustBeAtMost4GiB,
      .specMessage = "memory size must be at most 65536 pages (4GiB)" },
    { .code = ValidationErrorCode::SizeMinimumMustNotBeGreaterThanMaximum,
      .specMessage = "size minimum must not be greater than maximum" },
    // The interpreter's full text is "start function must have type [] -> []".
    { .code = ValidationErrorCode::StartFunction, .specMessage = "start function" },
    { .code = ValidationErrorCode::UndeclaredFunctionReference,
      .specMessage = "undeclared function reference",
      .detail = MessageDetail::Index },
    { .code = ValidationErrorCode::InvalidLaneIndex, .specMessage = "invalid lane index" },
    // An export of a tag, which no P1 module defines: the exception-handling interpreter's text.
    { .code = ValidationErrorCode::UnknownTag, .specMessage = "unknown tag" },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: ValidationErrorTable)
        {
            if (static_cast<std::size_t>(row.code) != index)
                return false;
            ++index;
        }
        return index == static_cast<std::size_t>(LastValidationErrorCode) + 1;
    }(),
    "ValidationErrorTable must hold one row per ValidationErrorCode, at the index of its enumerator");

/// ValidationError::function of an error outside any function body: in a type, an import, a table,
/// a memory, a global, a segment, the start function or an export.
inline constexpr auto NoFunction = std::numeric_limits<uint32_t>::max();

/// ValidationError::instruction of an error that no single instruction of a body causes.
inline constexpr auto NoInstruction = std::numeric_limits<uint32_t>::max();

/// A validation failure: what went wrong, and where.
struct ValidationError
{
    /// Builds an error; there is no default code, so an error that forgets to name one does not
    /// compile.
    /// @param code What went wrong.
    /// @param function The function whose body failed, or NoFunction.
    /// @param instruction The failing instruction's index in that body, or NoInstruction.
    /// @param subject The index the error names, if any.
    constexpr explicit ValidationError(ValidationErrorCode code,
                                       uint32_t function = NoFunction,
                                       uint32_t instruction = NoInstruction,
                                       std::optional<uint32_t> subject = {}) noexcept:
        code { code }, function { function }, instruction { instruction }, subject { subject }
    {
    }

    /// What went wrong.
    ValidationErrorCode code;

    /// The function whose body failed, by its index in the module's function index space (imported
    /// functions first), or NoFunction for an error outside every body.
    uint32_t function;

    /// The index in that body of the instruction that failed, or NoInstruction.
    uint32_t instruction;

    /// The index the error names, the unknown or undeclared one; empty when it names none.
    std::optional<uint32_t> subject;

    bool operator==(ValidationError const&) const = default;
};

/// The message the spec test suite expects for @p code.
/// @param code The error to describe.
/// @return Its row's message in ValidationErrorTable.
[[nodiscard]] constexpr std::string_view specMessage(ValidationErrorCode code) noexcept
{
    return ValidationErrorTable[static_cast<std::size_t>(code)].specMessage;
}

/// The message the reference interpreter reports for @p error: its row's message, followed by the
/// index it names when the row's detail is Index.
/// @param error The error to describe.
/// @return The message the spec suite's assertions match as a prefix.
[[nodiscard]] inline std::string specMessage(ValidationError const& error)
{
    auto const& row = ValidationErrorTable[static_cast<std::size_t>(error.code)];
    if (row.detail == MessageDetail::Index && error.subject)
        return std::format("{} {}", row.specMessage, *error.subject);
    return std::string { row.specMessage };
}

} // namespace isoline::validate
