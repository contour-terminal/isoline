// SPDX-License-Identifier: Apache-2.0

// A component must never make the host run out of memory (spec §7). The component decoder's share of
// that: a vector count reserves nothing, so a count that lies about what follows fails having
// allocated nothing beyond the decoder's copy of its input.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>

#include "AllocationCounter.hpp"
#include <isoline/component/Features.hpp>
#include <isoline/component/Limits.hpp>
#include <isoline/component/binary/Decoder.hpp>
#include <isoline/component/binary/testing/DecoderFixtures.hpp>

using isoline::component::ComponentFeatureSet;
using isoline::component::ComponentLimits;
using isoline::component::binary::decodeComponent;
using isoline::component::binary::DecodeErrorCode;
using isoline::component::binary::testing::appendBytes;
using isoline::component::binary::testing::appendLeb128;
using isoline::component::binary::testing::Bytes;
using isoline::component::binary::testing::componentOf;
using isoline::component::binary::testing::section;
using isoline::testing::AllocationLimit;

namespace
{

/// The code decoding @p bytes fails with, decoded under a budget of 4 KiB; empty when it succeeds.
/// Nothing is asserted under the budget, as an assertion allocates.
std::optional<DecodeErrorCode> codeUnderBudget(Bytes const& bytes)
{
    auto code = std::optional<DecodeErrorCode> {};
    {
        auto const limit = AllocationLimit { 4096 };
        auto const result = decodeComponent(bytes, ComponentFeatureSet::wasi03(), ComponentLimits {});
        if (!result)
            code = result.error().code;
    }
    return code;
}

} // namespace

TEST_CASE("ComponentMemory.lyingCountsStayInBudget")
{
    // A count of 2^32 - 1 imports with three bytes left.
    auto lying = Bytes {};
    appendBytes(lying, { 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x00, 0x01, 0x61 });
    CHECK(codeUnderBudget(componentOf({ section(10, lying) })) == DecodeErrorCode::UnexpectedEnd);

    // A count equal to the bytes left, of elements that fail at their first byte: a decoder that
    // reserved for the count would hold a thousand definitions.
    auto equal = Bytes {};
    appendLeb128(equal, 1'000);
    for ([[maybe_unused]] auto const index: std::views::iota(0, 1'000))
        equal.push_back(std::byte { 0xFF });
    CHECK(codeUnderBudget(componentOf({ section(10, equal) })) == DecodeErrorCode::InvalidNameKind);
    CHECK(codeUnderBudget(componentOf({ section(5, equal) })) == DecodeErrorCode::InvalidInstanceKind);
}
