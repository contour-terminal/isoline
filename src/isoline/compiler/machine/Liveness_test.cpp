// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory_resource>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include <isoline/compiler/machine/Linearize.hpp>
#include <isoline/compiler/machine/Liveness.hpp>
#include <isoline/compiler/machine/MachineFunction.hpp>
#include <isoline/compiler/machine/testing/FakeTarget.hpp>

using isoline::compiler::machine::BlockId;
using isoline::compiler::machine::BlockOrder;
using isoline::compiler::machine::computeLiveIntervals;
using isoline::compiler::machine::EdgeTo;
using isoline::compiler::machine::Hotness;
using isoline::compiler::machine::Immediate;
using isoline::compiler::machine::linearize;
using isoline::compiler::machine::LiveIntervals;
using isoline::compiler::machine::LiveRange;
using isoline::compiler::machine::MachineFunction;
using isoline::compiler::machine::MachineOperand;
using isoline::compiler::machine::MemoryOperand;
using isoline::compiler::machine::OperandRole;
using isoline::compiler::machine::PhysicalRegister;
using isoline::compiler::machine::RegisterClass;
using isoline::compiler::machine::RegisterName;
using isoline::compiler::machine::RegisterOperand;
using isoline::compiler::machine::VirtualRegister;
using isoline::compiler::machine::testing::def;
using isoline::compiler::machine::testing::early;
using isoline::compiler::machine::testing::FakeOpcode;
using isoline::compiler::machine::testing::LoopsOf;
using isoline::compiler::machine::testing::opcodeOf;
using isoline::compiler::machine::testing::terminate;
using isoline::compiler::machine::testing::use;
using isoline::compiler::machine::testing::useDef;

namespace
{

/// Appends `reg ← value` to @p block.
uint32_t constant(MachineFunction& function, BlockId block, VirtualRegister reg, int64_t value = 0)
{
    return function.append(block,
                           opcodeOf(FakeOpcode::Constant),
                           std::to_array<MachineOperand>({ def(function, reg), Immediate { value } }));
}

/// Appends `result ← left + right` to @p block.
uint32_t add(MachineFunction& function,
             BlockId block,
             VirtualRegister result,
             VirtualRegister left,
             VirtualRegister right)
{
    return function.append(
        block,
        opcodeOf(FakeOpcode::Add),
        std::to_array<MachineOperand>({ def(function, result), use(function, left), use(function, right) }));
}

/// A new General virtual register of @p function.
VirtualRegister general(MachineFunction& function)
{
    return function.createVirtualRegister(RegisterClass::General);
}

/// One edge to @p target, passing @p arguments.
std::array<EdgeTo, 1> to(BlockId target, std::span<VirtualRegister const> arguments = {})
{
    return { EdgeTo { .target = target, .arguments = arguments } };
}

/// Two edges, to @p first and @p second, passing nothing.
std::array<EdgeTo, 2> to(BlockId first, BlockId second)
{
    return { EdgeTo { .target = first, .arguments = {} }, EdgeTo { .target = second, .arguments = {} } };
}

/// The ranges of @p reg's interval, as a plain vector.
std::vector<LiveRange> rangesOf(LiveIntervals const& live, VirtualRegister reg)
{
    auto const& ranges = live.intervals[std::to_underlying(reg)].ranges;
    return { ranges.begin(), ranges.end() };
}

/// The use positions of @p reg's interval, as a plain vector.
std::vector<uint32_t> usesOf(LiveIntervals const& live, VirtualRegister reg)
{
    auto const& uses = live.intervals[std::to_underlying(reg)].usePositions;
    return { uses.begin(), uses.end() };
}

/// Whether @p reg is live over all of @p range.
bool coversRange(LiveIntervals const& live, VirtualRegister reg, LiveRange range)
{
    return std::ranges::any_of(live.intervals[std::to_underlying(reg)].ranges, [range](LiveRange const& r) {
        return r.start <= range.start && range.end <= r.end;
    });
}

/// Whether @p reg is live anywhere in @p range.
bool touchesRange(LiveIntervals const& live, VirtualRegister reg, LiveRange range)
{
    return std::ranges::any_of(live.intervals[std::to_underlying(reg)].ranges, [range](LiveRange const& r) {
        return r.start < range.end && range.start < r.end;
    });
}

/// Laid out and analysed: the function's loops, its layout and its intervals.
struct Analysed
{
    Analysed(MachineFunction const& function, std::pmr::memory_resource& memory):
        loops { function, memory },
        order { linearize(function, loops.loops, memory).value() },
        live { computeLiveIntervals(function, order, loops.loops, memory) }
    {
    }

