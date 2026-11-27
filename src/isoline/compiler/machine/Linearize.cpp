// SPDX-License-Identifier: Apache-2.0
#include <isoline/compiler/machine/Linearize.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <utility>

namespace isoline::compiler::machine
{

using intermediate::Unnumbered;

namespace
{
    /// A block on an explicit depth-first stack: the block, and the index of its next successor to visit.
    struct Frame
    {
        uint32_t block;
        uint32_t next;
    };

    /// Where one loop lies in a layout: its members' smallest and largest positions, and how many there are,
    /// nested loops' members included.
    struct LoopExtent
    {
        uint32_t first = Unnumbered;
        uint32_t last = 0;
        uint32_t members = 0;
    };

    /// The refusal of a layout that does not keep its loops contiguous: a compiler bug.
    [[nodiscard]] std::unexpected<CompileError> notContiguous() noexcept
    {
        return std::unexpected { CompileError { .code = CompileErrorCode::InternalVerificationFailed,
                                                .pass = "linearize" } };
    }

    /// The blocks of @p graph reachable from the entry, in reverse postorder of a depth-first walk that
    /// visits successors in their edges' order.
    [[nodiscard]] std::pmr::vector<uint32_t> reversePostorder(intermediate::ControlFlowGraph const& graph,
                                                              std::pmr::memory_resource& memory)
    {
        auto visited = std::pmr::vector<uint8_t>(graph.blockCount(), 0, &memory);
        auto order = std::pmr::vector<uint32_t> { &memory };
        auto stack = std::pmr::vector<Frame> { &memory };
        visited[0] = 1;
        stack.push_back(Frame { .block = 0, .next = 0 });
        while (!stack.empty())
        {
            auto const top = stack.size() - 1;
            auto const successors = graph.successors(BlockId { stack[top].block });
            if (stack[top].next < successors.size())
            {
                auto const successor = std::to_underlying(successors[stack[top].next]);
                ++stack[top].next;
                if (visited[successor] == 0)
                {
                    visited[successor] = 1;
                    stack.push_back(Frame { .block = successor, .next = 0 });
                }
                continue;
            }
            order.push_back(stack[top].block);
            stack.pop_back();
        }
        std::ranges::reverse(order);
        return order;
    }

    /// Per block, the extent of the loop it heads in @p order (empty for a block that heads none). Loops are
    /// summed innermost first, each into its parent, so that nesting depth costs no walk up the parents.
    [[nodiscard]] std::pmr::vector<LoopExtent> loopExtents(BlockOrder const& order,
                                                           intermediate::LoopNesting const& loops,
                                                           std::pmr::memory_resource& memory)
    {
        auto const blockCount = order.blockCount();
        auto extents = std::pmr::vector<LoopExtent>(blockCount, LoopExtent {}, &memory);
        auto const blocks = order.blocks();
        for (auto const position: std::views::iota(uint32_t { 0 }, static_cast<uint32_t>(blocks.size())))
        {
            auto const block = blocks[position];
            auto const loop = loops.innermostLoop(block);
            if (!loop)
                continue;
            auto& extent = extents[std::to_underlying(*loop)];
            extent.first = std::min(extent.first, position);
            extent.last = std::max(extent.last, position);
            ++extent.members;
        }

        // Each header's nesting depth, by walking up to the first header whose depth is known.
        auto depth = std::pmr::vector<uint32_t>(blockCount, Unnumbered, &memory);
        auto headers = std::pmr::vector<uint32_t> { &memory };
        auto chain = std::pmr::vector<uint32_t> { &memory };
        for (auto const block: std::views::iota(uint32_t { 0 }, blockCount))
        {
            if (!loops.isHeader(BlockId { block }))
                continue;
            headers.push_back(block);
            chain.clear();
            auto walk = std::optional<uint32_t> { block };
            while (walk && depth[*walk] == Unnumbered)
            {
                chain.push_back(*walk);
                auto const parent = loops.parentLoop(BlockId { *walk });
                walk = parent ? std::optional { std::to_underlying(*parent) } : std::nullopt;
            }
            auto next = walk ? depth[*walk] + 1 : 0;
            for (auto const header: chain | std::views::reverse)
                depth[header] = next++;
        }

        // Innermost first: each loop's extent is complete before it joins its parent's.
        std::ranges::sort(headers, [&depth](uint32_t left, uint32_t right) {
            return depth[left] > depth[right] || (depth[left] == depth[right] && left < right);
        });
        for (auto const header: headers)
        {
            auto const parent = loops.parentLoop(BlockId { header });
            auto const& extent = extents[header];
            if (!parent || extent.members == 0)
                continue;
            auto& outer = extents[std::to_underlying(*parent)];
            outer.first = std::min(outer.first, extent.first);
            outer.last = std::max(outer.last, extent.last);
            outer.members += extent.members;
        }
        return extents;
    }

    /// The container linearize() places @p block in: the loop around it, by header, or @p root for none. A
    /// header is an item of the loop around its own.
    [[nodiscard]] uint32_t containerOf(BlockId block, intermediate::LoopNesting const& loops, uint32_t root)
    {
        auto const around = loops.isHeader(block) ? loops.parentLoop(block) : loops.innermostLoop(block);
        return around ? std::to_underlying(*around) : root;
    }

