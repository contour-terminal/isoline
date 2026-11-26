// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <isoline/component/Features.hpp>
#include <isoline/component/Limits.hpp>
#include <isoline/component/binary/Component.hpp>

namespace isoline::component::binary
{

/// What the first eight bytes of a WebAssembly binary say it is.
enum class LayerKind : uint8_t
{
    CoreModule, ///< "\0asm", version 1, layer 0: a core module.
    Component,  ///< "\0asm", version 0x0d, layer 1: a component.
    Unknown,    ///< Anything else, a buffer shorter than eight bytes included.
};

/// Tells a core module from a component by its preamble, as `.wasm` files may hold either.
/// @param bytes The binary, from its first byte on.
/// @return The kind its preamble names.
[[nodiscard]] LayerKind layerOf(std::span<std::byte const> bytes) noexcept;

/// Why the component decoder rejected a component.
///
/// The enumerators are the rows of DecodeErrorMessages, in the same order; adding an error is adding
/// an enumerator here and a row there.
enum class DecodeErrorCode : uint8_t
{
    UnexpectedEnd,
    IntegerTooLarge,
    IntegerRepresentationTooLong,
    MalformedUtf8Encoding,
    MagicHeaderNotDetected,
    UnknownBinaryVersion,
    ExpectedComponentHeader,
    ExpectedModuleHeader,
    MalformedSectionId,
    SectionTooLarge,
    SectionSizeMismatch,
    UnimplementedSection,
    InvalidCoreInstanceKind,
    InvalidInstantiationArgKind,
    InvalidCoreExternKind,
    InvalidInstanceKind,
    InvalidExternalKind,
    InvalidAliasTarget,
    InvalidCoreExportAliasKind,
    InvalidOuterAliasKind,
    InvalidTypeBound,
    InvalidOptionalExportType,
    InvalidNameKind,
    InvalidNameAttribute,
    DuplicateNameAttribute,
    FeatureDisabled,
    LimitExceeded,
};

/// The last DecodeErrorCode enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastDecodeErrorCode = DecodeErrorCode::LimitExceeded;

/// How a decode error's message is built from its row's text and the error's subject.
enum class DecodeErrorDetail : uint8_t
{
    None,          ///< The row's text, as it is.
    LeadingByte,   ///< "invalid leading byte (0x{subject:x}) for {text}", as wasm-tools words it.
    SectionId,     ///< The text, then ": {subject}" when the error has a subject.
    BinaryVersion, ///< The text, then ": {subject:#10x}", the version and layer word read.
    Attribute,     ///< "duplicate '{keyword}' option in name", the subject a NameAttributeTable index.
    Feature,       ///< The disabled feature's refusal, then its gate in parentheses.
    Limit,         ///< The exceeded limit's message (limitMessage()), the subject from limitSubject().
};

/// One row of DecodeErrorMessages: an error and how its message reads.
struct DecodeErrorRow
{
    DecodeErrorCode code;                               ///< The error; equal to the row's index.
    std::string_view text;                              ///< Its text, wasm-tools' own where it has one.
    DecodeErrorDetail detail = DecodeErrorDetail::None; ///< How the message uses the subject.
};

/// Every decode error, one row per DecodeErrorCode enumerator, in enumerator order.
///
/// The texts are wasm-tools 1.263.0's (wasmparser 0.263.0), which the component model's
/// `test/binary/binary.wast` and wasm-tools' own `.wast` tests expect: their `assert_malformed` and
/// `assert_invalid` match the expected text against the reported message. Isoline's own wording
/// marks the cases wasm-tools reports differently or not at all.
inline constexpr auto DecodeErrorMessages = std::to_array<DecodeErrorRow>({
    // Also a vector count or a length larger than the bytes left, which wasm-tools reads on until
    // it runs out.
    { .code = DecodeErrorCode::UnexpectedEnd, .text = "unexpected end-of-file" },
    { .code = DecodeErrorCode::IntegerTooLarge, .text = "invalid var_u32: integer too large" },
    { .code = DecodeErrorCode::IntegerRepresentationTooLong,
      .text = "invalid var_u32: integer representation too long" },
    { .code = DecodeErrorCode::MalformedUtf8Encoding, .text = "malformed UTF-8 encoding" },
    { .code = DecodeErrorCode::MagicHeaderNotDetected,
      .text = "magic header not detected: bad magic number" },
    { .code = DecodeErrorCode::UnknownBinaryVersion,
      .text = "unknown binary version",
      .detail = DecodeErrorDetail::BinaryVersion },
    // A core module's preamble where a component's is expected: at the top level (Isoline's own, as
    // wasm-tools' parser takes either there) and in a component section.
    { .code = DecodeErrorCode::ExpectedComponentHeader, .text = "expected a version header for a component" },
    { .code = DecodeErrorCode::ExpectedModuleHeader, .text = "expected a version header for a module" },
    // With the id as its subject when the id is a byte below 0x80 that names no section.
    { .code = DecodeErrorCode::MalformedSectionId,
      .text = "malformed section id",
      .detail = DecodeErrorDetail::SectionId },
    // A section of a nested component that runs past the section holding that component.
    { .code = DecodeErrorCode::SectionTooLarge, .text = "section too large" },
    { .code = DecodeErrorCode::SectionSizeMismatch,
      .text = "section size mismatch: unexpected data at the end of the section" },
    // Isoline's own, for a section whose decoder P11 has not landed yet.
    { .code = DecodeErrorCode::UnimplementedSection,
      .text = "section not yet decoded",
      .detail = DecodeErrorDetail::SectionId },
    { .code = DecodeErrorCode::InvalidCoreInstanceKind,
      .text = "core instance",
      .detail = DecodeErrorDetail::LeadingByte },
    { .code = DecodeErrorCode::InvalidInstantiationArgKind,
      .text = "instantiation arg kind",
      .detail = DecodeErrorDetail::LeadingByte },
    { .code = DecodeErrorCode::InvalidCoreExternKind,
      .text = "external kind",
      .detail = DecodeErrorDetail::LeadingByte },
    { .code = DecodeErrorCode::InvalidInstanceKind,
      .text = "instance",
      .detail = DecodeErrorDetail::LeadingByte },
    // A sort or an extern type's kind outside what a component may name: wasm-tools' ExternalKind.
    { .code = DecodeErrorCode::InvalidExternalKind,
      .text = "component external kind",
      .detail = DecodeErrorDetail::LeadingByte },
    { .code = DecodeErrorCode::InvalidAliasTarget,
      .text = "alias",
      .detail = DecodeErrorDetail::LeadingByte },
    { .code = DecodeErrorCode::InvalidCoreExportAliasKind,
      .text = "core instance export kind",
      .detail = DecodeErrorDetail::LeadingByte },
    { .code = DecodeErrorCode::InvalidOuterAliasKind,
      .text = "component outer alias kind",
      .detail = DecodeErrorDetail::LeadingByte },
    { .code = DecodeErrorCode::InvalidTypeBound,
      .text = "type bound",
      .detail = DecodeErrorDetail::LeadingByte },
    { .code = DecodeErrorCode::InvalidOptionalExportType,
      .text = "optional component export type",
      .detail = DecodeErrorDetail::LeadingByte },
    { .code = DecodeErrorCode::InvalidNameKind,
      .text = "component name",
      .detail = DecodeErrorDetail::LeadingByte },
    { .code = DecodeErrorCode::InvalidNameAttribute,
      .text = "name option",
      .detail = DecodeErrorDetail::LeadingByte },
    { .code = DecodeErrorCode::DuplicateNameAttribute,
      .text = "option in name",
      .detail = DecodeErrorDetail::Attribute },
    { .code = DecodeErrorCode::FeatureDisabled,
      .text = "feature not enabled",
      .detail = DecodeErrorDetail::Feature },
    { .code = DecodeErrorCode::LimitExceeded, .text = "limit exceeded", .detail = DecodeErrorDetail::Limit },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: DecodeErrorMessages)
        {
            if (static_cast<std::size_t>(row.code) != index)
                return false;
            ++index;
        }
        return index == static_cast<std::size_t>(LastDecodeErrorCode) + 1;
    }(),
    "DecodeErrorMessages must hold one row per DecodeErrorCode, at the index of its enumerator");

