// SPDX-License-Identifier: Apache-2.0
#include <core/Utils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include <isoline/compiler/intermediate/Dominators.hpp>
#include <isoline/compiler/intermediate/Function.hpp>
#include <isoline/compiler/intermediate/Verifier.hpp>

using isoline::compiler::intermediate::BlockId;
using isoline::compiler::intermediate::Builder;
using isoline::compiler::intermediate::ControlFlowGraph;
using isoline::compiler::intermediate::controlFlowGraphOf;
using isoline::compiler::intermediate::DominatorTree;
using isoline::compiler::intermediate::Function;
using isoline::compiler::intermediate::LoopNesting;
using isoline::compiler::intermediate::Type;
using isoline::compiler::intermediate::verify;

namespace
{

/// An edge of a graph a test spells out.
struct Edge
{
    uint32_t from; ///< Its source.
    uint32_t to;   ///< Its target.
};

/// The graph of @p blockCount blocks and @p edges, each block's successors in the edges' order.
ControlFlowGraph graphOf(uint32_t blockCount, std::span<Edge const> edges, std::pmr::memory_resource& memory)
{
    auto begins = std::pmr::vector<uint32_t>(blockCount + 1, 0, &memory);
    for (auto const& edge: edges)
        ++begins[edge.from + 1];
    for (auto const block: std::views::iota(uint32_t { 0 }, blockCount))
        begins[block + 1] += begins[block];
    auto successors = std::pmr::vector<BlockId>(edges.size(), BlockId {}, &memory);
    auto next = std::pmr::vector<uint32_t>(begins.begin(), begins.end() - 1, &memory);
    for (auto const& edge: edges)
        successors[next[edge.from]++] = BlockId { edge.to };
    return ControlFlowGraph { std::move(begins), std::move(successors), memory };
}

/// BlockId @p index.
constexpr BlockId block(uint32_t index)
{
    return BlockId { index };
}

/// What every test allocates from.
struct Arena
{
    std::pmr::monotonic_buffer_resource memory { std::pmr::new_delete_resource() };
};

} // namespace

TEST_CASE("Dominators.diamondAndLoop")
{
    auto arena = Arena {};
    SECTION("diamond")
    {
        // 0 -> 1, 2 -> 3
        auto const edges = std::to_array<Edge>({ { 0, 1 }, { 0, 2 }, { 1, 3 }, { 2, 3 } });
        auto const graph = graphOf(4, edges, arena.memory);
        CHECK(std::ranges::equal(graph.predecessors(block(3)), std::array { block(1), block(2) }));
        auto const tree = DominatorTree { graph, arena.memory };
        CHECK(tree.immediateDominator(block(0)) == std::nullopt);
        CHECK(tree.immediateDominator(block(1)) == block(0));
        CHECK(tree.immediateDominator(block(2)) == block(0));
        CHECK(tree.immediateDominator(block(3)) == block(0));
        CHECK(tree.dominates(block(0), block(3)));
        CHECK(tree.dominates(block(3), block(3)));
        CHECK_FALSE(tree.dominates(block(1), block(3)));
        CHECK_FALSE(tree.dominates(block(3), block(0)));
        // Depth-first through 1 first: postorder 3, 1, 2, 0.
        CHECK(std::ranges::equal(tree.reversePostorder(),
                                 std::array { block(0), block(2), block(1), block(3) }));
        auto const loops = LoopNesting { graph, tree, arena.memory };
        for (auto const index: std::views::iota(uint32_t { 0 }, uint32_t { 4 }))
        {
            CHECK_FALSE(loops.isHeader(block(index)));
            CHECK(loops.innermostLoop(block(index)) == std::nullopt);
        }
    }
    SECTION("two nested loops")
    {
        // 0 -> 1 (outer header) -> 2 (inner header) -> 3 -> 2 | 4 ; 4 -> 1 | 5
        auto const edges =
            std::to_array<Edge>({ { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 2 }, { 3, 4 }, { 4, 1 }, { 4, 5 } });
        auto const graph = graphOf(6, edges, arena.memory);
        auto const tree = DominatorTree { graph, arena.memory };
        CHECK(tree.immediateDominator(block(2)) == block(1));
        CHECK(tree.immediateDominator(block(4)) == block(3));
        CHECK(tree.immediateDominator(block(5)) == block(4));
        CHECK(std::ranges::equal(tree.reversePostorder(),
                                 std::array { block(0), block(1), block(2), block(3), block(4), block(5) }));
        auto const loops = LoopNesting { graph, tree, arena.memory };
        CHECK(loops.isHeader(block(1)));
        CHECK(loops.isHeader(block(2)));
        CHECK_FALSE(loops.isHeader(block(3)));
        CHECK(loops.innermostLoop(block(0)) == std::nullopt);
        CHECK(loops.innermostLoop(block(1)) == block(1));
        CHECK(loops.innermostLoop(block(2)) == block(2));
        CHECK(loops.innermostLoop(block(3)) == block(2));
        CHECK(loops.innermostLoop(block(4)) == block(1));
        CHECK(loops.innermostLoop(block(5)) == std::nullopt);
        CHECK(loops.parentLoop(block(2)) == block(1));
        CHECK(loops.parentLoop(block(1)) == std::nullopt);
        CHECK(loops.loopEnd(block(2)) == block(3));
        CHECK(loops.loopEnd(block(1)) == block(4));
    }
    SECTION("a self loop and an unreachable block")
    {
        // 0 -> 1 -> 1 | 2 ; 3 -> 2 (3 unreachable)
        auto const edges = std::to_array<Edge>({ { 0, 1 }, { 1, 1 }, { 1, 2 }, { 3, 2 } });
        auto const graph = graphOf(4, edges, arena.memory);
        auto const tree = DominatorTree { graph, arena.memory };
        CHECK(tree.isReachable(block(2)));
        CHECK_FALSE(tree.isReachable(block(3)));
        CHECK(tree.immediateDominator(block(2)) == block(1));
        CHECK(tree.immediateDominator(block(3)) == std::nullopt);
        CHECK(tree.dominates(block(2), block(3)));
        CHECK_FALSE(tree.dominates(block(3), block(2)));
        CHECK(tree.reversePostorder().size() == 3);
        auto const loops = LoopNesting { graph, tree, arena.memory };
        CHECK(loops.isHeader(block(1)));
        CHECK(loops.loopEnd(block(1)) == block(1));
        CHECK(loops.innermostLoop(block(3)) == std::nullopt);
    }
}

