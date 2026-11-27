// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>

#include <isoline/api/Module.hpp>
#include <isoline/component/Features.hpp>
#include <isoline/component/Limits.hpp>
#include <isoline/component/binary/Component.hpp>
#include <isoline/component/binary/Decoder.hpp>
#include <isoline/component/validate/ValidatedComponent.hpp>

namespace isoline::component::validate
{

/// Why the component validator refused a component.
///
/// The enumerators are the rows of ValidationErrorMessages, in the same order; adding an error is
/// adding an enumerator here and a row there.
enum class ValidationErrorCode : uint8_t
{
    LimitExceeded,
    FeatureDisabled,
    NotYetValidated,
    CoreModuleRefused,
    TypeIndexOutOfBounds,
    NotDefinedType,
    NotResourceType,
    NotFunctionType,
    NotComponentType,
    NotInstanceType,
    InvalidOuterAliasCount,
    EmptyRecord,
    EmptyVariant,
    EmptyTuple,
    EmptyFlags,
    TooManyFlags,
    EmptyEnum,
    EmptyFixedList,
    FixedListTooLong,
    EmptyLabel,
    LabelNotKebabCase,
    DuplicateLabel,
    InvalidMapKey,
    HandleInMapValue,
    BorrowInResult,
    ResourceOutsideComponent,
    InvalidResourceRepresentation,
    ResourceRepresentationI64,
};

/// The last ValidationErrorCode enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastValidationErrorCode = ValidationErrorCode::ResourceRepresentationI64;

/// How a validation error's message is built from its row's text and the error's fields.
enum class ValidationErrorDetail : uint8_t
{
    None,     ///< The row's text, as it is.
    Index,    ///< The text formatted with the subject, an index.
    Label,    ///< The text formatted with the LabelKind in subject, its name and the previous name.
    Deferred, ///< The text formatted with the DeferredForm in subject's DeferredFormTable text.
    Limit,    ///< The exceeded limit's message (limitMessage()), the subject from binary::limitSubject().
    Refusal,  ///< The error's name: a gated form's refusal, wasm-tools' text for that site.
};

/// One row of ValidationErrorMessages: an error and how its message reads.
struct ValidationErrorRow
{
    ValidationErrorCode code;                                   ///< The error; equal to the row's index.
    std::string_view text;                                      ///< Its text, a std::format string.
    ValidationErrorDetail detail = ValidationErrorDetail::None; ///< How the message uses the fields.
};

/// Every validation error, one row per ValidationErrorCode enumerator, in enumerator order.
///
/// The texts are wasm-tools 1.263.0's (wasmparser 0.263.0, src/validator/component.rs), which the
/// component model's suites and wasm-tools' own `.wast` tests expect: their `assert_invalid` matches
/// the expected text against the reported message. A Label row formats {0} as the label kind's
/// description, {1} as the name, {2} as the kind's noun and {3} as the previous name.
inline constexpr auto ValidationErrorMessages = std::to_array<ValidationErrorRow>({
    { .code = ValidationErrorCode::LimitExceeded,
      .text = "limit exceeded",
      .detail = ValidationErrorDetail::Limit },
    { .code = ValidationErrorCode::FeatureDisabled,
      .text = "feature not enabled",
      .detail = ValidationErrorDetail::Refusal },
    // Isoline's own, for a definition whose validation lands with Task 7: refused, never accepted
    // unchecked.
    { .code = ValidationErrorCode::NotYetValidated,
      .text = "not yet validated: {0}",
      .detail = ValidationErrorDetail::Deferred },
    // Isoline's own: the ICoreModuleValidator refused the embedded core module of this ordinal.
    { .code = ValidationErrorCode::CoreModuleRefused,
      .text = "invalid core module {0}",
      .detail = ValidationErrorDetail::Index },
    { .code = ValidationErrorCode::TypeIndexOutOfBounds,
      .text = "unknown type {0}: type index out of bounds",
      .detail = ValidationErrorDetail::Index },
    { .code = ValidationErrorCode::NotDefinedType,
      .text = "type index {0} is not a defined type",
      .detail = ValidationErrorDetail::Index },
    { .code = ValidationErrorCode::NotResourceType,
      .text = "type index {0} is not a resource type",
      .detail = ValidationErrorDetail::Index },
    { .code = ValidationErrorCode::NotFunctionType,
      .text = "type index {0} is not a function type",
      .detail = ValidationErrorDetail::Index },
    { .code = ValidationErrorCode::NotComponentType,
      .text = "type index {0} is not a component type",
      .detail = ValidationErrorDetail::Index },
    { .code = ValidationErrorCode::NotInstanceType,
      .text = "type index {0} is not an instance type",
      .detail = ValidationErrorDetail::Index },
    { .code = ValidationErrorCode::InvalidOuterAliasCount,
      .text = "invalid outer alias count of {0}",
      .detail = ValidationErrorDetail::Index },
    // Binary.md's grammar conditions on counts, which wasm-tools checks in validation
    // (component.rs:4198-4343).
    { .code = ValidationErrorCode::EmptyRecord, .text = "record type must have at least one field" },
    { .code = ValidationErrorCode::EmptyVariant, .text = "variant type must have at least one case" },
    { .code = ValidationErrorCode::EmptyTuple, .text = "tuple type must have at least one type" },
    { .code = ValidationErrorCode::EmptyFlags, .text = "flags must have at least one entry" },
    { .code = ValidationErrorCode::TooManyFlags, .text = "cannot have more than 32 flags" },
    { .code = ValidationErrorCode::EmptyEnum, .text = "enum type must have at least one variant" },
    { .code = ValidationErrorCode::EmptyFixedList,
      .text = "Fixed-length lists must have more than zero elements" },
    { .code = ValidationErrorCode::FixedListTooLong,
      .text = "fixed-length list element count exceeds limit of {0}",
      .detail = ValidationErrorDetail::Index },
    { .code = ValidationErrorCode::EmptyLabel,
      .text = "{0} name cannot be empty",
      .detail = ValidationErrorDetail::Label },
    { .code = ValidationErrorCode::LabelNotKebabCase,
      .text = "{0} name `{1}` is not in kebab case",
      .detail = ValidationErrorDetail::Label },
    { .code = ValidationErrorCode::DuplicateLabel,
      .text = "{0} name `{1}` conflicts with previous {2} name `{3}`",
      .detail = ValidationErrorDetail::Label },
    { .code = ValidationErrorCode::InvalidMapKey, .text = "invalid map key type" },
    { .code = ValidationErrorCode::HandleInMapValue, .text = "map value type cannot contain a handle" },
    { .code = ValidationErrorCode::BorrowInResult, .text = "function result cannot contain a `borrow` type" },
    { .code = ValidationErrorCode::ResourceOutsideComponent,
      .text = "resources can only be defined within a concrete component" },
    { .code = ValidationErrorCode::InvalidResourceRepresentation,
      .text = "resources can only be represented by `i32` or `i64`" },
    // 🐘's refusal in wasm-tools' words for a resource; disabled names ComponentFeature::Memory64.
    { .code = ValidationErrorCode::ResourceRepresentationI64,
      .text = "resources with `i64` require the `cm64` feature to be enabled" },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: ValidationErrorMessages)
        {
            if (static_cast<std::size_t>(row.code) != index)
                return false;
            ++index;
        }
        return index == static_cast<std::size_t>(LastValidationErrorCode) + 1;
    }(),
    "ValidationErrorMessages must hold one row per ValidationErrorCode, at the index of its enumerator");