    LoopsOf loops;
    BlockOrder order;
    LiveIntervals live;

    /// The positions of @p block.
    [[nodiscard]] LiveRange rangeOf(BlockId block) const
    {
        return live.blockRanges[std::to_underlying(block)];
    }
};

} // namespace

TEST_CASE("Liveness.straightLine")
{
    std::pmr::monotonic_buffer_resource memory;
    auto function = MachineFunction { memory };
    auto const entry = function.createBlock(Hotness::Normal);
    auto const a = general(function);
    auto const b = general(function);
    auto const c = general(function);
    auto const d = general(function);
    auto const dead = general(function);
    constant(function, entry, a);  // 2
    constant(function, entry, b);  // 4
    add(function, entry, c, a, b); // 6
    function.append(entry,         // 8: c += a, tied
                    opcodeOf(FakeOpcode::AddTied),
                    std::to_array<MachineOperand>({ useDef(function, c), use(function, a) }));
    function.append(entry, // 10: d written before c is read
                    opcodeOf(FakeOpcode::EarlyClobber),
                    std::to_array<MachineOperand>({ early(function, d), use(function, c) }));
    constant(function, entry, dead);   // 12
    terminate(function, entry, {}, d); // 14: return d

    auto const analysed = Analysed { function, memory };
    auto const& live = analysed.live;
    CHECK(analysed.rangeOf(entry) == LiveRange { 0, 16 });
    CHECK(std::vector(live.instructionPositions.begin(), live.instructionPositions.end())
          == std::vector<uint32_t> { 2, 4, 6, 8, 10, 12, 14 });

    // a dies at its last use; b dies where c, defined at the same position, may take its register.
    CHECK(rangesOf(live, a) == std::vector<LiveRange> { { 2, 8 } });
    CHECK(usesOf(live, a) == std::vector<uint32_t> { 2, 6, 8 });
    CHECK(rangesOf(live, b) == std::vector<LiveRange> { { 4, 6 } });
    // The tied operand keeps c in one piece across its redefinition.
    CHECK(rangesOf(live, c) == std::vector<LiveRange> { { 6, 10 } });
    CHECK(usesOf(live, c) == std::vector<uint32_t> { 6, 8, 10 });
    // The early clobber starts one position before its instruction, overlapping c's last use.
    CHECK(rangesOf(live, d) == std::vector<LiveRange> { { 9, 14 } });
    CHECK(usesOf(live, d) == std::vector<uint32_t> { 10, 14 });
    // A definition never used still holds its register where it is written.
    CHECK(rangesOf(live, dead) == std::vector<LiveRange> { { 12, 13 } });
    CHECK(live.intervals[std::to_underlying(dead)].reg == dead);
}

TEST_CASE("Liveness.loopExtendsLiveIns")
{
    // 0: v, c; → 1. 1 (header): w = v + v; → 2. 2: branch c → 1, 3. 3: return.
    // v is last used in the header, but the back edge brings control back to that use: v is live to the
    // loop's end. So is c, read at the latch.
    std::pmr::monotonic_buffer_resource memory;
    auto function = MachineFunction { memory };
    auto const entry = function.createBlock(Hotness::Normal);
    auto const header = function.createBlock(Hotness::Normal);
    auto const latch = function.createBlock(Hotness::Normal);
    auto const exit = function.createBlock(Hotness::Normal);
    auto const v = general(function);
    auto const c = general(function);
    auto const w = general(function);
    constant(function, entry, v);
    constant(function, entry, c);
    terminate(function, entry, to(header));
    add(function, header, w, v, v);
    terminate(function, header, to(latch));
    terminate(function, latch, to(header, exit), c);
    terminate(function, exit, {});

    auto const analysed = Analysed { function, memory };
    auto const& live = analysed.live;
    CHECK(analysed.rangeOf(header) == LiveRange { 8, 14 });
    CHECK(analysed.rangeOf(latch) == LiveRange { 14, 18 });
    CHECK(rangesOf(live, v) == std::vector<LiveRange> { { 2, 18 } });
    CHECK(usesOf(live, v) == std::vector<uint32_t> { 2, 10 });
    CHECK(rangesOf(live, c) == std::vector<LiveRange> { { 4, 18 } });
    CHECK(rangesOf(live, w) == std::vector<LiveRange> { { 10, 11 } });
    CHECK_FALSE(touchesRange(live, v, analysed.rangeOf(exit)));
}

