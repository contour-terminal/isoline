// SPDX-License-Identifier: Apache-2.0
#pragma once

// Live intervals of a machine function's virtual registers, for linear-scan allocation (Wimmer and Franz,
// "Linear Scan Register Allocation on SSA Form", 2010). One backward pass over the blocks in reverse layout
// order: a block's live-out is its successors' live-ins and the arguments its edges pass; each instruction
// ends the intervals it defines and extends those it uses; and at a loop header every value live in is
// extended to the loop's end, which covers what the back edges carry without iterating. That extension is
// valid only on a layout whose loops are contiguous (verifyContiguousLoops()).
//
// Positions are even: a block takes one position for its entry, where its parameters are defined, and one
// per instruction, two apart. Ranges are half-open, [start, end). An instruction at position p reads its
// uses at p (their ranges end at p, unless the value lives on) and writes its definitions from p, so a
// result may take the register of an operand that dies there; an Early definition starts at p − 1, so it
// overlaps the uses and takes none of their registers. A definition never used lives [p, p + 1). The odd
// positions between instructions are for the moves the allocator inserts.

#include <cstdint>
#include <memory_resource>
#include <span>
#include <vector>

#include <isoline/compiler/intermediate/Dominators.hpp>
#include <isoline/compiler/machine/Linearize.hpp>
#include <isoline/compiler/machine/MachineFunction.hpp>

namespace isoline::compiler::machine
{

/// A half-open range of positions, [start, end).
struct LiveRange
{
    uint32_t start = 0; ///< The first position in it.
    uint32_t end = 0;   ///< One past the last.

    /// Whether both are the same range.
    [[nodiscard]] constexpr bool operator==(LiveRange const&) const noexcept = default;
};

/// Where one virtual register is live, and where it is used or defined.
struct LiveInterval
{
    VirtualRegister reg {};             ///< The register.
    std::pmr::vector<LiveRange> ranges; ///< Where it is live: sorted, disjoint and not adjacent.
    std::pmr::vector<uint32_t>
        usePositions; ///< The positions of the instructions that read or write it, sorted, once each.
};

/// The live intervals of a function, and the positions they are measured in.
struct LiveIntervals
{
    std::pmr::vector<LiveInterval> intervals; ///< Per virtual register, by its number.
    std::pmr::vector<uint32_t>
        instructionPositions;                ///< Per instruction, by index; Unnumbered if not laid out.
    std::pmr::vector<LiveRange> blockRanges; ///< Per block, by BlockId: its positions; empty if left out.
};

/// The live intervals of @p function's virtual registers, under the layout @p order.
/// @param function The function: no value defined in one block is redefined in another, and a value
///                 crossing a back edge does so as a block parameter (MachineFunction).
/// @param order Its layout, with contiguous loops (verifyContiguousLoops(); a precondition).
/// @param loops The loop nesting of @p function's control-flow graph.
/// @param memory What the intervals and the temporaries allocate from.
/// @return The intervals, and the positions of the instructions and blocks.
[[nodiscard]] LiveIntervals computeLiveIntervals(MachineFunction const& function,
                                                 BlockOrder const& order,
                                                 intermediate::LoopNesting const& loops,
                                                 std::pmr::memory_resource& memory);

} // namespace isoline::compiler::machine