/// A form validateComponent() does not validate yet, and so refuses as NotYetValidated: never accepted
/// unchecked. Task 7 (and Task 5, for names) removes a row as it validates the form, and the compiler
/// then names each place that refuses it.
enum class DeferredForm : uint8_t
{
    CanonicalFunctions,
    CoreInstances,
    ComponentInstances,
    ExportDefinitions,
    InstanceExportAliases,
    CoreInstanceExportAliases,
    OuterAliasesOfOtherSorts,
    OuterAliasesIntoComponents,
    CoreTypeDefinitions,
    ResourceDestructors,
    CoreModuleTypes,
    ExternNames,
    DuplicateExternNames,
};

/// One row of DeferredFormTable: a deferred form and how its refusal names it.
struct DeferredFormRow
{
    DeferredForm form;     ///< The form; equal to the row's index.
    std::string_view text; ///< What "not yet validated: …" names.
};

/// Every deferred form, one row per DeferredForm enumerator, in enumerator order.
inline constexpr auto DeferredFormTable = std::to_array<DeferredFormRow>({
    { .form = DeferredForm::CanonicalFunctions, .text = "canonical function definitions" },
    { .form = DeferredForm::CoreInstances, .text = "core instances" },
    { .form = DeferredForm::ComponentInstances, .text = "component instances" },
    { .form = DeferredForm::ExportDefinitions, .text = "export definitions" },
    { .form = DeferredForm::InstanceExportAliases, .text = "aliases of instance exports" },
    { .form = DeferredForm::CoreInstanceExportAliases, .text = "aliases of core instance exports" },
    { .form = DeferredForm::OuterAliasesOfOtherSorts, .text = "outer aliases of this sort" },
    // The rule that a type crossing into a component refers to no resource defined outside it.
    { .form = DeferredForm::OuterAliasesIntoComponents,
      .text = "outer aliases into a component of a type that may refer to a resource" },
    { .form = DeferredForm::CoreTypeDefinitions, .text = "core type definitions" },
    { .form = DeferredForm::ResourceDestructors, .text = "resource destructors" },
    { .form = DeferredForm::CoreModuleTypes, .text = "core module types" },
    // The externname grammar beyond a plain label (Task 5's parseExternName).
    { .form = DeferredForm::ExternNames, .text = "extern names other than a plain label" },
    { .form = DeferredForm::DuplicateExternNames, .text = "extern names not strongly unique" },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: DeferredFormTable)
        {
            if (static_cast<std::size_t>(row.form) != index)
                return false;
            ++index;
        }
        return index == static_cast<std::size_t>(DeferredForm::DuplicateExternNames) + 1;
    }(),
    "DeferredFormTable must hold one row per DeferredForm, at the index of its enumerator");

