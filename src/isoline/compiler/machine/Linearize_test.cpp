// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory_resource>
#include <optional>
#include <ranges>
#include <span>
#include <vector>

#include <isoline/compiler/CompileError.hpp>
#include <isoline/compiler/intermediate/Function.hpp>
#include <isoline/compiler/machine/Linearize.hpp>
#include <isoline/compiler/machine/MachineFunction.hpp>
#include <isoline/compiler/machine/testing/FakeTarget.hpp>

using isoline::compiler::CompileErrorCode;
using isoline::compiler::intermediate::Unnumbered;
using isoline::compiler::machine::BlockId;
using isoline::compiler::machine::BlockOrder;
using isoline::compiler::machine::EdgeTo;
using isoline::compiler::machine::Hotness;
using isoline::compiler::machine::linearize;
using isoline::compiler::machine::loopEndPositions;
using isoline::compiler::machine::MachineFunction;
using isoline::compiler::machine::RegisterClass;
using isoline::compiler::machine::verifyContiguousLoops;
using isoline::compiler::machine::verifyLayoutCoversItsSuccessors;
using isoline::compiler::machine::testing::LoopsOf;
using isoline::compiler::machine::testing::terminate;

namespace
{

/// A block of a shape: its successors and whether it is cold.
struct ShapeBlock
{
    std::vector<uint32_t> successors; ///< At most two.
    Hotness hotness = Hotness::Normal;
};

/// A machine function of @p shape's control flow, one terminator per block, branching on one value.
MachineFunction functionOf(std::span<ShapeBlock const> shape, std::pmr::memory_resource& memory)
{
    auto function = MachineFunction { memory };
    for (auto const& block: shape)
        std::ignore = function.createBlock(block.hotness);
    auto const condition = function.createVirtualRegister(RegisterClass::General);
    for (auto const index: std::views::iota(uint32_t { 0 }, static_cast<uint32_t>(shape.size())))
    {
        auto edges = std::vector<EdgeTo> {};
        for (auto const successor: shape[index].successors)
            edges.push_back(EdgeTo { .target = BlockId { successor }, .arguments = {} });
        terminate(function, BlockId { index }, edges, condition);
    }
    return function;
}

/// The blocks of @p order, as numbers.
std::vector<uint32_t> numbersOf(BlockOrder const& order)
{
    auto numbers = std::vector<uint32_t> {};
    for (auto const block: order.blocks())
        numbers.push_back(std::to_underlying(block));
    return numbers;
}

/// The order @p numbers of a function of @p blockCount blocks.
BlockOrder orderOf(std::vector<uint32_t> const& numbers,
                   uint32_t blockCount,
                   std::pmr::memory_resource& memory)
{
    auto blocks = std::pmr::vector<BlockId> { &memory };
    for (auto const number: numbers)
        blocks.push_back(BlockId { number });
    return BlockOrder { std::move(blocks), blockCount, memory };
}

} // namespace

TEST_CASE("Linearize.loopsAreContiguous")
{
    std::pmr::monotonic_buffer_resource memory;

    SECTION("a loop whose exit reverse postorder places inside it")
    {
        // 0 → 1; 1 → 2, 3; 2 → 1 (the back edge); 3 returns. A depth-first walk that visits 2 first finishes
        // it first, so reverse postorder is 0 1 3 2, with the exit between the loop's blocks.
        auto const shape = std::vector<ShapeBlock> { { { 1 } }, { { 2, 3 } }, { { 1 } }, { {} } };
        auto const function = functionOf(shape, memory);
        auto const analysis = LoopsOf { function, memory };
        auto const order = linearize(function, analysis.loops, memory);
        REQUIRE(order.has_value());
        CHECK(numbersOf(*order) == std::vector<uint32_t> { 0, 1, 2, 3 });
        CHECK(verifyContiguousLoops(*order, analysis.loops).has_value());
        auto const ends = loopEndPositions(*order, analysis.loops, memory);
        CHECK(ends[1] == 2);
        CHECK(ends[0] == Unnumbered);
        CHECK(ends[3] == Unnumbered);
    }

    SECTION("nested loops, each contiguous inside its parent")
    {
        // 0 → 1 (outer header); 1 → 2 (inner header), 6 (exit); 2 → 3, 5; 3 → 4; 4 → 2 (inner back edge);
        // 5 → 1 (outer back edge); 6 returns.
        auto const shape = std::vector<ShapeBlock> {
            { { 1 } }, { { 2, 6 } }, { { 3, 5 } }, { { 4 } }, { { 2 } }, { { 1 } }, { {} },
        };
        auto const function = functionOf(shape, memory);
        auto const analysis = LoopsOf { function, memory };
        auto const order = linearize(function, analysis.loops, memory);
        REQUIRE(order.has_value());
        CHECK(order->blocks().front() == BlockId { 0 });
        CHECK(order->blocks().back() == BlockId { 6 });
        CHECK(verifyContiguousLoops(*order, analysis.loops).has_value());
        auto const ends = loopEndPositions(*order, analysis.loops, memory);
        // The inner loop {2, 3, 4} is a run starting at its header; the outer loop {1 .. 5} holds it.
        auto const inner = *order->position(BlockId { 2 });
        CHECK(ends[2] == inner + 2);
        for (auto const member: { 3U, 4U })
        {
            CHECK(*order->position(BlockId { member }) > inner);
            CHECK(*order->position(BlockId { member }) <= ends[2]);
        }
        CHECK(*order->position(BlockId { 1 }) == 1);
        CHECK(ends[1] == 5);
    }
}

