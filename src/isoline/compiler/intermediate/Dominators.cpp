// SPDX-License-Identifier: Apache-2.0
#include <isoline/compiler/intermediate/Dominators.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <utility>

namespace isoline::compiler::intermediate
{

namespace
{
    /// An entry of a per-block or per-number array that names nothing.
    constexpr auto Nothing = std::numeric_limits<uint32_t>::max();

    /// @p value as a count or index of a graph, which stays below 2^32 (Require).
    [[nodiscard]] uint32_t narrow(std::size_t value) noexcept
    {
        Require(value < Nothing);
        return static_cast<uint32_t>(value);
    }

    /// A block on an explicit depth-first stack: the block, and the index of its next child to visit.
    struct Frame
    {
        uint32_t block;
        uint32_t next;
    };
} // namespace

ControlFlowGraph::ControlFlowGraph(std::pmr::vector<uint32_t> successorBegins,
                                   std::pmr::vector<BlockId> successors,
                                   std::pmr::memory_resource& memory):
    _successorBegins { std::move(successorBegins), &memory },
    _successors { std::move(successors), &memory },
    _predecessorBegins { &memory },
    _predecessors { &memory }
{
    Require(!_successorBegins.empty());
    Require(_successorBegins.front() == 0 && _successorBegins.back() == _successors.size());
    Require(std::ranges::is_sorted(_successorBegins));
    auto const count = blockCount();
    for (auto const successor: _successors)
        Require(std::to_underlying(successor) < count);

    // Predecessors: count each block's, then place each edge, sources in ascending order.
    _predecessorBegins.assign(std::size_t { count } + 1, 0);
    for (auto const successor: _successors)
        ++_predecessorBegins[std::to_underlying(successor) + 1];
    for (auto const block: std::views::iota(uint32_t { 0 }, count))
        _predecessorBegins[block + 1] += _predecessorBegins[block];
    _predecessors.assign(_successors.size(), BlockId {});
    auto next = std::pmr::vector<uint32_t>(_predecessorBegins.begin(), _predecessorBegins.end() - 1, &memory);
    for (auto const block: std::views::iota(uint32_t { 0 }, count))
        for (auto const successor: this->successors(BlockId { block }))
            _predecessors[next[std::to_underlying(successor)]++] = BlockId { block };
}

std::span<BlockId const> ControlFlowGraph::successors(BlockId block) const noexcept
{
    auto const index = std::to_underlying(block);
    Require(index < blockCount());
    return std::span { _successors }.subspan(_successorBegins[index],
                                             _successorBegins[index + 1] - _successorBegins[index]);
}

std::span<BlockId const> ControlFlowGraph::predecessors(BlockId block) const noexcept
{
    auto const index = std::to_underlying(block);
    Require(index < blockCount());
    return std::span { _predecessors }.subspan(_predecessorBegins[index],
                                               _predecessorBegins[index + 1] - _predecessorBegins[index]);
}

ControlFlowGraph controlFlowGraphOf(Function const& function, std::pmr::memory_resource& memory)
{
    auto begins = std::pmr::vector<uint32_t> { &memory };
    auto successors = std::pmr::vector<BlockId> { &memory };
    begins.reserve(std::size_t { function.blockCount() } + 1);
    for (auto const block: std::views::iota(uint32_t { 0 }, function.blockCount()))
    {
        begins.push_back(narrow(successors.size()));
        if (auto const index = function.terminator(BlockId { block }))
            function.forEachTarget(function.instruction(*index), [&successors](BranchTarget const& target) {
                successors.push_back(target.block);
            });
    }
    begins.push_back(narrow(successors.size()));
    return ControlFlowGraph { std::move(begins), std::move(successors), memory };
}

DominatorTree::DominatorTree(ControlFlowGraph const& graph, std::pmr::memory_resource& memory):
    _immediateDominator(graph.blockCount(), Nothing, &memory),
    _treeEntry(graph.blockCount(), Nothing, &memory),
    _treeExit(graph.blockCount(), Nothing, &memory),
    _reversePostorder { &memory }
{
    auto const blockCount = graph.blockCount();
    if (blockCount == 0)
        return;

    // Depth-first numbering from the entry with an explicit stack: per block its preorder number, per
    // number its block and its parent's number; the postorder on the way out.
    auto number = std::pmr::vector<uint32_t>(blockCount, Nothing, &memory);
    auto vertex = std::pmr::vector<uint32_t> { &memory };
    auto parent = std::pmr::vector<uint32_t> { &memory };
    auto stack = std::pmr::vector<Frame> { &memory };
    number[0] = 0;
    vertex.push_back(0);
    parent.push_back(Nothing);
    stack.push_back(Frame { .block = 0, .next = 0 });
    while (!stack.empty())
    {
        auto& top = stack.back();
        auto const successors = graph.successors(BlockId { top.block });
        if (top.next == successors.size())
        {
            _reversePostorder.push_back(BlockId { top.block });
            stack.pop_back();
            continue;
        }
        auto const successor = std::to_underlying(successors[top.next++]);
        if (number[successor] != Nothing)
            continue;
        number[successor] = narrow(vertex.size());
        parent.push_back(number[top.block]);
        vertex.push_back(successor);
        stack.push_back(Frame { .block = successor, .next = 0 });
    }
    std::ranges::reverse(_reversePostorder);

    // Lengauer and Tarjan's semidominators and immediate dominators over preorder numbers, with path
    // compression done by an explicit path rather than recursion: O(E log V) whatever the graph.
    auto const count = narrow(vertex.size());
    auto semi = std::pmr::vector<uint32_t>(count, 0, &memory);
    auto label = std::pmr::vector<uint32_t>(count, 0, &memory);
    auto ancestor = std::pmr::vector<uint32_t>(count, Nothing, &memory);
    auto idom = std::pmr::vector<uint32_t>(count, Nothing, &memory);
    auto bucketHead = std::pmr::vector<uint32_t>(count, Nothing, &memory);
    auto bucketNext = std::pmr::vector<uint32_t>(count, Nothing, &memory);
    auto path = std::pmr::vector<uint32_t> { &memory };
    for (auto const index: std::views::iota(uint32_t { 0 }, count))
    {
        semi[index] = index;
        label[index] = index;
    }
    auto const evaluate = [&](uint32_t node) {
        if (ancestor[node] == Nothing)
            return node;
        path.clear();
        auto walk = node;
        while (ancestor[ancestor[walk]] != Nothing)
        {
            path.push_back(walk);
            walk = ancestor[walk];
        }
        for (auto const compressed: path | std::views::reverse)
        {
            auto const up = ancestor[compressed];
            if (semi[label[up]] < semi[label[compressed]])
                label[compressed] = label[up];
            ancestor[compressed] = ancestor[up];
        }
        return label[node];
    };
    for (auto const node: std::views::iota(uint32_t { 1 }, count) | std::views::reverse)
    {
        for (auto const predecessor: graph.predecessors(BlockId { vertex[node] }))
        {
            auto const from = number[std::to_underlying(predecessor)];
            if (from == Nothing)
                continue; // not reachable: no path from the entry runs through it
            auto const candidate = evaluate(from);
            semi[node] = std::min(semi[node], semi[candidate]);
        }
        bucketNext[node] = bucketHead[semi[node]];
        bucketHead[semi[node]] = node;
        auto const up = parent[node];
        ancestor[node] = up;
        auto member = bucketHead[up];
        while (member != Nothing)
        {
            auto const candidate = evaluate(member);
            idom[member] = semi[candidate] < semi[member] ? candidate : up;
            member = bucketNext[member];
        }
        bucketHead[up] = Nothing;
    }
    for (auto const node: std::views::iota(uint32_t { 1 }, count))
        if (idom[node] != semi[node])
            idom[node] = idom[idom[node]];

    // The tree by block, then its pre- and post-order numbering, again with an explicit stack.
    for (auto const node: std::views::iota(uint32_t { 1 }, count))
        _immediateDominator[vertex[node]] = vertex[idom[node]];
    auto childBegin = std::pmr::vector<uint32_t>(std::size_t { blockCount } + 1, 0, &memory);
    for (auto const node: std::views::iota(uint32_t { 1 }, count))
        ++childBegin[vertex[idom[node]] + 1];
    for (auto const block: std::views::iota(uint32_t { 0 }, blockCount))
        childBegin[block + 1] += childBegin[block];
    auto children = std::pmr::vector<uint32_t>(count == 0 ? 0 : count - 1, 0, &memory);
    auto nextChild = std::pmr::vector<uint32_t>(childBegin.begin(), childBegin.end() - 1, &memory);
    for (auto const node: std::views::iota(uint32_t { 1 }, count))
        children[nextChild[vertex[idom[node]]]++] = vertex[node];
    auto preorder = uint32_t { 0 };
    _treeEntry[0] = preorder++;
    stack.push_back(Frame { .block = 0, .next = childBegin[0] });
    while (!stack.empty())
    {
        auto& top = stack.back();
        if (top.next == childBegin[top.block + 1])
        {
            _treeExit[top.block] = preorder - 1;
            stack.pop_back();
            continue;
        }
        auto const child = children[top.next++];
        _treeEntry[child] = preorder++;
        stack.push_back(Frame { .block = child, .next = childBegin[child] });
    }
}

bool DominatorTree::isReachable(BlockId block) const noexcept
{
    Require(std::to_underlying(block) < _treeEntry.size());
    return _treeEntry[std::to_underlying(block)] != Nothing;
}

std::optional<BlockId> DominatorTree::immediateDominator(BlockId block) const noexcept
{
    Require(std::to_underlying(block) < _immediateDominator.size());
    auto const dominator = _immediateDominator[std::to_underlying(block)];
    if (dominator == Nothing)
        return std::nullopt;
    return BlockId { dominator };
}

bool DominatorTree::dominates(BlockId dominator, BlockId dominated) const noexcept
{
    if (!isReachable(dominated))
        return true;
    if (!isReachable(dominator))
        return false;
    auto const entry = _treeEntry[std::to_underlying(dominated)];
    return _treeEntry[std::to_underlying(dominator)] <= entry
           && entry <= _treeExit[std::to_underlying(dominator)];
}

LoopNesting::LoopNesting(ControlFlowGraph const& graph,
                         DominatorTree const& dominators,
                         std::pmr::memory_resource& memory):
    _innermostLoop(graph.blockCount(), None, &memory),
    _parentLoop(graph.blockCount(), None, &memory),
    _loopEnd(graph.blockCount(), None, &memory)
{
    // Headers innermost first: a loop's header dominates every header nested in it, so it comes earlier
    // in reverse postorder, and walking that order backwards meets the nested ones first. Each loop's
    // body is found by walking predecessors back from its back edges' sources to the header; a block
    // already claimed by a nested loop stands for that loop's outermost loop found so far, whose header
    // the walk continues from. That outermost loop is looked up through a union-find with path
    // compression, so that 100'000 nested loops cost no walk up 100'000 parents each.
    auto representative = std::pmr::vector<uint32_t>(graph.blockCount(), None, &memory);
    auto path = std::pmr::vector<uint32_t> { &memory };
    auto const outermost = [&](uint32_t header) {
        auto root = header;
        while (representative[root] != root)
            root = representative[root];
        path.clear();
        auto walk = header;
        while (walk != root)
        {
            path.push_back(walk);
            walk = representative[walk];
        }
        for (auto const compressed: path)
            representative[compressed] = root;
        return root;
    };
    auto worklist = std::pmr::vector<uint32_t> { &memory };
    auto headers = std::pmr::vector<uint32_t> { &memory };
    for (auto const block: dominators.reversePostorder() | std::views::reverse)
    {
        auto const header = std::to_underlying(block);
        worklist.clear();
        for (auto const predecessor: graph.predecessors(block))
            if (dominators.isReachable(predecessor) && dominators.dominates(block, predecessor))
                worklist.push_back(std::to_underlying(predecessor));
        if (worklist.empty())
            continue;
        headers.push_back(header);
        representative[header] = header;
        _innermostLoop[header] = header;
        while (!worklist.empty())
        {
            auto const member = worklist.back();
            worklist.pop_back();
            auto from = member;
            if (_innermostLoop[member] == None)
                _innermostLoop[member] = header;
            else
            {
                auto const root = outermost(_innermostLoop[member]);
                if (root == header)
                    continue; // already in this loop
                _parentLoop[root] = header;
                representative[root] = header;
                from = root;
            }
            for (auto const predecessor: graph.predecessors(BlockId { from }))
                if (dominators.isReachable(predecessor))
                    worklist.push_back(std::to_underlying(predecessor));
        }
    }

    // Each loop's last member: its own blocks', then, innermost first, its nested loops'.
    for (auto const block: std::views::iota(uint32_t { 0 }, graph.blockCount()))
        if (auto const loop = _innermostLoop[block]; loop != None)
            _loopEnd[loop] = _loopEnd[loop] == None ? block : std::max(_loopEnd[loop], block);
    for (auto const header: headers)
        if (auto const parent = _parentLoop[header]; parent != None)
            _loopEnd[parent] = std::max(_loopEnd[parent], _loopEnd[header]);
}

bool LoopNesting::isHeader(BlockId block) const noexcept
{
    Require(std::to_underlying(block) < _innermostLoop.size());
    return _innermostLoop[std::to_underlying(block)] == std::to_underlying(block);
}

std::optional<BlockId> LoopNesting::innermostLoop(BlockId block) const noexcept
{
    Require(std::to_underlying(block) < _innermostLoop.size());
    auto const loop = _innermostLoop[std::to_underlying(block)];
    if (loop == None)
        return std::nullopt;
    return BlockId { loop };
}

std::optional<BlockId> LoopNesting::parentLoop(BlockId header) const noexcept
{
    Require(isHeader(header));
    auto const parent = _parentLoop[std::to_underlying(header)];
    if (parent == None)
        return std::nullopt;
    return BlockId { parent };
}

BlockId LoopNesting::loopEnd(BlockId header) const noexcept
{
    Require(isHeader(header));
    return BlockId { _loopEnd[std::to_underlying(header)] };
}

} // namespace isoline::compiler::intermediate
