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
#include <utility>
#include <vector>

#include <isoline/Features.hpp>
#include <isoline/binary/Types.hpp>
#include <isoline/meta/EnumRows.hpp>

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
    ImmutableGlobal,
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
    InvalidCustomPageSize,
    UninitializedLocal,
    NonEmptyTagResultType,
    OperandStackLimitExceeded,
};

/// The last ValidationErrorCode enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastValidationErrorCode = ValidationErrorCode::OperandStackLimitExceeded;

/// What the reference interpreter appends to an error's message.
enum class MessageDetail : uint8_t
{
    None,        ///< Nothing: the message is the row's text.
    Index,       ///< The index the error names (ValidationError::subject), after a space.
    MemoryBound, ///< The memory size bound of the MemoryBoundTable row ValidationError::subject names.
    TableBound,  ///< The table size bound of the address type ValidationError::subject names.
    /// The operand types an instruction requires and those the stack has (ValidationError::mismatch),
    /// as valid.ml's match_stack spells them: ": instruction requires [t*] but stack has [t*]".
    Operands,
};

/// The bounds the interpreter's table size errors spell, one row per binary::AddressType in
/// enumerator order; a table size error's subject is the address type's enumerator value.
struct AddressBoundRow
{
    binary::AddressType addressType; ///< The address type; equal to the row's index.
    std::string_view tableBound;     ///< The largest table, in the words of check_tabletype.
};

/// Every address type's table size bound, as valid.ml spells it.
inline constexpr auto AddressBoundTable = std::to_array<AddressBoundRow>({
    { .addressType = binary::AddressType::I32, .tableBound = "2^32-1 for i32" },
    { .addressType = binary::AddressType::I64, .tableBound = "2^64-1 for i64" },
});

static_assert(meta::firstMisplacedRow<binary::LastAddressType>(AddressBoundTable,
                                                               &AddressBoundRow::addressType)
                  == meta::NoMisplacedRow,
              "AddressBoundTable holds one row per AddressType, at the index of its enumerator");

/// The bound a memory size error spells for one address type and page size.
struct MemoryBoundRow
{
    binary::AddressType addressType; ///< The memory's address type.
    uint8_t pageSizeLog2;            ///< The base-2 logarithm of its page size.
    std::string_view bound;          ///< The largest memory, in the words of check_memorytype.
};

/// Every valid memory type's size bound: for 64 KiB pages as WebAssembly 3.0's valid.ml spells it,
/// for 1-byte pages as custom-page-sizes' does. A memory size error's subject is a row's index, which
/// memoryBoundRow() computes.
inline constexpr auto MemoryBoundTable = std::to_array<MemoryBoundRow>({
    { .addressType = binary::AddressType::I32, .pageSizeLog2 = 16, .bound = "2^16 pages (4 GiB) for i32" },
    { .addressType = binary::AddressType::I64, .pageSizeLog2 = 16, .bound = "2^48 pages (256 TiB) for i64" },
    { .addressType = binary::AddressType::I32, .pageSizeLog2 = 0, .bound = "4294967295 pages for i32" },
    { .addressType = binary::AddressType::I64,
      .pageSizeLog2 = 0,
      .bound = "18446744073709551615 pages for i64" },
});

/// The index of the MemoryBoundTable row of a memory of @p addressType and 64 KiB or 1-byte pages.
/// @param addressType The memory's address type.
/// @param pageSizeLog2 Its page size's base-2 logarithm, 0 or 16, as validation admits.
/// @return The row's index: the address type's, after the two rows of 64 KiB pages for 1-byte ones.
[[nodiscard]] constexpr uint32_t memoryBoundRow(binary::AddressType addressType,
                                                uint8_t pageSizeLog2) noexcept
{
    constexpr auto AddressTypeCount = static_cast<uint32_t>(meta::EnumeratorCount<binary::LastAddressType>);
    auto const bytePages = pageSizeLog2 == 0 ? AddressTypeCount : 0;
    return bytePages + static_cast<uint32_t>(addressType);
}

static_assert(
    [] {
        for (auto const& row: MemoryBoundTable)
            if (&MemoryBoundTable[memoryBoundRow(row.addressType, row.pageSizeLog2)] != &row)
                return false;
        return true;
    }(),
    "memoryBoundRow() finds every MemoryBoundTable row at its own index");

/// What a validation error says of the module: that it is invalid, or only that it is past a limit.
enum class ValidationErrorKind : uint8_t
{
    Invalid,             ///< Invalid under any limits: the spec's validation refuses it.
    ImplementationLimit, ///< Past a binary::DecodeLimits value: a module a raised limit may accept.
};

/// One row of ValidationErrorTable: an error and the message the spec test suite expects for it.
struct ValidationErrorRow
{
    ValidationErrorCode code; ///< The error this row describes; equal to the row's index.
    std::string_view
        specMessage; ///< The reference interpreter's message, which spec tests match as a prefix.
    MessageDetail detail = MessageDetail::None;              ///< What the interpreter appends to it.
    ValidationErrorKind kind = ValidationErrorKind::Invalid; ///< Invalid, or past an implementation limit.
};

