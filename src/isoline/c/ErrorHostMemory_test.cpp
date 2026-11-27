// SPDX-License-Identifier: Apache-2.0
//
// Errors out of host memory: under an allocator that refuses everything (tests/memory/AllocationCounter.cpp,
// whose replaced operator new is the whole program's, hence this binary of its own), the C API still
// returns an error, the immortal one, and the process carries on. Each refusal is scoped tightly, and
// nothing is asserted inside it: Catch2 allocates.
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <utility>

#include "AllocationCounter.hpp"
#include <isoline/api/Module.hpp>
#include <isoline/binary/DecodeError.hpp>
#include <isoline/c/detail/Error.hpp>
#include <isoline/c/detail/ErrorRows.hpp>
#include <isoline/c/isoline.h>

using isoline::c::detail::hostMemoryExhaustedError;
using isoline::c::detail::makeError;
using isoline::testing::AllocationLimit;
using isoline::testing::AllocationScope;

TEST_CASE("Boundary.badAllocBecomesHostMemoryExhausted", "[c-api][host-memory]")
{
    // Built in place, as GCC 14's release build wants a LoadError (Error_test.cpp).
    auto const decodeError =
        isoline::api::LoadError { std::in_place_type<isoline::binary::DecodeError>,
                                  isoline::binary::DecodeErrorCode::MagicHeaderNotDetected,
                                  0 };
    isoline_error_t* error = nullptr;
    auto retained = std::size_t { 1 };
    {
        auto const scope = AllocationScope {};
        {
            auto const refuseAll = AllocationLimit { 0 };
            error = makeError(decodeError);
        }
        retained = scope.retainedBytes();
    }
    CHECK(error == hostMemoryExhaustedError());
    CHECK(retained == 0);
    CHECK(isoline_error_code(error) == ISOLINE_ERROR_HOST_MEMORY_EXHAUSTED);

    // The process carries on, and with memory the same error is made as usual.
    auto* const made = makeError(decodeError);
    CHECK(isoline_error_code(made) == ISOLINE_ERROR_DECODE);
    isoline_error_delete(made);
}

TEST_CASE("Error.newUnderOomIsImmortalNotNull", "[c-api][host-memory]")
{
    isoline_error_t* error = nullptr;
    auto retained = std::size_t { 1 };
    {
        auto const scope = AllocationScope {};
        {
            auto const refuseAll = AllocationLimit { 0 };
            error = isoline_error_new(ISOLINE_ERROR_EMBEDDER, "the callback failed", 19);
            // Deleting it is a no-op, so the immortal error survives what a binding does with any error.
            isoline_error_delete(error);
        }
        retained = scope.retainedBytes();
    }
    REQUIRE(error != nullptr);
    CHECK(error == hostMemoryExhaustedError());
    CHECK(isoline_error_code(error) == ISOLINE_ERROR_HOST_MEMORY_EXHAUSTED);
    CHECK(isoline_error_message_length(error) > 0);
    CHECK(retained == 0);

    // Allocations that fit a budget but not the message's copy refuse the same way: the error object is
    // freed again, and the immortal error is what comes back.
    isoline_error_t* partial = nullptr;
    {
        auto const scope = AllocationScope {};
        {
            auto const roomForTheObjectAlone = AllocationLimit { sizeof(isoline_error_t) };
            partial = isoline_error_new(ISOLINE_ERROR_EMBEDDER, "the callback failed", 19);
        }
        retained = scope.retainedBytes();
    }
    CHECK(partial == hostMemoryExhaustedError());
    CHECK(retained == 0);

    // Refusing an embedder's error describes the refusal, which needs memory too.
    isoline_error_t* refusal = nullptr;
    {
        auto const scope = AllocationScope {};
        {
            auto const refuseAll = AllocationLimit { 0 };
            refusal = isoline_error_new(0, "x", 1);
        }
        retained = scope.retainedBytes();
    }
    CHECK(refusal == hostMemoryExhaustedError());
    CHECK(retained == 0);
}
