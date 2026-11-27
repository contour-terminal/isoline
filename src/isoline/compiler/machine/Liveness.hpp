// SPDX-License-Identifier: Apache-2.0
#pragma once

// Live intervals of a machine function's virtual registers, for linear-scan allocation (Wimmer and Franz,
// "Linear Scan Register Allocation on SSA Form", 2010).
//
// Liveness is two steps. First, the live-in set of every block as a dataflow fixpoint over the control-flow
// graph, not the layout: live-in(b) = uses(b) ∪ (live-out(b) − defs(b)), live-out(b) the union of its
// successors' live-ins, iterated with a worklist seeded in postorder until nothing changes. The layout cannot
// stand in for it: linearize() moves cold blocks after blocks they jump to, so a forward edge may point
// backwards in the layout, and a single backward pass in layout order (which handles back edges by extending
// a loop header's live-ins to the loop's end) would miss what lives through such a block. Then the ranges,
// one backward walk per block in reverse layout order from its live-out. The sets hold only the values live
// across a block boundary (used in a block they are not defined in, or before their definition), one bit per
// block each; their size, values × blocks, is charged against CompileLimits::maxLiveSetBits before any is
// built, and the cancellation is polled as the fixpoint and the walk proceed.
//
// Positions are even: a block takes one position for its entry, where its parameters are defined, and one
// per instruction, two apart. Ranges are half-open, [start, end). An instruction at position p reads its
// uses at p (their ranges end at p, unless the value lives on) and writes its definitions from p, so a
// result may take the register of an operand that dies there; an Early definition starts at p − 1, so it
// overlaps the uses and takes none of their registers. A definition never used lives [p, p + 1). An edge's
// arguments are read at its block's terminator and live to the block's end. The odd positions between
// instructions are for the moves the allocator inserts.
//
// P5 has no exception edges: exception handling is refused before compilation (P5b, which adds it, gives a
// throwing call an edge to its handler, and liveness then takes those edges as it takes the others).

#include <cstdint>
#include <expected>
#include <memory_resource>
#include <span>
#include <vector>

#include <isoline/compiler/CompileConfig.hpp>
#include <isoline/compiler/CompileError.hpp>
#include <isoline/compiler/CompileLimits.hpp>
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
/// @param function The function: no value is defined in two blocks, and a value crossing a back edge does so
///                 as a block parameter (MachineFunction).
/// @param order Its layout, with contiguous loops (verifyContiguousLoops(), checked).
/// @param loops The loop nesting of @p function's control-flow graph.
/// @param limits The compile's caps: maxLiveSetBits.
/// @param cancellation Polled every 4'096 blocks of the fixpoint and of the walk.
/// @param memory What the intervals and the temporaries allocate from.
/// @return The intervals, and the positions of the instructions and blocks; LiveSetTooLarge (values live
///         across blocks × blocks requested, the cap as the limit) before any set is built; Cancelled; or
///         InternalVerificationFailed (pass "liveness") for a value defined in two blocks or a layout whose
///         loops are not contiguous, a compiler bug refused, never run.
[[nodiscard]] std::expected<LiveIntervals, CompileError> computeLiveIntervals(
    MachineFunction const& function,
    BlockOrder const& order,
    intermediate::LoopNesting const& loops,
    CompileLimits const& limits,
    CompileCancellation const& cancellation,
    std::pmr::memory_resource& memory);

} // namespace isoline::compiler::machine
