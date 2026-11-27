// SPDX-License-Identifier: Apache-2.0
#pragma once

// Block layout: the order the encoder emits a machine function's blocks in, and the order liveness numbers
// them in. Every loop is contiguous (its header first, then its other members, nested loops included), so
// that a loop is the range [header, loop end] of the layout, which liveness's loop extension relies on. Cold
// blocks (a branch hint's unlikely successor) go to the end of their innermost loop, not of the function, so
// that a cold block, a per-edge poll block or a split critical edge inside a loop keeps the loop contiguous.
// Within a loop, blocks and nested loops keep their reverse postorder. Nothing recurses; every walk keeps an
// explicit stack, and the layout is linear in the function.

#include <cstdint>
#include <expected>
#include <memory_resource>
#include <optional>
#include <span>
#include <vector>

#include <isoline/compiler/CompileError.hpp>
#include <isoline/compiler/intermediate/Dominators.hpp>
#include <isoline/compiler/machine/MachineFunction.hpp>

namespace isoline::compiler::machine
{

/// The order of a machine function's blocks: each block at most once, the entry first. Blocks no path
/// reaches are left out: never laid out, never encoded.
class BlockOrder
{
  public:
    /// The order @p blocks, of a function of @p blockCount blocks.
    /// @param blocks The blocks in layout order; each below @p blockCount and at most once (Require).
    /// @param blockCount How many blocks the function has.
    /// @param memory What the order allocates from, verifyContiguousLoops()'s temporaries included.
    BlockOrder(std::pmr::vector<BlockId> blocks, uint32_t blockCount, std::pmr::memory_resource& memory);

    /// The blocks, in layout order.
    [[nodiscard]] std::span<BlockId const> blocks() const noexcept { return _blocks; }

    /// The position of @p block in the layout.
    /// @return It; none for a block left out.
    [[nodiscard]] std::optional<uint32_t> position(BlockId block) const noexcept;

    /// How many blocks the function has, laid out or not.
    [[nodiscard]] uint32_t blockCount() const noexcept { return static_cast<uint32_t>(_positions.size()); }

    /// What the order allocates from.
    [[nodiscard]] std::pmr::memory_resource& memory() const noexcept { return *_memory; }

  private:
    std::pmr::memory_resource* _memory;
    std::pmr::vector<BlockId> _blocks;
    std::pmr::vector<uint32_t> _positions; ///< Per block, its position, or intermediate::Unnumbered.
};

/// The blocks of @p graph reachable from its entry, in reverse postorder of a depth-first walk that visits
/// successors in their edges' order. Iterative.
/// @param graph The graph.
/// @param memory What the order and the walk's stack allocate from.
[[nodiscard]] std::pmr::vector<uint32_t> reversePostorderOf(intermediate::ControlFlowGraph const& graph,
                                                            std::pmr::memory_resource& memory);

/// Lays out @p function's reachable blocks: the entry first, every loop contiguous with its header first,
/// cold blocks (and loops whose header is cold) at the end of their innermost enclosing loop, or of the
/// function for those in none; otherwise in reverse postorder. Verifies the result (verifyContiguousLoops()).
/// @param function The function.
/// @param loops The loop nesting of @p function's control-flow graph (controlFlowGraphOf()).
/// @param memory What the order and the temporaries allocate from.
/// @return The order; or InternalVerificationFailed (pass "linearize") should it not keep its loops
///         contiguous, a compiler bug refused, never run.
[[nodiscard]] std::expected<BlockOrder, CompileError> linearize(MachineFunction const& function,
                                                                intermediate::LoopNesting const& loops,
                                                                std::pmr::memory_resource& memory);

/// Checks that @p order lays every loop of @p loops out contiguously: the entry is first, every block of a
/// loop is laid out, and each loop's blocks (nested loops included) are one run of the layout that starts
/// at its header. The machine verifier's check before liveness, which is valid only on such a layout.
/// @param order The layout.
/// @param loops The loop nesting of the function's control-flow graph.
/// @return Nothing; or InternalVerificationFailed (pass "linearize").
[[nodiscard]] std::expected<void, CompileError> verifyContiguousLoops(BlockOrder const& order,
                                                                      intermediate::LoopNesting const& loops);

/// Per block, the layout position of the last block of the loop it heads: the largest position of the loop's
/// members, nested loops included; intermediate::Unnumbered for a block that heads no loop or is left out.
/// On a layout verifyContiguousLoops() accepts, the loop headed by h is the positions
/// [position(h), loopEndPositions()[h]].
/// @param order The layout.
/// @param loops The loop nesting of the function's control-flow graph.
/// @param memory What the result and the temporaries allocate from.
[[nodiscard]] std::pmr::vector<uint32_t> loopEndPositions(BlockOrder const& order,
                                                          intermediate::LoopNesting const& loops,
                                                          std::pmr::memory_resource& memory);

} // namespace isoline::compiler::machine
