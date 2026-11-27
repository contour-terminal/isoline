// SPDX-License-Identifier: Apache-2.0
//
// How each C++ error value of the embedding API becomes an isoline_error_t: one row per error type, and
// one row per enumerator of the error enumerations (plan P17a, D5).
//
// - ErrorCodeRows names every code of error.h, one row each, in code order: what
//   isoline_error_code_name() reads. A code is appended, never renumbered.
// - EnumErrorRows<E> gives each enumerator of an error enumeration E the code it is reported with and its
//   message, one row per enumerator in enumerator order, checked against E's Last constant.
// - ErrorRow<T> reports one error type T: its code, its detail (detailOf(): the enumerator of its code's one
//   family that names it, plus one; 0 when none does), its message and what it carries beyond those. A
//   variant is reported by the row of the alternative it holds, so a variant one of whose alternatives has no
//   row does not compile: adding an alternative to an API error is adding a row here.
//
// makeError() is the one way the C layer turns a C++ error into the C API's.
#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include <isoline/api/CallError.hpp>
#include <isoline/api/Engine.hpp>
#include <isoline/api/Module.hpp>
#include <isoline/api/Store.hpp>
#include <isoline/binary/DecodeError.hpp>
#include <isoline/c/detail/Boundary.hpp>
#include <isoline/c/detail/Error.hpp>
#include <isoline/c/error.h>
#include <isoline/runtime/Abrupt.hpp>
#include <isoline/runtime/Instantiate.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/validate/ValidationError.hpp>

