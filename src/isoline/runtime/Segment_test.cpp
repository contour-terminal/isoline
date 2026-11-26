// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <span>

#include <isoline/runtime/Segment.hpp>
#include <isoline/runtime/Trap.hpp>

using isoline::runtime::copySegment;
using isoline::runtime::fits;
using isoline::runtime::Trap;
using isoline::runtime::TrapCode;

TEST_CASE("Segment.fits")
{
    CHECK(fits(0, 0, 0));
    CHECK(fits(2, 3, 5));
    CHECK_FALSE(fits(3, 3, 5));
    CHECK_FALSE(fits(6, 0, 5));
    // No overflow: offset + count would wrap to a small number.
    CHECK_FALSE(fits(1, std::numeric_limits<uint64_t>::max(), 5));
}

TEST_CASE("Segment.copySegment")
{
    auto const segment = std::to_array<int>({ 1, 2, 3 });
    auto target = std::to_array<int>({ 0, 0, 0, 0 });
    auto const source = std::span<int const> { segment };

    SECTION("copies the part asked for")
    {
        REQUIRE(copySegment(source, 1, 2, std::span { target }, 2, TrapCode::OutOfBoundsTableAccess));
        CHECK(target == std::to_array<int>({ 0, 0, 2, 3 }));
    }

    SECTION("an empty copy at either end is in bounds")
    {
        CHECK(copySegment(source, 3, 0, std::span { target }, 4, TrapCode::OutOfBoundsTableAccess));
        CHECK(copySegment(
            std::span<int const> {}, 0, 0, std::span { target }, 0, TrapCode::OutOfBoundsTableAccess));
    }

    SECTION("either range out of bounds traps and copies nothing")
    {
        CHECK(copySegment(source, 2, 2, std::span { target }, 0, TrapCode::OutOfBoundsMemoryAccess).error()
              == Trap { TrapCode::OutOfBoundsMemoryAccess });
        CHECK(copySegment(source, 0, 3, std::span { target }, 2, TrapCode::OutOfBoundsTableAccess).error()
              == Trap { TrapCode::OutOfBoundsTableAccess });
        CHECK(target == std::to_array<int>({ 0, 0, 0, 0 }));
    }
}