TEST_CASE("Liveness.holesAcrossDiamond")
{
    // 0: v, u, c; branch c → 1, 2. 1: x = v + v; → 3 (x). 2: y = u + u; → 3 (y). 3 (p): return p.
    // Each arm's operand is live only into its own arm: whichever arm is laid out second, its operand has a
    // hole across the first. The arms' results live to their blocks' ends, where the edges pass them.
    std::pmr::monotonic_buffer_resource memory;
    auto function = MachineFunction { memory };
    auto const entry = function.createBlock(Hotness::Normal);
    auto const left = function.createBlock(Hotness::Normal);
    auto const right = function.createBlock(Hotness::Normal);
    auto const join = function.createBlock(Hotness::Normal);
    auto const v = general(function);
    auto const u = general(function);
    auto const c = general(function);
    auto const x = general(function);
    auto const y = general(function);
    auto const p = general(function);
    constant(function, entry, v);
    constant(function, entry, u);
    constant(function, entry, c);
    terminate(function, entry, to(left, right), c);
    auto const leftUse = add(function, left, x, v, v);
    terminate(function, left, to(join, std::array { x }));
    auto const rightUse = add(function, right, y, u, u);
    terminate(function, right, to(join, std::array { y }));
    function.setParameters(join, std::array { p });
    terminate(function, join, {}, p);

    auto const analysed = Analysed { function, memory };
    auto const& live = analysed.live;
    auto const& order = analysed.order;
    REQUIRE(order.blocks().size() == 4);
    auto const firstArm = order.blocks()[1];
    auto const secondArm = order.blocks()[2];
    auto const [firstOperand, secondOperand] = firstArm == left ? std::pair { v, u } : std::pair { u, v };
    auto const lateUse = live.instructionPositions[secondArm == left ? leftUse : rightUse];
    auto const earlyUse = live.instructionPositions[firstArm == left ? leftUse : rightUse];
    // The second arm's operand: live out of the entry, a hole across the first arm, live into its use.
    CHECK(rangesOf(live, secondOperand)
          == std::vector<LiveRange> {
              { live.instructionPositions[secondOperand == v ? 0U : 1U], analysed.rangeOf(entry).end },
              { analysed.rangeOf(secondArm).start, lateUse },
          });
    CHECK_FALSE(touchesRange(live, secondOperand, analysed.rangeOf(firstArm)));
    // The first arm's operand: one range, nothing after its use.
    CHECK(rangesOf(live, firstOperand).size() == 1);
    CHECK(rangesOf(live, firstOperand).back().end == earlyUse);
    CHECK_FALSE(touchesRange(live, firstOperand, analysed.rangeOf(secondArm)));
    // Edge arguments live to their block's end; the parameter from the join's entry.
    CHECK(rangesOf(live, x)
          == std::vector<LiveRange> { { live.instructionPositions[leftUse], analysed.rangeOf(left).end } });
    CHECK(rangesOf(live, y)
          == std::vector<LiveRange> { { live.instructionPositions[rightUse], analysed.rangeOf(right).end } });
    CHECK(rangesOf(live, p)
          == std::vector<LiveRange> { { analysed.rangeOf(join).start, analysed.rangeOf(join).start + 2 } });
    CHECK(usesOf(live, p)
          == std::vector<uint32_t> { analysed.rangeOf(join).start, analysed.rangeOf(join).start + 2 });
    // The condition dies at the branch that reads it.
    CHECK(rangesOf(live, c).back().end == analysed.rangeOf(entry).end - 2);
}

TEST_CASE("Liveness.coldBlockInsideALoop")
{
    // 0: v, c; → 1. 1 (header): branch c → 2 (cold), 3. 2 (cold): x = v + v; → 4. 3: → 4.
    // 4: branch c → 1, 5. 5: return. The cold block stays inside the loop, at its end, and v, used only
    // there, is live across every block of the loop.
    std::pmr::monotonic_buffer_resource memory;
    auto function = MachineFunction { memory };
    auto const entry = function.createBlock(Hotness::Normal);
    auto const header = function.createBlock(Hotness::Normal);
    auto const cold = function.createBlock(Hotness::Cold);
    auto const hot = function.createBlock(Hotness::Normal);
    auto const latch = function.createBlock(Hotness::Normal);
    auto const exit = function.createBlock(Hotness::Normal);
    auto const v = general(function);
    auto const c = general(function);
    auto const x = general(function);
    constant(function, entry, v);
    constant(function, entry, c);
    terminate(function, entry, to(header));
    terminate(function, header, to(cold, hot), c);
    add(function, cold, x, v, v);
    terminate(function, cold, to(latch));
    terminate(function, hot, to(latch));
    terminate(function, latch, to(header, exit), c);
    terminate(function, exit, {});

    auto const analysed = Analysed { function, memory };
    auto const& order = analysed.order;
    auto const headerPosition = *order.position(header);
    auto const loopEnd = isoline::compiler::machine::loopEndPositions(order, analysed.loops.loops, memory)[1];
    CHECK(*order.position(cold) > headerPosition);
    CHECK(*order.position(cold) == loopEnd);
    CHECK(*order.position(exit) == loopEnd + 1);
    for (auto const block: { header, hot, latch, cold })
        CHECK(coversRange(analysed.live, v, analysed.rangeOf(block)));
    CHECK_FALSE(touchesRange(analysed.live, v, analysed.rangeOf(exit)));
}

