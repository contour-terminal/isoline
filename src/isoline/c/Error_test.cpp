// SPDX-License-Identifier: Apache-2.0
//
// Errors as values (error.h): the code table, the rows that turn each C++ error into one, the cause chain
// and the message's bound. What happens out of host memory is ErrorHostMemory_test.cpp's, a binary of its
// own (its allocator is the whole program's).
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <print>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <isoline/api/CallError.hpp>
#include <isoline/api/Engine.hpp>
#include <isoline/api/Module.hpp>
#include <isoline/api/Store.hpp>
#include <isoline/c/detail/Boundary.hpp>
#include <isoline/c/detail/Error.hpp>
#include <isoline/c/detail/ErrorRows.hpp>
#include <isoline/c/isoline.h>

namespace api = isoline::api;
namespace binary = isoline::binary;
namespace runtime = isoline::runtime;
namespace validate = isoline::validate;

using isoline::c::detail::ErrorPtr;
using isoline::c::detail::everyAlternativeHasARow;
using isoline::c::detail::hostMemoryExhaustedError;
using isoline::c::detail::ImportIndex;
using isoline::c::detail::makeError;
using isoline::c::detail::MaxCauseDepth;
using isoline::c::detail::MaxMessageBytes;
using isoline::c::detail::newError;
using isoline::c::detail::rowsAreComplete;
using isoline::c::detail::utf8Prefix;
using isoline::c::detail::withCause;

namespace
{

/// The message of @p error, its own NUL bytes included.
[[nodiscard]] auto messageOf(isoline_error_t const* error) -> std::string_view
{
    return { isoline_error_message(error), isoline_error_message_length(error) };
}

/// How many errors the chain from @p error holds, @p error included.
[[nodiscard]] auto chainLength(isoline_error_t const* error) -> std::size_t
{
    auto length = std::size_t { 0 };
    for (auto const* link = error; link != nullptr; link = isoline_error_cause(link))
        ++length;
    return length;
}

/// The detail of an error its code's family names with @p value: the enumerator's number plus one.
template <typename Enum>
[[nodiscard]] constexpr auto inFamily(Enum value) noexcept -> uint32_t
{
    return static_cast<uint32_t>(value) + 1;
}

/// One error value and what the C API reports it as.
struct Expectation
{
    std::string_view what;                 ///< The C++ error, for the failure message.
    ErrorPtr error;                        ///< What makeError() made of it.
    isoline_error_code_t code;             ///< The code it must have.
    uint32_t detail;                       ///< The detail it must have.
    isoline::c::detail::ErrorDetail extra; ///< What it must carry.
};

/// An Expectation of @p value.
template <typename Error>
[[nodiscard]] auto expect(std::string_view what,
                          Error const& value,
                          isoline_error_code_t code,
                          uint32_t detail = 0,
                          isoline::c::detail::ErrorDetail extra = {}) -> Expectation
{
    return {
        .what = what, .error = ErrorPtr { makeError(value) }, .code = code, .detail = detail, .extra = extra
    };
}

} // namespace

TEST_CASE("ErrorRows.everyAlternativeHasARow", "[c-api]")
{
    // A variant alternative without a row does not compile: makeError() has no overload for the variant.
    STATIC_CHECK(everyAlternativeHasARow<api::LoadError>());
    STATIC_CHECK(everyAlternativeHasARow<api::InstantiateError>());
    STATIC_CHECK(everyAlternativeHasARow<api::DefineError>());
    STATIC_CHECK(everyAlternativeHasARow<api::MemoryGrowthError>());
    STATIC_CHECK(everyAlternativeHasARow<api::TableGrowthError>());

    // An enumeration's rows are one per enumerator up to its Last constant (M4).
    STATIC_CHECK(rowsAreComplete<api::CallFailure>());
    STATIC_CHECK(rowsAreComplete<api::InvalidTypeReason>());
    STATIC_CHECK(rowsAreComplete<api::MemoryAccessError>());
    STATIC_CHECK(rowsAreComplete<api::FunctionRefError>());
    STATIC_CHECK(rowsAreComplete<api::GlobalAccessError>());
    STATIC_CHECK(rowsAreComplete<api::TableAccessError>());
    STATIC_CHECK(rowsAreComplete<api::ExportAccessError>());
    STATIC_CHECK(rowsAreComplete<api::PrepareError>());
    STATIC_CHECK(rowsAreComplete<runtime::LinkErrorCode>());
}

