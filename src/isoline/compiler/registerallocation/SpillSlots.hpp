// SPDX-License-Identifier: Apache-2.0
#pragma once

// Spill slots: one per spilled value, a slot reused once the value it held is no longer live. Lifetimes are
// taken in order of their start, and the slot that frees first is the top of a min-heap (interval
// partitioning: as few slots as the most lifetimes live at once, in O(n log n)), as P5 Decision "Register
// allocation" has it. First-fit packing, which scans the slots for a free one, is quadratic.

#include <cstdint>
#include <memory_resource>
#include <span>
#include <vector>

namespace isoline::compiler::registerallocation
{

/// The positions a spilled value's slot is in use: [start, end).
struct SlotLifetime
{
    uint32_t start = 0; ///< Its first position.
    uint32_t end = 0;   ///< One past its last.
};

/// Where each lifetime's value is spilled.
struct SlotPacking
{
    std::pmr::vector<uint32_t> slots; ///< Per lifetime, in the input's order, its slot's index.
    uint32_t slotCount = 0;           ///< How many slots there are.
    uint64_t steps = 0;               ///< The slot candidates looked at and the heap's comparisons: the work.
};

/// Packs @p lifetimes into as few slots as the most of them that overlap, deterministically: lifetimes are
/// taken by (start, index), and a slot is reused from the first one to free.
/// @param lifetimes The spilled values' lifetimes, in any order.
/// @param memory What the result and the temporaries allocate from.
/// @return Each lifetime's slot, the slot count, and the work done (O(n log n)).
[[nodiscard]] SlotPacking packSpillSlots(std::span<SlotLifetime const> lifetimes,
                                         std::pmr::memory_resource& memory);

} // namespace isoline::compiler::registerallocation