TEST_CASE("Liveness.deepLoopsAreIterative")
{
    // 100'000 nested loops: header h_i jumps into h_{i+1}, the innermost branches to itself or out, and exit
    // x_i branches back to h_i or out to x_{i-1}. Under ASan on the default stack, a recursive walk anywhere
    // (the graph, the layout, its verification, the loop extents, liveness) overflows. v, defined at the
    // entry and read after the outermost loop, is live everywhere in one range; c, read at every exit, to the
    // outermost loop's end.
    constexpr auto Depth = uint32_t { 100'000 };
    std::pmr::monotonic_buffer_resource memory;
    auto function = MachineFunction { memory };
    auto const entry = function.createBlock(Hotness::Normal);
    auto headers = std::vector<BlockId> {};
    auto exits = std::vector<BlockId> {};
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, Depth))
        headers.push_back(function.createBlock(Hotness::Normal));
    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, Depth))
        exits.push_back(function.createBlock(Hotness::Normal));
    auto const done = function.createBlock(Hotness::Normal);
    auto const v = general(function);
    auto const c = general(function);
    constant(function, entry, v);
    constant(function, entry, c);
    terminate(function, entry, to(headers.front()));
    for (auto const index: std::views::iota(uint32_t { 0 }, Depth))
    {
        if (index + 1 < Depth)
            terminate(function, headers[index], to(headers[index + 1]));
        else
            terminate(function, headers[index], to(headers[index], exits[index]), c);
        terminate(function, exits[index], to(headers[index], index == 0 ? done : exits[index - 1]), c);
    }
    terminate(function, done, {}, v);

    auto const analysed = Analysed { function, memory };
    auto const& order = analysed.order;
    REQUIRE(order.blocks().size() == function.blockCount());
    CHECK(order.blocks().front() == entry);
    CHECK(*order.position(headers.back()) == Depth);
    CHECK(*order.position(exits.front()) == (2 * Depth));
    CHECK(order.blocks().back() == done);
    auto const doneRange = analysed.rangeOf(done);
    CHECK(rangesOf(analysed.live, v) == std::vector<LiveRange> { { 2, doneRange.start + 2 } });
    CHECK(rangesOf(analysed.live, c)
          == std::vector<LiveRange> { { 4, analysed.rangeOf(exits.front()).end } });
    CHECK(usesOf(analysed.live, c).size() == Depth + 2);
}

TEST_CASE("Liveness.memoryOperandsReadTheirRegisters")
{
    // A memory operand's base and index are read by its instruction; a physical register operand (a
    // selector's fixed register) has no interval of its own.
    std::pmr::monotonic_buffer_resource memory;
    auto function = MachineFunction { memory };
    auto const entry = function.createBlock(Hotness::Normal);
    auto const base = general(function);
    auto const index = general(function);
    auto const loaded = general(function);
    constant(function, entry, base);  // 2
    constant(function, entry, index); // 4
    function.append(
        entry, // 6: loaded ← [base + index × 8 + 16]
        opcodeOf(FakeOpcode::Load),
        std::to_array<MachineOperand>({
            def(function, loaded),
            MemoryOperand {
                .base = function.name(base), .index = function.name(index), .scale = 8, .displacement = 16 },
        }));
    auto const g0 = PhysicalRegister { .registerClass = RegisterClass::General, .number = 0 };
    function.append(entry, // 8: g0 ← loaded
                    opcodeOf(FakeOpcode::Copy),
                    std::to_array<MachineOperand>({
                        RegisterOperand { .name = RegisterName::of(g0), .role = OperandRole::Def },
                        use(function, loaded),
                    }));
    terminate(function, entry, {}); // 10

    auto const analysed = Analysed { function, memory };
    CHECK(rangesOf(analysed.live, base) == std::vector<LiveRange> { { 2, 6 } });
    CHECK(rangesOf(analysed.live, index) == std::vector<LiveRange> { { 4, 6 } });
    CHECK(usesOf(analysed.live, index) == std::vector<uint32_t> { 4, 6 });
    CHECK(rangesOf(analysed.live, loaded) == std::vector<LiveRange> { { 6, 8 } });
    CHECK(analysed.live.intervals.size() == 3);
}
