// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cstdint>
#include <memory_resource>
#include <ranges>
#include <vector>

#include <isoline/compiler/registerallocation/SpillSlots.hpp>

using isoline::compiler::registerallocation::packSpillSlots;
using isoline::compiler::registerallocation::SlotLifetime;

namespace
{

/// The most packSpillSlots() may work on @p count lifetimes: one look at the heap's top and a heap
/// operation of at most 2 × log2 comparisons each, O(n log n).
uint64_t bound(uint64_t count)
{
    return count * (1 + (2 * (std::bit_width(count) + 1)));
}

} // namespace

TEST_CASE("SpillSlots.packIntoTheFewestSlots")
{
    auto memory = std::pmr::monotonic_buffer_resource {};
    // [0, 10) [2, 4) [5, 12) [11, 20) [12, 14): never more than two live at once, so two slots, each reused
    // from the one that freed first.
    auto const lifetimes = std::to_array<SlotLifetime>({
        { .start = 0, .end = 10 },
        { .start = 2, .end = 4 },
        { .start = 5, .end = 12 },
        { .start = 11, .end = 20 },
        { .start = 12, .end = 14 },
    });
    auto const packing = packSpillSlots(lifetimes, memory);
    CHECK(packing.slotCount == 2);
    CHECK(packing.slots[0] != packing.slots[1]);
    CHECK(packing.slots[2] == packing.slots[1]); // [2, 4) ended before [5, 12) began
    CHECK(packing.slots[3] == packing.slots[0]); // [0, 10) before [11, 20)
    CHECK(packing.slots[4] == packing.slots[2]); // [5, 12) before [12, 14)
    // Overlapping lifetimes never share a slot.
    for (auto const left: std::views::iota(std::size_t { 0 }, lifetimes.size()))
        for (auto const right: std::views::iota(left + 1, lifetimes.size()))
            if (lifetimes[left].start < lifetimes[right].end && lifetimes[right].start < lifetimes[left].end)
                CHECK(packing.slots[left] != packing.slots[right]);
    CHECK(packSpillSlots({}, memory).slotCount == 0);
}

TEST_CASE("SpillSlots.workIsNLogN")
{
    // The crafted worst cases, counted rather than timed: n lifetimes live at once (a new slot each, a push
    // onto a heap of all of them), and a staircase where each reuses the slot the one before freed (a pop and
    // a push each). First-fit packing looks at every slot for the first and is quadratic there.
    auto memory = std::pmr::monotonic_buffer_resource {};
    for (auto const count: { 1'000U, 4'000U, 50'000U })
    {
        auto overlapping = std::vector<SlotLifetime> {};
        auto staircase = std::vector<SlotLifetime> {};
        for (auto const index: std::views::iota(0U, count))
        {
            overlapping.push_back(SlotLifetime { .start = index, .end = 2 * count });
            staircase.push_back(SlotLifetime { .start = 2 * index, .end = (2 * index) + 3 });
        }
        auto const wide = packSpillSlots(overlapping, memory);
        CHECK(wide.slotCount == count);
        CHECK(wide.steps <= bound(count));
        auto const narrow = packSpillSlots(staircase, memory);
        CHECK(narrow.slotCount == 2);
        CHECK(narrow.steps <= bound(count));
    }
}
