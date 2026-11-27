// SPDX-License-Identifier: Apache-2.0
//
// The details of each code's family, as published: detail n is the enumerator listed with it here, and
// the list is every enumerator of the family. An enumerator inserted into a family's C++ enumeration
// renumbers the ones after it, which these lists, checked in by hand, refuse; one appended extends its
// list. Each code has one family at most (ErrorCodeRows), and detail 0 means none.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>

#include <isoline/api/CallError.hpp>
#include <isoline/api/Engine.hpp>
#include <isoline/api/Store.hpp>
#include <isoline/binary/DecodeError.hpp>
#include <isoline/c/detail/ErrorRows.hpp>
#include <isoline/runtime/Abrupt.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/validate/ValidationError.hpp>

namespace api = isoline::api;
namespace binary = isoline::binary;
namespace runtime = isoline::runtime;
namespace validate = isoline::validate;

using isoline::c::detail::DetailFamily;
using isoline::c::detail::detailOf;
using isoline::c::detail::ErrorCodeRows;

namespace
{

/// One published detail: its number, and the enumerator it names.
template <typename Enum>
struct Pinned
{
    uint32_t detail = 0;
    Enum value {};
};

/// Checks @p pinned against the family of @p code: every enumerator up to @p last listed once, in order,
/// each the detail it was published as.
template <typename Enum, std::size_t Count>
void checkPinned(isoline_error_code_t code, Enum last, std::array<Pinned<Enum>, Count> const& pinned)
{
    CHECK(Count == static_cast<std::size_t>(last) + 1);
    auto expected = uint32_t { 1 };
    for (auto const& row: pinned)
    {
        INFO("detail " << row.detail);
        CHECK(row.detail == expected++);
        CHECK(detailOf(code, row.value) == row.detail);
    }
}

} // namespace

TEST_CASE("ErrorDetails.DecodeErrorCodeIsPinned", "[c-api]")
{
    checkPinned(ISOLINE_ERROR_DECODE,
                binary::LastDecodeErrorCode,
                std::to_array<Pinned<binary::DecodeErrorCode>>({
                    { 1, binary::DecodeErrorCode::UnexpectedEnd },
                    { 2, binary::DecodeErrorCode::MagicHeaderNotDetected },
                    { 3, binary::DecodeErrorCode::UnknownBinaryVersion },
                    { 4, binary::DecodeErrorCode::IntegerTooLarge },
                    { 5, binary::DecodeErrorCode::IntegerRepresentationTooLong },
                    { 6, binary::DecodeErrorCode::MalformedSectionId },
                    { 7, binary::DecodeErrorCode::SectionSizeMismatch },
                    { 8, binary::DecodeErrorCode::JunkAfterLastSection },
                    { 9, binary::DecodeErrorCode::MalformedUtf8Encoding },
                    { 10, binary::DecodeErrorCode::TooManyLocals },
                    { 11, binary::DecodeErrorCode::LocalCountOverflow },
                    { 12, binary::DecodeErrorCode::IllegalOpcode },
                    { 13, binary::DecodeErrorCode::MalformedReferenceType },
                    { 14, binary::DecodeErrorCode::MalformedImportKind },
                    { 15, binary::DecodeErrorCode::MalformedMutability },
                    { 16, binary::DecodeErrorCode::IntegerTooLargeForLimits },
                    { 17, binary::DecodeErrorCode::LimitsFlagTooLarge },
                    { 18, binary::DecodeErrorCode::FunctionAndCodeSectionInconsistent },
                    { 19, binary::DecodeErrorCode::DataCountAndDataSectionInconsistent },
                    { 20, binary::DecodeErrorCode::DataCountSectionRequired },
                    { 21, binary::DecodeErrorCode::UnexpectedContentAfterEnd },
                    { 22, binary::DecodeErrorCode::MalformedElementSegmentKind },
                    { 23, binary::DecodeErrorCode::ZeroByteExpected },
                    { 24, binary::DecodeErrorCode::LengthOutOfBounds },
                    { 25, binary::DecodeErrorCode::ImplementationLimitExceeded },
                    { 26, binary::DecodeErrorCode::DuplicateSection },
                    { 27, binary::DecodeErrorCode::SectionOutOfOrder },
                    { 28, binary::DecodeErrorCode::MalformedExportKind },
                    { 29, binary::DecodeErrorCode::MalformedElementKind },
                    { 30, binary::DecodeErrorCode::MalformedDataSegmentKind },
                    { 31, binary::DecodeErrorCode::MalformedFunctionType },
                    { 32, binary::DecodeErrorCode::EndOpcodeExpected },
                    { 33, binary::DecodeErrorCode::MalformedMemopFlags },
                    { 34, binary::DecodeErrorCode::MissingFeaturePrerequisite },
                    { 35, binary::DecodeErrorCode::MalformedLimitsFlags },
                    { 36, binary::DecodeErrorCode::InvalidCustomPageSize },
                    { 37, binary::DecodeErrorCode::MalformedCatchClause },
                    { 38, binary::DecodeErrorCode::NameSubsectionSizeMismatch },
                    { 39, binary::DecodeErrorCode::NameIndexOutOfOrder },
                }));
}

