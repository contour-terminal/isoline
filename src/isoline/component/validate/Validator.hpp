// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
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
    InvalidExternName,
    InvalidExternNameUse,
    ConflictingExternName,
    ImplementsNameNotPlain,
    ImplementsNotInstance,
    InvalidImplementsName,
    ImplementsNotInterface,
    InvalidImplementsVersion,
    FunctionIndexOutOfBounds,
    ComponentIndexOutOfBounds,
    InstanceIndexOutOfBounds,
    CoreFunctionIndexOutOfBounds,
    ModuleIndexOutOfBounds,
    CoreInstanceIndexOutOfBounds,
    TableIndexOutOfBounds,
    MemoryIndexOutOfBounds,
    GlobalIndexOutOfBounds,
    TagIndexOutOfBounds,
    NotModuleType,
    ModuleTypeNotSubType,
    CoreAliasOfModuleType,
    DuplicateCoreExport,
    DuplicateCoreImport,
    CoreLimitsOrder,
    CoreTableTooLarge,
    CoreMemoryTooLarge,
    InvalidPageSize,
    CoreTagResults,
    DuplicateModuleArgument,
    MissingModuleArgument,
    ArgumentMissingExport,
    CoreArgumentMismatch,
    DuplicateCoreInstanceExport,
    CoreInstanceNoExport,
    CoreExportNotOfSort,
    InstanceNoExport,
    ExportNotOfSort,
    AliasInTypeScope,
    OuterAliasResource,
    ArgumentConflict,
    InstantiationMismatch,
    AscriptionMismatch,
    NotValidAs,
    LocalResourceInImports,
    LocalResourceNotExported,
    NotLocalResource,
    DestructorSignature,
    EncodingConflict,
    RepeatedOption,
    CallbackWithoutAsync,
    ReallocWithoutMemory,
    ReallocSignature,
    MemoryRequired,
    ReallocRequired,
    PostReturnInLower,
    PostReturnSignature,
    LiftParamsMismatch,
    LiftResultsMismatch,
};

/// The last ValidationErrorCode enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastValidationErrorCode = ValidationErrorCode::LiftResultsMismatch;

