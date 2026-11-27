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
    InvalidDefinedType,
    InvalidValueType,
    InvalidOptionalValueType,
    ZeroByteRequired,
    InvalidResultCount,
    InvalidResults,
    InvalidDestructor,
    InvalidCoreValueType,
    InvalidTypeDeclaration,
    InvalidModuleTypeDeclaration,
    InvalidCoreOuterAliasKind,
    InvalidCoreOuterAliasTarget,
    InvalidNonFinalSubType,
    InvalidCoreTypeForm,
    MalformedCoreType,
    InvalidCanonFunction,
    InvalidCanonSort,
    InvalidCanonOption,
    InvalidBooleanValue,
    NonZeroReservedByte,
    CoreModuleRefused,
};

/// The last DecodeErrorCode enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastDecodeErrorCode = DecodeErrorCode::CoreModuleRefused;

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
    CoreError,     ///< The core decoder's message for the isoline::binary::DecodeErrorCode in subject.
    /// "invalid leading byte (0x{byte:x}) for canonical function {text}", the subject canonSortSubject().
    CanonSort,
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
    // Isoline's own, for a section Isoline decodes nothing of: the 🪙 start and values sections, which their
    // gate refuses first while P11 refuses 🪙.
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
    { .code = DecodeErrorCode::InvalidDefinedType,
      .text = "component defined type",
      .detail = DecodeErrorDetail::LeadingByte },
    // A valtype that is neither a primitive nor a non-negative s33 type index.
    { .code = DecodeErrorCode::InvalidValueType,
      .text = "component value type",
      .detail = DecodeErrorDetail::LeadingByte },
    { .code = DecodeErrorCode::InvalidOptionalValueType,
      .text = "optional component value type",
      .detail = DecodeErrorDetail::LeadingByte },
    // The byte that ends a variant case, once its `refines`.
    { .code = DecodeErrorCode::ZeroByteRequired,
      .text = "zero byte required",
      .detail = DecodeErrorDetail::LeadingByte },
    { .code = DecodeErrorCode::InvalidResultCount,
      .text = "number of results",
      .detail = DecodeErrorDetail::LeadingByte },
    { .code = DecodeErrorCode::InvalidResults,
      .text = "component function results",
      .detail = DecodeErrorDetail::LeadingByte },
    { .code = DecodeErrorCode::InvalidDestructor,
      .text = "resource destructor",
      .detail = DecodeErrorDetail::LeadingByte },
    // Isoline's own: wasm-tools reads any core value type there and refuses it in validation.
    { .code = DecodeErrorCode::InvalidCoreValueType,
      .text = "core value type",
      .detail = DecodeErrorDetail::LeadingByte },
    { .code = DecodeErrorCode::InvalidTypeDeclaration,
      .text = "component or instance type declaration",
      .detail = DecodeErrorDetail::LeadingByte },
    { .code = DecodeErrorCode::InvalidModuleTypeDeclaration,
      .text = "type definition",
      .detail = DecodeErrorDetail::LeadingByte },
    { .code = DecodeErrorCode::InvalidCoreOuterAliasKind,
      .text = "outer alias kind",
      .detail = DecodeErrorDetail::LeadingByte },
    { .code = DecodeErrorCode::InvalidCoreOuterAliasTarget,
      .text = "outer alias target",
      .detail = DecodeErrorDetail::LeadingByte },
    { .code = DecodeErrorCode::InvalidNonFinalSubType,
      .text = "non-final sub type",
      .detail = DecodeErrorDetail::LeadingByte },
    // A core type whose composite type byte is none: wasmparser's read_composite_type.
    { .code = DecodeErrorCode::InvalidCoreTypeForm,
      .text = "type",
      .detail = DecodeErrorDetail::LeadingByte },
    // A core function or extern type the core decoder refused, in its words; and a core GC type (a
    // rec group, a subtype, a struct or an array), refused in the words the core decoder refuses one
    // with, as Isoline's core decodes no GC.
    { .code = DecodeErrorCode::MalformedCoreType,
      .text = "malformed core type",
      .detail = DecodeErrorDetail::CoreError },
    { .code = DecodeErrorCode::InvalidCanonFunction,
      .text = "canonical function",
      .detail = DecodeErrorDetail::LeadingByte },
    { .code = DecodeErrorCode::InvalidCanonSort,
      .text = "canonical function",
      .detail = DecodeErrorDetail::CanonSort },
    { .code = DecodeErrorCode::InvalidCanonOption,
      .text = "canonical option",
      .detail = DecodeErrorDetail::LeadingByte },
    // `async?`, `sh?` and the reserved byte read as a boolean; the texts are the component model's
    // test/binary/binary.wast's, whose reserved byte was the boolean `cancellable` once.
    { .code = DecodeErrorCode::InvalidBooleanValue, .text = "invalid boolean value" },
    { .code = DecodeErrorCode::NonZeroReservedByte, .text = "non-zero reserved byte" },
    // Isoline's own, from canonicalizeComponent(): the ICoreModuleCanonicalizer refused the embedded
    // core module at the error's offset.
    { .code = DecodeErrorCode::CoreModuleRefused, .text = "core module refused by its canonicalizer" },
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