/// Every validation error, one row per ValidationErrorCode enumerator, in enumerator order.
///
/// The messages are those of the reference interpreter (`interpreter/valid/valid.ml`): 2.0's, or 3.0's
/// where 3.0 reworded them (the size bounds), since the spec suite's `assert_invalid` matches its
/// expected text as a prefix of the reported one; the interpreter appends details: the types
/// involved, which the prefix leaves out, and the index an unknown or undeclared entity has, which
/// the suite asserts and the Index rows reproduce.
inline constexpr auto ValidationErrorTable = std::to_array<ValidationErrorRow>({
    // The operands, where the validator records them (throw's, as the exception-handling suite asserts).
    { .code = ValidationErrorCode::TypeMismatch,
      .specMessage = "type mismatch",
      .detail = MessageDetail::Operands },
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
    // WebAssembly 3.0's wording; 2.0's was "global is immutable".
    { .code = ValidationErrorCode::ImmutableGlobal, .specMessage = "immutable global" },
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
    { .code = ValidationErrorCode::UnknownTag, .specMessage = "unknown tag", .detail = MessageDetail::Index },
    // A memarg offset of 2^32 or more on a memory of i32 addresses (memory64 reads offsets as u64s).
    { .code = ValidationErrorCode::OffsetOutOfRange, .specMessage = "offset out of range" },
    { .code = ValidationErrorCode::TableSizeMustBeAtMost,
      .specMessage = "table size must be at most",
      .detail = MessageDetail::TableBound },
    // custom-page-sizes: a page size other than 1 byte or 64 KiB.
    { .code = ValidationErrorCode::InvalidCustomPageSize, .specMessage = "invalid custom page size" },
    // Typed function references: local.get of a local of a non-defaultable type no local.set or
    // local.tee of the same or an enclosing block has set yet.
    { .code = ValidationErrorCode::UninitializedLocal, .specMessage = "uninitialized local" },
    // Exception handling: a tag whose function type has results.
    { .code = ValidationErrorCode::NonEmptyTagResultType, .specMessage = "non-empty tag result type" },
    // Isoline's own implementation limit, DecodeLimits::maxOperandsPerInstruction; the spec suite has
    // no such error.
    { .code = ValidationErrorCode::OperandStackLimitExceeded,
      .specMessage = "operand stack limit exceeded",
      .kind = ValidationErrorKind::ImplementationLimit },
});

static_assert(
    meta::firstMisplacedRow<LastValidationErrorCode>(ValidationErrorTable, &ValidationErrorRow::code)
        == meta::NoMisplacedRow,
    "ValidationErrorTable must hold one row per ValidationErrorCode, at the index of its enumerator");

/// ValidationError::function of an error outside any function body: in a type, an import, a table,
/// a memory, a global, a segment, the start function or an export.
inline constexpr auto NoFunction = std::numeric_limits<uint32_t>::max();

/// ValidationError::instruction of an error that no single instruction of a body causes.
inline constexpr auto NoInstruction = std::numeric_limits<uint32_t>::max();

/// The operands an instruction required and those the stack had, when they did not match.
struct OperandMismatch
{
    /// The types the instruction requires, bottom of the stack first.
    std::vector<binary::ValueType> required;

    /// The operands the stack has in their place, bottom first: as many as are required, or all the
    /// block has when that is fewer. nullopt is an operand of unknown type (valid.ml's bot), which
    /// the polymorphic stack of unreachable code supplies.
    std::vector<std::optional<binary::ValueType>> actual;

    bool operator==(OperandMismatch const&) const = default;
};

/// The text format's spelling of an abstract heap type, by AbstractHeapType; Concrete's is its index.
inline constexpr auto AbstractHeapTypeNames = std::to_array<std::string_view>({
    "func",
    "extern",
    "exn",
    "nofunc",
    "noextern",
    "noexn",
    "something", // valid.ml's spelling of bot
    "",
});

static_assert(meta::sizedByEnum<binary::LastAbstractHeapType>(AbstractHeapTypeNames.size()),
              "AbstractHeapTypeNames holds one name per AbstractHeapType");

/// The text format's spelling of a number or vector type, by ValueKind: I32 through V128, every kind
/// but Ref.
inline constexpr auto NumberNames = std::to_array<std::string_view>({ "i32", "i64", "f32", "f64", "v128" });

static_assert(meta::sizedByEnum<binary::ValueKind::V128>(NumberNames.size()),
              "NumberNames holds one name per ValueKind, I32 through V128");

