// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#include <isoline/Features.hpp>

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
    LocalCountOverflow,
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
    MissingFeaturePrerequisite,
    MalformedLimitsFlags,
    InvalidCustomPageSize,
    MalformedCatchClause,
    NameSubsectionSizeMismatch,
    NameIndexOutOfOrder,
};

/// The last DecodeErrorCode enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastDecodeErrorCode = DecodeErrorCode::NameIndexOutOfOrder;

/// What a decode error's message appends to its row's text, from DecodeError::subject.
enum class DecodeErrorDetail : uint8_t
{
    None,                ///< Nothing: the message is the row's text.
    OpcodeBytes,         ///< The opcode read, in lowercase hex, as the interpreter's illegal/illegal2.
    FeaturePrerequisite, ///< The feature and the prerequisite it lacks, by name.
};

/// What a decode error says of the module: that it is malformed, or that it is past a limit.
enum class DecodeErrorKind : uint8_t
{
    Malformed,           ///< Not a module under any limits: the binary format refuses it.
    ImplementationLimit, ///< Past a DecodeLimits value: a module a raised limit may decode.
};

/// One row of DecodeErrorTable: an error and the message the spec test suite expects for it.
struct DecodeErrorRow
{
    DecodeErrorCode code; ///< The error this row describes; equal to the row's index.
    std::string_view
        specMessage; ///< The reference interpreter's message, which spec tests match as a prefix.
    DecodeErrorDetail detail = DecodeErrorDetail::None; ///< What the message appends to specMessage.
    DecodeErrorKind kind = DecodeErrorKind::Malformed;  ///< Malformed, or past an implementation limit.
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
    // Past DecodeLimits::maxLocals: a raised limit may decode the function.
    { .code = DecodeErrorCode::TooManyLocals,
      .specMessage = "too many locals",
      .kind = DecodeErrorKind::ImplementationLimit },
    // Past 2^32 - 1 locals, which the binary format refuses whatever the limits: malformed.
    { .code = DecodeErrorCode::LocalCountOverflow, .specMessage = "too many locals" },
    // The interpreter appends the opcode's bytes: "illegal opcode ff", or "illegal opcode fc 12".
    { .code = DecodeErrorCode::IllegalOpcode,
      .specMessage = "illegal opcode",
      .detail = DecodeErrorDetail::OpcodeBytes },
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
    { .code = DecodeErrorCode::ImplementationLimitExceeded,
      .specMessage = "implementation limit exceeded",
      .kind = DecodeErrorKind::ImplementationLimit },
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
    // WebAssembly 2.0: an alignment exponent of 32 or more; multi-memory: flags of 0x80 or more.
    { .code = DecodeErrorCode::MalformedMemopFlags, .specMessage = "malformed memop flags" },
    // Isoline's own: decodeModule refuses a FeatureSet not closed over FeatureRow::prerequisites.
    { .code = DecodeErrorCode::MissingFeaturePrerequisite,
      .specMessage = "feature enabled without its prerequisite",
      .detail = DecodeErrorDetail::FeaturePrerequisite },
    // WebAssembly 3.0 reads the limits flags as a byte and admits only the flags of its table; 2.0's
    // reading is LimitsFlagTooLarge's.
    { .code = DecodeErrorCode::MalformedLimitsFlags, .specMessage = "malformed limits flags" },
    // custom-page-sizes: a page size exponent of 64 or more, whose page size is no u64.
    { .code = DecodeErrorCode::InvalidCustomPageSize, .specMessage = "invalid custom page size" },
    // Exception handling: a try_table catch clause whose kind byte is above 3.
    { .code = DecodeErrorCode::MalformedCatchClause, .specMessage = "malformed catch clause" },
    // The name section (decodeNameSection), which module decoding keeps verbatim: a subsection whose
    // contents do not fill its size, in the reference interpreter's words (`handler_name.ml`), and a
    // name map whose indices do not increase strictly, in Isoline's.
    { .code = DecodeErrorCode::NameSubsectionSizeMismatch, .specMessage = "name subsection size mismatch" },
    { .code = DecodeErrorCode::NameIndexOutOfOrder, .specMessage = "name map indices out of order" },
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

/// A decode failure: what went wrong, where, and what it names.
///
/// A module that uses a proposal the FeatureSet does not enable is refused as a decoder of a standard
/// without the proposal refuses it: with the code, and so the message prefix, the spec suite expects
/// there (an unknown opcode's IllegalOpcode, a tag section's MalformedSectionId, an i64 memory's limits
/// flag). Such an error names in disabled the proposal that would decode the module past that point,
/// and the message appends it ("illegal opcode 0c (simd not enabled)"), so an embedder knows what to
/// enable.
struct DecodeError
{
    /// Builds an error; the code and offset are required, so one that forgets its code does not
    /// compile.
    /// @param code What went wrong.
    /// @param offset The absolute byte offset it is reported at.
    /// @param subject What it names, as its row's detail reads it; empty when it names nothing.
    /// @param disabled The proposal the decoder's FeatureSet lacks that would decode the module past
    ///                 this point; empty when none would.
    constexpr DecodeError(DecodeErrorCode code,
                          std::size_t offset,
                          std::optional<uint64_t> subject = {},
                          std::optional<Feature> disabled = {}) noexcept:
        code { code }, offset { offset }, subject { subject }, disabled { disabled }
    {
    }