namespace isoline::c::detail
{

// --- the codes -----------------------------------------------------------------

/// The C++ enumeration whose enumerators an error's detail numbers. Each code has one family at most, so
/// that a detail means one thing per code; an error of a code without one has detail 0.
enum class DetailFamily : uint8_t
{
    None,                ///< The code's details are all 0.
    DecodeErrorCode,     ///< binary::DecodeErrorCode: why a module is malformed.
    ValidationErrorCode, ///< validate::ValidationErrorCode: why a module is invalid.
    Limit,               ///< runtime::Limit: the store's limit exceeded.
    Refusal,             ///< runtime::Refusal: why an allocation or a growth was refused.
    MismatchKind,        ///< runtime::MismatchKind: what is wrong with a value.
    InvalidTypeReason,   ///< api::InvalidTypeReason: what is wrong with a type the host gave.
    CallFailure,         ///< api::CallFailure: how a call into the guest ended.
    PrepareError,        ///< api::PrepareError: why a thread could not be prepared.
};

/// The family @p Enum is, or DetailFamily::None for an enumeration no code's details number.
template <typename Enum>
inline constexpr auto FamilyOf = DetailFamily::None;

template <>
inline constexpr auto FamilyOf<binary::DecodeErrorCode> = DetailFamily::DecodeErrorCode;
template <>
inline constexpr auto FamilyOf<validate::ValidationErrorCode> = DetailFamily::ValidationErrorCode;
template <>
inline constexpr auto FamilyOf<runtime::Limit> = DetailFamily::Limit;
template <>
inline constexpr auto FamilyOf<runtime::Refusal> = DetailFamily::Refusal;
template <>
inline constexpr auto FamilyOf<runtime::MismatchKind> = DetailFamily::MismatchKind;
template <>
inline constexpr auto FamilyOf<api::InvalidTypeReason> = DetailFamily::InvalidTypeReason;
template <>
inline constexpr auto FamilyOf<api::CallFailure> = DetailFamily::CallFailure;
template <>
inline constexpr auto FamilyOf<api::PrepareError> = DetailFamily::PrepareError;

/// One code of error.h: its name, and the family its details number.
struct ErrorCodeRow
{
    isoline_error_code_t code = 0;            ///< The code; equal to the row's index plus one.
    char const* name = nullptr;               ///< The constant's name after ISOLINE_ERROR_, in lower case.
    DetailFamily family = DetailFamily::None; ///< The enumeration its details number, if any.
};

/// Every code, one row each, in code order.
inline constexpr auto ErrorCodeRows = std::to_array<ErrorCodeRow>({
    { .code = ISOLINE_ERROR_INVALID_ARGUMENT,
      .name = "invalid_argument",
      .family = DetailFamily::InvalidTypeReason },
    { .code = ISOLINE_ERROR_HOST_MEMORY_EXHAUSTED,
      .name = "host_memory_exhausted",
      .family = DetailFamily::None },
    { .code = ISOLINE_ERROR_UNKNOWN_OPTION, .name = "unknown_option", .family = DetailFamily::None },
    { .code = ISOLINE_ERROR_UNMET_FEATURE_PREREQUISITE,
      .name = "unmet_feature_prerequisite",
      .family = DetailFamily::None },
    { .code = ISOLINE_ERROR_ENGINE_UNAVAILABLE, .name = "engine_unavailable", .family = DetailFamily::None },
    { .code = ISOLINE_ERROR_MODULE_TOO_LARGE, .name = "module_too_large", .family = DetailFamily::None },
    { .code = ISOLINE_ERROR_DECODE, .name = "decode", .family = DetailFamily::DecodeErrorCode },
    { .code = ISOLINE_ERROR_VALIDATION, .name = "validation", .family = DetailFamily::ValidationErrorCode },
    { .code = ISOLINE_ERROR_COMPILE_LIMIT_EXCEEDED,
      .name = "compile_limit_exceeded",
      .family = DetailFamily::None },
    { .code = ISOLINE_ERROR_ENGINE_MISMATCH, .name = "engine_mismatch", .family = DetailFamily::None },
    { .code = ISOLINE_ERROR_FOREIGN_STORE, .name = "foreign_store", .family = DetailFamily::None },
    { .code = ISOLINE_ERROR_STORE_DELETED, .name = "store_deleted", .family = DetailFamily::None },
    { .code = ISOLINE_ERROR_UNKNOWN_IMPORT, .name = "unknown_import", .family = DetailFamily::None },
    { .code = ISOLINE_ERROR_INCOMPATIBLE_IMPORT,
      .name = "incompatible_import",
      .family = DetailFamily::None },
    { .code = ISOLINE_ERROR_IMPORT_COUNT_MISMATCH,
      .name = "import_count_mismatch",
      .family = DetailFamily::None },
    { .code = ISOLINE_ERROR_LIMIT_EXCEEDED, .name = "limit_exceeded", .family = DetailFamily::Limit },
    { .code = ISOLINE_ERROR_ALLOCATION_FAILURE,
      .name = "allocation_failure",
      .family = DetailFamily::Refusal },
    { .code = ISOLINE_ERROR_CALL, .name = "call", .family = DetailFamily::CallFailure },
    { .code = ISOLINE_ERROR_THREAD_PREPARATION_UNAVAILABLE,
      .name = "thread_preparation_unavailable",
      .family = DetailFamily::PrepareError },
    { .code = ISOLINE_ERROR_CPU_FEATURES_MISSING,
      .name = "cpu_features_missing",
      .family = DetailFamily::None },
    { .code = ISOLINE_ERROR_VALUE_MISMATCH, .name = "value_mismatch", .family = DetailFamily::MismatchKind },
    { .code = ISOLINE_ERROR_OUT_OF_BOUNDS, .name = "out_of_bounds", .family = DetailFamily::None },
    { .code = ISOLINE_ERROR_GROWTH_REFUSED, .name = "growth_refused", .family = DetailFamily::Refusal },
    { .code = ISOLINE_ERROR_IMMUTABLE_GLOBAL, .name = "immutable_global", .family = DetailFamily::None },
    { .code = ISOLINE_ERROR_PRECOMPILED_REFUSED,
      .name = "precompiled_refused",
      .family = DetailFamily::None },
    { .code = ISOLINE_ERROR_CACHE_KEY, .name = "cache_key", .family = DetailFamily::None },
    { .code = ISOLINE_ERROR_CACHE_STORAGE, .name = "cache_storage", .family = DetailFamily::None },
    { .code = ISOLINE_ERROR_UNSUPPORTED_VALUE, .name = "unsupported_value", .family = DetailFamily::None },
    { .code = ISOLINE_ERROR_EMBEDDER, .name = "embedder", .family = DetailFamily::None },
    { .code = ISOLINE_ERROR_REENTRANT_CALL, .name = "reentrant_call", .family = DetailFamily::None },
});

static_assert(
    [] {
        auto expected = isoline_error_code_t { 1 };
        for (auto const& row: ErrorCodeRows)
            if (row.code != expected++)
                return false;
        return ErrorCodeRows.size() == ISOLINE_ERROR_LAST;
    }(),
    "ErrorCodeRows must hold one row per code of error.h, in code order, up to ISOLINE_ERROR_LAST");

/// Whether @p code is one of error.h's.
[[nodiscard]] constexpr auto isKnownCode(isoline_error_code_t code) noexcept -> bool
{
    return code >= 1 && code <= ISOLINE_ERROR_LAST;
}

/// The name of @p code.
/// @return Its row's name, a static string; null for a code error.h does not define.
[[nodiscard]] constexpr auto errorCodeName(isoline_error_code_t code) noexcept -> char const*
{
    return isKnownCode(code) ? ErrorCodeRows[code - 1].name : nullptr;
}

// --- the enumerations ----------------------------------------------------------

/// One enumerator of an error enumeration: the code it is reported with, and its message.
template <typename Enum>
struct EnumErrorRow
{
    Enum value {};                 ///< The enumerator; equal to the row's index.
    isoline_error_code_t code = 0; ///< The code an error it names is reported with.
    std::string_view text;         ///< Its message.
};

/// The rows of an error enumeration: `Last`, its last enumerator, and `Rows`, one EnumErrorRow per
/// enumerator in enumerator order. Defined for each enumeration the C API reports.
template <typename Enum>
struct EnumErrorRows;

template <>
struct EnumErrorRows<api::CallFailure>
{
    static constexpr auto Last = api::LastCallFailure;
    static constexpr auto Rows = std::to_array<EnumErrorRow<api::CallFailure>>({
        { api::CallFailure::Trap, ISOLINE_ERROR_CALL, "the call trapped" },
        { api::CallFailure::Interrupted, ISOLINE_ERROR_CALL, "the call was interrupted" },
        { api::CallFailure::HostFailure, ISOLINE_ERROR_CALL, "a host function the guest called failed" },
        { api::CallFailure::UncaughtException,
          ISOLINE_ERROR_CALL,
          "the guest threw an exception nothing caught" },
        { api::CallFailure::ArgumentMismatch,
          ISOLINE_ERROR_VALUE_MISMATCH,
          "the arguments do not fit the function" },
        { api::CallFailure::ForeignHandle, ISOLINE_ERROR_FOREIGN_STORE, "the function is another store's" },
        { api::CallFailure::StoreDeleted, ISOLINE_ERROR_STORE_DELETED, "the instance's store is deleted" },
    });
};

template <>
struct EnumErrorRows<api::InvalidTypeReason>
{
    static constexpr auto Last = api::LastInvalidTypeReason;
    static constexpr auto Rows = std::to_array<EnumErrorRow<api::InvalidTypeReason>>({
        { api::InvalidTypeReason::ConcreteTypeIndex,
          ISOLINE_ERROR_INVALID_ARGUMENT,
          "the type names a concrete type index, which only a module's own types may" },
        { api::InvalidTypeReason::PageSize,
          ISOLINE_ERROR_INVALID_ARGUMENT,
          "the memory's page size is not one admitted" },
        { api::InvalidTypeReason::BottomHeap,
          ISOLINE_ERROR_INVALID_ARGUMENT,
          "the type names the bottom heap" },
        { api::InvalidTypeReason::NonReferenceElement,
          ISOLINE_ERROR_INVALID_ARGUMENT,
          "the table's element type is not a reference type" },
    });
};

template <>
struct EnumErrorRows<api::MemoryAccessError>
{
    static constexpr auto Last = api::LastMemoryAccessError;
    static constexpr auto Rows = std::to_array<EnumErrorRow<api::MemoryAccessError>>({
        { api::MemoryAccessError::ForeignHandle,
          ISOLINE_ERROR_FOREIGN_STORE,
          "the memory is another store's" },
        { api::MemoryAccessError::OutOfBounds,
          ISOLINE_ERROR_OUT_OF_BOUNDS,
          "the range lies outside the memory" },
        { api::MemoryAccessError::StoreDeleted,
          ISOLINE_ERROR_STORE_DELETED,
          "the instance's store is deleted" },
    });
};

template <>
struct EnumErrorRows<api::FunctionRefError>
{
    static constexpr auto Last = api::LastFunctionRefError;
    static constexpr auto Rows = std::to_array<EnumErrorRow<api::FunctionRefError>>({
        { api::FunctionRefError::Null, ISOLINE_ERROR_INVALID_ARGUMENT, "the reference is null" },
        { api::FunctionRefError::NotFunction,
          ISOLINE_ERROR_VALUE_MISMATCH,
          "the reference refers to no function" },
        { api::FunctionRefError::UnknownAddress,
          ISOLINE_ERROR_FOREIGN_STORE,
          "the store holds no such function" },
    });
};

template <>
struct EnumErrorRows<api::GlobalAccessError>
{
    static constexpr auto Last = api::LastGlobalAccessError;
    static constexpr auto Rows = std::to_array<EnumErrorRow<api::GlobalAccessError>>({
        { api::GlobalAccessError::ForeignHandle,
          ISOLINE_ERROR_FOREIGN_STORE,
          "the global is another store's" },
        { api::GlobalAccessError::Immutable, ISOLINE_ERROR_IMMUTABLE_GLOBAL, "the global is const" },
        { api::GlobalAccessError::ValueMismatch,
          ISOLINE_ERROR_VALUE_MISMATCH,
          "the value is not of the global's type" },
    });
};

template <>
struct EnumErrorRows<api::TableAccessError>
{
    static constexpr auto Last = api::LastTableAccessError;
    static constexpr auto Rows = std::to_array<EnumErrorRow<api::TableAccessError>>({
        { api::TableAccessError::ForeignHandle, ISOLINE_ERROR_FOREIGN_STORE, "the table is another store's" },
        { api::TableAccessError::OutOfBounds,
          ISOLINE_ERROR_OUT_OF_BOUNDS,
          "the index is past the table's size" },
        { api::TableAccessError::ValueMismatch,
          ISOLINE_ERROR_VALUE_MISMATCH,
          "the reference is not of the table's element type" },
    });
};

template <>
struct EnumErrorRows<api::ExportAccessError>
{
    static constexpr auto Last = api::LastExportAccessError;
    static constexpr auto Rows = std::to_array<EnumErrorRow<api::ExportAccessError>>({
        { api::ExportAccessError::OutOfRange,
          ISOLINE_ERROR_OUT_OF_BOUNDS,
          "the index is past the instance's exports" },
        { api::ExportAccessError::StoreDeleted,
          ISOLINE_ERROR_STORE_DELETED,
          "the instance's store is deleted" },
    });
};

template <>
struct EnumErrorRows<api::PrepareError>
{
    static constexpr auto Last = api::LastPrepareError;
    static constexpr auto Rows = std::to_array<EnumErrorRow<api::PrepareError>>({
        { api::PrepareError::Unavailable,
          ISOLINE_ERROR_THREAD_PREPARATION_UNAVAILABLE,
          "what the engine needs of a thread could not be set up" },
    });
};

template <>
struct EnumErrorRows<runtime::LinkErrorCode>
{
    static constexpr auto Last = runtime::LastLinkErrorCode;
    static constexpr auto Rows = std::to_array<EnumErrorRow<runtime::LinkErrorCode>>({
        { runtime::LinkErrorCode::UnknownImport,
          ISOLINE_ERROR_UNKNOWN_IMPORT,
          runtime::specMessage(runtime::LinkErrorCode::UnknownImport) },
        { runtime::LinkErrorCode::IncompatibleImportType,
          ISOLINE_ERROR_INCOMPATIBLE_IMPORT,
          runtime::specMessage(runtime::LinkErrorCode::IncompatibleImportType) },
    });
};

/// An error enumeration the C API reports: one with rows.
template <typename Enum>
concept TabledEnum = std::is_enum_v<Enum> && requires {
    EnumErrorRows<Enum>::Last;
    EnumErrorRows<Enum>::Rows;
};

/// Whether the rows of @p Enum are one per enumerator up to its last, each at its enumerator's index, each
/// of a code error.h defines.
template <TabledEnum Enum>
[[nodiscard]] consteval auto rowsAreComplete() -> bool
{
    auto index = std::size_t { 0 };
    for (auto const& row: EnumErrorRows<Enum>::Rows)
    {
        if (static_cast<std::size_t>(row.value) != index++ || !isKnownCode(row.code))
            return false;
    }
    return index == static_cast<std::size_t>(EnumErrorRows<Enum>::Last) + 1;
}

static_assert(rowsAreComplete<api::CallFailure>());
static_assert(rowsAreComplete<api::InvalidTypeReason>());
static_assert(rowsAreComplete<api::MemoryAccessError>());
static_assert(rowsAreComplete<api::FunctionRefError>());
static_assert(rowsAreComplete<api::GlobalAccessError>());
static_assert(rowsAreComplete<api::TableAccessError>());
static_assert(rowsAreComplete<api::ExportAccessError>());
static_assert(rowsAreComplete<api::PrepareError>());
static_assert(rowsAreComplete<runtime::LinkErrorCode>());

/// The row of @p value.
template <TabledEnum Enum>
[[nodiscard]] constexpr auto enumRow(Enum value) noexcept -> EnumErrorRow<Enum> const&
{
    // Read by its index: the static_asserts of rowsAreComplete() put every enumerator's row at its index,
    // one per enumerator up to the last, so an enumerator's row is never missed. For C++ values only, each an
    // enumerator.
    return EnumErrorRows<Enum>::Rows[static_cast<std::size_t>(value)];
}

/// Whether @p family is the family of @p code's details: never DetailFamily::None, and never for a code
/// error.h does not define.
[[nodiscard]] constexpr auto isFamilyOf(isoline_error_code_t code, DetailFamily family) noexcept -> bool
{
    return family != DetailFamily::None && isKnownCode(code) && ErrorCodeRows[code - 1].family == family;
}

/// The detail of an error of code @p code that @p value names: the enumerator's number plus one when
/// @p Enum is @p code's family, so that 0 always means none; 0 otherwise. (A product, not a branch, so the
/// one decision is isFamilyOf()'s for every enumeration.)
template <typename Enum>
    requires std::is_enum_v<Enum>
[[nodiscard]] constexpr auto detailOf(isoline_error_code_t code, Enum value) noexcept -> uint32_t
{
    return static_cast<uint32_t>(isFamilyOf(code, FamilyOf<Enum>)) * (static_cast<uint32_t>(value) + 1);
}

// --- the error types -----------------------------------------------------------

/// What an error is reported with: what newError() makes it from.
struct ErrorReport
{
    isoline_error_code_t code = 0; ///< Its code.
    uint32_t detail = 0;           ///< Its detail (detailOf()), or 0.
    std::string message {};        ///< Its message.
    ErrorDetail extra {};          ///< What it carries beyond those.
};

/// How an error type is reported: `report(error)` gives its ErrorReport, or, for an error that needs no
/// memory, `error(error)` gives the immortal error itself. Defined for each error type the C API reports,
/// and for each alternative of each error variant.
template <typename Error>
struct ErrorRow;

/// An error enumeration is reported by its rows.
template <TabledEnum Enum>
struct ErrorRow<Enum>
{
    static auto report(Enum error) -> ErrorReport
    {
        auto const& row = enumRow(error);
        return { .code = row.code, .detail = detailOf(row.code, error), .message = std::string { row.text } };
    }
};

template <>
struct ErrorRow<api::ModuleTooLarge>
{
    static auto report(api::ModuleTooLarge const& error) -> ErrorReport
    {
        return { .code = ISOLINE_ERROR_MODULE_TOO_LARGE,
                 .message =
                     std::format("the module's {} bytes exceed the limit of {}", error.bytes, error.limit) };
    }
};

/// The host out of memory is the immortal error itself, which needs none: a row that names an error
/// instead of reporting one.
template <>
struct ErrorRow<api::HostMemoryExhausted>
{
    static auto error(api::HostMemoryExhausted const& /*error*/) noexcept -> isoline_error_t*
    {
        return hostMemoryExhaustedError();
    }
};

template <>
struct ErrorRow<binary::DecodeError>
{
    static auto report(binary::DecodeError const& error) -> ErrorReport
    {
        return { .code = ISOLINE_ERROR_DECODE,
                 .detail = detailOf(ISOLINE_ERROR_DECODE, error.code),
                 .message = std::format("{} at offset {}", binary::specMessage(error), error.offset) };
    }
};

template <>
struct ErrorRow<validate::ValidationError>
{
    static auto report(validate::ValidationError const& error) -> ErrorReport
    {
        return { .code = ISOLINE_ERROR_VALIDATION,
                 .detail = detailOf(ISOLINE_ERROR_VALIDATION, error.code),
                 .message = validate::specMessage(error) };
    }
};

template <>
struct ErrorRow<api::EngineMismatch>
{
    static auto report(api::EngineMismatch const& /*error*/) -> ErrorReport
    {
        return { .code = ISOLINE_ERROR_ENGINE_MISMATCH,
                 .message = "the module was loaded by an engine of another identity than the store's" };
    }
};

template <>
struct ErrorRow<api::ImportCountMismatch>
{
    static auto report(api::ImportCountMismatch const& error) -> ErrorReport
    {
        return { .code = ISOLINE_ERROR_IMPORT_COUNT_MISMATCH,
                 .message = std::format(
                     "the module has {} imports, and {} externs were given", error.expected, error.given) };
    }
};

template <>
struct ErrorRow<api::ForeignImport>
{
    static auto report(api::ForeignImport const& error) -> ErrorReport
    {
        return { .code = ISOLINE_ERROR_FOREIGN_STORE,
                 .message = std::format("import {} is bound to another store's entity", error.index),
                 .extra = ImportIndex { error.index } };
    }
};

template <>
struct ErrorRow<runtime::LinkError>
{
    static auto report(runtime::LinkError const& error) -> ErrorReport
    {
        auto const& row = enumRow(error.code);
        return { .code = row.code,
                 .detail = detailOf(row.code, error.code),
                 .message = std::format("import {}: {}", error.importIndex, row.text),
                 .extra = ImportIndex { error.importIndex } };
    }
};

template <>
struct ErrorRow<runtime::LimitExceeded>
{
    static auto report(runtime::LimitExceeded const& error) -> ErrorReport
    {
        return { .code = ISOLINE_ERROR_LIMIT_EXCEEDED,
                 .detail = detailOf(ISOLINE_ERROR_LIMIT_EXCEEDED, error.limit),
                 .message = runtime::describe(error),
                 .extra = error.limit };
    }
};

template <>
struct ErrorRow<runtime::AllocationFailure>
{
    static auto report(runtime::AllocationFailure const& error) -> ErrorReport
    {
        auto const* const kind = error.kind == binary::ExternKind::Memory ? "memory" : "table";
        return { .code = ISOLINE_ERROR_ALLOCATION_FAILURE,
                 .detail = detailOf(ISOLINE_ERROR_ALLOCATION_FAILURE, error.reason),
                 .message =
                     std::format("the {} could not be allocated: {}", kind, runtime::describe(error.reason)),
                 .extra = error.reason };
    }
};

/// A refused growth (MemoryGrowthError, TableGrowthError).
template <>
struct ErrorRow<runtime::Refusal>
{
    static auto report(runtime::Refusal error) -> ErrorReport
    {
        return { .code = ISOLINE_ERROR_GROWTH_REFUSED,
                 .detail = detailOf(ISOLINE_ERROR_GROWTH_REFUSED, error),
                 .message = std::format("growth refused: {}", runtime::describe(error)),
                 .extra = error };
    }
};

template <>
struct ErrorRow<runtime::ValueMismatch>
{
    static auto report(runtime::ValueMismatch const& error) -> ErrorReport
    {
        return { .code = ISOLINE_ERROR_VALUE_MISMATCH,
                 .detail = detailOf(ISOLINE_ERROR_VALUE_MISMATCH, error.kind),
                 .message = std::format("value {} is not of its type", error.index) };
    }
};

template <>
struct ErrorRow<api::InvalidType>
{
    static auto report(api::InvalidType const& error) -> ErrorReport
    {
        return ErrorRow<api::InvalidTypeReason>::report(error.reason);
    }
};

template <>
struct ErrorRow<api::InitValueMismatch>
{
    static auto report(api::InitValueMismatch const& /*error*/) -> ErrorReport
    {
        return { .code = ISOLINE_ERROR_VALUE_MISMATCH,
                 .message = "the initial value is not of the table's element type" };
    }
};

template <>
struct ErrorRow<api::ForeignHandle>
{
    static auto report(api::ForeignHandle const& /*error*/) -> ErrorReport
    {
        return { .code = ISOLINE_ERROR_FOREIGN_STORE, .message = "the handle is another store's" };
    }
};

template <>
struct ErrorRow<api::StoreDeleted>
{
    static auto report(api::StoreDeleted const& /*error*/) -> ErrorReport
    {
        return { .code = ISOLINE_ERROR_STORE_DELETED, .message = "the handle's store is deleted" };
    }
};

/// A failed call, by the failure it names (CallError::failure()); a trap's message follows, and the trap
/// is carried for isoline_error_trap. An argument mismatch's detail is its MismatchKind's.
template <>
struct ErrorRow<api::CallError>
{
    static auto report(api::CallError const& error) -> ErrorReport
    {
        auto const failure = error.failure();
        auto report = ErrorRow<api::CallFailure>::report(failure);
        if (auto const* const trap = std::get_if<runtime::Trap>(&error.detail))
        {
            report.message = std::format("{}: {}", report.message, runtime::specMessage(*trap));
            report.extra = *trap;
        }
        // An argument that does not fit is reported as the mismatch it is, in VALUE_MISMATCH's family.
        if (auto const* const mismatch = std::get_if<runtime::ValueMismatch>(&error.detail))
            report.detail = detailOf(report.code, mismatch->kind);
        return report;
    }
};

/// Whether @p Error has a row: the C API reports it.
template <typename Error>
concept HasErrorRow = requires(Error const& error) {
    { ErrorRow<Error>::report(error) } -> std::same_as<ErrorReport>;
} || requires(Error const& error) {
    { ErrorRow<Error>::error(error) } noexcept -> std::same_as<isoline_error_t*>;
};

/// Whether every alternative of @p Variant has a row.
template <typename Variant>
[[nodiscard]] consteval auto everyAlternativeHasARow() -> bool
{
    return []<std::size_t... Index>(std::index_sequence<Index...>) {
        return (HasErrorRow<std::variant_alternative_t<Index, Variant>> && ...);
    }(std::make_index_sequence<std::variant_size_v<Variant>> {});
}

/// The error variants of the embedding API the C layer reports, each a std::variant all of whose
/// alternatives have rows.
template <typename Error>
inline constexpr auto IsVariant = false;

template <typename... Alternatives>
inline constexpr auto IsVariant<std::variant<Alternatives...>> = true;

template <typename Error>
concept ReportedVariant = IsVariant<Error> && everyAlternativeHasARow<Error>();

// --- makeError -----------------------------------------------------------------

/// The C API's error for @p error: its row's report, or the error its row names; whenever memory runs out
/// on the way, the immortal hostMemoryExhaustedError().
/// @return The error, owned by the caller; never null.
template <HasErrorRow Error>
[[nodiscard]] auto makeError(Error const& error) noexcept -> isoline_error_t*
{
    if constexpr (requires { ErrorRow<Error>::error(error); })
        return ErrorRow<Error>::error(error);
    else
        return boundary(
            [&] {
                auto const report = ErrorRow<Error>::report(error);
                return newError(report.code, report.detail, report.message, report.extra);
            },
            [] { return hostMemoryExhaustedError(); });
}

/// The C API's error for the alternative @p error holds.
/// @return The error, owned by the caller; never null.
template <ReportedVariant Error>
[[nodiscard]] auto makeError(Error const& error) noexcept -> isoline_error_t*
{
    return std::visit([](auto const& alternative) noexcept { return makeError(alternative); }, error);
}

} // namespace isoline::c::detail