TEST_CASE("Linearize.coldBlocksGoToTheEndOfTheirInnermostLoop")
{
    // 0 → 1 (header); 1 → 2 (cold), 3; 2 → 4; 3 → 4; 4 → 1 (back edge), 5; 5 → 6 (cold, in no loop), 7;
    // 6 → 7; 7 returns. The cold block 2 ends the loop; the cold block 6 ends the function.
    std::pmr::monotonic_buffer_resource memory;
    auto const shape = std::vector<ShapeBlock> {
        { { 1 } },    { { 2, 3 } }, { { 4 }, Hotness::Cold }, { { 4 } },
        { { 1, 5 } }, { { 6, 7 } }, { { 7 }, Hotness::Cold }, { {} },
    };
    auto const function = functionOf(shape, memory);
    auto const analysis = LoopsOf { function, memory };
    auto const order = linearize(function, analysis.loops, memory);
    REQUIRE(order.has_value());
    CHECK(numbersOf(*order) == std::vector<uint32_t> { 0, 1, 3, 4, 2, 5, 7, 6 });
    CHECK(loopEndPositions(*order, analysis.loops, memory)[1] == *order->position(BlockId { 2 }));

    // A cold block of the loop moved to the end of the function splits the loop: the verifier refuses it.
    auto const split = orderOf({ 0, 1, 3, 4, 5, 7, 6, 2 }, 8, memory);
    auto const refused = verifyContiguousLoops(split, analysis.loops);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == CompileErrorCode::InternalVerificationFailed);
    CHECK(refused.error().pass == "linearize");
}

TEST_CASE("Linearize.coldLoopsMoveWhole")
{
    // 0 → 1, 3; 1 (cold header) → 2; 2 → 1, 3; 3 returns. The loop {1, 2} is cold: it goes after 3, whole.
    std::pmr::monotonic_buffer_resource memory;
    auto const shape = std::vector<ShapeBlock> {
        { { 1, 3 } },
        { { 2 }, Hotness::Cold },
        { { 1, 3 } },
        { {} },
    };
    auto const function = functionOf(shape, memory);
    auto const analysis = LoopsOf { function, memory };
    auto const order = linearize(function, analysis.loops, memory);
    REQUIRE(order.has_value());
    CHECK(numbersOf(*order) == std::vector<uint32_t> { 0, 3, 1, 2 });
}

TEST_CASE("Linearize.leavesUnreachableBlocksOut")
{
    // 0 → 2; 1 → 2 (no path reaches 1); 2 returns.
    std::pmr::monotonic_buffer_resource memory;
    auto const shape = std::vector<ShapeBlock> { { { 2 } }, { { 2 } }, { {} } };
    auto const function = functionOf(shape, memory);
    auto const analysis = LoopsOf { function, memory };
    auto const order = linearize(function, analysis.loops, memory);
    REQUIRE(order.has_value());
    CHECK(numbersOf(*order) == std::vector<uint32_t> { 0, 2 });
    CHECK(order->position(BlockId { 1 }) == std::nullopt);
    CHECK(order->blockCount() == 3);
}

TEST_CASE("Linearize.verifierRefusesBrokenLayouts")
{
    std::pmr::monotonic_buffer_resource memory;
    auto const shape = std::vector<ShapeBlock> { { { 1 } }, { { 2, 3 } }, { { 1 } }, { {} } };
    auto const function = functionOf(shape, memory);
    auto const analysis = LoopsOf { function, memory };
    auto const refuses = [&](std::vector<uint32_t> const& numbers) {
        auto const verified = verifyContiguousLoops(orderOf(numbers, 4, memory), analysis.loops);
        return !verified.has_value() && verified.error().code == CompileErrorCode::InternalVerificationFailed;
    };
    CHECK_FALSE(refuses({ 0, 1, 2, 3 }));
    CHECK(refuses({ 0, 1, 3, 2 })); // the exit inside the loop
    CHECK(refuses({ 0, 2, 1, 3 })); // the header not first
    CHECK(refuses({ 1, 2, 0, 3 })); // the entry not first
    CHECK(refuses({ 0, 1, 3 }));    // a loop block left out
    CHECK(refuses({}));             // nothing laid out
}

TEST_CASE("Linearize.verifierRefusesAReachableBlockLeftOut")
{
    // 0 → 1; 1 returns; laid out as {0} alone. Block 1 is in no loop, so loop contiguity alone accepts it,
    // but every block a laid-out block jumps to must be laid out too.
    std::pmr::monotonic_buffer_resource memory;
    auto const shape = std::vector<ShapeBlock> { { { 1 } }, { {} } };
    auto const function = functionOf(shape, memory);
    auto const analysis = LoopsOf { function, memory };
    auto const order = orderOf({ 0 }, 2, memory);
    CHECK(verifyContiguousLoops(order, analysis.loops).has_value());
    auto const refused = verifyLayoutCoversItsSuccessors(function, order);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == CompileErrorCode::InternalVerificationFailed);
    CHECK(refused.error().pass == "linearize");
    CHECK(verifyLayoutCoversItsSuccessors(function, orderOf({ 0, 1 }, 2, memory)).has_value());
}