/// A gated form the validator refuses, at one site: a FeatureDisabled refusal reads its row's text.
enum class GatedForm : uint8_t
{
    ErrorContextType,
    FixedLengthList,
    Stream,
    Future,
    Map,
    AsyncFunction,
    Value,
    Implements,
    VersionSuffix,
    ExternalId,
    AsyncOption,
    CallbackOption,
};

/// One row of GatedFormTable: a gated form, its feature, and wasm-tools' refusal of it at that site.
struct GatedFormRow
{
    GatedForm form;           ///< The form; equal to the row's index.
    ComponentFeature feature; ///< The feature that admits it.
    std::string_view text;    ///< The refusal, wasm-tools 1.263.0's for the site (component.rs line).
};

/// Every gated form but the canonical built-ins (CanonRefusalTable), in GatedForm's order.
inline constexpr auto GatedFormTable = std::to_array<GatedFormRow>({
    { .form = GatedForm::ErrorContextType, // 4664
      .feature = ComponentFeature::ErrorContext,
      .text = "`error-context` requires the component model error-context feature" },
    { .form = GatedForm::FixedLengthList, // 4061
      .feature = ComponentFeature::FixedLengthLists,
      .text = "Fixed-length lists require the component model fixed-length lists feature" },
    { .form = GatedForm::Stream, // 4151
      .feature = ComponentFeature::Async,
      .text = "`stream` requires the component model async feature" },
    { .form = GatedForm::Future, // 4132
      .feature = ComponentFeature::Async,
      .text = "`future` requires the component model async feature" },
    { .form = GatedForm::Map, // 4007
      .feature = ComponentFeature::Map,
      .text = "Maps require the component model map feature" },
    { .form = GatedForm::AsyncFunction, // 3150
      .feature = ComponentFeature::Async,
      .text = "async component functions require the component model async feature" },
    { .form = GatedForm::Value, // 2602, 4654
      .feature = ComponentFeature::Values,
      .text = "support for component model `value`s is not enabled" },
    { .form = GatedForm::Implements, // 4758
      .feature = ComponentFeature::Implements,
      .text = "the `cm-implements` feature is not active" },
    { .form = GatedForm::VersionSuffix, // 4735
      .feature = ComponentFeature::CanonicalInterfaceNames,
      .text = "the `cm-canon-names` feature is not active" },
    // wasm-tools gates external-id under cm-implements and words it so.
    { .form = GatedForm::ExternalId, // 4786
      .feature = ComponentFeature::ExternalId,
      .text = "the `cm-implements` feature is not active" },
    { .form = GatedForm::AsyncOption, // 2749
      .feature = ComponentFeature::Async,
      .text = "canonical option `async` requires the component model async feature" },
    // Isoline's own: wasm-tools refuses a callback without `async` as "cannot specify callback
    // without async", which Task 7's option rules check; the gate words it as its `async` sibling.
    { .form = GatedForm::CallbackOption,
      .feature = ComponentFeature::Async,
      .text = "canonical option `callback` requires the component model async feature" },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: GatedFormTable)
        {
            if (static_cast<std::size_t>(row.form) != index)
                return false;
            ++index;
        }
        return index == static_cast<std::size_t>(GatedForm::CallbackOption) + 1;
    }(),
    "GatedFormTable must hold one row per GatedForm, at the index of its enumerator");