TEST_CASE("ErrorRows.eachErrorHasItsCode", "[c-api]")
{
    // A LoadError is built in place: moved in from a temporary alternative, its validation error's unset
    // optionals are copied, which GCC 14's release build reports as maybe-uninitialized.
    auto const trap = runtime::Trap { runtime::TrapCode::Unreachable };
    auto expectations = std::array {
        expect("ModuleTooLarge",
               api::LoadError { std::in_place_type<api::ModuleTooLarge>,
                                api::ModuleTooLarge { .bytes = 10, .limit = 5 } },
               ISOLINE_ERROR_MODULE_TOO_LARGE),
        expect("CompileLimitExceeded",
               api::LoadError { std::in_place_type<api::CompileLimitExceeded>,
                                api::CompileLimitExceeded { .codeIndex = 3, .requested = 10, .limit = 5 } },
               ISOLINE_ERROR_COMPILE_LIMIT_EXCEEDED),
        expect("DecodeError",
               api::LoadError { std::in_place_type<binary::DecodeError>,
                                binary::DecodeErrorCode::MagicHeaderNotDetected,
                                0 },
               ISOLINE_ERROR_DECODE,
               inFamily(binary::DecodeErrorCode::MagicHeaderNotDetected)),
        expect("ValidationError",
               api::LoadError { std::in_place_type<validate::ValidationError>,
                                validate::ValidationErrorCode::TypeMismatch },
               ISOLINE_ERROR_VALIDATION,
               inFamily(validate::ValidationErrorCode::TypeMismatch)),
        expect("EngineMismatch",
               api::InstantiateError { api::EngineMismatch {} },
               ISOLINE_ERROR_ENGINE_MISMATCH),
        expect("ImportCountMismatch",
               api::InstantiateError { api::ImportCountMismatch { .expected = 2, .given = 1 } },
               ISOLINE_ERROR_IMPORT_COUNT_MISMATCH),
        expect("ForeignImport",
               api::InstantiateError { api::ForeignImport { .index = 3 } },
               ISOLINE_ERROR_FOREIGN_STORE,
               0,
               ImportIndex { 3 }),
        expect("LinkError unknown",
               api::InstantiateError { runtime::LinkError { runtime::LinkErrorCode::UnknownImport, 4 } },
               ISOLINE_ERROR_UNKNOWN_IMPORT,
               0,
               ImportIndex { 4 }),
        expect("LinkError incompatible",
               api::InstantiateError {
                   runtime::LinkError { runtime::LinkErrorCode::IncompatibleImportType, 5 } },
               ISOLINE_ERROR_INCOMPATIBLE_IMPORT,
               0,
               ImportIndex { 5 }),
        expect("LimitExceeded",
               api::InstantiateError { runtime::LimitExceeded { runtime::Limit::MemoryBytes } },
               ISOLINE_ERROR_LIMIT_EXCEEDED,
               inFamily(runtime::Limit::MemoryBytes),
               runtime::Limit::MemoryBytes),
        expect("AllocationFailure",
               api::DefineError { runtime::AllocationFailure { .kind = binary::ExternKind::Table,
                                                               .reason = runtime::Refusal::Embedder } },
               ISOLINE_ERROR_ALLOCATION_FAILURE,
               inFamily(runtime::Refusal::Embedder),
               runtime::Refusal::Embedder),
        expect("CallError trap",
               api::InstantiateError { api::CallError { .detail = trap, .refusedGrowth = {} } },
               ISOLINE_ERROR_CALL,
               inFamily(api::CallFailure::Trap),
               trap),
        expect("CallError store deleted",
               api::CallError { .detail = api::StoreDeleted {}, .refusedGrowth = {} },
               ISOLINE_ERROR_STORE_DELETED,
               0),
        expect("CallError argument mismatch",
               api::CallError {
                   .detail = runtime::ValueMismatch { .kind = runtime::MismatchKind::Kind, .index = 1 },
                   .refusedGrowth = {} },
               ISOLINE_ERROR_VALUE_MISMATCH,
               inFamily(runtime::MismatchKind::Kind)),
        expect(
            "ValueMismatch",
            api::DefineError { runtime::ValueMismatch { .kind = runtime::MismatchKind::Heap, .index = 0 } },
            ISOLINE_ERROR_VALUE_MISMATCH,
            inFamily(runtime::MismatchKind::Heap)),
        expect("InvalidType",
               api::DefineError { api::InvalidType { api::InvalidTypeReason::PageSize } },
               ISOLINE_ERROR_INVALID_ARGUMENT,
               inFamily(api::InvalidTypeReason::PageSize)),
        expect("MemoryGrowthError foreign",
               api::MemoryGrowthError { api::ForeignHandle {} },
               ISOLINE_ERROR_FOREIGN_STORE),
        expect("MemoryGrowthError refusal",
               api::MemoryGrowthError { runtime::Refusal::Limit },
               ISOLINE_ERROR_GROWTH_REFUSED,
               inFamily(runtime::Refusal::Limit),
               runtime::Refusal::Limit),
        expect("TableGrowthError init value",
               api::TableGrowthError { api::InitValueMismatch {} },
               ISOLINE_ERROR_VALUE_MISMATCH),
        expect("StoreDeleted", api::StoreDeleted {}, ISOLINE_ERROR_STORE_DELETED),
        expect("MemoryAccessError", api::MemoryAccessError::OutOfBounds, ISOLINE_ERROR_OUT_OF_BOUNDS, 0),
        expect("FunctionRefError", api::FunctionRefError::NotFunction, ISOLINE_ERROR_VALUE_MISMATCH, 0),
        expect("GlobalAccessError", api::GlobalAccessError::Immutable, ISOLINE_ERROR_IMMUTABLE_GLOBAL, 0),
        expect("TableAccessError", api::TableAccessError::ForeignHandle, ISOLINE_ERROR_FOREIGN_STORE, 0),
        expect("ExportAccessError", api::ExportAccessError::StoreDeleted, ISOLINE_ERROR_STORE_DELETED, 0),
        expect("PrepareError",
               api::PrepareError::Unavailable,
               ISOLINE_ERROR_THREAD_PREPARATION_UNAVAILABLE,
               inFamily(api::PrepareError::Unavailable)),
    };
    for (auto const& expectation: expectations)
    {
        INFO(expectation.what);
        REQUIRE(expectation.error != nullptr);
        CHECK(isoline_error_code(expectation.error.get()) == expectation.code);
        CHECK(isoline_error_detail(expectation.error.get()) == expectation.detail);
        CHECK(expectation.error->extra == expectation.extra);
        CHECK_FALSE(messageOf(expectation.error.get()).empty());
        CHECK(isoline_error_cause(expectation.error.get()) == nullptr);
    }
}