/// The text format's spelling of @p type, after valid.ml's string_of_valtype: i32, v128,
/// (ref null func), bot for an operand of unknown type. A concrete reference is Isoline's own
/// spelling, (ref 3) by its type index; valid.ml prints the substituted definition type instead.
/// @param type The type, or nullopt for an operand of unknown type.
/// @return Its spelling.
[[nodiscard]] inline std::string typeText(std::optional<binary::ValueType> const& type)
{
    if (!type)
        return "bot";
    if (type->kind != binary::ValueKind::Ref)
        return std::string { NumberNames.at(static_cast<std::size_t>(type->kind)) };
    auto const heap =
        type->heap.kind == binary::AbstractHeapType::Concrete
            ? std::to_string(type->heap.typeIndex)
            : std::string { AbstractHeapTypeNames.at(static_cast<std::size_t>(type->heap.kind)) };
    return std::format("(ref {}{})", type->nullable == binary::Nullable::Yes ? "null " : "", heap);
}

/// The spelling of the result type @p types: their typeText()s, space-separated, in brackets.
/// @param types The types, bottom of the stack first.
/// @return For example "[i32 (ref null exn)]".
template <typename Types>
[[nodiscard]] std::string resultTypeText(Types const& types)
{
    auto text = std::string { "[" };
    for (auto const& type: types)
    {
        if (text.size() > 1)
            text += ' ';
        text += typeText(type);
    }
    return text + "]";
}

/// A validation failure: what went wrong, and where.
struct ValidationError
{
    /// Builds an error; there is no default code, so an error that forgets to name one does not
    /// compile.
    /// @param code What went wrong.
    /// @param function The function whose body failed, or NoFunction.
    /// @param instruction The failing instruction's index in that body, or NoInstruction.
    /// @param subject The index the error names, if any.
    /// @param mismatch The operands that did not match, if the validator recorded them.
    /// @param disabled The proposal the validator's FeatureSet lacks that would admit the module past
    ///                 this point; empty when none would.
    constexpr explicit ValidationError(ValidationErrorCode code,
                                       uint32_t function = NoFunction,
                                       uint32_t instruction = NoInstruction,
                                       std::optional<uint32_t> subject = {},
                                       std::optional<OperandMismatch> mismatch = {},
                                       std::optional<Feature> disabled = {}) noexcept:
        code { code },
        function { function },
        instruction { instruction },
        subject { subject },
        mismatch { std::move(mismatch) },
        disabled { disabled }
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

    /// For a type mismatch of an instruction's operands: what it required and what the stack had;
    /// empty where the validator records no operands.
    std::optional<OperandMismatch> mismatch;

    /// The proposal the validator's FeatureSet lacks that would admit the module past this point: a
    /// second memory's multi-memory, a type of two results' multi-value, a constant i32.add's
    /// extended-const. The error is the one the spec suite expects of a validator without it, and the
    /// message appends the proposal (isoline::detail::appendDisabledFeature()).
    std::optional<Feature> disabled;

    bool operator==(ValidationError const&) const = default;
};

/// The message the spec test suite expects for @p code.
/// @param code The error to describe.
/// @return Its row's message in ValidationErrorTable.
[[nodiscard]] constexpr std::string_view specMessage(ValidationErrorCode code) noexcept
{
    return ValidationErrorTable[static_cast<std::size_t>(code)].specMessage;
}

/// Whether @p code reports a module past an implementation limit (binary::DecodeLimits), not an
/// invalid one.
/// @param code The error.
/// @return Whether its row's kind is ImplementationLimit.
[[nodiscard]] constexpr bool isImplementationLimit(ValidationErrorCode code) noexcept
{
    return ValidationErrorTable[static_cast<std::size_t>(code)].kind
           == ValidationErrorKind::ImplementationLimit;
}

/// The message the reference interpreter reports for @p error: its row's message, followed by what
/// its detail names (the index, the size bound, or the operands), and then by the disabled proposal
/// that would admit the module, when the error names one (isoline::detail::appendDisabledFeature()).
/// @param error The error to describe.
/// @return The message the spec suite's assertions match as a prefix.
[[nodiscard]] inline std::string specMessage(ValidationError const& error)
{
    auto const& row = ValidationErrorTable[static_cast<std::size_t>(error.code)];
    auto const message = [&]() -> std::string {
        if (row.detail == MessageDetail::Operands && error.mismatch)
            return std::format("{}: instruction requires {} but stack has {}",
                               row.specMessage,
                               resultTypeText(error.mismatch->required),
                               resultTypeText(error.mismatch->actual));
        if (!error.subject)
            return std::string { row.specMessage };
        switch (row.detail)
        {
            case MessageDetail::None:
            case MessageDetail::Operands: break;
            case MessageDetail::Index: return std::format("{} {}", row.specMessage, *error.subject);
            case MessageDetail::MemoryBound:
                return std::format("{} {}", row.specMessage, MemoryBoundTable.at(*error.subject).bound);
            case MessageDetail::TableBound:
                return std::format("{} {}", row.specMessage, AddressBoundTable.at(*error.subject).tableBound);
        }
        return std::string { row.specMessage };
    }();
    return isoline::detail::appendDisabledFeature(message, error.disabled);
}

} // namespace isoline::validate