/// How a validation error's message is built from its row's text and the error's fields.
enum class ValidationErrorDetail : uint8_t
{
    None,       ///< The row's text, as it is.
    Index,      ///< The text formatted with the subject, an index.
    Label,      ///< The text formatted with the LabelKind in subject, its name and the previous name.
    Limit,      ///< The exceeded limit's message (limitMessage()), the subject from binary::limitSubject().
    Refusal,    ///< The error's name: a gated form's refusal, wasm-tools' text for that site.
    Extern,     ///< The text formatted with the ExternKind in subject's word, the name and previous.
    Named,      ///< The text formatted with the name and previous.
    IndexNamed, ///< The text formatted with the subject, an index, then the name and previous.
    Mismatch,   ///< As Named, then a line of the error's detail: the mismatch found inside.
    Verbatim,   ///< The error's detail, as it is: a type relation's message, its contexts included.
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
    // validate_extern (component.rs:4709-4880): an Extern row's {0} is "import" or "export", {1} the
    // name, {2} previous: the grammar's refusal (nameErrorMessage()), the rule's, or the earlier name.
    { .code = ValidationErrorCode::InvalidExternName,
      .text = "{0} name `{1}` is not a valid extern name\n{2}",
      .detail = ValidationErrorDetail::Extern },
    { .code = ValidationErrorCode::InvalidExternNameUse,
      .text = "{0} name `{1}` is not valid\n{2}",
      .detail = ValidationErrorDetail::Extern },
    { .code = ValidationErrorCode::ConflictingExternName,
      .text = "{0} name `{1}` conflicts with previous name `{2}`",
      .detail = ValidationErrorDetail::Extern },
    // A Named row's {0} is the name, {1} previous: the grammar's refusal of the implemented name.
    { .code = ValidationErrorCode::ImplementsNameNotPlain,
      .text = "name `{0}` is not valid with `implements`",
      .detail = ValidationErrorDetail::Named },
    { .code = ValidationErrorCode::ImplementsNotInstance, .text = "only instances can have an `implements`" },
    { .code = ValidationErrorCode::InvalidImplementsName,
      .text = "`{0}` is not a valid name\n{1}",
      .detail = ValidationErrorDetail::Named },
    { .code = ValidationErrorCode::ImplementsNotInterface,
      .text = "name `{0}` must be an interface",
      .detail = ValidationErrorDetail::Named },
    { .code = ValidationErrorCode::InvalidImplementsVersion,
      .text = "{1}",
      .detail = ValidationErrorDetail::Named },
    // The index spaces of a component and the core definitions in it (component.rs:4378-4530 and
    // core.rs:719-930); an IndexNamed row's {0} is the index, {1} the name, {2} previous.
    { .code = ValidationErrorCode::FunctionIndexOutOfBounds,
      .text = "unknown function {0}: function index out of bounds",
      .detail = ValidationErrorDetail::Index },
    { .code = ValidationErrorCode::ComponentIndexOutOfBounds,
      .text = "unknown component {0}: component index out of bounds",
      .detail = ValidationErrorDetail::Index },
    { .code = ValidationErrorCode::InstanceIndexOutOfBounds,
      .text = "unknown instance {0}: instance index out of bounds",
      .detail = ValidationErrorDetail::Index },
    { .code = ValidationErrorCode::CoreFunctionIndexOutOfBounds,
      .text = "unknown core function {0}: function index out of bounds",
      .detail = ValidationErrorDetail::Index },
    { .code = ValidationErrorCode::ModuleIndexOutOfBounds,
      .text = "unknown module {0}: module index out of bounds",
      .detail = ValidationErrorDetail::Index },
    { .code = ValidationErrorCode::CoreInstanceIndexOutOfBounds,
      .text = "unknown core instance {0}: instance index out of bounds",
      .detail = ValidationErrorDetail::Index },
    { .code = ValidationErrorCode::TableIndexOutOfBounds,
      .text = "unknown table {0}: table index out of bounds",
      .detail = ValidationErrorDetail::Index },
    { .code = ValidationErrorCode::MemoryIndexOutOfBounds,
      .text = "unknown memory {0}: memory index out of bounds",
      .detail = ValidationErrorDetail::Index },
    { .code = ValidationErrorCode::GlobalIndexOutOfBounds,
      .text = "unknown global {0}: global index out of bounds",
      .detail = ValidationErrorDetail::Index },
    { .code = ValidationErrorCode::TagIndexOutOfBounds,
      .text = "unknown tag {0}: tag index out of bounds",
      .detail = ValidationErrorDetail::Index },
    { .code = ValidationErrorCode::NotModuleType,
      .text = "core type index {0} is not a module type",
      .detail = ValidationErrorDetail::Index },
    { .code = ValidationErrorCode::ModuleTypeNotSubType,
      .text = "type index {0} is a module type, not a sub type",
      .detail = ValidationErrorDetail::Index },
    { .code = ValidationErrorCode::CoreAliasOfModuleType,
      .text = "not implemented: aliasing core module types into a core module's types index space" },
    { .code = ValidationErrorCode::DuplicateCoreExport,
      .text = "duplicate export name `{0}` already defined",
      .detail = ValidationErrorDetail::Named },
    { .code = ValidationErrorCode::DuplicateCoreImport,
      .text = "module has a duplicate import name `{0}:{1}` that is not allowed in components",
      .detail = ValidationErrorDetail::Named },
    { .code = ValidationErrorCode::CoreLimitsOrder, .text = "size minimum must not be greater than maximum" },
    { .code = ValidationErrorCode::CoreTableTooLarge,
      .text = "table size must be at most {0} entries",
      .detail = ValidationErrorDetail::Named },
    { .code = ValidationErrorCode::CoreMemoryTooLarge,
      .text = "memory size must be at most {0} {1}-byte pages",
      .detail = ValidationErrorDetail::Named },
    { .code = ValidationErrorCode::InvalidPageSize, .text = "invalid custom page size" },
    { .code = ValidationErrorCode::CoreTagResults,
      .text = "invalid exception type: non-empty tag result type" },
    { .code = ValidationErrorCode::DuplicateModuleArgument,
      .text = "duplicate module instantiation argument named `{0}`",
      .detail = ValidationErrorDetail::Named },
    { .code = ValidationErrorCode::MissingModuleArgument,
      .text = "missing module instantiation argument named `{0}`",
      .detail = ValidationErrorDetail::Named },
    { .code = ValidationErrorCode::ArgumentMissingExport,
      .text = "module instantiation argument `{0}` does not export an item named `{1}`",
      .detail = ValidationErrorDetail::Named },
    { .code = ValidationErrorCode::CoreArgumentMismatch,
      .text = "type mismatch for export `{1}` of module instantiation argument `{0}`",
      .detail = ValidationErrorDetail::Mismatch },
    { .code = ValidationErrorCode::DuplicateCoreInstanceExport,
      .text = "duplicate instantiation export name `{0}` already defined",
      .detail = ValidationErrorDetail::Named },
    { .code = ValidationErrorCode::CoreInstanceNoExport,
      .text = "core instance {0} has no export named `{1}`",
      .detail = ValidationErrorDetail::IndexNamed },
    { .code = ValidationErrorCode::CoreExportNotOfSort,
      .text = "export `{1}` for core instance {0} is not a {2}",
      .detail = ValidationErrorDetail::IndexNamed },
    { .code = ValidationErrorCode::InstanceNoExport,
      .text = "instance {0} has no export named `{1}`",
      .detail = ValidationErrorDetail::IndexNamed },
    { .code = ValidationErrorCode::ExportNotOfSort,
      .text = "export `{1}` for instance {0} is not a {2}",
      .detail = ValidationErrorDetail::IndexNamed },
    { .code = ValidationErrorCode::AliasInTypeScope,
      .text = "aliases in a component or instance type may only refer to types or instances" },
    { .code = ValidationErrorCode::OuterAliasResource,
      .text = "cannot alias outer type which transitively refers to resources not defined in the current "
              "component" },
    { .code = ValidationErrorCode::ArgumentConflict,
      .text = "instantiation argument `{0}` conflicts with previous argument `{1}`",
      .detail = ValidationErrorDetail::Named },
    { .code = ValidationErrorCode::InstantiationMismatch,
      .text = "{0}",
      .detail = ValidationErrorDetail::Verbatim },
    { .code = ValidationErrorCode::AscriptionMismatch,
      .text = "ascribed type of export is not compatible with item's type",
      .detail = ValidationErrorDetail::Mismatch },
    { .code = ValidationErrorCode::NotValidAs,
      .text = "{0} not valid to be used as {1}",
      .detail = ValidationErrorDetail::Named },
    { .code = ValidationErrorCode::LocalResourceInImports, .text = "local resource type found in imports" },
    { .code = ValidationErrorCode::LocalResourceNotExported,
      .text = "local resource type found in export but not exported itself" },
    { .code = ValidationErrorCode::NotLocalResource,
      .text = "type {0} is not a local resource",
      .detail = ValidationErrorDetail::Index },
    { .code = ValidationErrorCode::DestructorSignature,
      .text = "core function {0} has wrong signature for a destructor",
      .detail = ValidationErrorDetail::Index },
    { .code = ValidationErrorCode::EncodingConflict,
      .text = "canonical encoding option `{0}` conflicts with option `{1}`",
      .detail = ValidationErrorDetail::Named },
    { .code = ValidationErrorCode::RepeatedOption,
      .text = "canonical option `{0}` is specified more than once",
      .detail = ValidationErrorDetail::Named },
    { .code = ValidationErrorCode::CallbackWithoutAsync, .text = "cannot specify callback without async" },
    { .code = ValidationErrorCode::ReallocWithoutMemory,
      .text = "canonical option `realloc` requires `memory` to also be specified" },
    { .code = ValidationErrorCode::ReallocSignature,
      .text = "canonical option `realloc` uses a core function with an incorrect signature" },
    { .code = ValidationErrorCode::MemoryRequired, .text = "canonical option `memory` is required" },
    { .code = ValidationErrorCode::ReallocRequired, .text = "canonical option `realloc` is required" },
    { .code = ValidationErrorCode::PostReturnInLower,
      .text = "canonical option `post-return` cannot be specified for lowerings" },
    { .code = ValidationErrorCode::PostReturnSignature,
      .text = "canonical option `post-return` uses a core function with an incorrect signature" },
    { .code = ValidationErrorCode::LiftParamsMismatch,
      .text = "lowered parameter types `{1}` do not match parameter types `{2}` of core function {0}",
      .detail = ValidationErrorDetail::IndexNamed },
    { .code = ValidationErrorCode::LiftResultsMismatch,
      .text = "lowered result types `{1}` do not match result types `{2}` of core function {0}",
      .detail = ValidationErrorDetail::IndexNamed },
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
    Memory64CanonicalMemory,
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
      .feature = ComponentFeature::Implements,
      .text = "the `cm-implements` feature is not active" },
    { .form = GatedForm::AsyncOption, // 2749
      .feature = ComponentFeature::Async,
      .text = "canonical option `async` requires the component model async feature" },
    { .form = GatedForm::Memory64CanonicalMemory, // 4542
      .feature = ComponentFeature::Memory64,
      .text = "64-bit memories require the `cm64` feature to be enabled" },
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
        return index == static_cast<std::size_t>(GatedForm::Memory64CanonicalMemory) + 1;
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