    DecodeErrorCode code;            ///< What went wrong.
    std::size_t offset;              ///< The absolute byte offset in the module the error is reported at.
    std::optional<uint64_t> subject; ///< What it names; see opcodeSubject() and prerequisiteSubject().
    std::optional<Feature> disabled; ///< The disabled proposal that would decode it, if any.

    /// Two errors are equal when their code, offset, subject and disabled proposal are.
    bool operator==(DecodeError const&) const = default;
};

/// The subject of an IllegalOpcode error: the opcode as read.
/// @param prefix The prefix byte read before @p code, or 0 for a single-byte opcode.
/// @param code The opcode byte, or the sub-opcode after @p prefix.
/// @return The prefix in bits 32-39 and the code in bits 0-31.
[[nodiscard]] constexpr uint64_t opcodeSubject(uint8_t prefix, uint32_t code) noexcept
{
    return (uint64_t { prefix } << 32U) | code;
}

/// The subject of a MissingFeaturePrerequisite error.
/// @param unmet The feature that is on and the prerequisite of it that is off.
/// @return The feature in bits 0-7 and the prerequisite in bits 8-15.
[[nodiscard]] constexpr uint64_t prerequisiteSubject(UnmetPrerequisite unmet) noexcept
{
    return static_cast<uint64_t>(unmet.feature) | (static_cast<uint64_t>(unmet.prerequisite) << 8U);
}

/// The message the spec test suite expects for @p code.
/// @param code The error to describe.
/// @return Its row's message in DecodeErrorTable.
[[nodiscard]] constexpr std::string_view specMessage(DecodeErrorCode code) noexcept
{
    return DecodeErrorTable[static_cast<std::size_t>(code)].specMessage;
}

/// Whether @p code reports a module past an implementation limit (DecodeLimits), not a malformed one.
/// @param code The error.
/// @return Whether its row's kind is ImplementationLimit.
[[nodiscard]] constexpr bool isImplementationLimit(DecodeErrorCode code) noexcept
{
    return DecodeErrorTable[static_cast<std::size_t>(code)].kind == DecodeErrorKind::ImplementationLimit;
}

/// The message the reference interpreter reports for @p error: its row's message, followed by what
/// the error names when the row has a detail and the error a subject, and then by the disabled
/// proposal that would decode the module, when the error names one
/// (isoline::detail::appendDisabledFeature()).
/// @param error The error to describe.
/// @return The message the spec suite's assertions match as a prefix.
[[nodiscard]] inline std::string specMessage(DecodeError const& error)
{
    auto const& row = DecodeErrorTable[static_cast<std::size_t>(error.code)];
    auto const message = [&]() -> std::string {
        if (!error.subject)
            return std::string { row.specMessage };
        auto const subject = *error.subject;
        // The name of the feature whose index is the low byte of @p bits.
        auto const nameOf = [](uint64_t bits) {
            return featureRow(static_cast<Feature>(bits & 0xFFU)).name;
        };
        switch (row.detail)
        {
            case DecodeErrorDetail::None: break;
            case DecodeErrorDetail::OpcodeBytes: {
                auto const prefix = (subject >> 32U) & 0xFFU;
                auto const code = subject & 0xFFFF'FFFFU;
                if (prefix == 0)
                    return std::format("{} {:02x}", row.specMessage, code);
                return std::format("{} {:02x} {:02x}", row.specMessage, prefix, code);
            }
            case DecodeErrorDetail::FeaturePrerequisite:
                return std::format(
                    "{}: {} requires {}", row.specMessage, nameOf(subject), nameOf(subject >> 8U));
        }
        return std::string { row.specMessage };
    }();
    return isoline::detail::appendDisabledFeature(message, error.disabled);
}

} // namespace isoline::binary
