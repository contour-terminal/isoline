// SPDX-License-Identifier: Apache-2.0
#pragma once

// Parallel moves: a set of moves that happen at once, as at a control-flow edge (each target parameter takes
// its argument) or where several values change places between two instructions. Every move reads its
// source before any writes its destination, so a set may swap two registers or rotate a cycle. A machine has
// only sequential moves; sequentializeParallelMoves() orders them so that no move overwrites a place another
// still has to read, and breaks each cycle through one scratch place.

#include <memory_resource>
#include <span>
#include <vector>

#include <isoline/compiler/machine/MachineFunction.hpp>

namespace isoline::compiler::registerallocation
{

/// Where a value lives: a register or a stack slot (machine::Location).
using machine::Location;

/// One move of a value from one place to another (machine::Move).
using machine::Move;

/// Orders the parallel moves @p moves into sequential ones with the same effect.
///
/// A move whose destination no pending move reads goes first; when only cycles are left, the destination of
/// the lowest-indexed pending move is saved to @p scratch, its readers read the scratch instead, and the
/// cycle unwinds. Deterministic: the order depends only on @p moves' order. O(n log n) in the moves.
/// @param moves The moves, each destination a different place (Require), none of them @p scratch's
///              (Require). A move whose source is its destination is dropped.
/// @param scratch A place no move names: the register allocator's scratch slot. It takes the class of the
///                value it saves.
/// @param memory What the result and the temporaries allocate from.
/// @return The sequential moves: at most one more than the moves that change something per cycle.
[[nodiscard]] std::pmr::vector<Move> sequentializeParallelMoves(std::span<Move const> moves,
                                                                Location scratch,
                                                                std::pmr::memory_resource& memory);

} // namespace isoline::compiler::registerallocation