TEST_CASE("ErrorRows.hostMemoryExhaustedIsTheImmortalError", "[c-api]")
{
    // Neither value allocates: the variant's alternative and the bare type both give the one static error.
    CHECK(makeError(api::LoadError { std::in_place_type<api::HostMemoryExhausted> })
          == hostMemoryExhaustedError());
    CHECK(makeError(api::DefineError { api::HostMemoryExhausted {} }) == hostMemoryExhaustedError());
    CHECK(isoline_error_code(hostMemoryExhaustedError()) == ISOLINE_ERROR_HOST_MEMORY_EXHAUSTED);
    CHECK(messageOf(hostMemoryExhaustedError()) == "out of host memory");
}

TEST_CASE("ErrorRows.codesAreUniqueAndStable", "[c-api]")
{
    // The codes as they were published: a code is never renumbered or renamed, only appended. This list is
    // checked in by hand, so that a change of ErrorCodeRows or error.h shows up here as a change of the ABI.
    struct PublishedCode
    {
        isoline_error_code_t code;
        std::string_view name;
    };
    constexpr auto Published = std::to_array<PublishedCode>({
        { 1, "invalid_argument" },
        { 2, "host_memory_exhausted" },
        { 3, "unknown_option" },
        { 4, "unmet_feature_prerequisite" },
        { 5, "engine_unavailable" },
        { 6, "module_too_large" },
        { 7, "decode" },
        { 8, "validation" },
        { 9, "compile_limit_exceeded" },
        { 10, "engine_mismatch" },
        { 11, "foreign_store" },
        { 12, "store_deleted" },
        { 13, "unknown_import" },
        { 14, "incompatible_import" },
        { 15, "import_count_mismatch" },
        { 16, "limit_exceeded" },
        { 17, "allocation_failure" },
        { 18, "call" },
        { 19, "thread_preparation_unavailable" },
        { 20, "cpu_features_missing" },
        { 21, "value_mismatch" },
        { 22, "out_of_bounds" },
        { 23, "growth_refused" },
        { 24, "immutable_global" },
        { 25, "precompiled_refused" },
        { 26, "cache_key" },
        { 27, "cache_storage" },
        { 28, "unsupported_value" },
        { 29, "embedder" },
        { 30, "reentrant_call" },
    });
    STATIC_CHECK(Published.size() == ISOLINE_ERROR_LAST);
    STATIC_CHECK(ISOLINE_ERROR_DECODE == 7);
    STATIC_CHECK(ISOLINE_ERROR_REENTRANT_CALL == 30);
    for (auto const& row: Published)
    {
        INFO(row.name);
        REQUIRE(isoline_error_code_name(row.code) != nullptr);
        CHECK(isoline_error_code_name(row.code) == row.name);
        for (auto const& other: Published)
            if (other.code != row.code)
                CHECK(other.name != row.name);
    }
    CHECK(isoline_error_code_name(0) == nullptr);
    CHECK(isoline_error_code_name(ISOLINE_ERROR_LAST + 1) == nullptr);
    CHECK(isoline_error_code_name(UINT32_MAX) == nullptr);
}