/// A decode failure: what went wrong, where, and what it names.
///
/// A form gated by a feature the decoder does not admit (decodeAdmits()) is refused as
/// FeatureDisabled, naming the feature in disabled.
struct DecodeError
{
    /// Builds an error; the code and offset are required.
    /// @param code What went wrong.
    /// @param offset The absolute byte offset it is reported at.
    /// @param subject What it names, as its row's detail reads it; empty when it names nothing.
    /// @param disabled For FeatureDisabled, the feature the decoder does not admit; empty otherwise.
    constexpr DecodeError(DecodeErrorCode code,
                          uint64_t offset,
                          std::optional<uint64_t> subject = {},
                          std::optional<ComponentFeature> disabled = {}) noexcept:
        code { code }, offset { offset }, subject { subject }, disabled { disabled }
    {
    }

    DecodeErrorCode code;                     ///< What went wrong.
    uint64_t offset;                          ///< The absolute byte offset in the component.
    std::optional<uint64_t> subject;          ///< What it names; see DecodeErrorDetail.
    std::optional<ComponentFeature> disabled; ///< The feature not admitted, for FeatureDisabled.

    /// Two errors are equal when their code, offset, subject and disabled feature are.
    bool operator==(DecodeError const&) const = default;
};

/// The subject of a LimitExceeded error: the limit, and its value for the message.
/// @param limit The limit exceeded.
/// @param value Its value; a value of 2^56 or more is reported as 2^56 - 1.
/// @return The limit in bits 0-7 and the value in bits 8-63.
[[nodiscard]] constexpr uint64_t limitSubject(ComponentLimit limit, uint64_t value) noexcept
{
    constexpr auto MaxValue = (uint64_t { 1 } << 56U) - 1;
    return static_cast<uint64_t>(limit) | ((value < MaxValue ? value : MaxValue) << 8U);
}

