// SPDX-License-Identifier: Apache-2.0
#include <isoline/compiler/machine/Liveness.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <variant>

namespace isoline::compiler::machine
{

using intermediate::Unnumbered;

namespace
{
    /// The registers live at a point of the backward walk: a dense slot per register, and the list of the
    /// live ones, so that adding, removing and walking them are O(1) per register.
    class LiveSet
    {
      public:
        LiveSet(uint32_t registerCount, std::pmr::memory_resource& memory):
            _slots(registerCount, Unnumbered, &memory), _members { &memory }
        {
        }

        void add(uint32_t reg)
        {
            if (_slots[reg] != Unnumbered)
                return;
            _slots[reg] = static_cast<uint32_t>(_members.size());
            _members.push_back(reg);
        }

        void remove(uint32_t reg) noexcept
        {
            auto const slot = _slots[reg];
            if (slot == Unnumbered)
                return;
            auto const last = _members.back();
            _members[slot] = last;
            _slots[last] = slot;
            _members.pop_back();
            _slots[reg] = Unnumbered;
        }

        [[nodiscard]] bool contains(uint32_t reg) const noexcept { return _slots[reg] != Unnumbered; }

        void clear() noexcept
        {
            for (auto const reg: _members)
                _slots[reg] = Unnumbered;
            _members.clear();
        }

        [[nodiscard]] std::span<uint32_t const> members() const noexcept { return _members; }

      private:
        std::pmr::vector<uint32_t> _slots; ///< Per register, its index in _members, or Unnumbered.
        std::pmr::vector<uint32_t> _members;
    };

    /// The intervals under construction. The walk goes backwards, so each interval's ranges are kept in
    /// descending order (the earliest last) and every new range starts at or before the earliest one: it
    /// merges with the ranges it overlaps or touches by popping them, O(1) amortized.
    class IntervalBuilder
    {
      public:
        explicit IntervalBuilder(std::pmr::vector<LiveInterval>& intervals): _intervals { intervals } {}

        /// Makes @p reg live over [from, to).
        void addRange(uint32_t reg, uint32_t from, uint32_t to)
        {
            auto& ranges = _intervals[reg].ranges;
            while (!ranges.empty() && ranges.back().start <= to)
            {
                from = std::min(from, ranges.back().start);
                to = std::max(to, ranges.back().end);
                ranges.pop_back();
            }
            ranges.push_back(LiveRange { .start = from, .end = to });
        }

        /// Starts @p reg's earliest range at its definition, @p from (Require: it is live there).
        void setFrom(uint32_t reg, uint32_t from) noexcept
        {
            auto& ranges = _intervals[reg].ranges;
            Require(!ranges.empty() && ranges.back().start <= from && from < ranges.back().end);
            ranges.back().start = from;
        }

        /// Records that the instruction at @p position reads or writes @p reg.
        void addUse(uint32_t reg, uint32_t position) { _intervals[reg].usePositions.push_back(position); }

      private:
        std::pmr::vector<LiveInterval>& _intervals;
    };

    /// The virtual register @p name names, if it names one.
    [[nodiscard]] std::optional<uint32_t> virtualOf(RegisterName const& name) noexcept
    {
        if (name.kind != RegisterKind::Virtual)
            return std::nullopt;
        return name.number;
    }

    /// The empty intervals of @p function's registers, and the positions of @p order: two per block entry
    /// and per instruction.
    [[nodiscard]] LiveIntervals numbered(MachineFunction const& function,
                                         BlockOrder const& order,
                                         std::pmr::memory_resource& memory)
    {
        auto result = LiveIntervals {
            .intervals = std::pmr::vector<LiveInterval> { &memory },
            .instructionPositions =
                std::pmr::vector<uint32_t>(function.instructionCount(), Unnumbered, &memory),
            .blockRanges = std::pmr::vector<LiveRange>(function.blockCount(), LiveRange {}, &memory),
        };
        result.intervals.reserve(function.virtualRegisterCount());
        for (auto const reg: std::views::iota(uint32_t { 0 }, function.virtualRegisterCount()))
            result.intervals.push_back(
                LiveInterval { .reg = VirtualRegister { reg },
                               .ranges = std::pmr::vector<LiveRange> { &memory },
                               .usePositions = std::pmr::vector<uint32_t> { &memory } });
        auto next = uint64_t { 0 };
        for (auto const block: order.blocks())
        {
            auto const start = next;
            next += 2;
            for (auto const instruction: function.instructions(block))
            {
                result.instructionPositions[instruction] = static_cast<uint32_t>(next);
                next += 2;
            }
            Require(next < std::numeric_limits<uint32_t>::max());
            result.blockRanges[std::to_underlying(block)] =
                LiveRange { .start = static_cast<uint32_t>(start), .end = static_cast<uint32_t>(next) };
        }
        return result;
    }

    /// The backward walk over a numbered layout: per block its live-out, its instructions in reverse, its
    /// parameters, the loop extension at a header, and its live-in, kept for its predecessors.
    class BackwardWalk
    {
      public:
        BackwardWalk(MachineFunction const& function,
                     BlockOrder const& order,
                     intermediate::LoopNesting const& loops,
                     LiveIntervals& result,
                     std::pmr::memory_resource& memory):
            _function { function },
            _order { order },
            _loops { loops },
            _result { result },
            _loopEnds { loopEndPositions(order, loops, memory) },
            _liveIn(function.blockCount(), PoolRange {}, &memory),
            _computed(function.blockCount(), 0, &memory),
            _liveInPool { &memory },
            _live { function.virtualRegisterCount(), memory },
            _builder { result.intervals }
        {
        }