/// The subject of an InvalidCanonSort error: the built-in whose sort byte it is, and the byte.
/// @param opcode The built-in's opcode, which names its CanonBuiltinTable row.
/// @param byte The sort byte read.
/// @return The opcode in bits 8-15 and the byte in bits 0-7.
[[nodiscard]] constexpr uint64_t canonSortSubject(uint8_t opcode, uint8_t byte) noexcept
{
    return (uint64_t { opcode } << 8U) | byte;
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
/// ComponentLimits::maxNesting is refused. Every item it decodes (a definition, a nested component,
/// an element of a vector inside a definition) is charged against ComponentLimits::maxDecodedItems
/// before it is read, so what the pool holds beyond the names it copies is bounded by that count,
/// whatever the input's shape. Vector counts never reserve memory: a vector grows as its
/// elements decode, so a count larger than its bytes fails with UnexpectedEnd having allocated
/// nothing for it. Embedded core modules are not decoded (D10): only their range and preamble are
/// checked. Value types are decoded as encoded: what Binary.md's grammar conditions on their counts
/// (a record of no fields, flags past 32) is refused by validation, the stage wasm-tools reports it
/// at. Component and instance types are scopes of the pool like nested components, read on an
/// explicit stack too and counted against maxNesting; value types nest only through type indices, so
/// their depth and effective size are the validator's to bound (maxTypeDepth, maxTypeSize). Core types
/// are read by the core decoder under every core feature Isoline has (FeatureSet::all()); the
/// validator narrows them to the engine's. Sections 9 (start) and 12 (values) are refused as
/// FeatureDisabled 🪙.
/// @param bytes The component's bytes, from its preamble on.
/// @param features The gated features whose forms are admitted (decodeAdmits()).
/// @param limits The caps to enforce: maxComponentBytes, maxNesting, maxTypes (the types and core types
///               a scope defines, a lower bound of its index spaces of sort type), maxDefinitions,
///               maxDecodedItems and maxNameBytes.
/// @return The pool, or the first error, at its absolute offset.
[[nodiscard]] std::expected<ComponentPool, DecodeError> decodeComponent(std::span<std::byte const> bytes,
                                                                        ComponentFeatureSet features,
                                                                        ComponentLimits const& limits);

/// Where one definition sits in a pool: its scope, and its index among that scope's definitions.
struct DefinitionPosition
{
    uint32_t scope = 0;         ///< The index of its scope in ComponentPool::components.
    std::size_t definition = 0; ///< Its index in that scope's Component::definitions.

    /// Two positions are equal when their scopes and indices are.
    bool operator==(DefinitionPosition const&) const = default;
};

/// The embedded core modules of @p pool in definition order, pre-order over nested components: the
/// order of their ordinals (D10). Walks an explicit stack, never recursively.
///
/// Precondition: every NestedComponent of @p pool names an entry after its own scope, as the decoder
/// and testing::ComponentBuilder lay pools out.
/// @param pool A decoded or built pool.
/// @return The position of each CoreModuleDefinition, the first ordinal first.
[[nodiscard]] std::vector<DefinitionPosition> coreModulePositions(ComponentPool const& pool);

/// The embedded core modules in definition order (pre-order over nested components): what the
/// compile helper keys by ordinal (D10).
/// @param bytes The component's bytes, from its preamble on.
/// @param features The gated features whose forms are admitted, as decodeComponent() takes them.
/// @param limits The caps to enforce, as decodeComponent() takes them.
/// @return The modules, each a view into @p bytes, or the error decodeComponent() reports.
[[nodiscard]] std::expected<std::vector<std::span<std::byte const>>, DecodeError> splitCoreModules(
    std::span<std::byte const> bytes, ComponentFeatureSet features, ComponentLimits const& limits);

} // namespace isoline::component::binary
