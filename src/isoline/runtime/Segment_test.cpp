// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include <isoline/runtime/Interrupt.hpp>
#include <isoline/runtime/Segment.hpp>
#include <isoline/runtime/Trap.hpp>
#include <isoline/runtime/Value.hpp>

using isoline::runtime::BulkPollBytes;
using isoline::runtime::copyItems;
using isoline::runtime::copyOrder;
using isoline::runtime::copySegment;
using isoline::runtime::fillItems;
using isoline::runtime::fits;
using isoline::runtime::forEachGranule;
using isoline::runtime::GranuleItems;
using isoline::runtime::GranuleOrder;
using isoline::runtime::Ref;
using isoline::runtime::Trap;
using isoline::runtime::TrapCode;

namespace
{

/// A poll that always lets the copy go on.
constexpr auto Unpolled = []() noexcept -> std::expected<void, Trap> {
    return {};
};

/// Bytes 0, 1, 2, … of @p size, each its index modulo 251: no two granules alike.
[[nodiscard]] std::vector<std::byte> patterned(std::size_t size)
{
    auto bytes = std::vector<std::byte>(size);
    for (auto const index: std::views::iota(std::size_t { 0 }, size))
        bytes[index] = static_cast<std::byte>(index % 251);
    return bytes;
}

/// A poll that fails at its @p failing-th call, counting its calls in @p polls.
[[nodiscard]] auto failingAt(int failing, int& polls)
{
    return [failing, &polls]() noexcept -> std::expected<void, Trap> {
        ++polls;
        if (polls == failing)
            return isoline::runtime::trap(TrapCode::Interrupted);
        return {};
    };
}

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

TEST_CASE("Segment.granuleItems")
{
    CHECK(GranuleItems<std::byte> == BulkPollBytes);
    CHECK(GranuleItems<Ref> == isoline::runtime::BulkPollElements);
}

TEST_CASE("Segment.fillItems")
{
    auto polls = 0;
    auto target = std::vector<std::byte>((3 * BulkPollBytes) + 5);

    SECTION("fills the range, polling before each granule")
    {
        auto poll = failingAt(0, polls);
        REQUIRE(fillItems(std::span { target },
                          1,
                          target.size() - 1,
                          std::byte { 7 },
                          TrapCode::OutOfBoundsMemoryAccess,
                          poll));
        CHECK(polls == 4);
        CHECK(target.front() == std::byte { 0 });
        CHECK(std::ranges::all_of(std::span { target }.subspan(1),
                                  [](std::byte b) { return b == std::byte { 7 }; }));
    }

    SECTION("out of bounds, it writes and polls nothing")
    {
        auto poll = failingAt(0, polls);
        auto const filled = fillItems(
            std::span { target }, 1, target.size(), std::byte { 7 }, TrapCode::OutOfBoundsMemoryAccess, poll);
        REQUIRE_FALSE(filled.has_value());
        CHECK(filled.error() == Trap { TrapCode::OutOfBoundsMemoryAccess });
        CHECK(polls == 0);
        CHECK(std::ranges::all_of(target, [](std::byte b) { return b == std::byte { 0 }; }));
    }

    SECTION("a failing poll leaves the granules before it filled")
    {
        auto poll = failingAt(3, polls);
        auto const filled = fillItems(
            std::span { target }, 0, target.size(), std::byte { 7 }, TrapCode::OutOfBoundsMemoryAccess, poll);
        REQUIRE_FALSE(filled.has_value());
        CHECK(filled.error() == Trap { TrapCode::Interrupted });
        CHECK(target[(2 * BulkPollBytes) - 1] == std::byte { 7 });
        CHECK(target[2 * BulkPollBytes] == std::byte { 0 });
    }
}

TEST_CASE("Segment.copyItems")
{
    auto polls = 0;
    auto const original = patterned((4 * BulkPollBytes) + 3);

    SECTION("an overlapping copy upwards is a memmove")
    {
        auto bytes = original;
        auto expected = original;
        std::memmove(expected.data() + 3, expected.data(), 4 * BulkPollBytes);
        auto poll = failingAt(0, polls);
        REQUIRE(copyItems(std::span<std::byte const> { bytes },
                          0,
                          std::span { bytes },
                          3,
                          4 * BulkPollBytes,
                          TrapCode::OutOfBoundsMemoryAccess,
                          poll));
        CHECK(polls == 4);
        CHECK(bytes == expected);
    }

    SECTION("an overlapping copy downwards is a memmove")
    {
        auto bytes = original;
        auto expected = original;
        std::memmove(expected.data(), expected.data() + 3, 4 * BulkPollBytes);
        auto poll = failingAt(0, polls);
        REQUIRE(copyItems(std::span<std::byte const> { bytes },
                          3,
                          std::span { bytes },
                          0,
                          4 * BulkPollBytes,
                          TrapCode::OutOfBoundsMemoryAccess,
                          poll));
        CHECK(bytes == expected);
    }

    SECTION("a copy onto itself changes nothing")
    {
        auto bytes = original;
        auto poll = failingAt(0, polls);
        REQUIRE(copyItems(std::span<std::byte const> { bytes },
                          5,
                          std::span { bytes },
                          5,
                          2 * BulkPollBytes,
                          TrapCode::OutOfBoundsMemoryAccess,
                          poll));
        CHECK(polls == 2);
        CHECK(bytes == original);
    }

    SECTION("upwards, a failing poll leaves the last granules copied, the first not")
    {
        auto bytes = original;
        auto poll = failingAt(2, polls);
        auto const copied = copyItems(std::span<std::byte const> { bytes },
                                      0,
                                      std::span { bytes },
                                      3,
                                      4 * BulkPollBytes,
                                      TrapCode::OutOfBoundsMemoryAccess,
                                      poll);
        REQUIRE_FALSE(copied.has_value());
        CHECK(copied.error() == Trap { TrapCode::Interrupted });
        // Only the last granule, [3 × granule, 4 × granule) of the source, went to 3 bytes above it.
        CHECK(std::ranges::equal(std::span { bytes }.subspan((3 * BulkPollBytes) + 3, BulkPollBytes),
                                 std::span { original }.subspan(3 * BulkPollBytes, BulkPollBytes)));
        CHECK(std::ranges::equal(std::span { bytes }.first((3 * BulkPollBytes) + 3),
                                 std::span { original }.first((3 * BulkPollBytes) + 3)));
    }

    SECTION("between two tables, either range out of bounds writes and polls nothing")
    {
        auto const from = std::vector<Ref>(4, Ref { .kind = isoline::runtime::RefKind::Func, .address = 9 });
        auto to = std::vector<Ref>(4);
        auto poll = failingAt(0, polls);
        CHECK_FALSE(copyItems(std::span<Ref const> { from },
                              1,
                              std::span { to },
                              0,
                              4,
                              TrapCode::OutOfBoundsTableAccess,
                              poll));
        CHECK_FALSE(copyItems(std::span<Ref const> { from },
                              0,
                              std::span { to },
                              1,
                              4,
                              TrapCode::OutOfBoundsTableAccess,
                              poll));
        CHECK(polls == 0);
        CHECK(to == std::vector<Ref>(4));
        REQUIRE(copyItems(std::span<Ref const> { from },
                          0,
                          std::span { to },
                          0,
                          4,
                          TrapCode::OutOfBoundsTableAccess,
                          poll));
        CHECK(to == from);
    }
}

TEST_CASE("Segment.rangeMaskAtTheEdges")
{
    using isoline::runtime::rangeMask;
    // Every offset and count of 8 bits against sizes below 2^7: the mask is all ones exactly where fits()
    // says the range fits, so the bound it is derived from is the check's, wherever the differences would
    // wrap.
    for (auto const size: { 0U, 1U, 2U, 63U, 64U, 126U, 127U })
    {
        for (auto const offset: std::views::iota(0U, 256U))
        {
            for (auto const count: std::views::iota(0U, 256U))
            {
                auto const expected =
                    offset <= size && count <= size - offset ? uint8_t { 0xFF } : uint8_t { 0 };
                auto const mask = rangeMask<uint8_t>(
                    static_cast<uint8_t>(offset), static_cast<uint8_t>(count), static_cast<uint8_t>(size));
                if (mask != expected)
                {
                    INFO("size " << size << ", offset " << offset << ", count " << count);
                    CHECK(mask == expected);
                }
            }
        }
    }
    // The 64-bit edges: sums past 2^64, offsets and counts of 2^63 and more, a size of 0.
    constexpr auto All = std::numeric_limits<uint64_t>::max();
    constexpr auto Half = uint64_t { 1 } << 63U;
    constexpr auto Size = uint64_t { 4 } * BulkPollBytes;
    struct Row
    {
        uint64_t offset;
        uint64_t count;
        uint64_t size;
        uint64_t mask;
    };
    constexpr auto Rows = std::to_array<Row>({
        { .offset = 0, .count = 0, .size = 0, .mask = All },
        { .offset = 0, .count = 1, .size = 0, .mask = 0 },
        { .offset = 1, .count = 0, .size = 0, .mask = 0 },
        { .offset = 0, .count = Size, .size = Size, .mask = All },
        { .offset = Size, .count = 0, .size = Size, .mask = All },
        { .offset = Size, .count = 1, .size = Size, .mask = 0 },
        { .offset = 1, .count = Size, .size = Size, .mask = 0 },
        { .offset = 8, .count = All - 7, .size = Size, .mask = 0 },
        { .offset = All - 7, .count = 16, .size = Size, .mask = 0 },
        { .offset = Half, .count = 0, .size = Size, .mask = 0 },
        { .offset = 0, .count = Half, .size = Size, .mask = 0 },
        { .offset = Half - 1, .count = 1, .size = Size, .mask = 0 },
        { .offset = 16, .count = Half + 16, .size = Size, .mask = 0 },
        { .offset = All, .count = All, .size = Size, .mask = 0 },
    });
    for (auto const& row: Rows)
    {
        INFO("offset " << row.offset << ", count " << row.count << ", size " << row.size);
        CHECK(rangeMask<uint64_t>(row.offset, row.count, row.size) == row.mask);
        CHECK(fits(row.offset, row.count, row.size) == (row.mask == All));
    }
    static_assert(rangeMask<uint64_t>(16, Half + 16, Size) == 0);
    static_assert(rangeMask<uint64_t>(0, Size, Size) == All);
}