        void walk(BlockId block)
        {
            auto const range = _result.blockRanges[std::to_underlying(block)];
            startAtLiveOut(block, range);
            for (auto const instruction: _function.instructions(block) | std::views::reverse)
                walkInstruction(instruction, range);
            defineParameters(block, range);
            extendOverLoop(block, range);
            auto const begin = static_cast<uint32_t>(_liveInPool.size());
            _liveInPool.insert(_liveInPool.end(), _live.members().begin(), _live.members().end());
            _liveIn[std::to_underlying(block)] =
                PoolRange { .begin = begin, .count = static_cast<uint32_t>(_live.members().size()) };
            _computed[std::to_underlying(block)] = 1;
        }

      private:
        /// Live out: the successors' live-ins (a back edge's target has none yet: the loop extension covers
        /// it) and the arguments the edges pass, live over the whole block to start with.
        void startAtLiveOut(BlockId block, LiveRange range)
        {
            _live.clear();
            for (auto const& edge: _function.edges(block))
            {
                auto const target = std::to_underlying(edge.target);
                if (_computed[target] != 0)
                    for (auto const reg:
                         std::span { _liveInPool }.subspan(_liveIn[target].begin, _liveIn[target].count))
                        _live.add(reg);
                for (auto const argument: _function.arguments(edge))
                    _live.add(std::to_underlying(argument));
            }
            for (auto const reg: _live.members())
                _builder.addRange(reg, range.start, range.end);
        }

        /// Definitions end the value's liveness above the instruction (an early one a position sooner); uses
        /// make it live from the block's start to the instruction.
        void walkInstruction(uint32_t instruction, LiveRange range)
        {
            auto const position = _result.instructionPositions[instruction];
            auto const operands = _function.operands(instruction);
            for (auto const& operand: operands)
            {
                auto const* reg = std::get_if<RegisterOperand>(&operand);
                if (reg != nullptr && reg->role != OperandRole::Use)
                    if (auto const defined = virtualOf(reg->name))
                        define(*defined, reg->role == OperandRole::Early ? position - 1 : position, position);
            }
            for (auto const& operand: operands)
            {
                if (auto const* reg = std::get_if<RegisterOperand>(&operand))
                {
                    if (reg->role == OperandRole::Use || reg->role == OperandRole::UseDef)
                        read(reg->name, range.start, position);
                }
                else if (auto const* address = std::get_if<MemoryOperand>(&operand))
                {
                    read(address->base, range.start, position);
                    read(address->index, range.start, position);
                }
            }
        }

        /// @p reg is written from @p from by the instruction at @p position; a value never read lives
        /// [from, position + 1).
        void define(uint32_t reg, uint32_t from, uint32_t position)
        {
            if (_live.contains(reg))
                _builder.setFrom(reg, from);
            else
                _builder.addRange(reg, from, position + 1);
            _live.remove(reg);
            _builder.addUse(reg, position);
        }

        /// @p name, if virtual, is read at @p position, so live from @p blockStart up to it.
        void read(RegisterName const& name, uint32_t blockStart, uint32_t position)
        {
            if (auto const used = virtualOf(name))
            {
                _builder.addRange(*used, blockStart, position);
                _live.add(*used);
                _builder.addUse(*used, position);
            }
        }

        /// The parameters are defined at the block's entry.
        void defineParameters(BlockId block, LiveRange range)
        {
            for (auto const parameter: _function.parameters(block))
                define(std::to_underlying(parameter), range.start, range.start);
        }

        /// At a loop header, whatever is live in is live to the loop's end: the back edges bring it back.
        void extendOverLoop(BlockId block, LiveRange range)
        {
            auto const loopEnd = _loopEnds[std::to_underlying(block)];
            if (!_loops.isHeader(block) || loopEnd == Unnumbered)
                return;
            auto const end = _result.blockRanges[std::to_underlying(_order.blocks()[loopEnd])].end;
            for (auto const reg: _live.members())
                _builder.addRange(reg, range.start, end);
        }

        MachineFunction const& _function;
        BlockOrder const& _order;
        intermediate::LoopNesting const& _loops;
        LiveIntervals& _result;
        std::pmr::vector<uint32_t> _loopEnds;
        std::pmr::vector<PoolRange> _liveIn; ///< Per block, its live-in registers in _liveInPool.
        std::pmr::vector<uint8_t> _computed; ///< Per block, whether its live-in is known yet.
        std::pmr::vector<uint32_t> _liveInPool;
        LiveSet _live;
        IntervalBuilder _builder;
    };
} // namespace

LiveIntervals computeLiveIntervals(MachineFunction const& function,
                                   BlockOrder const& order,
                                   intermediate::LoopNesting const& loops,
                                   std::pmr::memory_resource& memory)
{
    auto result = numbered(function, order, memory);
    auto walk = BackwardWalk { function, order, loops, result, memory };
    for (auto const block: order.blocks() | std::views::reverse)
        walk.walk(block);

    // Built backwards: put each interval in ascending order.
    for (auto& interval: result.intervals)
    {
        std::ranges::reverse(interval.ranges);
        std::ranges::reverse(interval.usePositions);
        auto const duplicates = std::ranges::unique(interval.usePositions);
        interval.usePositions.erase(duplicates.begin(), duplicates.end());
    }
    return result;
}

} // namespace isoline::compiler::machine
