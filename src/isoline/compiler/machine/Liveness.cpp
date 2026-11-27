// SPDX-License-Identifier: Apache-2.0
#include <isoline/compiler/machine/Liveness.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
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

    /// How often the fixpoint and the walk poll the cancellation, in blocks.
    constexpr auto PollInterval = uint32_t { 4'096 };

    /// The cancellation's poll, counted across the fixpoint and the walk: every PollInterval blocks taken.
    class LivenessPoll
    {
      public:
        explicit LivenessPoll(CompileCancellation const& cancellation) noexcept:
            _cancellation { cancellation }
        {
        }

        /// Counts one block taken.
        /// @return Nothing; or Cancelled, once an interval is complete and the compile was cancelled.
        [[nodiscard]] std::expected<void, CompileError> step() noexcept
        {
            if (++_taken % PollInterval != 0 || _cancellation.state() != CancellationState::Cancelled)
                return {};
            return std::unexpected { CompileError { .code = CompileErrorCode::Cancelled } };
        }

      private:
        CompileCancellation const& _cancellation;
        uint32_t _taken = 0;
    };

    /// The refusal of a function liveness cannot take: a compiler bug.
    [[nodiscard]] std::unexpected<CompileError> illFormed() noexcept
    {
        return std::unexpected { CompileError { .code = CompileErrorCode::InternalVerificationFailed,
                                                .pass = "liveness" } };
    }

    /// Calls @p visit with each register @p operand reads: a Use or UseDef register, a memory operand's base
    /// and index.
    template <typename Visit>
    void forEachRead(MachineOperand const& operand, Visit const& visit)
    {
        if (auto const* reg = std::get_if<RegisterOperand>(&operand))
        {
            if (reg->role == OperandRole::Use || reg->role == OperandRole::UseDef)
                if (auto const read = virtualOf(reg->name))
                    visit(*read);
        }
        else if (auto const* address = std::get_if<MemoryOperand>(&operand))
        {
            for (auto const& part: { address->base, address->index })
                if (auto const read = virtualOf(part))
                    visit(*read);
        }
    }

    /// The virtual register @p operand writes, if it writes one.
    [[nodiscard]] std::optional<uint32_t> writtenBy(MachineOperand const& operand) noexcept
    {
        auto const* reg = std::get_if<RegisterOperand>(&operand);
        if (reg == nullptr || reg->role == OperandRole::Use)
            return std::nullopt;
        return virtualOf(reg->name);
    }

    /// What each laid-out block reads before it writes (gen) and writes (kill), over the values live across
    /// block boundaries only: those some block reads before it, or without, defining them. Each such value
    /// has a dense index, its bit in the live-in sets.
    class BlockFacts
    {
      public:
        BlockFacts(MachineFunction const& function,
                   BlockOrder const& order,
                   std::pmr::memory_resource& memory):
            _globalIndex(function.virtualRegisterCount(), Unnumbered, &memory),
            _globals { &memory },
            _gen(order.blocks().size(), PoolRange {}, &memory),
            _kill(order.blocks().size(), PoolRange {}, &memory),
            _genPool { &memory },
            _killPool { &memory }
        {
            auto const registers = function.virtualRegisterCount();
            auto definingBlock = std::pmr::vector<uint32_t>(registers, Unnumbered, &memory);
            auto definedHere = std::pmr::vector<uint32_t>(registers, Unnumbered, &memory);
            auto exposedHere = std::pmr::vector<uint32_t>(registers, Unnumbered, &memory);
            auto const blocks = order.blocks();
            for (auto const slot: std::views::iota(uint32_t { 0 }, static_cast<uint32_t>(blocks.size())))
            {
                auto const block = blocks[slot];
                auto const genBegin = static_cast<uint32_t>(_genPool.size());
                auto const killBegin = static_cast<uint32_t>(_killPool.size());
                auto const read = [&](uint32_t reg) {
                    if (definedHere[reg] != slot && exposedHere[reg] != slot)
                    {
                        exposedHere[reg] = slot;
                        _genPool.push_back(reg);
                    }
                };
                auto const write = [&](uint32_t reg) {
                    if (definingBlock[reg] != Unnumbered && definingBlock[reg] != slot)
                        _isIllFormed = true;
                    definingBlock[reg] = slot;
                    if (definedHere[reg] != slot)
                    {
                        definedHere[reg] = slot;
                        _killPool.push_back(reg);
                    }
                };
                for (auto const parameter: function.parameters(block))
                    write(std::to_underlying(parameter));
                for (auto const instruction: function.instructions(block))
                {
                    auto const operands = function.operands(instruction);
                    for (auto const& operand: operands)
                        forEachRead(operand, read);
                    for (auto const& operand: operands)
                        if (auto const written = writtenBy(operand))
                            write(*written);
                }
                for (auto const& edge: function.edges(block))
                    for (auto const argument: function.arguments(edge))
                        read(std::to_underlying(argument));
                _gen[slot] = PoolRange { .begin = genBegin,
                                         .count = static_cast<uint32_t>(_genPool.size()) - genBegin };
                _kill[slot] = PoolRange { .begin = killBegin,
                                          .count = static_cast<uint32_t>(_killPool.size()) - killBegin };
            }
            // A value read before its definition anywhere is live across a boundary: number them, and keep in
            // the kill lists only those.
            for (auto const reg: _genPool)
                if (_globalIndex[reg] == Unnumbered)
                {
                    _globalIndex[reg] = static_cast<uint32_t>(_globals.size());
                    _globals.push_back(reg);
                }
            for (auto& reg: _genPool)
                reg = _globalIndex[reg];
            for (auto& reg: _killPool)
                reg = _globalIndex[reg];
        }

        /// Whether a value is defined in two blocks.
        [[nodiscard]] bool isIllFormed() const noexcept { return _isIllFormed; }

        /// How many values live across block boundaries.
        [[nodiscard]] uint32_t globalCount() const noexcept { return static_cast<uint32_t>(_globals.size()); }

        /// The register of global value @p index.
        [[nodiscard]] uint32_t registerOf(uint32_t index) const noexcept { return _globals[index]; }

        /// The global values the block at layout slot @p slot reads before it defines them.
        [[nodiscard]] std::span<uint32_t const> gen(uint32_t slot) const noexcept
        {
            return std::span { _genPool }.subspan(_gen[slot].begin, _gen[slot].count);
        }

        /// What the block at layout slot @p slot defines, as global indices (Unnumbered for a local value).
        [[nodiscard]] std::span<uint32_t const> kill(uint32_t slot) const noexcept
        {
            return std::span { _killPool }.subspan(_kill[slot].begin, _kill[slot].count);
        }

      private:
        std::pmr::vector<uint32_t> _globalIndex; ///< Per register, its global index, or Unnumbered.
        std::pmr::vector<uint32_t> _globals;     ///< Per global index, its register.
        std::pmr::vector<PoolRange> _gen;
        std::pmr::vector<PoolRange> _kill;
        std::pmr::vector<uint32_t> _genPool;
        std::pmr::vector<uint32_t> _killPool;
        bool _isIllFormed = false; ///< Whether a value is defined in two blocks.
    };

    /// One bit set per laid-out block, over the global values.
    class LiveSets
    {
      public:
        LiveSets(uint32_t blocks, uint32_t values, std::pmr::memory_resource& memory):
            _words { (values + 63) / 64 }, _bits(std::size_t { blocks } * _words, 0, &memory)
        {
        }

        [[nodiscard]] std::span<uint64_t> of(uint32_t slot) noexcept
        {
            return std::span { _bits }.subspan(std::size_t { slot } * _words, _words);
        }

        [[nodiscard]] std::span<uint64_t const> of(uint32_t slot) const noexcept
        {
            return std::span { _bits }.subspan(std::size_t { slot } * _words, _words);
        }

        [[nodiscard]] uint32_t words() const noexcept { return _words; }

      private:
        uint32_t _words;
        std::pmr::vector<uint64_t> _bits;
    };

    /// The live-in sets of every laid-out block: the dataflow fixpoint over the control-flow graph, by a
    /// worklist seeded in postorder. Each block is taken again only when a successor's live-in grew, and a
    /// set only grows, so the work is bounded by the bits that are set.
    [[nodiscard]] std::expected<LiveSets, CompileError> liveIns(MachineFunction const& function,
                                                                BlockOrder const& order,
                                                                BlockFacts const& facts,
                                                                LivenessPoll& poll,
                                                                std::pmr::memory_resource& memory)
    {
        auto const blockCount = static_cast<uint32_t>(order.blocks().size());
        auto sets = LiveSets { blockCount, facts.globalCount(), memory };
        auto const graph = controlFlowGraphOf(function, memory);
        auto worklist = std::pmr::vector<uint32_t> { &memory };
        auto queued = std::pmr::vector<uint8_t>(blockCount, 1, &memory);
        for (auto const block: reversePostorderOf(graph, memory))
            worklist.push_back(*order.position(BlockId { block }));
        auto next = std::pmr::vector<uint64_t>(sets.words(), 0, &memory);
        while (!worklist.empty())
        {
            if (auto polled = poll.step(); !polled)
                return std::unexpected { polled.error() };
            auto const slot = worklist.back();
            worklist.pop_back();
            queued[slot] = 0;
            auto const block = order.blocks()[slot];
            std::ranges::fill(next, 0);
            for (auto const successor: graph.successors(block))
            {
                auto const in = sets.of(*order.position(successor));
                for (auto const word: std::views::iota(uint32_t { 0 }, sets.words()))
                    next[word] |= in[word];
            }
            for (auto const index: facts.kill(slot))
                if (index != Unnumbered)
                    next[index / 64] &= ~(uint64_t { 1 } << (index % 64));
            for (auto const index: facts.gen(slot))
                next[index / 64] |= uint64_t { 1 } << (index % 64);
            auto const in = sets.of(slot);
            if (std::ranges::equal(next, in))
                continue;
            std::ranges::copy(next, in.begin());
            for (auto const predecessor: graph.predecessors(block))
                if (auto const position = order.position(predecessor); position && queued[*position] == 0)
                {
                    queued[*position] = 1;
                    worklist.push_back(*position);
                }
        }
        return sets;
    }

    /// The backward walk over a numbered layout: per block its live-out (the successors' live-ins and the
    /// edges' arguments), its instructions in reverse, its parameters.
    class BackwardWalk
    {
      public:
        BackwardWalk(MachineFunction const& function,
                     BlockOrder const& order,
                     BlockFacts const& facts,
                     LiveSets const& sets,
                     LiveIntervals& result,
                     std::pmr::memory_resource& memory):
            _function { function },
            _order { order },
            _facts { facts },
            _sets { sets },
            _result { result },
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
            for (auto const parameter: _function.parameters(block))
                define(std::to_underlying(parameter), range.start, range.start);
        }

      private:
        /// Live out: the successors' live-ins, and the arguments the edges pass, read at the terminator; all
        /// live over the whole block to start with.
        void startAtLiveOut(BlockId block, LiveRange range)
        {
            _live.clear();
            for (auto const& edge: _function.edges(block))
            {
                auto const in = _sets.of(*_order.position(edge.target));
                for (auto const word: std::views::iota(uint32_t { 0 }, static_cast<uint32_t>(in.size())))
                    for (auto bits = in[word]; bits != 0; bits &= bits - 1)
                        _live.add(
                            _facts.registerOf((word * 64) + static_cast<uint32_t>(std::countr_zero(bits))));
            }
            auto const instructions = _function.instructions(block);
            auto const terminator =
                instructions.empty() ? range.start : _result.instructionPositions[instructions.back()];
            for (auto const& edge: _function.edges(block))
                for (auto const argument: _function.arguments(edge))
                {
                    _live.add(std::to_underlying(argument));
                    _builder.addUse(std::to_underlying(argument), terminator);
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
                if (auto const defined = writtenBy(operand))
                {
                    auto const early = std::get<RegisterOperand>(operand).role == OperandRole::Early;
                    define(*defined, early ? position - 1 : position, position);
                }
            for (auto const& operand: operands)
                forEachRead(operand, [&](uint32_t reg) {
                    _builder.addRange(reg, range.start, position);
                    _live.add(reg);
                    _builder.addUse(reg, position);
                });
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

        MachineFunction const& _function;
        BlockOrder const& _order;
        BlockFacts const& _facts;
        LiveSets const& _sets;
        LiveIntervals& _result;
        LiveSet _live;
        IntervalBuilder _builder;
    };
} // namespace

std::expected<LiveIntervals, CompileError> computeLiveIntervals(MachineFunction const& function,
                                                                BlockOrder const& order,
                                                                intermediate::LoopNesting const& loops,
                                                                CompileLimits const& limits,
                                                                CompileCancellation const& cancellation,
                                                                std::pmr::memory_resource& memory)
{
    // Every position this reads is of a laid-out block: the layout covers every block a laid-out one jumps
    // to, and with it every reachable one.
    if (!verifyContiguousLoops(order, loops) || !verifyLayoutCoversItsSuccessors(function, order))
        return illFormed();
    auto const facts = BlockFacts { function, order, memory };
    if (facts.isIllFormed())
        return illFormed();
    auto const bits = uint64_t { facts.globalCount() } * order.blocks().size();
    if (bits > limits.maxLiveSetBits)
        return std::unexpected { CompileError {
            .code = CompileErrorCode::LiveSetTooLarge, .requested = bits, .limit = limits.maxLiveSetBits } };
    auto poll = LivenessPoll { cancellation };
    auto sets = liveIns(function, order, facts, poll, memory);
    if (!sets)
        return std::unexpected { sets.error() };
    auto result = numbered(function, order, memory);
    auto walk = BackwardWalk { function, order, facts, *sets, result, memory };
    for (auto const block: order.blocks() | std::views::reverse)
    {
        if (auto polled = poll.step(); !polled)
            return std::unexpected { polled.error() };
        walk.walk(block);
    }

    // Built backwards: put each interval in ascending order.
    for (auto& interval: result.intervals)
    {
        std::ranges::reverse(interval.ranges);
        std::ranges::sort(interval.usePositions);
        auto const duplicates = std::ranges::unique(interval.usePositions);
        interval.usePositions.erase(duplicates.begin(), duplicates.end());
    }
    return result;
}

} // namespace isoline::compiler::machine