/// The gated form of each canonical option, by CanonOptionKind: empty for one validation admits
/// ungated. `callback` is decoded under 🔀's gate, but wasm-tools' option check gates only `async`: a
/// callback is refused by the rule that it needs `async` ("cannot specify callback without async",
/// component.rs:2829-2831), never as a disabled feature, so its entry is empty.
inline constexpr auto CanonOptionGates = std::to_array<std::optional<GatedForm>>({
    std::nullopt,           // string-encoding=utf8
    std::nullopt,           // string-encoding=utf16
    std::nullopt,           // string-encoding=latin1+utf16
    std::nullopt,           // memory
    std::nullopt,           // realloc
    std::nullopt,           // post-return
    GatedForm::AsyncOption, // async
    std::nullopt,           // callback
});

/// The canonical options decoded under a gate that validation checks by a rule of its own instead.
inline constexpr auto RuleCheckedOptions =
    std::to_array<binary::CanonOptionKind>({ binary::CanonOptionKind::Callback });

static_assert(
    [] {
        if (CanonOptionGates.size() != binary::CanonOptionTable.size())
            return false;
        for (auto const index: std::views::iota(std::size_t { 0 }, CanonOptionGates.size()))
        {
            auto const& gate = binary::CanonOptionTable[index].gate;
            auto const& form = CanonOptionGates[index];
            auto const isRuleChecked =
                std::ranges::find(RuleCheckedOptions, binary::CanonOptionTable[index].kind)
                != RuleCheckedOptions.end();
            if (gate.has_value() != (form.has_value() || isRuleChecked)
                || (form && gatedFormRow(*form).feature != *gate))
                return false;
        }
        return true;
    }(),
    "CanonOptionGates names a gated form, of the same feature, for each gated option not checked by a rule");

