// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <ranges>
#include <span>
#include <type_traits>

#include <isoline/runtime/Interrupt.hpp>
#include <isoline/runtime/Trap.hpp>

namespace isoline::runtime
{

/// Whether @p count items from @p offset fit in @p size, without overflowing.
/// @param offset The first item's index.
/// @param count How many items.
/// @param size The size of what they must fit in.
/// @return Whether offset + count <= size.
[[nodiscard]] constexpr bool fits(uint64_t offset, uint64_t count, uint64_t size) noexcept
{
    return offset <= size && count <= size - offset;
}

/// The Spectre mask of @p count items from @p offset in something of @p size items: all ones where they fit
/// (fits()), zero where they do not, computed without a branch from the same bound, so that code running
/// ahead of a mispredicted check sees an empty range at the start, never one the guest chose.
///
/// It is the sign of the items left after the range, of the items left after its first, and of the offset
/// and the count themselves, any of them negative meaning the range does not fit. Sizes stay below 2^63 (what
/// they measure is allocated), so for an offset and a count below 2^63 the differences cannot wrap past the
/// sign, and an offset or a count of 2^63 or more never fits. Every bounds check of an executor derives its
/// mask from it: a memory access of width bytes, a table element, the ranges of a bulk operation.
///
/// A template over the unsigned type, so that a test can try every pair of a narrow one.
/// @param offset The first item.
/// @param count How many items.
/// @param size The size; below half the type's range.
/// @return The mask.
template <std::unsigned_integral U>
[[nodiscard]] constexpr U rangeMask(U offset, U count, U size) noexcept
{
    using Signed = std::make_signed_t<U>;
    constexpr auto SignShift = std::numeric_limits<U>::digits - 1;
    auto const room = static_cast<Signed>(static_cast<U>(size - offset));
    auto const left = static_cast<Signed>(static_cast<U>(size - offset - count));
    auto const signs = left | room | static_cast<Signed>(offset) | static_cast<Signed>(count);
    return static_cast<U>(~static_cast<U>(signs >> SignShift));
}

/// A poll a bulk operation calls before each granule: Store::poll(PollSite::BulkGranule), or a test's.
template <typename Poll>
concept GranulePoll = std::is_nothrow_invocable_r_v<std::expected<void, Trap>, Poll&>;

/// The order in which a bulk operation visits its granules.
enum class GranuleOrder : uint8_t
{
    Ascending,  ///< From the first item up: a fill, or a copy whose destination is not above its source.
    Descending, ///< From the last granule down: a copy whose destination is above its source, so that
                ///< no granule overwrites the source of one still to come.
};

/// The granule order of a copy from @p source to @p destination: what keeps an overlapping copy
/// within one memory or table correct, and the order every engine copies in.
/// @param source The first source item.
/// @param destination The first destination item.
/// @return Descending when @p destination is above @p source; Ascending otherwise.
[[nodiscard]] constexpr GranuleOrder copyOrder(uint64_t source, uint64_t destination) noexcept
{
    return destination > source ? GranuleOrder::Descending : GranuleOrder::Ascending;
}

/// Runs @p work over the items [0, @p count) in granules of @p granule items, the last one possibly
/// shorter, polling before each: the bounded unit of work between two polls of a bulk operation.
/// Nothing is polled when @p count is 0.
/// @param count How many items.
/// @param granule The items of one granule; not 0.
/// @param order Which granule comes first.
/// @param poll Polled before each granule; its error ends the work there.
/// @param work Called with each granule's first item and its size.
/// @return Nothing, or the poll's error.
template <GranulePoll Poll, typename Work>
[[nodiscard]] std::expected<void, Trap> forEachGranule(
    uint64_t count, uint64_t granule, GranuleOrder order, Poll& poll, Work const& work)
{
    auto const granules = (count / granule) + (count % granule != 0 ? 1 : 0);
    for (auto const index: std::views::iota(uint64_t { 0 }, granules))
    {
        auto const ordinal = order == GranuleOrder::Ascending ? index : granules - 1 - index;
        auto const offset = ordinal * granule;
        if (auto const polled = poll(); !polled)
            return polled;
        work(offset, std::min(granule, count - offset));
    }
    return {};
}

/// The items of one granule of a bulk operation over items of type @p T: BulkPollBytes of them, so 65'536
/// bytes of a memory or BulkPollElements references of a table.
template <typename T>
inline constexpr auto GranuleItems = BulkPollBytes / sizeof(T);

/// Fills @p count items of @p target from @p destination with @p value, as memory.fill and table.fill do:
/// the range is checked before anything is written, so an out-of-bounds fill changes nothing, and masked by
/// rangeMask(), so that code ahead of a mispredicted check fills nothing. The fill runs
/// upwards in granules of GranuleItems<T>, polling before each, so a poll that fails leaves the granules
/// before it filled.
///
/// Every engine runs a fill through it (the reference interpreter and the production one), so that they poll
/// and leave partial writes alike.
/// @param target The memory's bytes or the table's elements.
/// @param destination The first item filled.
/// @param count How many items.
/// @param value What each becomes.
/// @param outOfBounds The trap an out-of-bounds range ends with.
/// @param poll Polled before each granule.
/// @return Nothing, the trap @p outOfBounds, or the poll's error.
template <typename T, GranulePoll Poll>
[[nodiscard]] std::expected<void, Trap> fillItems(std::span<T> target,
                                                  uint64_t destination,
                                                  uint64_t count,
                                                  std::type_identity_t<T> const& value,
                                                  TrapCode outOfBounds,
                                                  Poll& poll)
{
    if (!fits(destination, count, target.size()))
        return trap(outOfBounds);
    // Spectre: ahead of a mispredicted check the range is masked to an empty one at the start (rangeMask()).
    auto const mask = rangeMask<uint64_t>(destination, count, target.size());
    destination &= mask;
    count &= mask;
    return forEachGranule(
        count, GranuleItems<T>, GranuleOrder::Ascending, poll, [&](uint64_t offset, uint64_t size) {
            std::ranges::fill(target.subspan(static_cast<std::size_t>(destination + offset),
                                             static_cast<std::size_t>(size)),
                              value);
        });
}

/// Copies @p count items from @p source of @p from to @p destination of @p to, as memory.copy and
/// table.copy do, @p from and @p to being the same memory or table or two of them: both ranges are checked
/// before anything is written, so an out-of-bounds copy changes nothing, and masked by rangeMask(), so that
/// code ahead of a mispredicted check copies nothing. The copy runs in granules of
/// GranuleItems<T>, polling before each, in copyOrder()'s order, and within a granule in the same direction,
/// so that an overlapping copy within one memory or table is correct and a poll that fails leaves the
/// granules before it copied.
///
/// Every engine runs a copy through it (the reference interpreter and the production one), so that they poll
/// and leave partial writes alike.
/// @param from The source's bytes or elements.
/// @param source The first item copied.
/// @param to The destination's bytes or elements; may be @p from.
/// @param destination Where the first item goes.
/// @param count How many items.
/// @param outOfBounds The trap either range out of bounds ends with.
/// @param poll Polled before each granule.
/// @return Nothing, the trap @p outOfBounds, or the poll's error.
template <typename T, GranulePoll Poll>
[[nodiscard]] std::expected<void, Trap> copyItems(std::span<T const> from,
                                                  uint64_t source,
                                                  std::type_identity_t<std::span<T>> to,
                                                  uint64_t destination,
                                                  uint64_t count,
                                                  TrapCode outOfBounds,
                                                  Poll& poll)
{
    if (!fits(source, count, from.size()) || !fits(destination, count, to.size()))
        return trap(outOfBounds);
    // Spectre: ahead of a mispredicted check both ranges are masked to empty ones at the start (rangeMask()),
    // so no speculative copy moves host bytes into the guest's memory, where its next load could forward
    // them.
    auto const mask =
        rangeMask<uint64_t>(source, count, from.size()) & rangeMask<uint64_t>(destination, count, to.size());
    source &= mask;
    destination &= mask;
    count &= mask;
    auto const order = copyOrder(source, destination);
    return forEachGranule(count, GranuleItems<T>, order, poll, [&](uint64_t offset, uint64_t size) {
        auto const granuleFrom =
            from.subspan(static_cast<std::size_t>(source + offset), static_cast<std::size_t>(size));
        auto const granuleTo =
            to.subspan(static_cast<std::size_t>(destination + offset), static_cast<std::size_t>(size));
        // A copy onto itself changes nothing, and std::ranges::copy may not overlap it.
        if (granuleFrom.data() == granuleTo.data())
            return;
        if (order == GranuleOrder::Ascending)
            std::ranges::copy(granuleFrom, granuleTo.begin());
        else
            std::ranges::copy_backward(granuleFrom, granuleTo.end());
    });
}

/// Copies part of a segment into a table or memory, as table.init and memory.init do, and as
/// instantiation does with each whole active segment: both ranges are checked before anything is
/// copied, so an out-of-bounds copy changes nothing, and masked by rangeMask(), so that code ahead of a
/// mispredicted check copies nothing. The copy runs in granules of BulkPollBytes
/// bytes, polling before each, so a poll that fails leaves the granules before it copied.
/// @param segment The segment's items: element references or data bytes; empty once dropped.
/// @param source The first item of @p segment to copy.
/// @param count How many items to copy.
/// @param target The table's elements or the memory's bytes; of the item type @p segment names.
/// @param destination Where in @p target the first item goes.
/// @param outOfBounds The trap either range out of bounds ends with.
/// @param poll Polled before each granule.
/// @return Nothing, the trap @p outOfBounds, or the poll's error.
template <typename T, GranulePoll Poll>
[[nodiscard]] std::expected<void, Trap> copySegment(std::span<T const> segment,
                                                    uint64_t source,
                                                    uint64_t count,
                                                    std::type_identity_t<std::span<T>> target,
                                                    uint64_t destination,
                                                    TrapCode outOfBounds,
                                                    Poll poll)
{
    if (!fits(source, count, segment.size()) || !fits(destination, count, target.size()))
        return trap(outOfBounds);
    // Spectre: ahead of a mispredicted check both ranges are masked to empty ones at the start (rangeMask()).
    auto const mask = rangeMask<uint64_t>(source, count, segment.size())
                      & rangeMask<uint64_t>(destination, count, target.size());
    source &= mask;
    destination &= mask;
    count &= mask;
    // A segment is never the table or memory it is copied into, so the granules go upwards.
    return forEachGranule(
        count, GranuleItems<T>, GranuleOrder::Ascending, poll, [&](uint64_t offset, uint64_t size) {
            std::ranges::copy(
                segment.subspan(static_cast<std::size_t>(source + offset), static_cast<std::size_t>(size)),
                target.begin() + static_cast<std::ptrdiff_t>(destination + offset));
        });
}

} // namespace isoline::runtime