    /// A singly linked list of items, through a shared next array.
    struct ItemList
    {
        uint32_t head = Unnumbered;
        uint32_t tail = Unnumbered;
    };
} // namespace

BlockOrder::BlockOrder(std::pmr::vector<BlockId> blocks,
                       uint32_t blockCount,
                       std::pmr::memory_resource& memory):
    _memory { &memory }, _blocks { std::move(blocks), &memory }, _positions(blockCount, Unnumbered, &memory)
{
    Require(_blocks.size() <= blockCount);
    for (auto const position: std::views::iota(uint32_t { 0 }, static_cast<uint32_t>(_blocks.size())))
    {
        auto const block = _blocks[position];
        auto const index = std::to_underlying(block);
        Require(index < blockCount && _positions[index] == Unnumbered);
        _positions[index] = position;
    }
}

std::optional<uint32_t> BlockOrder::position(BlockId block) const noexcept
{
    auto const index = std::to_underlying(block);
    Require(index < _positions.size());
    if (_positions[index] == Unnumbered)
        return std::nullopt;
    return _positions[index];
}

std::expected<BlockOrder, CompileError> linearize(MachineFunction const& function,
                                                  intermediate::LoopNesting const& loops,
                                                  std::pmr::memory_resource& memory)
{
    auto const blockCount = function.blockCount();
    Require(blockCount > 0);
    auto const graph = controlFlowGraphOf(function, memory);
    auto const reachable = reversePostorder(graph, memory);

    // Each container (a loop, by its header, or the function, by blockCount) lists its items in reverse
    // postorder: its blocks and the loops nested directly in it, normal ones then cold ones. The entry is
    // never moved, cold or not.
    auto const root = blockCount;
    auto normal = std::pmr::vector<ItemList>(std::size_t { blockCount } + 1, ItemList {}, &memory);
    auto cold = std::pmr::vector<ItemList>(std::size_t { blockCount } + 1, ItemList {}, &memory);
    auto next = std::pmr::vector<uint32_t>(blockCount, Unnumbered, &memory);
    for (auto const block: reachable)
    {
        auto const isCold = block != 0 && function.block(BlockId { block }).hotness == Hotness::Cold;
        auto& list = (isCold ? cold : normal)[containerOf(BlockId { block }, loops, root)];
        if (list.tail == Unnumbered)
            list.head = block;
        else
            next[list.tail] = block;
        list.tail = block;
    }

    // Lay the containers out depth first: an item is laid out where it comes, and a loop's items right after
    // its header. The stack holds, per open container, the next item to lay out and whether the cold list
    // has begun.
    struct Open
    {
        uint32_t container;
        uint32_t item;
        Hotness list; ///< Which of the container's lists the cursor walks.
    };
    auto blocks = std::pmr::vector<BlockId> { &memory };
    blocks.reserve(reachable.size());
    auto stack = std::pmr::vector<Open> { &memory };
    stack.push_back(Open { .container = root, .item = normal[root].head, .list = Hotness::Normal });
    while (!stack.empty())
    {
        auto const top = stack.size() - 1;
        if (stack[top].item == Unnumbered)
        {
            if (stack[top].list == Hotness::Normal)
            {
                stack[top].list = Hotness::Cold;
                stack[top].item = cold[stack[top].container].head;
                continue;
            }
            stack.pop_back();
            continue;
        }
        auto const item = stack[top].item;
        stack[top].item = next[item];
        blocks.push_back(BlockId { item });
        if (loops.isHeader(BlockId { item }))
            stack.push_back(Open { .container = item, .item = normal[item].head, .list = Hotness::Normal });
    }

    auto order = BlockOrder { std::move(blocks), blockCount, memory };
    if (auto const verified = verifyContiguousLoops(order, loops); !verified)
        return std::unexpected { verified.error() }; // uncovered(linearize-breaks-a-loop)
    return order;
}

std::expected<void, CompileError> verifyContiguousLoops(BlockOrder const& order,
                                                        intermediate::LoopNesting const& loops)
{
    if (order.blocks().empty() || order.blocks().front() != BlockId { 0 })
        return notContiguous();
    for (auto const block: std::views::iota(uint32_t { 0 }, order.blockCount()))
        if (loops.innermostLoop(BlockId { block }) && !order.position(BlockId { block }))
            return notContiguous();
    auto const extents = loopExtents(order, loops, order.memory());
    for (auto const block: order.blocks())
    {
        if (!loops.isHeader(block))
            continue;
        auto const& extent = extents[std::to_underlying(block)];
        if (extent.first != *order.position(block) || extent.last - extent.first + 1 != extent.members)
            return notContiguous();
    }
    return {};
}

std::pmr::vector<uint32_t> loopEndPositions(BlockOrder const& order,
                                            intermediate::LoopNesting const& loops,
                                            std::pmr::memory_resource& memory)
{
    auto ends = std::pmr::vector<uint32_t>(order.blockCount(), Unnumbered, &memory);
    auto const extents = loopExtents(order, loops, memory);
    for (auto const block: order.blocks())
    {
        auto const& extent = extents[std::to_underlying(block)];
        if (loops.isHeader(block) && extent.members != 0)
            ends[std::to_underlying(block)] = extent.last;
    }
    return ends;
}

} // namespace isoline::compiler::machine