/// How wasm-tools names each string encoding option, by CanonOptionKind (check_options' display,
/// component.rs:2658-2670): Utf8, Utf16 and Latin1Utf16 only.
inline constexpr auto StringEncodingWords =
    std::to_array<std::string_view>({ "utf8", "utf16", "latin1-utf16" });

/// One rule of a `[constructor]`, `[method]` or `[static]` name, which the function it names breaks.
///
/// The enumerators are the rows of ResourceNameRuleMessages, in the same order.
enum class ResourceNameRule : uint8_t
{
    NotAFunction,
    ConstructorWithoutResult,
    ConstructorResultNotOwn,
    MethodWithoutParameters,
    MethodFirstNotSelf,
    MethodSelfNotBorrow,
    StaticResourceUnknown,
    ResourceUnnamed,
    ResourceNameMismatch,
};

/// One row of ResourceNameRuleMessages: a rule and wasm-tools' text when it is broken.
struct ResourceNameRuleRow
{
    ResourceNameRule rule; ///< The rule; equal to the row's index.
    std::string_view text; ///< Its text; {0} names the expected resource name where it has one.
};

/// Every resource name rule, in ResourceNameRule's order: wasm-tools 1.263.0's texts
/// (ComponentNameContext::validate and validate_resource_name, component.rs:4885-5090). A broken rule is
/// reported as InvalidExternNameUse, its text after the name's.
inline constexpr auto ResourceNameRuleMessages = std::to_array<ResourceNameRuleRow>({
    { .rule = ResourceNameRule::NotAFunction, .text = "item is not a func" },
    { .rule = ResourceNameRule::ConstructorWithoutResult, .text = "function should return one value" },
    { .rule = ResourceNameRule::ConstructorResultNotOwn,
      .text = "function should return `(own $T)` or `(result (own $T))`" },
    { .rule = ResourceNameRule::MethodWithoutParameters,
      .text = "function should have at least one argument" },
    { .rule = ResourceNameRule::MethodFirstNotSelf,
      .text = "function should have a first argument called `self`" },
    { .rule = ResourceNameRule::MethodSelfNotBorrow,
      .text = "function should take a first argument of `(borrow $T)`" },
    { .rule = ResourceNameRule::StaticResourceUnknown,
      .text = "static resource name is not known in this context" },
    { .rule = ResourceNameRule::ResourceUnnamed,
      .text = "resource used in function does not have a name in this context" },
    { .rule = ResourceNameRule::ResourceNameMismatch,
      .text = "function does not match expected resource name `{0}`" },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: ResourceNameRuleMessages)
        {
            if (static_cast<std::size_t>(row.rule) != index)
                return false;
            ++index;
        }
        return index == static_cast<std::size_t>(ResourceNameRule::ResourceNameMismatch) + 1;
    }(),
    "ResourceNameRuleMessages must hold one row per ResourceNameRule, at the index of its enumerator");

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
    /// For DuplicateLabel and ConflictingExternName: the earlier, conflicting name; for the other
    /// Extern and Named codes, the refusal the message ends with.
    std::string previous {};
    std::optional<ComponentFeature> disabled {};  ///< For FeatureDisabled: the feature not admitted.
    std::optional<CoreModuleError> coreModule {}; ///< For CoreModuleRefused: the module and its refusal.
    /// For a Mismatch row, the mismatch found inside, its own lines; for a Verbatim row, the message.
    std::string detail {};
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

