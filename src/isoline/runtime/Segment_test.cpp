// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include <isoline/runtime/Interrupt.hpp>
#include <isoline/runtime/Segment.hpp>
#include <isoline/runtime/Trap.hpp>

using isoline::runtime::BulkPollBytes;
using isoline::runtime::copyOrder;
using isoline::runtime::copySegment;
using isoline::runtime::fits;
using isoline::runtime::forEachGranule;
using isoline::runtime::GranuleOrder;
using isoline::runtime::Trap;
using isoline::runtime::TrapCode;

namespace
{

/// A poll that always lets the copy go on.
constexpr auto Unpolled = []() noexcept -> std::expected<void, Trap> {
    return {};
};

} // namespace

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
        REQUIRE(
            copySegment(source, 1, 2, std::span { target }, 2, TrapCode::OutOfBoundsTableAccess, Unpolled));
        CHECK(target == std::to_array<int>({ 0, 0, 2, 3 }));
    }

    SECTION("an empty copy at either end is in bounds")
    {
        CHECK(copySegment(source, 3, 0, std::span { target }, 4, TrapCode::OutOfBoundsTableAccess, Unpolled));
        CHECK(copySegment(std::span<int const> {},
                          0,
                          0,
                          std::span { target },
                          0,
                          TrapCode::OutOfBoundsTableAccess,
                          Unpolled));
    }

    SECTION("either range out of bounds traps and copies nothing")
    {
        CHECK(copySegment(source, 2, 2, std::span { target }, 0, TrapCode::OutOfBoundsMemoryAccess, Unpolled)
                  .error()
              == Trap { TrapCode::OutOfBoundsMemoryAccess });
        CHECK(copySegment(source, 0, 3, std::span { target }, 2, TrapCode::OutOfBoundsTableAccess, Unpolled)
                  .error()
              == Trap { TrapCode::OutOfBoundsTableAccess });
        CHECK(target == std::to_array<int>({ 0, 0, 0, 0 }));
    }
}

TEST_CASE("Segment.copyPollsPerGranule")
{
    auto const segment = std::vector<std::byte>(200'000, std::byte { 0xAB });
    auto target = std::vector<std::byte>(segment.size());
    auto polls = 0;

    SECTION("a 200'000-byte copy polls before each of its four granules")
    {
        auto const counting = [&]() noexcept -> std::expected<void, Trap> {
            ++polls;
            return {};
        };
        REQUIRE(copySegment(std::span<std::byte const> { segment },
                            0,
                            segment.size(),
                            std::span { target },
                            0,
                            TrapCode::OutOfBoundsMemoryAccess,
                            counting));
        CHECK(polls == 4);
        CHECK(target == segment);
    }

    SECTION("a poll that fails stops the copy there, the granules before it copied")
    {
        auto const interruptedAfterTwo = [&]() noexcept -> std::expected<void, Trap> {
            ++polls;
            if (polls > 2)
                return isoline::runtime::trap(TrapCode::Interrupted);
            return {};
        };
        CHECK(copySegment(std::span<std::byte const> { segment },
                          0,
                          segment.size(),
                          std::span { target },
                          0,
                          TrapCode::OutOfBoundsMemoryAccess,
                          interruptedAfterTwo)
                  .error()
              == Trap { TrapCode::Interrupted });
        CHECK(polls == 3);
        CHECK(target[(2 * BulkPollBytes) - 1] == std::byte { 0xAB });
        CHECK(target[2 * BulkPollBytes] == std::byte { 0 });
    }

    SECTION("an out-of-bounds copy polls nothing")
    {
        auto const counting = [&]() noexcept -> std::expected<void, Trap> {
            ++polls;
            return {};
        };
        CHECK_FALSE(copySegment(std::span<std::byte const> { segment },
                                1,
                                segment.size(),
                                std::span { target },
                                0,
                                TrapCode::OutOfBoundsMemoryAccess,
                                counting));
        CHECK(polls == 0);
    }
}

TEST_CASE("Segment.granuleOrder")
{
    CHECK(copyOrder(0, 1) == GranuleOrder::Descending);
    CHECK(copyOrder(1, 1) == GranuleOrder::Ascending);
    CHECK(copyOrder(2, 1) == GranuleOrder::Ascending);

    auto const poll = []() noexcept -> std::expected<void, Trap> {
        return {};
    };
    auto const visit = [&](uint64_t count, GranuleOrder order) {
        auto granules = std::vector<std::pair<uint64_t, uint64_t>> {};
        REQUIRE(forEachGranule(count, 4, order, poll, [&](uint64_t offset, uint64_t size) {
            granules.emplace_back(offset, size);
        }));
        return granules;
    };
    using Granules = std::vector<std::pair<uint64_t, uint64_t>>;
    CHECK(visit(0, GranuleOrder::Ascending).empty());
    CHECK(visit(10, GranuleOrder::Ascending) == Granules { { 0, 4 }, { 4, 4 }, { 8, 2 } });
    CHECK(visit(10, GranuleOrder::Descending) == Granules { { 8, 2 }, { 4, 4 }, { 0, 4 } });
    CHECK(visit(8, GranuleOrder::Descending) == Granules { { 4, 4 }, { 0, 4 } });
}