TEST_CASE("ErrorDetails.ValidationErrorCodeIsPinned", "[c-api]")
{
    checkPinned(ISOLINE_ERROR_VALIDATION,
                validate::LastValidationErrorCode,
                std::to_array<Pinned<validate::ValidationErrorCode>>({
                    { 1, validate::ValidationErrorCode::TypeMismatch },
                    { 2, validate::ValidationErrorCode::UnknownLocal },
                    { 3, validate::ValidationErrorCode::UnknownGlobal },
                    { 4, validate::ValidationErrorCode::UnknownFunction },
                    { 5, validate::ValidationErrorCode::UnknownTable },
                    { 6, validate::ValidationErrorCode::UnknownMemory },
                    { 7, validate::ValidationErrorCode::UnknownType },
                    { 8, validate::ValidationErrorCode::UnknownLabel },
                    { 9, validate::ValidationErrorCode::UnknownElemSegment },
                    { 10, validate::ValidationErrorCode::UnknownDataSegment },
                    { 11, validate::ValidationErrorCode::InvalidResultArity },
                    { 12, validate::ValidationErrorCode::ImmutableGlobal },
                    { 13, validate::ValidationErrorCode::ConstantExpressionRequired },
                    { 14, validate::ValidationErrorCode::DuplicateExportName },
                    { 15, validate::ValidationErrorCode::MultipleMemories },
                    { 16, validate::ValidationErrorCode::MultipleTables },
                    { 17, validate::ValidationErrorCode::AlignmentMustNotBeLarger },
                    { 18, validate::ValidationErrorCode::MemorySizeMustBeAtMost },
                    { 19, validate::ValidationErrorCode::SizeMinimumMustNotBeGreaterThanMaximum },
                    { 20, validate::ValidationErrorCode::StartFunction },
                    { 21, validate::ValidationErrorCode::UndeclaredFunctionReference },
                    { 22, validate::ValidationErrorCode::InvalidLaneIndex },
                    { 23, validate::ValidationErrorCode::UnknownTag },
                    { 24, validate::ValidationErrorCode::OffsetOutOfRange },
                    { 25, validate::ValidationErrorCode::TableSizeMustBeAtMost },
                    { 26, validate::ValidationErrorCode::InvalidCustomPageSize },
                    { 27, validate::ValidationErrorCode::UninitializedLocal },
                    { 28, validate::ValidationErrorCode::NonEmptyTagResultType },
                    { 29, validate::ValidationErrorCode::OperandStackLimitExceeded },
                }));
}

TEST_CASE("ErrorDetails.LimitIsPinned", "[c-api]")
{
    checkPinned(ISOLINE_ERROR_LIMIT_EXCEEDED,
                runtime::LastLimit,
                std::to_array<Pinned<runtime::Limit>>({
                    { 1, runtime::Limit::CallDepth },
                    { 2, runtime::Limit::StackValues },
                    { 3, runtime::Limit::NestedInvocations },
                    { 4, runtime::Limit::MemoryBytes },
                    { 5, runtime::Limit::TableElements },
                    { 6, runtime::Limit::Memories },
                    { 7, runtime::Limit::Tables },
                    { 8, runtime::Limit::Instances },
                    { 9, runtime::Limit::Exceptions },
                    { 10, runtime::Limit::ExceptionValues },
                    { 11, runtime::Limit::Types },
                    { 12, runtime::Limit::Tags },
                    { 13, runtime::Limit::Steps },
                }));
}

TEST_CASE("ErrorDetails.RefusalIsPinned", "[c-api]")
{
    checkPinned(ISOLINE_ERROR_GROWTH_REFUSED,
                runtime::LastRefusal,
                std::to_array<Pinned<runtime::Refusal>>({
                    { 1, runtime::Refusal::TypeMaximum },
                    { 2, runtime::Refusal::Limit },
                    { 3, runtime::Refusal::HostMemory },
                    { 4, runtime::Refusal::Guard },
                    { 5, runtime::Refusal::Embedder },
                }));
}