TEST_CASE("Error.causeChainIsBoundedAtEight", "[c-api]")
{
    // Each new error takes the chain so far as its cause, ten times over: the chain keeps the eight newest.
    auto* chain = newError(ISOLINE_ERROR_EMBEDDER, 0, "error 0");
    for (auto const index: std::views::iota(1, 10))
        chain = withCause(newError(ISOLINE_ERROR_EMBEDDER, 0, std::format("error {}", index)), chain);
    auto const error = ErrorPtr { chain };
    CHECK(chainLength(error.get()) == MaxCauseDepth);
    CHECK(messageOf(error.get()) == "error 9");
    auto const* deepest = error.get();
    while (isoline_error_cause(deepest) != nullptr)
        deepest = isoline_error_cause(deepest);
    CHECK(messageOf(deepest) == "error 2");

    // A cause appended to a chain goes last.
    auto const appended = ErrorPtr { withCause(
        withCause(newError(ISOLINE_ERROR_CALL, 0, "head"), newError(ISOLINE_ERROR_CALL, 0, "middle")),
        newError(ISOLINE_ERROR_CALL, 0, "tail")) };
    REQUIRE(chainLength(appended.get()) == 3);
    CHECK(messageOf(isoline_error_cause(isoline_error_cause(appended.get()))) == "tail");

    // No cause is none.
    auto const alone = ErrorPtr { withCause(newError(ISOLINE_ERROR_CALL, 0, "alone"), nullptr) };
    CHECK(chainLength(alone.get()) == 1);

    // An immortal link ends the chain: a cause appended after it is deleted (ASan's leak check holds it to
    // that), and the immortal error is never written.
    auto* const head = withCause(newError(ISOLINE_ERROR_CALL, 0, "head"), hostMemoryExhaustedError());
    auto const endsAtImmortal = ErrorPtr { withCause(head, newError(ISOLINE_ERROR_CALL, 0, "dropped")) };
    CHECK(chainLength(endsAtImmortal.get()) == 2);
    CHECK(isoline_error_cause(endsAtImmortal.get()) == hostMemoryExhaustedError());
    CHECK(isoline_error_cause(hostMemoryExhaustedError()) == nullptr);

    // An immortal link as the eighth ends a chain of eight: nothing past it, and it is not written.
    auto* seven = newError(ISOLINE_ERROR_CALL, 0, "link 1");
    for (auto const index: std::views::iota(2, 8))
        seven = withCause(seven, newError(ISOLINE_ERROR_CALL, 0, std::format("link {}", index)));
    auto const eight = ErrorPtr { withCause(seven, hostMemoryExhaustedError()) };
    CHECK(chainLength(eight.get()) == MaxCauseDepth);
    CHECK(isoline_error_cause(hostMemoryExhaustedError()) == nullptr);

    // The immortal error takes no cause; the cause is deleted (ASan holds the leak check to it).
    CHECK(withCause(hostMemoryExhaustedError(), newError(ISOLINE_ERROR_CALL, 0, "dropped"))
          == hostMemoryExhaustedError());
    CHECK(isoline_error_cause(hostMemoryExhaustedError()) == nullptr);
}