/// Validates a decoded component, as wasm-tools 1.263.0 validates one (wasmparser 0.263.0
/// src/validator/component.rs), refusing at the stage and in the words it does.
///
/// Scopes are walked in definition order on an explicit stack of frames, never by recursion: nested
/// components, and component and instance types. Each scope's index spaces (ScopeInfo) are built
/// incrementally, an export, import or alias adding an entry to its sort's space, and every lookup is
/// checked against them and against the caps of ComponentLimits. Every type is interned into one
/// TypeArena and measured as wasm-tools measures it; each definition is a type of its own and each
/// export or import of a type a fresh copy of it, as wasm-tools' type ids are, which the rule that an
/// export or import names only exported or imported types (validate_and_register_named_types) reads.
/// A form gated by a feature the validator does not admit (validationAdmits()) is refused as
/// FeatureDisabled where wasm-tools refuses it.
///
/// What it validates: the type definitions of every scope; core module types, and every embedded core
/// module through @p coreModules, all their types interned into the arena's one canonical core type
/// space, so that core instantiation matches an import by an equality of ids; core instances made by
/// instantiation (each import supplied by an export of the right type) or of inline exports; aliases
/// of instance and core instance exports, and outer aliases, a type crossing into a component naming
/// no resource defined outside it; component instances, their arguments subtypes of the imports
/// (TypeRelation, one for the whole validation), their resources bound or made fresh; imports and
/// exports, with their names (the grammar, `implements`, a resource function's signature, strong
/// uniqueness) and ascribed types; canonical lifts and lowers, their options and core signatures, and
/// the resource built-ins; and, as each component finishes, that its resources stay inside it but for
/// those it exports.
///
/// Precondition: @p pool is well formed, as decodeComponent() and testing::ComponentBuilder lay pools
/// out (entry 0 a component; each nested component and type scope a later entry of its kind).
/// @param pool The decoded component; the result keeps it.
/// @param features The gated features to admit (validationAdmits()).
/// @param limits The caps to enforce: the type caps (maxTypeDepth, maxTypeSize, maxTypes,
///               maxResourceTypes, maxSubstitutedTypes, maxSubtypingSteps) and the index space caps.
/// @param coreModules Validates each embedded core module, in ordinal order.
/// @return The validated component, or the first error.
[[nodiscard]] std::expected<std::shared_ptr<ValidatedComponent const>, ValidationError> validateComponent(
    binary::ComponentPool pool,
    ComponentFeatureSet features,
    ComponentLimits const& limits,
    ICoreModuleValidator& coreModules);

} // namespace isoline::component::validate
