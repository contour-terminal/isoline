// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace isoline::binary
{

/// Why the binary decoder rejected a module.
///
/// The enumerators are the rows of DecodeErrorTable, in the same order; adding an error is adding an
/// enumerator here and a row there.
enum class DecodeErrorCode : uint8_t
{
    UnexpectedEnd,
    MagicHeaderNotDetected,
    UnknownBinaryVersion,
    IntegerTooLarge,
    IntegerRepresentationTooLong,
    MalformedSectionId,
    SectionSizeMismatch,
    JunkAfterLastSection,
    MalformedUtf8Encoding,
    TooManyLocals,
    IllegalOpcode,
    MalformedReferenceType,
    MalformedImportKind,
    MalformedMutability,
    IntegerTooLargeForLimits,
    LimitsFlagTooLarge,
    FunctionAndCodeSectionInconsistent,
    DataCountAndDataSectionInconsistent,
    DataCountSectionRequired,
    UnexpectedContentAfterEnd,
    MalformedElementSegmentKind,
    ZeroByteExpected,
    LengthOutOfBounds,
    ImplementationLimitExceeded,
    DuplicateSection,
    SectionOutOfOrder,
    MalformedExportKind,
    MalformedElementKind,
    MalformedDataSegmentKind,
    MalformedFunctionType,
    EndOpcodeExpected,
    MalformedMemopFlags,
};

/// The last DecodeErrorCode enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastDecodeErrorCode = DecodeErrorCode::MalformedMemopFlags;

/// One row of DecodeErrorTable: an error and the message the spec test suite expects for it.
struct DecodeErrorRow
{
    DecodeErrorCode code; ///< The error this row describes; equal to the row's index.
    std::string_view
        specMessage; ///< The reference interpreter's message, which spec tests match as a prefix.
};

/// Every decode error, one row per DecodeErrorCode enumerator, in enumerator order.
///
/// The messages are those of the spec reference interpreter (`interpreter/binary/decode.ml`), since
/// the spec suite's `assert_malformed` matches its expected text as a prefix of the reported one.
inline constexpr auto DecodeErrorTable = std::to_array<DecodeErrorRow>({
    // The interpreter's own text; the suite expects both it and its prefix "unexpected end".
    { .code = DecodeErrorCode::UnexpectedEnd, .specMessage = "unexpected end of section or function" },
    { .code = DecodeErrorCode::MagicHeaderNotDetected, .specMessage = "magic header not detected" },
    { .code = DecodeErrorCode::UnknownBinaryVersion, .specMessage = "unknown binary version" },
    { .code = DecodeErrorCode::IntegerTooLarge, .specMessage = "integer too large" },
    { .code = DecodeErrorCode::IntegerRepresentationTooLong,
      .specMessage = "integer representation too long" },
    { .code = DecodeErrorCode::MalformedSectionId, .specMessage = "malformed section id" },
    { .code = DecodeErrorCode::SectionSizeMismatch, .specMessage = "section size mismatch" },
    { .code = DecodeErrorCode::JunkAfterLastSection, .specMessage = "junk after last section" },
    { .code = DecodeErrorCode::MalformedUtf8Encoding, .specMessage = "malformed UTF-8 encoding" },
    { .code = DecodeErrorCode::TooManyLocals, .specMessage = "too many locals" },
    { .code = DecodeErrorCode::IllegalOpcode, .specMessage = "illegal opcode" },
    { .code = DecodeErrorCode::MalformedReferenceType, .specMessage = "malformed reference type" },
    { .code = DecodeErrorCode::MalformedImportKind, .specMessage = "malformed import kind" },
    { .code = DecodeErrorCode::MalformedMutability, .specMessage = "malformed mutability" },
    // A limits bound that overflows its u32/u64 is reported by the interpreter's LEB128 reader.
    { .code = DecodeErrorCode::IntegerTooLargeForLimits, .specMessage = "integer too large" },
    // WebAssembly 2.0's interpreter reads the flag as a one-bit LEB128; 3.0's says "malformed limits
    // flags".
    { .code = DecodeErrorCode::LimitsFlagTooLarge, .specMessage = "integer too large" },
    { .code = DecodeErrorCode::FunctionAndCodeSectionInconsistent,
      .specMessage = "function and code section have inconsistent lengths" },
    { .code = DecodeErrorCode::DataCountAndDataSectionInconsistent,
      .specMessage = "data count and data section have inconsistent lengths" },
    { .code = DecodeErrorCode::DataCountSectionRequired, .specMessage = "data count section required" },
    { .code = DecodeErrorCode::UnexpectedContentAfterEnd,
      .specMessage = "unexpected content after last section" },
    { .code = DecodeErrorCode::MalformedElementSegmentKind,
      .specMessage = "malformed elements segment kind" },
    { .code = DecodeErrorCode::ZeroByteExpected, .specMessage = "zero byte expected" },
    { .code = DecodeErrorCode::LengthOutOfBounds, .specMessage = "length out of bounds" },
    // The interpreter has no decode-time limit; this is Isoline's own wording.
    { .code = DecodeErrorCode::ImplementationLimitExceeded, .specMessage = "implementation limit exceeded" },
    // The interpreter reads sections in their fixed order, so a repeated or misplaced one is left over
    // when it expects the end of the module.
    { .code = DecodeErrorCode::DuplicateSection, .specMessage = "unexpected content after last section" },
    { .code = DecodeErrorCode::SectionOutOfOrder, .specMessage = "unexpected content after last section" },
    { .code = DecodeErrorCode::MalformedExportKind, .specMessage = "malformed export kind" },
    // The byte after an element segment's flags in the forms that list function indices.
    { .code = DecodeErrorCode::MalformedElementKind, .specMessage = "malformed element kind" },
    { .code = DecodeErrorCode::MalformedDataSegmentKind, .specMessage = "malformed data segment kind" },
    // WebAssembly 2.0's interpreter; 3.0's, which knows more than function types, says "malformed
    // definition type". The suite asserts neither.
    { .code = DecodeErrorCode::MalformedFunctionType, .specMessage = "malformed function type" },
    // A block, or a body or constant expression, that ends in anything but end: else where no if
    // is open, or the next section's bytes when the final end is missing.
    { .code = DecodeErrorCode::EndOpcodeExpected, .specMessage = "END opcode expected" },
    // WebAssembly 2.0: an alignment exponent of 32 or more (3.0 gives bit 6 to multi-memory).
    { .code = DecodeErrorCode::MalformedMemopFlags, .specMessage = "malformed memop flags" },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: DecodeErrorTable)
        {
            if (static_cast<std::size_t>(row.code) != index)
                return false;
            ++index;
        }
        return index == static_cast<std::size_t>(LastDecodeErrorCode) + 1;
    }(),
    "DecodeErrorTable must hold one row per DecodeErrorCode, at the index of its enumerator");

/// A decode failure: what went wrong, and where.
struct DecodeError
{
    /// Builds an error; both fields are required, so one that forgets its code does not compile.
    /// @param code What went wrong.
    /// @param offset The absolute byte offset it is reported at.
    constexpr DecodeError(DecodeErrorCode code, std::size_t offset) noexcept: code { code }, offset { offset }
    {
    }

    DecodeErrorCode code; ///< What went wrong.
    std::size_t offset;   ///< The absolute byte offset in the module the error is reported at.
};

/// The message the spec test suite expects for @p code.
/// @param code The error to describe.
/// @return Its row's message in DecodeErrorTable.
[[nodiscard]] constexpr std::string_view specMessage(DecodeErrorCode code) noexcept
{
    return DecodeErrorTable[static_cast<std::size_t>(code)].specMessage;
}

} // namespace isoline::binary