TEST_CASE("Dominators.deepNestingIsIterative")
{
    // 100'000 nested loops, built as a function: header h_i jumps into h_{i+1}, the innermost loops on
    // itself, and exit x_i branches back to h_i or out to x_{i-1}. Under ASan on the default stack, a
    // recursive walk anywhere (the verifier, the depth-first numbering, path compression, the tree's
    // numbering, the loop discovery) overflows.
    constexpr auto Depth = uint32_t { 100'000 };
    auto arena = Arena {};
    auto function = Function { arena.memory };
    auto builder = Builder { function };
    auto const entry = builder.createBlock();
    auto headers = std::vector<BlockId> {};
    auto exits = std::vector<BlockId> {};
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, Depth))
        headers.push_back(builder.createBlock());
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, Depth))
        exits.push_back(builder.createBlock());
    auto const done = builder.createBlock();
    auto const condition = builder.addParameter(entry, Type::I32);
    builder.switchTo(entry);
    builder.jump(headers.front(), {});
    for (auto const index: std::views::iota(uint32_t { 0 }, Depth))
    {
        builder.switchTo(headers[index]);
        if (index + 1 < Depth)
            builder.jump(headers[index + 1], {});
        else
            builder.branch(condition, headers[index], {}, exits[index], {});
        builder.switchTo(exits[index]);
        builder.branch(condition, headers[index], {}, index == 0 ? done : exits[index - 1], {});
    }
    builder.switchTo(done);
    builder.ret({});

    REQUIRE(verify(function).has_value());
    auto const graph = controlFlowGraphOf(function, arena.memory);
    auto const tree = DominatorTree { graph, arena.memory };
    auto const loops = LoopNesting { graph, tree, arena.memory };
    CHECK(tree.reversePostorder().size() == function.blockCount());
    CHECK(tree.immediateDominator(headers.front()) == entry);
    CHECK(tree.dominates(entry, done));
    for (auto const index: { uint32_t { 1 }, Depth / 2, Depth - 1 })
    {
        INFO("depth " << index);
        CHECK(tree.immediateDominator(headers[index]) == headers[index - 1]);
        CHECK(tree.immediateDominator(exits[index - 1]) == exits[index]);
        CHECK(tree.dominates(headers[index - 1], exits[index]));
        CHECK_FALSE(tree.dominates(exits[index], headers[index]));
        CHECK(loops.isHeader(headers[index]));
        CHECK(loops.innermostLoop(exits[index]) == headers[index]);
        CHECK(loops.parentLoop(headers[index]) == headers[index - 1]);
        // Every loop holds the innermost one's exit, the block with the largest ID of all of them.
        CHECK(loops.loopEnd(headers[index]) == exits.back());
    }
    CHECK(tree.immediateDominator(exits.back()) == headers.back());
    CHECK(loops.parentLoop(headers.front()) == std::nullopt);
    CHECK(loops.innermostLoop(done) == std::nullopt);
}