/// The table row of @p form.
/// @param form A gated form.
/// @return Its row in GatedFormTable.
[[nodiscard]] constexpr GatedFormRow const& gatedFormRow(GatedForm form) noexcept
{
    return GatedFormTable[static_cast<std::size_t>(form)];
}

/// The gated form of each compound defvaltype, by its binary::DefinedValueTypeTable row: empty for an
/// ungated one.
inline constexpr auto DefinedTypeGates = std::to_array<std::optional<GatedForm>>({
    std::nullopt,               // record
    std::nullopt,               // variant
    std::nullopt,               // list
    GatedForm::FixedLengthList, // list N
    std::nullopt,               // tuple
    std::nullopt,               // flags
    std::nullopt,               // enum
    std::nullopt,               // option
    std::nullopt,               // result
    std::nullopt,               // own
    std::nullopt,               // borrow
    GatedForm::Stream,          // stream
    GatedForm::Future,          // future
    GatedForm::Map,             // map
});

static_assert(
    [] {
        if (DefinedTypeGates.size() != binary::DefinedValueTypeTable.size())
            return false;
        for (auto const index: std::views::iota(std::size_t { 0 }, DefinedTypeGates.size()))
        {
            auto const& gate = binary::DefinedValueTypeTable[index].gate;
            auto const& form = DefinedTypeGates[index];
            if (gate.has_value() != form.has_value() || (form && gatedFormRow(*form).feature != *gate))
                return false;
        }
        return true;
    }(),
    "DefinedTypeGates names a gated form, of the same feature, exactly for each gated defvaltype");

/// The gated form of each name attribute, by its binary::Attribute alternative.
inline constexpr auto AttributeGates =
    std::to_array<GatedForm>({ GatedForm::Implements, GatedForm::VersionSuffix, GatedForm::ExternalId });

static_assert(
    [] {
        if (AttributeGates.size() != binary::NameAttributeTable.size())
            return false;
        for (auto const index: std::views::iota(std::size_t { 0 }, AttributeGates.size()))
        {
            if (gatedFormRow(AttributeGates[index]).feature != binary::NameAttributeTable[index].gate)
                return false;
        }
        return true;
    }(),
    "AttributeGates names a gated form of each attribute's feature, in Attribute's order");

/// The gated form of each canonical option, by CanonOptionKind: empty for an ungated one.
inline constexpr auto CanonOptionGates = std::to_array<std::optional<GatedForm>>({
    std::nullopt,              // string-encoding=utf8
    std::nullopt,              // string-encoding=utf16
    std::nullopt,              // string-encoding=latin1+utf16
    std::nullopt,              // memory
    std::nullopt,              // realloc
    std::nullopt,              // post-return
    GatedForm::AsyncOption,    // async
    GatedForm::CallbackOption, // callback
});

static_assert(
    [] {
        if (CanonOptionGates.size() != binary::CanonOptionTable.size())
            return false;
        for (auto const index: std::views::iota(std::size_t { 0 }, CanonOptionGates.size()))
        {
            auto const& gate = binary::CanonOptionTable[index].gate;
            auto const& form = CanonOptionGates[index];
            if (gate.has_value() != form.has_value() || (form && gatedFormRow(*form).feature != *gate))
                return false;
        }
        return true;
    }(),
    "CanonOptionGates names a gated form, of the same feature, exactly for each gated option");

