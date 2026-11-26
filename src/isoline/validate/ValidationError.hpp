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

#include <isoline/binary/Types.hpp>

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
    MemorySizeMustBeAtMost,
    SizeMinimumMustNotBeGreaterThanMaximum,
    StartFunction,
    UndeclaredFunctionReference,
    InvalidLaneIndex,
    UnknownTag,
    OffsetOutOfRange,
    TableSizeMustBeAtMost,
};

/// The last ValidationErrorCode enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastValidationErrorCode = ValidationErrorCode::TableSizeMustBeAtMost;

/// What the reference interpreter appends to an error's message.
enum class MessageDetail : uint8_t
{
    None,        ///< Nothing: the message is the row's text.
    Index,       ///< The index the error names (ValidationError::subject), after a space.
    MemoryBound, ///< The memory size bound of the address type ValidationError::subject names.
    TableBound,  ///< The table size bound of the address type ValidationError::subject names.
};

/// The bounds the interpreter's memory and table size errors spell, one row per binary::AddressType
/// in enumerator order; a size error's subject is the address type's enumerator value.
struct AddressBoundRow
{
    binary::AddressType addressType; ///< The address type; equal to the row's index.
    std::string_view memoryBound;    ///< The largest memory, in the words of check_memorytype.
    std::string_view tableBound;     ///< The largest table, in the words of check_tabletype.
};

/// Every address type's size bounds, as valid.ml spells them.
inline constexpr auto AddressBoundTable = std::to_array<AddressBoundRow>({
    { .addressType = binary::AddressType::I32,
      .memoryBound = "2^16 pages (4 GiB) for i32",
      .tableBound = "2^32-1 for i32" },
    { .addressType = binary::AddressType::I64,
      .memoryBound = "2^48 pages (256 TiB) for i64",
      .tableBound = "2^64-1 for i64" },
});

static_assert(AddressBoundTable[static_cast<std::size_t>(binary::AddressType::I32)].addressType
                      == binary::AddressType::I32
                  && AddressBoundTable[static_cast<std::size_t>(binary::AddressType::I64)].addressType
                         == binary::AddressType::I64,
              "AddressBoundTable holds one row per AddressType, at the index of its enumerator");

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
/// The messages are those of the reference interpreter (`interpreter/valid/valid.ml`): 2.0's, or 3.0's
/// where 3.0 reworded them (the size bounds), since the spec suite's `assert_invalid` matches its
/// expected text as a prefix of the reported one; the interpreter appends details: the types
/// involved, which the prefix leaves out, and the index an unknown or undeclared entity has, which
/// the suite asserts and the Index rows reproduce.
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
    // WebAssembly 3.0's wording, which names the bound of the memory's address type; the suite
    // asserts the prefix "memory size".
    { .code = ValidationErrorCode::MemorySizeMustBeAtMost,
      .specMessage = "memory size must be at most",
      .detail = MessageDetail::MemoryBound },
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
    // A memarg offset of 2^32 or more on a memory of i32 addresses (memory64 reads offsets as u64s).
    { .code = ValidationErrorCode::OffsetOutOfRange, .specMessage = "offset out of range" },
    { .code = ValidationErrorCode::TableSizeMustBeAtMost,
      .specMessage = "table size must be at most",
      .detail = MessageDetail::TableBound },
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

    /// What the error names, as its row's detail reads it: the unknown or undeclared index, or the
    /// address type of a size error; empty when it names none.
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
    if (!error.subject)
        return std::string { row.specMessage };
    switch (row.detail)
    {
        case MessageDetail::None: break;
        case MessageDetail::Index: return std::format("{} {}", row.specMessage, *error.subject);
        case MessageDetail::MemoryBound:
            return std::format("{} {}", row.specMessage, AddressBoundTable.at(*error.subject).memoryBound);
        case MessageDetail::TableBound:
            return std::format("{} {}", row.specMessage, AddressBoundTable.at(*error.subject).tableBound);
    }
    return std::string { row.specMessage };
}

} // namespace isoline::validate