TEST_CASE("Dominators.backEdgesFromTheInnermostBlockAreNotQuadratic")
{
    // 100'000 nested loop headers h_1 .. h_n, and an innermost block that branches back to every one of
    // them (a br_table to every enclosing loop) or out. Each header's predecessors include that deepest
    // block, so without path compression the semidominator search walks the linked chain from it up to
    // the header, and the loop discovery walks the chain of loops found so far: O(n^2) either way.
    constexpr auto Depth = uint32_t { 100'000 };
    auto arena = Arena {};
    auto const innermost = Depth + 1;
    auto const exit = Depth + 2;
    auto edges = std::vector<Edge> {};
    for (auto const header: std::views::iota(uint32_t { 0 }, Depth + 1))
        edges.push_back({ header, header + 1 }); // entry 0 -> h_1 -> ... -> h_n -> innermost
    for (auto const header: std::views::iota(uint32_t { 1 }, Depth + 1))
        edges.push_back({ innermost, header });
    edges.push_back({ innermost, exit });
    auto const graph = graphOf(Depth + 3, edges, arena.memory);
    auto const tree = DominatorTree { graph, arena.memory };
    auto const loops = LoopNesting { graph, tree, arena.memory };
    CHECK(tree.immediateDominator(block(innermost)) == block(Depth));
    CHECK(tree.immediateDominator(block(exit)) == block(innermost));
    for (auto const header: { uint32_t { 2 }, Depth / 2, Depth })
    {
        INFO("header " << header);
        CHECK(tree.immediateDominator(block(header)) == block(header - 1));
        CHECK(loops.isHeader(block(header)));
        CHECK(loops.parentLoop(block(header)) == block(header - 1));
        CHECK(loops.loopEnd(block(header)) == block(innermost));
    }
    CHECK(loops.innermostLoop(block(innermost)) == block(Depth));
    CHECK(loops.parentLoop(block(1)) == std::nullopt);
    CHECK(loops.innermostLoop(block(exit)) == std::nullopt);
}

TEST_CASE("Dominators.irreducibleGraphs")
{
    auto arena = Arena {};
    SECTION("the smallest irreducible loop")
    {
        // 0 -> 1, 2 ; 1 <-> 2 ; 2 -> 3: a cycle entered at both of its blocks.
        auto const edges = std::to_array<Edge>({ { 0, 1 }, { 0, 2 }, { 1, 2 }, { 2, 1 }, { 2, 3 } });
        auto const graph = graphOf(4, edges, arena.memory);
        auto const tree = DominatorTree { graph, arena.memory };
        CHECK(tree.immediateDominator(block(1)) == block(0));
        CHECK(tree.immediateDominator(block(2)) == block(0));
        CHECK(tree.immediateDominator(block(3)) == block(2));
        // Neither block of the cycle dominates the other, so it is no natural loop.
        auto const loops = LoopNesting { graph, tree, arena.memory };
        CHECK_FALSE(loops.isHeader(block(1)));
        CHECK_FALSE(loops.isHeader(block(2)));
    }
    SECTION("a ladder of 100'000 rungs is not quadratic")
    {
        // Two chains a_i and b_i, both entered from 0, with rungs a_i <-> b_i: every block's immediate
        // dominator is the entry, at the end of two paths 100'000 blocks long. An eval() without path
        // compression, or an ascent per block, walks those paths once per block.
        constexpr auto Rungs = uint32_t { 50'000 };
        auto edges = std::vector<Edge> { { 0, 1 }, { 0, 2 } };
        for (auto const rung: std::views::iota(uint32_t { 0 }, Rungs))
        {
            auto const a = 1 + (2 * rung);
            auto const b = a + 1;
            edges.push_back({ a, b });
            edges.push_back({ b, a });
            if (rung + 1 < Rungs)
            {
                edges.push_back({ a, a + 2 });
                edges.push_back({ b, b + 2 });
            }
        }
        auto const graph = graphOf(1 + (2 * Rungs), edges, arena.memory);
        auto const tree = DominatorTree { graph, arena.memory };
        for (auto const index: std::views::iota(uint32_t { 1 }, 1 + (2 * Rungs)))
            if (tree.immediateDominator(block(index)) != block(0))
                FAIL("block " << index << " is not dominated by the entry alone");
    }
}

namespace
{

/// The blocks reachable from block 0 over @p edges, as a mask.
uint32_t reachableOf(std::span<Edge const> edges)
{
    auto reachable = uint32_t { 1 };
    auto changed = true;
    while (changed)
    {
        changed = false;
        for (auto const& edge: edges)
            if ((reachable >> edge.from & 1) != 0 && (reachable >> edge.to & 1) == 0)
            {
                reachable |= uint32_t { 1 } << edge.to;
                changed = true;
            }
    }
    return reachable;
}

/// Per block, its dominators as a mask, by the dataflow definition: Dom(entry) = {entry}, Dom(b) = {b}
/// united with the intersection of Dom(p) over b's reachable predecessors p.
std::vector<uint32_t> dominatorSetsOf(uint32_t blockCount, std::span<Edge const> edges, uint32_t reachable)
{
    auto const all = (uint32_t { 1 } << blockCount) - 1;
    auto dominators = std::vector<uint32_t>(blockCount, all);
    dominators[0] = 1;
    auto changed = true;
    while (changed)
    {
        changed = false;
        for (auto const index: std::views::iota(uint32_t { 1 }, blockCount))
        {
            auto meet = all;
            for (auto const& edge: edges)
                if (edge.to == index && (reachable >> edge.from & 1) != 0)
                    meet &= dominators[edge.from];
            auto const updated = meet | uint32_t { 1 } << index;
            if (updated != dominators[index])
            {
                dominators[index] = updated;
                changed = true;
            }
        }
    }
    return dominators;
}

/// The immediate dominator of @p b by @p dominators: the strict dominator whose own dominators are all of
/// b's others.
std::optional<BlockId> immediateDominatorOf(uint32_t b, std::span<uint32_t const> dominators)
{
    auto const strict = dominators[b] & ~(uint32_t { 1 } << b);
    for (auto const [candidate, mask]: core::views::enumerate(dominators))
        if ((strict >> candidate & 1) != 0 && mask == strict)
            return block(static_cast<uint32_t>(candidate));
    return std::nullopt;
}

/// Checks @p tree against the dataflow definition over @p edges.
void checkAgainstTheDefinition(DominatorTree const& tree, uint32_t blockCount, std::span<Edge const> edges)
{
    auto const reachable = reachableOf(edges);
    auto const dominators = dominatorSetsOf(blockCount, edges, reachable);
    CHECK(tree.reversePostorder().size() == static_cast<std::size_t>(std::popcount(reachable)));
    for (auto const b: std::views::iota(uint32_t { 0 }, blockCount))
    {
        auto const isReachable = (reachable >> b & 1) != 0;
        REQUIRE(tree.isReachable(block(b)) == isReachable);
        if (!isReachable)
            continue;
        for (auto const a: std::views::iota(uint32_t { 0 }, blockCount))
            if ((reachable >> a & 1) != 0
                && tree.dominates(block(a), block(b)) != ((dominators[b] >> a & 1) != 0))
                FAIL("dominates(" << a << ", " << b << ") disagrees");
        CHECK(tree.immediateDominator(block(b)) == immediateDominatorOf(b, dominators));
    }
    // Reverse postorder puts every reachable block after its immediate dominator.
    auto position = std::vector<std::size_t>(blockCount, 0);
    for (auto const [index, b]: core::views::enumerate(tree.reversePostorder()))
        position[std::to_underlying(b)] = index;
    CHECK(tree.reversePostorder().front() == block(0));
    for (auto const b: tree.reversePostorder())
        if (auto const idom = tree.immediateDominator(b))
            CHECK(position[std::to_underlying(*idom)] < position[std::to_underlying(b)]);
}

} // namespace

TEST_CASE("Dominators.matchTheDataflowDefinition")
{
    // On random graphs of up to 24 blocks, reducible or not, with unreachable blocks and self loops,
    // the tree agrees with the dataflow definition of dominance.
    auto arena = Arena {};
    auto state = uint64_t { 0x9E37'79B9'7F4A'7C15 };
    auto const next = [&state](uint32_t bound) {
        state = (state * 6'364'136'223'846'793'005) + 1'442'695'040'888'963'407;
        return static_cast<uint32_t>((state >> 33) % bound);
    };
    for (auto const round: std::views::iota(0, 500))
    {
        auto const blockCount = 2 + next(23);
        auto edges = std::vector<Edge> {};
        auto const edgeCount = next(3 * blockCount);
        for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, edgeCount))
            edges.push_back({ next(blockCount), next(blockCount) });
        auto const graph = graphOf(blockCount, edges, arena.memory);
        INFO("round " << round << ", " << blockCount << " blocks, " << edges.size() << " edges");
        checkAgainstTheDefinition(DominatorTree { graph, arena.memory }, blockCount, edges);
    }
}

namespace
{

/// The natural loops of a graph, by their definition: per header (the target of a back edge, an edge
/// whose target dominates its source), its body as a mask: the header, and every reachable block that
/// reaches a back edge's source without passing through the header.
std::vector<uint32_t> naturalLoopsOf(uint32_t blockCount,
                                     std::span<Edge const> edges,
                                     uint32_t reachable,
                                     std::span<uint32_t const> dominators)
{
    auto bodies = std::vector<uint32_t>(blockCount, 0);
    for (auto const& backEdge: edges)
    {
        auto const isBackEdge =
            (reachable >> backEdge.from & 1) != 0 && (dominators[backEdge.from] >> backEdge.to & 1) != 0;
        if (!isBackEdge)
            continue;
        auto const header = uint32_t { 1 } << backEdge.to;
        auto body = header | (reachable & uint32_t { 1 } << backEdge.from);
        auto changed = true;
        while (changed)
        {
            changed = false;
            for (auto const& edge: edges)
                if ((body >> edge.to & 1) != 0 && (header >> edge.to & 1) == 0
                    && (reachable >> edge.from & 1) != 0 && (body >> edge.from & 1) == 0)
                {
                    body |= uint32_t { 1 } << edge.from;
                    changed = true;
                }
        }
        bodies[backEdge.to] |= body;
    }
    return bodies;
}

/// The header of the smallest loop of @p bodies that holds @p block, other than the loop @p excluded
/// heads; none when no such loop holds it. Natural loops of distinct headers nest or are disjoint, so
/// the smallest is unique.
std::optional<BlockId> smallestLoopHolding(uint32_t block,
                                           std::span<uint32_t const> bodies,
                                           std::optional<uint32_t> excluded)
{
    auto best = std::optional<uint32_t> {};
    for (auto const [header, body]: core::views::enumerate(bodies))
    {
        if (body == 0 || (body >> block & 1) == 0 || excluded == header)
            continue;
        if (!best || std::popcount(body) < std::popcount(bodies[*best]))
            best = static_cast<uint32_t>(header);
    }
    return best.transform([](uint32_t header) { return BlockId { header }; });
}

} // namespace

TEST_CASE("Dominators.loopNestingMatchesTheNaturalLoops")
{
    // On random graphs of up to 24 blocks, reducible or not, with unreachable blocks and self loops,
    // LoopNesting agrees with the natural loops computed by their definition: which blocks are headers,
    // each block's innermost loop, each loop's parent and its last member.
    auto arena = Arena {};
    auto state = uint64_t { 0xD1B5'4A32'D192'ED03 };
    auto const next = [&state](uint32_t bound) {
        state = (state * 6'364'136'223'846'793'005) + 1'442'695'040'888'963'407;
        return static_cast<uint32_t>((state >> 33) % bound);
    };
    for (auto const round: std::views::iota(0, 500))
    {
        auto const blockCount = 2 + next(23);
        auto edges = std::vector<Edge> {};
        auto const edgeCount = next(3 * blockCount);
        for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, edgeCount))
            edges.push_back({ next(blockCount), next(blockCount) });
        auto const graph = graphOf(blockCount, edges, arena.memory);
        INFO("round " << round << ", " << blockCount << " blocks, " << edges.size() << " edges");
        auto const tree = DominatorTree { graph, arena.memory };
        auto const loops = LoopNesting { graph, tree, arena.memory };
        auto const reachable = reachableOf(edges);
        auto const dominators = dominatorSetsOf(blockCount, edges, reachable);
        auto const bodies = naturalLoopsOf(blockCount, edges, reachable, dominators);
        for (auto const b: std::views::iota(uint32_t { 0 }, blockCount))
        {
            INFO("block " << b);
            auto const isHeader = bodies[b] != 0;
            REQUIRE(loops.isHeader(block(b)) == isHeader);
            CHECK(loops.innermostLoop(block(b)) == smallestLoopHolding(b, bodies, std::nullopt));
            if (!isHeader)
                continue;
            CHECK(loops.parentLoop(block(b)) == smallestLoopHolding(b, bodies, b));
            auto const lastMember = static_cast<uint32_t>(std::bit_width(bodies[b]) - 1);
            CHECK(loops.loopEnd(block(b)) == block(lastMember));
        }
    }
}