/// One row of CanonRefusalTable: a gated canonical built-in and wasm-tools' refusal of it.
struct CanonRefusalRow
{
    binary::CanonBuiltin builtin; ///< The built-in.
    std::string_view text;        ///< Its refusal when its row's gate is not admitted.
};

/// The refusal of every gated canonical built-in, wasm-tools 1.263.0's (component.rs:1430-2494) for
/// the gate binary::CanonBuiltinTable gives it.
inline constexpr auto CanonRefusalTable = std::to_array<CanonRefusalRow>({
    { .builtin = binary::CanonBuiltin::TaskCancel,
      .text = "`task.cancel` requires the component model async feature" },
    { .builtin = binary::CanonBuiltin::SubtaskCancel,
      .text = "`subtask.cancel` requires the component model async feature" },
    { .builtin = binary::CanonBuiltin::TaskReturn,
      .text = "`task.return` requires the component model async feature" },
    { .builtin = binary::CanonBuiltin::ContextGet,
      .text = "`context.get` requires the component model async feature" },
    { .builtin = binary::CanonBuiltin::ContextSet,
      .text = "`context.set` requires the component model async feature" },
    { .builtin = binary::CanonBuiltin::ThreadYield,
      .text = "`thread.yield` requires the component model async feature" },
    { .builtin = binary::CanonBuiltin::SubtaskDrop,
      .text = "`subtask.drop` requires the component model async feature" },
    { .builtin = binary::CanonBuiltin::StreamNew,
      .text = "`stream.new` requires the component model async feature" },
    { .builtin = binary::CanonBuiltin::StreamRead,
      .text = "`stream.read` requires the component model async feature" },
    { .builtin = binary::CanonBuiltin::StreamWrite,
      .text = "`stream.write` requires the component model async feature" },
    { .builtin = binary::CanonBuiltin::StreamCancelRead,
      .text = "`stream.cancel-read` requires the component model async feature" },
    { .builtin = binary::CanonBuiltin::StreamCancelWrite,
      .text = "`stream.cancel-write` requires the component model async feature" },
    { .builtin = binary::CanonBuiltin::StreamDropReadable,
      .text = "`stream.drop-readable` requires the component model async feature" },
    { .builtin = binary::CanonBuiltin::StreamDropWritable,
      .text = "`stream.drop-writable` requires the component model async feature" },
    { .builtin = binary::CanonBuiltin::FutureNew,
      .text = "`future.new` requires the component model async feature" },
    { .builtin = binary::CanonBuiltin::FutureRead,
      .text = "`future.read` requires the component model async feature" },
    { .builtin = binary::CanonBuiltin::FutureWrite,
      .text = "`future.write` requires the component model async feature" },
    { .builtin = binary::CanonBuiltin::FutureCancelRead,
      .text = "`future.cancel-read` requires the component model async feature" },
    { .builtin = binary::CanonBuiltin::FutureCancelWrite,
      .text = "`future.cancel-write` requires the component model async feature" },
    { .builtin = binary::CanonBuiltin::FutureDropReadable,
      .text = "`future.drop-readable` requires the component model async feature" },
    { .builtin = binary::CanonBuiltin::FutureDropWritable,
      .text = "`future.drop-writable` requires the component model async feature" },
    { .builtin = binary::CanonBuiltin::ErrorContextNew,
      .text = "`error-context.new` requires the component model error-context feature" },
    { .builtin = binary::CanonBuiltin::ErrorContextDebugMessage,
      .text = "`error-context.debug-message` requires the component model error-context feature" },
    { .builtin = binary::CanonBuiltin::ErrorContextDrop,
      .text = "`error-context.drop` requires the component model error-context feature" },
    { .builtin = binary::CanonBuiltin::WaitableSetNew,
      .text = "`waitable-set.new` requires the component model async feature" },
    { .builtin = binary::CanonBuiltin::WaitableSetWait,
      .text = "`waitable-set.wait` requires the component model async feature" },
    { .builtin = binary::CanonBuiltin::WaitableSetPoll,
      .text = "`waitable-set.poll` requires the component model async feature" },
    { .builtin = binary::CanonBuiltin::WaitableSetDrop,
      .text = "`waitable-set.drop` requires the component model async feature" },
    { .builtin = binary::CanonBuiltin::WaitableJoin,
      .text = "`waitable.join` requires the component model async feature" },
    { .builtin = binary::CanonBuiltin::BackpressureInc,
      .text = "`backpressure.inc` requires the component model async feature" },
    { .builtin = binary::CanonBuiltin::BackpressureDec,
      .text = "`backpressure.dec` requires the component model async feature" },
    { .builtin = binary::CanonBuiltin::ThreadIndex,
      .text = "`thread.index` requires the component model threading feature" },
    { .builtin = binary::CanonBuiltin::ThreadNewIndirect,
      .text = "`thread.new-indirect` requires the component model threading feature" },
    { .builtin = binary::CanonBuiltin::ThreadResumeLater,
      .text = "`thread.resume-later` requires the component model threading feature" },
    { .builtin = binary::CanonBuiltin::ThreadSuspend,
      .text = "`thread.suspend` requires the component model threading feature" },
    { .builtin = binary::CanonBuiltin::ThreadSuspendThenResume,
      .text = "`thread.suspend-then-resume` requires the component model threading feature" },
    { .builtin = binary::CanonBuiltin::ThreadYieldThenResume,
      .text = "`thread.yield-then-resume` requires the component model threading feature" },
    { .builtin = binary::CanonBuiltin::ThreadSuspendThenPromote,
      .text = "`thread.suspend-then-promote` requires the component model threading feature" },
    { .builtin = binary::CanonBuiltin::ThreadYieldThenPromote,
      .text = "`thread.yield-then-promote` requires the component model threading feature" },
    { .builtin = binary::CanonBuiltin::StreamForward,
      .text = "`stream.forward` requires the component model forward feature" },
    { .builtin = binary::CanonBuiltin::FutureForward,
      .text = "`future.forward` requires the component model forward feature" },
    { .builtin = binary::CanonBuiltin::ThreadSpawnRef,
      .text = "`thread.spawn-ref` requires the shared-everything-threads proposal" },
    { .builtin = binary::CanonBuiltin::ThreadSpawnIndirect,
      .text = "`thread.spawn-indirect` requires the shared-everything-threads proposal" },
    { .builtin = binary::CanonBuiltin::ThreadAvailableParallelism,
      .text = "`thread.available_parallelism` requires the shared-everything-threads proposal" },
});