TEST_CASE("Error.messageCappedAtUtf8Boundary", "[c-api]")
{
    auto const euro = std::string_view { "\xE2\x82\xAC" }; // U+20AC, three bytes

    // Exactly the bound is kept whole.
    auto const exact = std::string(MaxMessageBytes, 'a');
    auto const whole = ErrorPtr { isoline_error_new(ISOLINE_ERROR_EMBEDDER, exact.data(), exact.size()) };
    CHECK(messageOf(whole.get()) == exact);

    // A character straddling the bound is cut before it, at each of its three positions.
    for (auto const before: std::views::iota(std::size_t { 1 }, std::size_t { 4 }))
    {
        INFO("ASCII bytes before the character: " << MaxMessageBytes - before);
        auto text = std::string(MaxMessageBytes - before, 'b');
        text += euro;
        text += std::string(16, 'c');
        auto const error = ErrorPtr { isoline_error_new(ISOLINE_ERROR_EMBEDDER, text.data(), text.size()) };
        auto const expectedLength = before == 3 ? MaxMessageBytes : MaxMessageBytes - before;
        CHECK(isoline_error_message_length(error.get()) == expectedLength);
        CHECK(messageOf(error.get()) == std::string_view { text }.substr(0, expectedLength));
        CHECK(isoline_error_message(error.get())[expectedLength] == '\0');
    }

    // Longer than the bound, by far: the bytes past it are not read, so a length that runs past the buffer
    // reads nothing beyond its first MaxMessageBytes + 1 bytes (ASan holds the buffer's end to it).
    auto const huge = std::string(MaxMessageBytes + 1, 'd');
    auto const capped = ErrorPtr { isoline_error_new(
        ISOLINE_ERROR_EMBEDDER, huge.data(), std::numeric_limits<std::size_t>::max() / 2) };
    CHECK(messageOf(capped.get()) == std::string_view { huge }.substr(0, MaxMessageBytes));

    // The largest length there is reads no more than the bound and the byte after it, either.
    auto const bounded = std::string(MaxMessageBytes + 1, 'e');
    auto const largest = ErrorPtr { isoline_error_new(
        ISOLINE_ERROR_EMBEDDER, bounded.data(), std::numeric_limits<std::size_t>::max()) };
    CHECK(messageOf(largest.get()) == std::string_view { bounded }.substr(0, MaxMessageBytes));

    // The prefix rule itself, on invalid UTF-8: at most three bytes back.
    auto const continuations = std::string(8, '\x80');
    CHECK(utf8Prefix(continuations, 6).size() == 3);
    CHECK(utf8Prefix("abc", 2) == "ab");
    CHECK(utf8Prefix("abc", 3) == "abc");
    CHECK(utf8Prefix(euro, 0).empty());
}

TEST_CASE("Error.newRefusesWhatIsNoError", "[c-api]")
{
    // An unknown code is reported, not passed on: no error the embedder makes reads as another.
    for (auto const code:
         { isoline_error_code_t { 0 }, isoline_error_code_t { ISOLINE_ERROR_LAST + 1 }, UINT32_MAX })
    {
        auto const error = ErrorPtr { isoline_error_new(code, "x", 1) };
        REQUIRE(error != nullptr);
        CHECK(isoline_error_code(error.get()) == ISOLINE_ERROR_INVALID_ARGUMENT);
        CHECK(messageOf(error.get()).contains("is no error code"));
    }

    // NULL with a length is reported; NULL without one is an empty message.
    auto const nullWithLength = ErrorPtr { isoline_error_new(ISOLINE_ERROR_EMBEDDER, nullptr, 5) };
    CHECK(isoline_error_code(nullWithLength.get()) == ISOLINE_ERROR_INVALID_ARGUMENT);
    auto const empty = ErrorPtr { isoline_error_new(ISOLINE_ERROR_EMBEDDER, nullptr, 0) };
    CHECK(isoline_error_code(empty.get()) == ISOLINE_ERROR_EMBEDDER);
    CHECK(isoline_error_message_length(empty.get()) == 0);
    CHECK(isoline_error_message(empty.get())[0] == '\0');
}

TEST_CASE("Error.immortalErrorIsNeverDeleted", "[c-api]")
{
    auto* const immortal = hostMemoryExhaustedError();
    isoline_error_delete(immortal);
    isoline_error_delete(immortal);
    CHECK(isoline_error_code(immortal) == ISOLINE_ERROR_HOST_MEMORY_EXHAUSTED);
    // An immortal error as a cause is left alone when its chain is deleted.
    isoline_error_delete(withCause(newError(ISOLINE_ERROR_CALL, 0, "head"), immortal));
    CHECK(messageOf(immortal) == "out of host memory");
}

// Registered as its own CTest (isoline_c_test.logicErrorTerminates), which passes on the terminate
// handler's line alone: the process must end in boundary(). Were the logic_error turned into an error, the
// case would return and say so, which fails the test.
TEST_CASE("Boundary.logicErrorTerminates", "[.terminates]")
{
    std::set_terminate([] {
        std::fputs("terminated in boundary()\n", stderr);
        // Ended, not aborted: CTest counts an abort as a failure whatever the output says.
        std::_Exit(EXIT_FAILURE);
    });
    auto const result = isoline::c::detail::boundary(
        []() -> int { throw std::logic_error { "a defect in Isoline" }; }, [] { return 0; });
    std::println(stderr, "boundary() returned {}: the logic_error did not end the process", result);
}
