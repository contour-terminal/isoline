// SPDX-License-Identifier: Apache-2.0
#pragma once

// The stretches of a function that no memory motion interrupts: what a guard's proof is good for. A guard
// (checkMemory, inBoundsAddress, checkTable, checkDispatch) returns a pointer into the store's current
// storage; an operation that may grow or move a memory or table (MemoryMotionTable, Guards.hpp) may leave
// that pointer dangling. So a pointer is used only in the stretch its guard ran in: the verifier refuses any
// other use (VerifierErrorCode::AddressAcrossMemoryMotion), and EliminateBoundsChecks takes a dominating
// check's proof only within its stretch. Both read this one numbering.
//
// Every program point is in one stretch: the entry's, one that starts right after each operation that may
// move memory, or one that starts at a block whose incoming edges arrive in different stretches. Two points
// share a stretch only when no path from the one to the other passes an operation that may move memory: a
// point of stretch s is dominated by where s starts, every path to it passed that start last with no motion
// since, and every point of s that dominates it lies after that start on each such path.

#include <cstdint>
#include <limits>
#include <memory_resource>
#include <vector>

#include <isoline/compiler/intermediate/Dominators.hpp>
#include <isoline/compiler/intermediate/Function.hpp>

namespace isoline::compiler::intermediate
{

/// A stretch: 0 is the entry's, 1 + i the one after the motion at instruction i, 1 + instructionCount() + b
/// the one that starts at block b's entry.
using Stretch = uint64_t;

/// The stretch of a point no path from the entry reaches.
inline constexpr auto UnreachedStretch = std::numeric_limits<Stretch>::max();

/// The entry stretch of every block of a function: a forward walk from the entry that changes each block's
/// at most twice (unreached, one stretch, its own), so it visits each block at most three times.
class MemoryStretches
{
  public:
    /// The stretches of @p function, whose control-flow graph is @p graph.
    /// @param function A function whose terminators are well-formed.
    /// @param graph Its control-flow graph; need not outlive this.
    /// @param memory What this allocates from.
    MemoryStretches(Function const& function,
                    ControlFlowGraph const& graph,
                    std::pmr::memory_resource& memory);

    /// The stretch @p block's entry is in; UnreachedStretch when no path reaches it.
    [[nodiscard]] Stretch entryOf(BlockId block) const noexcept
    {
        return _entries[std::to_underlying(block)];
    }

    /// The stretch that starts after the instruction at @p index when it may move memory; @p current
    /// otherwise. Walking a block's listed instructions from entryOf() with it gives each one's stretch.
    /// @param function The function.
    /// @param index An instruction it lists.
    /// @param current The stretch the instruction runs in.
    [[nodiscard]] static Stretch after(Function const& function, uint32_t index, Stretch current) noexcept;

  private:
    std::pmr::vector<Stretch> _entries;
};

} // namespace isoline::compiler::intermediate