static_assert(
    [] {
        for (auto const& builtin: binary::CanonBuiltinTable)
        {
            auto count = 0;
            for (auto const& row: CanonRefusalTable)
                count += row.builtin == builtin.builtin ? 1 : 0;
            if (count != (builtin.gate ? 1 : 0))
                return false;
        }
        return true;
    }(),
    "CanonRefusalTable holds one row for each gated canonical built-in, and none for the others");

/// What a label names, for the texts of the label rules.
enum class LabelKind : uint8_t
{
    RecordField,
    VariantCase,
    Flag,
    EnumTag,
    FunctionParameter,
};

/// One row of LabelKindTable: a label kind and the words its refusals use.
struct LabelKindRow
{
    LabelKind kind;               ///< The kind; equal to the row's index.
    std::string_view description; ///< What wasm-tools calls a label of the kind.
    std::string_view noun;        ///< What it calls the previous one in a conflict.
};

/// Every label kind, one row per LabelKind enumerator, in enumerator order (component.rs:4198-4343,
/// 3141-3200).
inline constexpr auto LabelKindTable = std::to_array<LabelKindRow>({
    { .kind = LabelKind::RecordField, .description = "record field", .noun = "field" },
    { .kind = LabelKind::VariantCase, .description = "variant case", .noun = "case" },
    { .kind = LabelKind::Flag, .description = "flag", .noun = "flag" },
    { .kind = LabelKind::EnumTag, .description = "enum tag", .noun = "tag" },
    { .kind = LabelKind::FunctionParameter, .description = "function parameter", .noun = "parameter" },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: LabelKindTable)
        {
            if (static_cast<std::size_t>(row.kind) != index)
                return false;
            ++index;
        }
        return index == static_cast<std::size_t>(LabelKind::FunctionParameter) + 1;
    }(),
    "LabelKindTable must hold one row per LabelKind, at the index of its enumerator");

/// An embedded core module the ICoreModuleValidator refused: its ordinal (D10), and why.
struct CoreModuleError
{
    uint32_t ordinal = 0;          ///< The module's ordinal: its position among the embedded modules.
    isoline::api::LoadError error; ///< The core validator's refusal.
};