TEST_CASE("ErrorDetails.MismatchKindIsPinned", "[c-api]")
{
    checkPinned(ISOLINE_ERROR_VALUE_MISMATCH,
                runtime::LastMismatchKind,
                std::to_array<Pinned<runtime::MismatchKind>>({
                    { 1, runtime::MismatchKind::Count },
                    { 2, runtime::MismatchKind::Kind },
                    { 3, runtime::MismatchKind::Heap },
                    { 4, runtime::MismatchKind::NotNullable },
                    { 5, runtime::MismatchKind::ForeignReference },
                }));
}

TEST_CASE("ErrorDetails.InvalidTypeReasonIsPinned", "[c-api]")
{
    checkPinned(ISOLINE_ERROR_INVALID_ARGUMENT,
                api::LastInvalidTypeReason,
                std::to_array<Pinned<api::InvalidTypeReason>>({
                    { 1, api::InvalidTypeReason::ConcreteTypeIndex },
                    { 2, api::InvalidTypeReason::PageSize },
                    { 3, api::InvalidTypeReason::BottomHeap },
                    { 4, api::InvalidTypeReason::NonReferenceElement },
                }));
    // error.h publishes this family as C constants too.
    CHECK(detailOf(ISOLINE_ERROR_INVALID_ARGUMENT, api::InvalidTypeReason::ConcreteTypeIndex)
          == ISOLINE_INVALID_TYPE_CONCRETE_TYPE_INDEX);
    CHECK(detailOf(ISOLINE_ERROR_INVALID_ARGUMENT, api::InvalidTypeReason::PageSize)
          == ISOLINE_INVALID_TYPE_PAGE_SIZE);
    CHECK(detailOf(ISOLINE_ERROR_INVALID_ARGUMENT, api::InvalidTypeReason::BottomHeap)
          == ISOLINE_INVALID_TYPE_BOTTOM_HEAP);
    CHECK(detailOf(ISOLINE_ERROR_INVALID_ARGUMENT, api::InvalidTypeReason::NonReferenceElement)
          == ISOLINE_INVALID_TYPE_NON_REFERENCE_ELEMENT);
}

TEST_CASE("ErrorDetails.CallFailureIsPinned", "[c-api]")
{
    checkPinned(ISOLINE_ERROR_CALL,
                api::LastCallFailure,
                std::to_array<Pinned<api::CallFailure>>({
                    { 1, api::CallFailure::Trap },
                    { 2, api::CallFailure::Interrupted },
                    { 3, api::CallFailure::HostFailure },
                    { 4, api::CallFailure::UncaughtException },
                    { 5, api::CallFailure::ArgumentMismatch },
                    { 6, api::CallFailure::ForeignHandle },
                    { 7, api::CallFailure::StoreDeleted },
                }));
}

TEST_CASE("ErrorDetails.PrepareErrorIsPinned", "[c-api]")
{
    checkPinned(ISOLINE_ERROR_THREAD_PREPARATION_UNAVAILABLE,
                api::LastPrepareError,
                std::to_array<Pinned<api::PrepareError>>({
                    { 1, api::PrepareError::Unavailable },
                }));
}

TEST_CASE("ErrorDetails.oneFamilyPerCode", "[c-api]")
{
    // A family is the details of the codes that name it, and no code names two: a code's row holds one.
    // The families named are the ones published above, each by the codes listed here.
    auto named = std::set<DetailFamily> {};
    for (auto const& row: ErrorCodeRows)
        if (row.family != DetailFamily::None)
            named.insert(row.family);
    CHECK(named.size() == 8);

    // Outside its family, an enumerator gives detail 0.
    CHECK(detailOf(ISOLINE_ERROR_CALL, runtime::Refusal::Limit) == 0);
    CHECK(detailOf(ISOLINE_ERROR_FOREIGN_STORE, api::CallFailure::ForeignHandle) == 0);
    CHECK(detailOf(ISOLINE_ERROR_OUT_OF_BOUNDS, api::MemoryAccessError::OutOfBounds) == 0);
    CHECK(detailOf(0, binary::DecodeErrorCode::UnexpectedEnd) == 0);
    CHECK(detailOf(ISOLINE_ERROR_DECODE, binary::DecodeErrorCode::UnexpectedEnd) == 1);
}
