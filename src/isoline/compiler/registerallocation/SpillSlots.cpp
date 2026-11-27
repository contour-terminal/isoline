// SPDX-License-Identifier: Apache-2.0
#include <isoline/compiler/registerallocation/SpillSlots.hpp>

#include <algorithm>
#include <ranges>
#include <tuple>
#include <utility>

namespace isoline::compiler::registerallocation
{

SlotPacking packSpillSlots(std::span<SlotLifetime const> lifetimes, std::pmr::memory_resource& memory)
{
    auto result = SlotPacking { .slots = std::pmr::vector<uint32_t>(lifetimes.size(), 0, &memory) };
    auto order = std::pmr::vector<uint32_t> { &memory };
    order.reserve(lifetimes.size());
    for (auto const index: std::views::iota(uint32_t { 0 }, static_cast<uint32_t>(lifetimes.size())))
        order.push_back(index);
    std::ranges::sort(
        order, {}, [&](uint32_t index) { return std::tuple { lifetimes[index].start, index }; });

    // (free from, slot): a min-heap, its comparisons counted as the work.
    auto freeAt = std::pmr::vector<std::pair<uint32_t, uint32_t>> { &memory };
    freeAt.reserve(lifetimes.size());
    auto const later = [&](auto const& left, auto const& right) {
        ++result.steps;
        return left > right;
    };
    for (auto const index: order)
    {
        auto const& lifetime = lifetimes[index];
        ++result.steps; // the one slot candidate looked at: the heap's top
        auto slot = result.slotCount;
        if (!freeAt.empty() && freeAt.front().first <= lifetime.start)
        {
            std::ranges::pop_heap(freeAt, later);
            slot = freeAt.back().second;
            freeAt.pop_back();
        }
        else
            ++result.slotCount;
        result.slots[index] = slot;
        freeAt.emplace_back(lifetime.end, slot);
        std::ranges::push_heap(freeAt, later);
    }
    return result;
}

} // namespace isoline::compiler::registerallocation