/// A validation failure: what went wrong, at which definition, and what it names.
///
/// The decoded AST records no byte offsets, so an error names the definition it was found at.
struct ValidationError
{
    ValidationErrorCode code;            ///< What went wrong.
    binary::DefinitionPosition position; ///< The definition at which it was found.
    std::optional<uint64_t> subject {};  ///< An index, a LabelKind or a limit; see ValidationErrorDetail.
    std::string name {};                 ///< The name or gated form it names, if any.
    std::string previous {};             ///< For DuplicateLabel: the earlier, conflicting name.
    std::optional<ComponentFeature> disabled {};  ///< For FeatureDisabled: the feature not admitted.
    std::optional<CoreModuleError> coreModule {}; ///< For CoreModuleRefused: the module and its refusal.
};

/// The limit a LimitExceeded error reports.
/// @param error An error.
/// @return Its limit, or nothing for an error of another code.
[[nodiscard]] std::optional<ComponentLimit> exceededLimit(ValidationError const& error) noexcept;

/// The message wasm-tools reports for @p error, as ValidationErrorMessages and its row's detail build it.
/// @param error The error to describe.
/// @return The message the component suites' assertions match.
[[nodiscard]] std::string specMessage(ValidationError const& error);

/// Validates an embedded core module without compiling it (D10): the seam through which the component
/// validator reaches core validation, injected so that the engine's features and limits apply.
class ICoreModuleValidator
{
  public:
    virtual ~ICoreModuleValidator() = default;

    /// Decodes and validates one core module, never compiling it.
    /// @param bytes The module's bytes, from its preamble on.
    /// @return The validated module, or why it was refused.
    [[nodiscard]] virtual std::expected<CoreModuleShape, isoline::api::LoadError> validate(
        std::span<std::byte const> bytes) = 0;

  protected:
    ICoreModuleValidator() = default;
    ICoreModuleValidator(ICoreModuleValidator const&) = default;
    ICoreModuleValidator& operator=(ICoreModuleValidator const&) = default;
    ICoreModuleValidator(ICoreModuleValidator&&) = default;
    ICoreModuleValidator& operator=(ICoreModuleValidator&&) = default;
};

/// Validates a decoded component.
///
/// Scopes are walked in definition order on an explicit stack of frames, never by recursion: nested
/// components, and component and instance types, each with its own type index space. Every type is
/// interned into one TypeArena and measured as wasm-tools measures it, so a type deeper than
/// maxTypeDepth or of an effective size of maxTypeSize is refused where it is defined; an outer alias
/// of a type into a component charges its effective size to that component's budget of maxTypeSize
/// (component.rs:3930-3960). A form gated by a feature the validator does not admit
/// (validationAdmits()) is refused as FeatureDisabled where wasm-tools refuses it, in validation.
///
/// What this validates fully: the type section's definitions in every scope (defined value types with
/// their label, count and key rules; function types; resource types bar their destructors; component
/// and instance types), outer aliases of sort type, the imports and export declarations of type scopes
/// and the imports of components, and the embedded core modules, through @p coreModules. Everything
/// else is refused as NotYetValidated, naming its DeferredForm, after its gated forms are checked:
/// every other definition; an extern name other than a plain label, or one not strongly unique; and an
/// outer type alias into a component of a type that may refer to a resource (a resource, a value type
/// holding a handle, a function, component or instance type). Nothing is accepted unchecked.
///
/// Precondition: @p pool is well formed, as decodeComponent() and testing::ComponentBuilder lay pools
/// out (entry 0 a component; each nested component and type scope a later entry of its kind).
/// @param pool The decoded component; the result keeps it.
/// @param features The gated features to admit (validationAdmits()).
/// @param limits The caps to enforce: maxTypeDepth, maxTypeSize, maxTypes and maxResourceTypes.
/// @param coreModules Validates each embedded core module, in ordinal order.
/// @return The validated component, or the first error.
[[nodiscard]] std::expected<std::shared_ptr<ValidatedComponent const>, ValidationError> validateComponent(
    binary::ComponentPool pool,
    ComponentFeatureSet features,
    ComponentLimits const& limits,
    ICoreModuleValidator& coreModules);

} // namespace isoline::component::validate