/// The limit a LimitExceeded error reports.
/// @param error An error.
/// @return Its limit, or nothing for an error of another code.
[[nodiscard]] constexpr std::optional<ComponentLimit> exceededLimit(DecodeError const& error) noexcept
{
    if (error.code != DecodeErrorCode::LimitExceeded || !error.subject)
        return std::nullopt;
    return static_cast<ComponentLimit>(*error.subject & 0xFFU);
}

/// The message wasm-tools reports for @p error, as DecodeErrorMessages and its row's detail build it.
/// @param error The error to describe.
/// @return The message the component suites' assertions match.
[[nodiscard]] std::string specMessage(DecodeError const& error);

/// Decodes a component.
///
/// The decoder copies @p bytes into the pool before it reads them, so the pool never aliases the
/// caller's memory. It reads sections as wasm-tools does, each bounded by its declared size, and
/// nested components on an explicit stack of frames, never by recursion; nesting deeper than
/// ComponentLimits::maxNesting is refused. Vector counts never reserve memory: a vector grows as its
/// elements decode, so a count larger than its bytes fails with UnexpectedEnd having allocated
/// nothing for it. Embedded core modules are not decoded (D10): only their range and preamble are
/// checked. Sections 3 (core types), 7 (types) and 8 (canon) are refused as UnimplementedSection
/// until P11 Task 2; sections 9 (start) and 12 (values) as FeatureDisabled 🪙.
/// @param bytes The component's bytes, from its preamble on.
/// @param features The gated features whose forms are admitted (decodeAdmits()).
/// @param limits The caps to enforce: maxComponentBytes, maxNesting, maxDefinitions, maxNameBytes.
/// @return The pool, or the first error, at its absolute offset.
[[nodiscard]] std::expected<ComponentPool, DecodeError> decodeComponent(std::span<std::byte const> bytes,
                                                                        ComponentFeatureSet features,
                                                                        ComponentLimits const& limits);

/// The embedded core modules in definition order (pre-order over nested components): what the
/// compile helper keys by ordinal (D10).
/// @param bytes The component's bytes, from its preamble on.
/// @param features The gated features whose forms are admitted, as decodeComponent() takes them.
/// @param limits The caps to enforce, as decodeComponent() takes them.
/// @return The modules, each a view into @p bytes, or the error decodeComponent() reports.
[[nodiscard]] std::expected<std::vector<std::span<std::byte const>>, DecodeError> splitCoreModules(
    std::span<std::byte const> bytes, ComponentFeatureSet features, ComponentLimits const& limits);

} // namespace isoline::component::binary
