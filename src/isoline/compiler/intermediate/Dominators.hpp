// SPDX-License-Identifier: Apache-2.0
#pragma once

// Dominance and loop nesting over a control-flow graph view: block count and successors, so that the
// machine layer builds the same trees over its own blocks (R6). Nothing here recurses: every walk keeps
// an explicit stack, so a guest's nesting depth costs heap, not native stack. The dominator tree is
// Lengauer and Tarjan's, O(E log V) on every graph, irreducible ones included; dominance queries are O(1)
// by the tree's pre- and post-order numbers.

#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <vector>

#include <isoline/compiler/intermediate/Function.hpp>

namespace isoline::compiler::intermediate
{

/// A control-flow graph: per block its successors and predecessors. Block 0 is the entry.
class ControlFlowGraph
{
  public:
    /// The graph whose block b has the successors successors[successorBegins[b], successorBegins[b + 1]).
    /// @param successorBegins One entry per block and one past the last, non-decreasing, the last
    ///                        successors.size() (Require).
    /// @param successors Every block's successors, block after block; each below the block count (Require).
    /// @param memory What the graph allocates from.
    ControlFlowGraph(std::pmr::vector<uint32_t> successorBegins,
                     std::pmr::vector<BlockId> successors,
                     std::pmr::memory_resource& memory);

    /// How many blocks there are.
    [[nodiscard]] uint32_t blockCount() const noexcept
    {
        return static_cast<uint32_t>(_successorBegins.size() - 1);
    }

    /// The successors of @p block, in their terminator's order.
    [[nodiscard]] std::span<BlockId const> successors(BlockId block) const noexcept;

    /// The predecessors of @p block, in ascending order, once per edge.
    [[nodiscard]] std::span<BlockId const> predecessors(BlockId block) const noexcept;

  private:
    std::pmr::vector<uint32_t> _successorBegins;
    std::pmr::vector<BlockId> _successors;
    std::pmr::vector<uint32_t> _predecessorBegins;
    std::pmr::vector<BlockId> _predecessors;
};

/// The control-flow graph of @p function: each block's successors are its terminator's targets.
/// @param function A function whose terminators are well-formed (verify() checks them first).
/// @param memory What the graph allocates from.
[[nodiscard]] ControlFlowGraph controlFlowGraphOf(Function const& function,
                                                  std::pmr::memory_resource& memory);

/// The dominator tree of a control-flow graph, rooted at its entry.
class DominatorTree
{
  public:
    /// The tree of @p graph.
    /// @param graph The graph; need not outlive the tree.
    /// @param memory What the tree allocates from, its temporaries included.
    DominatorTree(ControlFlowGraph const& graph, std::pmr::memory_resource& memory);

    /// Whether @p block is reachable from the entry.
    [[nodiscard]] bool isReachable(BlockId block) const noexcept;

    /// The immediate dominator of @p block.
    /// @return It; none for the entry and for a block that is not reachable.
    [[nodiscard]] std::optional<BlockId> immediateDominator(BlockId block) const noexcept;

    /// Whether every path from the entry to @p dominated passes through @p dominator. Reflexive. A block
    /// that is not reachable is dominated by every block, as no path reaches it, and dominates none but
    /// such blocks.
    [[nodiscard]] bool dominates(BlockId dominator, BlockId dominated) const noexcept;

    /// The reachable blocks in reverse postorder of a depth-first walk that visits successors in their
    /// terminator's order: the entry first, every block before the blocks it dominates.
    [[nodiscard]] std::span<BlockId const> reversePostorder() const noexcept { return _reversePostorder; }

  private:
    /// A block's number where it has none: not reachable.
    static constexpr auto Unnumbered = UINT32_MAX;

    std::pmr::vector<uint32_t> _immediateDominator; ///< Per block, its idom's BlockId; Unnumbered if none.
    std::pmr::vector<uint32_t> _treeEntry; ///< Per block, its preorder number in the tree; Unnumbered.
    std::pmr::vector<uint32_t> _treeExit;  ///< Per block, the largest preorder number of its subtree.
    std::pmr::vector<BlockId> _reversePostorder;
};

/// The natural loops of a control-flow graph and how they nest. A loop is named by its header, the target
/// of a back edge (an edge to a block that dominates its source); an edge into a cycle's middle that no
/// block dominates (irreducible flow, which WebAssembly cannot express) makes no loop.
class LoopNesting
{
  public:
    /// The loops of @p graph.
    /// @param graph The graph; need not outlive the nesting.
    /// @param dominators The graph's dominator tree.
    /// @param memory What the nesting allocates from, its temporaries included.
    LoopNesting(ControlFlowGraph const& graph,
                DominatorTree const& dominators,
                std::pmr::memory_resource& memory);

    /// Whether @p block is a loop's header.
    [[nodiscard]] bool isHeader(BlockId block) const noexcept;

    /// The header of the innermost loop @p block is in; a header is in its own loop.
    /// @return It; none for a block in no loop.
    [[nodiscard]] std::optional<BlockId> innermostLoop(BlockId block) const noexcept;

    /// The header of the loop around the loop @p header heads.
    /// @param header A loop's header (isHeader()).
    /// @return It; none for an outermost loop.
    [[nodiscard]] std::optional<BlockId> parentLoop(BlockId header) const noexcept;

    /// The last block of the loop @p header heads, nested loops included: its member with the largest
    /// BlockId. On a function numbered in layout order with contiguous loops (machine::linearize), the
    /// loop is the blocks [header, loopEnd(header)].
    /// @param header A loop's header (isHeader()).
    [[nodiscard]] BlockId loopEnd(BlockId header) const noexcept;

  private:
    /// A block's entry where it has none.
    static constexpr auto None = UINT32_MAX;

    std::pmr::vector<uint32_t> _innermostLoop; ///< Per block, its innermost loop's header, or None.
    std::pmr::vector<uint32_t> _parentLoop;    ///< Per header, its parent loop's header, or None.
    std::pmr::vector<uint32_t> _loopEnd;       ///< Per header, its last member; None for other blocks.
};

} // namespace isoline::compiler::intermediate
