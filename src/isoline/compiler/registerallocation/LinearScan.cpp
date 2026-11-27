// SPDX-License-Identifier: Apache-2.0
#include <isoline/compiler/registerallocation/LinearScan.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <span>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/compiler/intermediate/Dominators.hpp>
#include <isoline/compiler/machine/CallingConvention.hpp>
#include <isoline/compiler/machine/FrameRule.hpp>
#include <isoline/compiler/machine/Linearize.hpp>
#include <isoline/compiler/machine/Liveness.hpp>
#include <isoline/compiler/machine/MachineVerifier.hpp>
#include <isoline/compiler/registerallocation/ParallelMoves.hpp>
#include <isoline/compiler/registerallocation/SpillSlots.hpp>

namespace isoline::compiler::registerallocation
{

using machine::BlockId;
using machine::BlockOrder;
using machine::FrameLayout;
using machine::LiveIntervals;
using machine::LiveRange;
using machine::LocationKind;
using machine::MachineFunction;
using machine::MachineOperand;
using machine::MemoryOperand;
using machine::OperandConstraint;
using machine::OperandRole;
using machine::PhysicalRegister;
using machine::RegisterClass;
using machine::RegisterKind;
using machine::RegisterName;
using machine::RegisterOperand;
using machine::RegisterSet;

namespace
{
    /// No position, no child, no index.
    constexpr auto None = std::numeric_limits<uint32_t>::max();

    /// How often the scan polls the cancellation, in intervals taken.
    constexpr auto PollInterval = uint32_t { 4'096 };

    /// The bytes of one spill slot: a 64-bit word of any class.
    constexpr auto SlotBytes = uint32_t { 8 };

    /// The refusal of a function the allocator cannot take: a compiler bug.
    [[nodiscard]] std::unexpected<CompileError> refused() noexcept
    {
        return std::unexpected { CompileError { .code = CompileErrorCode::InternalVerificationFailed,
                                                .pass = "register-allocation" } };
    }

    /// A dense index per physical register: its class's block of MaximumRegistersPerClass, then its number.
    [[nodiscard]] uint32_t registerIndex(PhysicalRegister reg) noexcept
    {
        return (static_cast<uint32_t>(reg.registerClass) * machine::MaximumRegistersPerClass) + reg.number;
    }

    /// How many register indices there are.
    constexpr auto RegisterIndexCount =
        static_cast<uint32_t>(machine::RegisterClassCount) * machine::MaximumRegistersPerClass;

    /// @p value rounded up to a multiple of @p alignment (non-zero).
    [[nodiscard]] constexpr uint64_t alignUp(uint64_t value, uint64_t alignment) noexcept
    {
        return ((value + alignment - 1) / alignment) * alignment;
    }

    /// A compressed table: per key, a run of one pool.
    template <typename Entry>
    class Runs
    {
      public:
        explicit Runs(std::pmr::memory_resource& memory): _begins { &memory }, _entries { &memory } {}

        /// Builds the table from @p keyed, (key, entry) pairs sorted by key, for keys below @p keys.
        void build(std::span<std::pair<uint32_t, Entry> const> keyed, uint32_t keys)
        {
            _begins.assign(std::size_t { keys } + 1, 0);
            _entries.clear();
            _entries.reserve(keyed.size());
            for (auto const& [key, entry]: keyed)
            {
                ++_begins[std::size_t { key } + 1];
                _entries.push_back(entry);
            }
            for (auto const key: std::views::iota(uint32_t { 0 }, keys))
                _begins[std::size_t { key } + 1] += _begins[key];
        }

        [[nodiscard]] std::span<Entry const> of(uint32_t key) const noexcept
        {
            return std::span { _entries }.subspan(_begins[key],
                                                  _begins[std::size_t { key } + 1] - _begins[key]);
        }

        [[nodiscard]] std::span<Entry> of(uint32_t key) noexcept
        {
            return std::span { _entries }.subspan(_begins[key],
                                                  _begins[std::size_t { key } + 1] - _begins[key]);
        }

      private:
        std::pmr::vector<uint32_t> _begins;
        std::pmr::vector<Entry> _entries;
    };

    /// The positions of the laid-out blocks, and the split points between them: where an interval may end
    /// and the next begin. A split point is a block's entry (resolved on its incoming edges) or a gap within
    /// a block that an instruction follows (where its moves go).
    class Layout
    {
      public:
        Layout(BlockOrder const& order, LiveIntervals const& live, std::pmr::memory_resource& memory):
            _blocks { &memory }, _starts { &memory }, _ends { &memory }
        {
            for (auto const block: order.blocks())
            {
                auto const range = live.blockRanges[std::to_underlying(block)];
                _blocks.push_back(block);
                _starts.push_back(range.start);
                _ends.push_back(range.end);
            }
        }

        [[nodiscard]] uint32_t blockCount() const noexcept { return static_cast<uint32_t>(_blocks.size()); }
        [[nodiscard]] BlockId block(uint32_t slot) const noexcept { return _blocks[slot]; }
        [[nodiscard]] uint32_t start(uint32_t slot) const noexcept { return _starts[slot]; }
        [[nodiscard]] std::span<uint32_t const> starts() const noexcept { return _starts; }

        /// The position of the block's terminator, or its entry for a block without instructions.
        [[nodiscard]] uint32_t terminator(uint32_t slot) const noexcept
        {
            return _ends[slot] - _starts[slot] > 2 ? _ends[slot] - 2 : _starts[slot];
        }

        [[nodiscard]] uint32_t end() const noexcept { return _ends.empty() ? 0 : _ends.back(); }

        /// The layout slot of the block holding @p position (Require: within the layout).
        [[nodiscard]] uint32_t slotOf(uint32_t position) const noexcept
        {
            auto const after = std::ranges::upper_bound(_starts, position);
            Require(after != _starts.begin());
            return static_cast<uint32_t>(after - _starts.begin()) - 1;
        }

        /// The last split point at or before @p position (Require: within the layout).
        [[nodiscard]] uint32_t splitBefore(uint32_t position) const noexcept
        {
            Require(position < end());
            auto const slot = slotOf(position);
            auto const start = _starts[slot];
            auto const last = terminator(slot);
            if (position == start || last == start)
                return start;
            Require(position <= last); // no interval starts or meets another past a block's terminator
            return position % 2 == 1 ? position : position - 1;
        }

        /// The first split point at or after @p position (Require: within the layout); a block's end is the
        /// next block's entry.
        [[nodiscard]] uint32_t splitAfter(uint32_t position) const noexcept
        {
            Require(position < end());
            auto const slot = slotOf(position);
            if (position >= terminator(slot))
                return _ends[slot];
            return position % 2 == 1 ? position : position + 1;
        }

        /// Whether @p position is an instruction's.
        [[nodiscard]] bool isInstruction(uint32_t position) const noexcept
        {
            return position % 2 == 0 && position < end() && position != _starts[slotOf(position)];
        }

        /// Whether @p position is a gap within a block that an instruction follows.
        [[nodiscard]] bool isGap(uint32_t position) const noexcept
        {
            if (position % 2 == 0 || position >= end())
                return false;
            return position < terminator(slotOf(position));
        }

      private:
        std::pmr::vector<BlockId> _blocks;
        std::pmr::vector<uint32_t> _starts;
        std::pmr::vector<uint32_t> _ends;
    };

    /// What an instruction does with a value it needs in a register.
    enum class UseKind : uint8_t
    {
        Read,  ///< Only reads it: the value may leave the register in the gap before, as long as it stays.
        Write, ///< Writes it (and may read it too): the register holds the new value from the instruction on.
    };

    /// A position where a value must be in a register.
    struct UseSite
    {
        uint32_t position = 0;
        UseKind kind = UseKind::Read;
    };

    /// A copy between a value and the fixed register an operand asks for: into it before the instruction
    /// (a use), or out of it after (a definition).
    struct FixedCopy
    {
        uint32_t gap = 0;          ///< The gap the copy runs in.
        uint32_t reg = 0;          ///< The virtual register.
        PhysicalRegister fixed {}; ///< The fixed register.
    };

    /// What the allocator reads off the function before the scan: per virtual register its live ranges and
    /// the positions where it must be in a register; per physical register where it is taken (pre-coloured);
    /// the copies fixed operands need; and the registers written by fixed and physical operands.
    struct Facts
    {
        explicit Facts(std::pmr::memory_resource& memory):
            ranges { memory },
            uses { memory },
            fixed { memory },
            copiesIn { &memory },
            copiesOut { &memory },
            classes { &memory }
        {
        }

        Runs<LiveRange> ranges;                  ///< Per virtual register, its live ranges.
        Runs<UseSite> uses;                      ///< Per virtual register, where it needs a register.
        Runs<LiveRange> fixed;                   ///< Per register index, where it is taken.
        std::pmr::vector<FixedCopy> copiesIn;    ///< Before an instruction, sorted by gap.
        std::pmr::vector<FixedCopy> copiesOut;   ///< After an instruction, sorted by (register, gap).
        std::pmr::vector<RegisterClass> classes; ///< Per virtual register, its class.
        RegisterSet written {};                  ///< Physical registers fixed and physical operands write.
    };

    /// The constraint row of operand @p position among @p rows: the last row for a variadic tail.
    [[nodiscard]] OperandConstraint const& rowOf(std::span<OperandConstraint const> rows,
                                                 std::size_t position)
    {
        Require(!rows.empty());
        return rows[std::min(position, rows.size() - 1)];
    }

    /// Reads a function's facts under its liveness and layout: one pass over the laid-out instructions.
    class FactsReader
    {
      public:
        FactsReader(MachineFunction const& function,
                    machine::ITarget const& target,
                    LiveIntervals const& live,
                    Layout const& layout,
                    std::pmr::memory_resource& memory):
            _function { function },
            _target { target },
            _live { live },
            _layout { layout },
            _memory { memory },
            _facts { memory },
            _uses { &memory },
            _fixed { &memory },
            _trims { &memory }
        {
        }

        /// The facts; or InternalVerificationFailed for a fixed definition no instruction follows.
        [[nodiscard]] std::expected<Facts, CompileError> read()
        {
            auto const registers = _function.virtualRegisterCount();
            _facts.classes.reserve(registers);
            for (auto const reg: std::views::iota(uint32_t { 0 }, registers))
                _facts.classes.push_back(_function.registerClass(machine::VirtualRegister { reg }));
            for (auto const slot: std::views::iota(uint32_t { 0 }, _layout.blockCount()))
            {
                _openSince.fill(None);
                _lastRead.fill(None);
                _blockStart = _layout.start(slot);
                for (auto const instruction: _function.instructions(_layout.block(slot)))
                    if (!readInstruction(instruction))
                        return refused();
                for (auto const index: std::views::iota(uint32_t { 0 }, RegisterIndexCount))
                    if (_openSince[index] != None)
                        close(index);
            }
            finishFixed();
            finishUses();
            finishRanges();
            return std::move(_facts);
        }

      private:
        void block(PhysicalRegister reg, uint32_t from, uint32_t to)
        {
            _fixed.emplace_back(registerIndex(reg), LiveRange { .start = from, .end = to });
        }

        /// Ends the physical register at @p index's open range: from its definition (or the block's entry) to
        /// its last read, or one position for a definition nothing reads.
        void close(uint32_t index)
        {
            auto const from = _openSince[index];
            auto const to = _lastRead[index] != None && _lastRead[index] > from ? _lastRead[index] : from + 1;
            _fixed.emplace_back(index, LiveRange { .start = from, .end = to });
            _openSince[index] = None;
        }

        void physicalRead(PhysicalRegister reg, uint32_t position)
        {
            auto const index = registerIndex(reg);
            if (_openSince[index] == None)
                _openSince[index] = _blockStart;
            _lastRead[index] = position;
        }

        void physicalWrite(PhysicalRegister reg, uint32_t from)
        {
            auto const index = registerIndex(reg);
            if (_openSince[index] != None)
                close(index);
            _openSince[index] = from;
            _lastRead[index] = None;
            _facts.written = _facts.written.with(reg);
        }

        /// Reads @p name at @p position: a use of a virtual register, or a read of a physical one.
        void readName(RegisterName const& name, uint32_t position)
        {
            if (name.kind == RegisterKind::Virtual)
                _uses.emplace_back(name.number, UseSite { .position = position, .kind = UseKind::Read });
            else if (name.kind == RegisterKind::Physical)
                physicalRead(name.physicalRegister(), position);
        }

        /// Reads one instruction: its reads first, then its writes, as it does them.
        [[nodiscard]] bool readInstruction(uint32_t instruction)
        {
            auto const position = _live.instructionPositions[instruction];
            auto const operands = _function.operands(instruction);
            auto const rows = _target.constraints(_function.instruction(instruction).opcode);
            for (auto const& operand: operands)
                if (auto const* address = std::get_if<MemoryOperand>(&operand))
                {
                    readName(address->base, position);
                    readName(address->index, position);
                }
            for (auto const index: std::views::iota(std::size_t { 0 }, operands.size()))
            {
                auto const* reg = std::get_if<RegisterOperand>(&operands[index]);
                if (reg == nullptr)
                    continue;
                auto const& row = rowOf(rows, index);
                if (reg->name.kind == RegisterKind::Physical)
                {
                    if (reg->role == OperandRole::Use || reg->role == OperandRole::UseDef)
                        physicalRead(reg->name.physicalRegister(), position);
                }
                else if (reg->name.kind == RegisterKind::Virtual && !row.fixed)
                    _uses.emplace_back(
                        reg->name.number,
                        UseSite { .position = position,
                                  .kind = reg->role == OperandRole::Use ? UseKind::Read : UseKind::Write });
                else if (reg->name.kind == RegisterKind::Virtual && !readFixed(*reg, row, position))
                    return false;
            }
            for (auto const& operand: operands)
                if (auto const* reg = std::get_if<RegisterOperand>(&operand);
                    reg != nullptr && reg->role != OperandRole::Use
                    && reg->name.kind == RegisterKind::Physical)
                    physicalWrite(reg->name.physicalRegister(),
                                  reg->role == OperandRole::Early ? position - 1 : position);
            return true;
        }

        /// Reads a virtual operand fixed to a register: the copies into and out of it, and where the copies
        /// take it. A definition's value starts after its copy out.
        [[nodiscard]] bool readFixed(RegisterOperand const& reg,
                                     OperandConstraint const& row,
                                     uint32_t position)
        {
            auto const fixed = PhysicalRegister { .registerClass = row.registerClass, .number = *row.fixed };
            auto const reads = reg.role == OperandRole::Use || reg.role == OperandRole::UseDef;
            auto const from = reg.role == OperandRole::Early ? position - 1 : position;
            if (reads)
            {
                _facts.copiesIn.push_back(
                    FixedCopy { .gap = position - 1, .reg = reg.name.number, .fixed = fixed });
                block(fixed, position - 1, position);
            }
            if (reg.role == OperandRole::Use)
                return true;
            // The copy out runs in the gap after, among its splits, which read before they write: so the
            // register is taken up to that gap only. An instruction must follow in the block.
            if (!_layout.isGap(position + 1))
                return false;
            _facts.copiesOut.push_back(
                FixedCopy { .gap = position + 1, .reg = reg.name.number, .fixed = fixed });
            block(fixed, reads ? position - 1 : from, position + 1);
            _facts.written = _facts.written.with(fixed);
            if (!reads)
                _trims.emplace_back(reg.name.number, from);
            return true;
        }

        /// The fixed ranges per register, merged.
        void finishFixed()
        {
            std::ranges::sort(
                _fixed, {}, [](auto const& entry) { return std::pair { entry.first, entry.second.start }; });
            auto merged = std::pmr::vector<std::pair<uint32_t, LiveRange>> { &_memory };
            for (auto const& [index, range]: _fixed)
            {
                if (!merged.empty() && merged.back().first == index
                    && merged.back().second.end >= range.start)
                    merged.back().second.end = std::max(merged.back().second.end, range.end);
                else
                    merged.emplace_back(index, range);
            }
            _facts.fixed.build(merged, RegisterIndexCount);
            // Total orders: the copies of one gap come in operand order, which must not decide the code (and
            // std::sort orders equal keys differently in each standard library).
            std::ranges::sort(_facts.copiesIn, {}, [](FixedCopy const& copy) {
                return std::tuple { copy.gap, registerIndex(copy.fixed), copy.reg };
            });
            std::ranges::sort(_facts.copiesOut, {}, [](FixedCopy const& copy) {
                return std::tuple { copy.reg, copy.gap, registerIndex(copy.fixed) };
            });
        }

        void finishUses()
        {
            // One site per position: a write if any operand there writes.
            std::ranges::sort(_uses, {}, [](auto const& entry) {
                return std::pair { entry.first, entry.second.position };
            });
            auto merged = std::pmr::vector<std::pair<uint32_t, UseSite>> { &_memory };
            for (auto const& [reg, site]: _uses)
            {
                if (!merged.empty() && merged.back().first == reg
                    && merged.back().second.position == site.position)
                    merged.back().second.kind = std::max(merged.back().second.kind, site.kind);
                else
                    merged.emplace_back(reg, site);
            }
            _facts.uses.build(merged, _function.virtualRegisterCount());
        }

        /// The live ranges, a fixed definition's starting after its copy out.
        void finishRanges()
        {
            std::ranges::sort(_trims);
            auto ranges = std::pmr::vector<std::pair<uint32_t, LiveRange>> { &_memory };
            for (auto const reg: std::views::iota(uint32_t { 0 }, _function.virtualRegisterCount()))
                for (auto range: _live.intervals[reg].ranges)
                {
                    if (std::ranges::binary_search(_trims, std::pair { reg, range.start }))
                        range.start += range.start % 2 == 0 ? 1 : 2;
                    if (range.start < range.end)
                        ranges.emplace_back(reg, range);
                }
            _facts.ranges.build(ranges, _function.virtualRegisterCount());
        }

        MachineFunction const& _function;
        machine::ITarget const& _target;
        LiveIntervals const& _live;
        Layout const& _layout;
        std::pmr::memory_resource& _memory;
        Facts _facts;
        std::pmr::vector<std::pair<uint32_t, UseSite>> _uses;    ///< (register, where it needs one)
        std::pmr::vector<std::pair<uint32_t, LiveRange>> _fixed; ///< (register index, range)
        std::pmr::vector<std::pair<uint32_t, uint32_t>> _trims;  ///< (register, definition's start)
        std::array<uint32_t, RegisterIndexCount> _openSince {};  ///< Per register index, in the block.
        std::array<uint32_t, RegisterIndexCount> _lastRead {};   ///< Per register index, in the block.
        uint32_t _blockStart = 0;
    };

    /// Where a child of an interval lives.
    enum class Placement : uint8_t
    {
        Unplaced, ///< Not yet decided: in the scan's queue.
        Register, ///< In a register.
        Slot,     ///< In its virtual register's spill slot.
    };

    /// What a child is: a part of the interval that may be evicted, or a window around one use that may not.
    enum class ChildKind : uint8_t
    {
        Part,   ///< A part of its interval: the whole of it, a prefix or a remainder.
        Window, ///< The window of one use of an evicted interval, never evicted itself.
    };

    /// A child of a virtual register's interval: its positions [from, to), split at split points.
    struct Child
    {
        uint32_t reg = 0;  ///< The virtual register.
        uint32_t from = 0; ///< Its first position.
        uint32_t to = 0;   ///< One past its last.
        Placement placement = Placement::Unplaced;
        PhysicalRegister physical {}; ///< Its register, when placed in one.
        ChildKind kind = ChildKind::Part;
    };

    /// One range a register holds: its end and the child it is of.
    struct HeldRange
    {
        uint32_t end = 0;
        uint32_t child = 0;
    };

    /// What a register holds: per range start, its end and the child.
    using Holding = std::pmr::map<uint32_t, HeldRange>;

    /// The scan: the intervals' children by start, and what each register holds.
    class Scan
    {
      public:
        Scan(MachineFunction const& function,
             machine::ITarget const& target,
             Facts const& facts,
             Layout const& layout,
             CompileCancellation const& cancellation,
             std::pmr::memory_resource& memory):
            _target { target },
            _facts { facts },
            _layout { layout },
            _cancellation { cancellation },
            _children { &memory },
            _queue { &memory },
            _holdings { &memory },
            _hints(function.virtualRegisterCount(), None, &memory)
        {
            _holdings.reserve(RegisterIndexCount);
            for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, RegisterIndexCount))
                _holdings.emplace_back();
            auto uses = std::size_t { 0 };
            for (auto const reg: std::views::iota(uint32_t { 0 }, function.virtualRegisterCount()))
            {
                uses += facts.uses.of(reg).size();
                auto const ranges = facts.ranges.of(reg);
                if (ranges.empty())
                    continue;
                auto const child =
                    newChild(Child { .reg = reg, .from = ranges.front().start, .to = ranges.back().end });
                enqueue(child);
            }
            // Every child the scan makes consumes a use, or ends its interval's work: the bound it is held
            // to.
            _childBound = (8 * (std::size_t { function.virtualRegisterCount() } + uses)) + 16;
        }

        /// Runs the scan to its end.
        [[nodiscard]] std::expected<void, CompileError> run()
        {
            auto taken = uint32_t { 0 };
            auto reached = uint32_t { 0 };
            while (!_queue.empty())
            {
                if (taken++ % PollInterval == 0 && _cancellation.state() == CancellationState::Cancelled)
                    return std::unexpected { CompileError { .code = CompileErrorCode::Cancelled } };
                std::ranges::pop_heap(_queue, std::greater {});
                auto const [position, child] = _queue.back();
                _queue.pop_back();
                // The scan only moves forward: every child it makes starts at or after the one it places, so
                // what a register holds is only ever of children placed at or before here.
                if (position < reached)
                    return refused(); // uncovered(allocator-invariant)
                reached = position;
                if (auto placed = place(child, position); !placed)
                    return placed;
                if (_children.size() > _childBound)
                    return refused(); // uncovered(allocator-invariant)
            }
            return {};
        }

        [[nodiscard]] std::span<Child const> children() const noexcept { return _children; }

      private:
        [[nodiscard]] uint32_t newChild(Child child)
        {
            auto const index = static_cast<uint32_t>(_children.size());
            _children.push_back(child);
            return index;
        }

        /// The live ranges of @p child, clipped to it.
        template <typename Visit>
        void forEachRange(uint32_t child, Visit const& visit) const
        {
            auto const part = _children[child]; // a copy: a visit may add children, which moves them
            auto const ranges = _facts.ranges.of(part.reg);
            auto const first =
                std::ranges::partition_point(ranges, [&](LiveRange range) { return range.end <= part.from; });
            for (auto const& range: std::span { first, ranges.end() })
            {
                if (range.start >= part.to)
                    return;
                if (!visit(LiveRange { .start = std::max(range.start, part.from),
                                       .end = std::min(range.end, part.to) }))
                    return;
            }
        }

        /// The first position of @p child at which its register is live; None if never.
        [[nodiscard]] uint32_t firstLive(uint32_t child) const
        {
            auto first = None;
            forEachRange(child, [&](LiveRange range) {
                first = range.start;
                return false;
            });
            return first;
        }

        void enqueue(uint32_t child)
        {
            auto const position = firstLive(child);
            Require(position != None); // every child the scan makes holds a live position of its value
            _queue.emplace_back(position, child);
            std::ranges::push_heap(_queue, std::greater {});
        }

        /// The first use of @p reg in [from, to]: a child [from, to) reads a use at its end, to, from to − 1.
        [[nodiscard]] uint32_t firstUse(uint32_t reg, uint32_t from, uint32_t to) const noexcept
        {
            auto const uses = _facts.uses.of(reg);
            auto const found = std::ranges::lower_bound(uses, from, {}, &UseSite::position);
            return found == uses.end() || found->position > to ? None : found->position;
        }

        /// Whether @p reg's use at @p position only reads it.
        [[nodiscard]] bool onlyReadsAt(uint32_t reg, uint32_t position) const noexcept
        {
            auto const uses = _facts.uses.of(reg);
            auto const found = std::ranges::lower_bound(uses, position, {}, &UseSite::position);
            return found != uses.end() && found->position == position && found->kind == UseKind::Read;
        }

        /// The first position where @p child's ranges meet what @p reg holds; None if they never do.
        [[nodiscard]] uint32_t heldConflict(uint32_t child, PhysicalRegister reg) const
        {
            auto const& holding = _holdings[registerIndex(reg)];
            auto conflict = None;
            forEachRange(child, [&](LiveRange range) {
                auto const after = holding.upper_bound(range.start);
                if (after != holding.begin() && std::prev(after)->second.end > range.start)
                    conflict = range.start;
                else if (after != holding.end() && after->first < range.end)
                    conflict = after->first;
                return conflict == None;
            });
            return conflict;
        }

        /// The first position before @p limit where @p child's ranges meet a fixed range of @p reg.
        [[nodiscard]] uint32_t fixedConflict(uint32_t child,
                                             PhysicalRegister reg,
                                             uint32_t limit = None) const
        {
            auto const fixed = _facts.fixed.of(registerIndex(reg));
            auto conflict = None;
            forEachRange(child, [&](LiveRange range) {
                if (range.start >= limit)
                    return false;
                auto const found = std::ranges::partition_point(
                    fixed, [&](LiveRange taken) { return taken.end <= range.start; });
                if (found != fixed.end() && found->start < range.end)
                    conflict = std::max(range.start, found->start);
                return conflict == None;
            });
            return conflict < limit ? conflict : None;
        }

        /// Splits @p child at split point @p at: it keeps [from, at), and the new child takes [at, to).
        [[nodiscard]] uint32_t split(uint32_t child, uint32_t at)
        {
            auto rest = _children[child];
            rest.from = at;
            rest.kind = ChildKind::Part;
            _children[child].to = at;
            return newChild(rest);
        }

        void assign(uint32_t child, PhysicalRegister reg)
        {
            auto& holding = _holdings[registerIndex(reg)];
            forEachRange(child, [&](LiveRange range) {
                holding.emplace(range.start, HeldRange { .end = range.end, .child = child });
                return true;
            });
            _children[child].placement = Placement::Register;
            _children[child].physical = reg;
            _hints[_children[child].reg] = registerIndex(reg);
        }

        void spill(uint32_t child) { _children[child].placement = Placement::Slot; }

        /// Where an eviction at @p position cuts @p holder: the split point before its first live position at
        /// or after @p position.
        [[nodiscard]] uint32_t evictionCut(uint32_t holder, uint32_t position) const
        {
            auto resume = None;
            forEachRange(holder, [&](LiveRange range) {
                if (range.end <= position)
                    return true;
                resume = std::max(range.start, position);
                return false;
            });
            Require(resume != None);
            return std::max(_layout.splitBefore(resume), _children[holder].from);
        }

        /// Takes @p reg from @p holder from @p cut on: that part goes to the stack slot, with a window child
        /// around each of its uses after @p cut. @p cut is a split point, or the position of an instruction
        /// that only reads the holder, which keeps its register up to that read.
        void evict(uint32_t holder, PhysicalRegister reg, uint32_t cut)
        {
            auto const original = _children[holder];

            auto& holding = _holdings[registerIndex(reg)];
            forEachRange(holder, [&](LiveRange range) {
                if (range.end <= cut)
                    return true;
                if (range.start >= cut)
                    holding.erase(range.start);
                else
                    holding[range.start].end = cut;
                return true;
            });
            _children[holder].to = cut;
            spillWithWindows(original.reg, cut, original.to);
        }

        /// Places [@p cut, @p end) of @p reg in its slot, with a window child around each of its uses after
        /// @p cut: a read's ends at the read (the value leaves the register there), a write's in the gap
        /// after.
        void spillWithWindows(uint32_t reg, uint32_t cut, uint32_t end)
        {
            auto const original = Child { .reg = reg, .from = cut, .to = end };
            auto next = cut;
            auto const uses = _facts.uses.of(original.reg);
            for (auto const& site:
                 std::span { std::ranges::upper_bound(uses, cut, {}, &UseSite::position), uses.end() })
            {
                auto const use = site.position;
                if (use > original.to)
                    break;
                // A read needs the register up to the read, and may leave it there; a write from the write
                // on, to the gap after.
                auto const from = std::max(use - 1, next);
                auto const to = site.kind == UseKind::Read && _layout.isInstruction(use)
                                    ? std::min(use, original.to)
                                    : std::min(_layout.splitAfter(use), original.to);
                Require(from < to); // uses are two apart, so windows never overlap
                if (from > next)
                    spill(newChild(Child { .reg = original.reg, .from = next, .to = from }));
                enqueue(newChild(
                    Child { .reg = original.reg, .from = from, .to = to, .kind = ChildKind::Window }));
                next = to;
            }
            if (next < original.to)
                spill(newChild(Child { .reg = original.reg, .from = next, .to = original.to }));
        }

        /// Places @p child, first live at @p position.
        [[nodiscard]] std::expected<void, CompileError> place(uint32_t child, uint32_t position)
        {
            auto const reg = _children[child].reg;
            auto const registers = _target.allocatable(_facts.classes[reg]);
            auto const use = firstUse(reg, position, _children[child].to);
            auto const hint = _hints[reg];
            auto best = std::optional<PhysicalRegister> {};
            auto bestFree = uint32_t { 0 };
            auto whole = std::optional<PhysicalRegister> {};
            for (auto const candidate: registers)
            {
                auto const free = std::min(heldConflict(child, candidate), fixedConflict(child, candidate));
                if (free == None && (!whole || registerIndex(candidate) == hint))
                    whole = candidate;
                if (!best || free > bestFree || (free == bestFree && registerIndex(candidate) == hint))
                {
                    best = candidate;
                    bestFree = free;
                }
            }
            if (whole)
            {
                assign(child, *whole);
                return {};
            }
            if (!best)
                return refused(); // a class without registers
            if (use == None)
            {
                // Nothing needs a register: keep one while it is free, then the slot.
                auto const at = _layout.splitBefore(bestFree);
                if (at > position)
                {
                    auto const rest = split(child, at);
                    assign(child, *best);
                    spill(rest);
                }
                else
                    spill(child);
                return {};
            }
            auto const need = _layout.splitAfter(use);
            if (bestFree >= need)
            {
                auto const rest = split(child, _layout.splitBefore(bestFree));
                assign(child, *best);
                enqueue(rest);
                return {};
            }
            if (leavesAtRead(reg, use) && bestFree >= use)
            {
                // Free up to the read, which is all it needs: keep the register for it, then the slot.
                keepUpToTheRead(child, *best, use);
                return {};
            }
            return placeBlocked(child, position, use, need);
        }

        /// The child holding @p reg somewhere in [@p range.start, @p range.end), the first by position; None.
        [[nodiscard]] uint32_t holderIn(PhysicalRegister reg, LiveRange range) const
        {
            auto const& holding = _holdings[registerIndex(reg)];
            auto after = holding.upper_bound(range.start);
            if (after != holding.begin() && std::prev(after)->second.end > range.start)
                return std::prev(after)->second.child;
            return after != holding.end() && after->first < range.end ? after->second.child : None;
        }

        /// Whether an eviction at @p position, for a child whose first use @p use is a definition there, may
        /// leave @p holder its register up to its read at @p position (it leaves for its slot in the gap
        /// before, and the instruction still reads the register).
        [[nodiscard]] bool leavesAtTheRead(uint32_t holder, uint32_t position, uint32_t use) const noexcept
        {
            auto const& held = _children[holder];
            return use == position && held.from < position
                   && firstUse(held.reg, position, held.to) == position && leavesAtRead(held.reg, position);
        }

        /// When the holders of @p reg that @p child meets before @p reach next need it: the soonest of them,
        /// a window's never (it is never evicted), None if no holder is met.
        [[nodiscard]] uint32_t holdersNeed(
            uint32_t child, PhysicalRegister reg, uint32_t position, uint32_t use, uint32_t reach) const
        {
            auto const& holding = _holdings[registerIndex(reg)];
            auto soonest = None;
            forEachRange(child, [&](LiveRange range) {
                if (range.start >= reach)
                    return false;
                auto entry = holding.upper_bound(range.start);
                if (entry != holding.begin() && std::prev(entry)->second.end > range.start)
                    --entry;
                while (entry != holding.end() && entry->first < std::min(range.end, reach))
                {
                    auto const holder = entry->second.child;
                    ++entry;
                    auto next = nextUseOf(holder, position);
                    if (leavesAtTheRead(holder, position, use))
                        next = firstUse(_children[holder].reg, position + 1, _children[holder].to);
                    soonest = std::min(soonest, next);
                }
                return true;
            });
            return soonest;
        }

        /// When @p holder next needs its register, for ranking it against an eviction: a window never.
        [[nodiscard]] uint32_t nextUseOf(uint32_t holder, uint32_t position) const noexcept
        {
            auto const& held = _children[holder];
            return held.kind == ChildKind::Window ? 0 : firstUse(held.reg, position, held.to);
        }

        /// Whether @p reg's use at @p position may be its last in a register for now: an instruction that
        /// only reads it, which it may leave the register at.
        [[nodiscard]] bool leavesAtRead(uint32_t reg, uint32_t position) const noexcept
        {
            return _layout.isInstruction(position) && onlyReadsAt(reg, position);
        }

        /// Gives @p child @p reg up to its read at @p use, and the slot from there, with windows for its
        /// later uses.
        void keepUpToTheRead(uint32_t child, PhysicalRegister reg, uint32_t use)
        {
            auto const to = _children[child].to;
            _children[child].to = use;
            assign(child, reg);
            if (use < to)
                spillWithWindows(_children[child].reg, use, to);
        }

        /// Places @p child, first live at @p position, when no register is free up to its first use @p use.
        [[nodiscard]] std::expected<void, CompileError> placeBlocked(uint32_t child,
                                                                     uint32_t position,
                                                                     uint32_t use,
                                                                     uint32_t need)
        {
            auto const reg = _children[child].reg;
            // How far the child needs the register: to the read, which it may leave the register at, or past
            // the use.
            auto const reach = leavesAtRead(reg, use) ? use : need;
            auto best = std::optional<PhysicalRegister> {};
            auto bestNext = uint32_t { 0 };
            for (auto const candidate: _target.allocatable(_facts.classes[reg]))
            {
                if (fixedConflict(child, candidate, reach) != None)
                    continue;
                // Ranked by the soonest need of every holder met up to the reach (Wimmer and Franz's next-use
                // position); free up to it (the first conflict a fixed range past it), the register ranks
                // above all.
                auto const next = holdersNeed(child, candidate, position, use, reach);
                if (!best || next > bestNext)
                {
                    best = candidate;
                    bestNext = next;
                }
            }
            if (!best || bestNext <= use)
            {
                // Every holder needs its register first: wait in the slot until the gap before the use.
                auto const at = use - 1;
                if (at <= position)
                    return refused(); // the instruction asks for more registers than there are
                auto const rest = split(child, at);
                spill(child);
                enqueue(rest);
                return {};
            }
            // The child keeps the register up to its next fixed range, or up to its read when that range
            // starts at the read itself.
            auto const fixed = fixedConflict(child, *best);
            auto const original = _children[child].to;
            auto const keepsToTheRead = fixed != None && fixed < need;
            if (keepsToTheRead)
                _children[child].to = use;
            else if (fixed != None && _layout.splitBefore(fixed) < original)
                enqueue(split(child, _layout.splitBefore(fixed)));
            // Evict every holder the child meets, range by range, resuming where the last eviction left off:
            // each takes the holder's ranges from its cut on, so a holder met twice in a row is a bug,
            // refused rather than looped on.
            auto previous = None;
            auto intact = true;
            forEachRange(child, [&](LiveRange range) {
                while (intact)
                {
                    auto const holder = holderIn(*best, range);
                    if (holder == None)
                        break;
                    intact =
                        _children[holder].kind != ChildKind::Window && holder != child && holder != previous;
                    if (intact)
                        evict(holder,
                              *best,
                              leavesAtTheRead(holder, position, use) ? position
                                                                     : evictionCut(holder, position));
                    previous = holder;
                }
                return intact;
            });
            if (!intact)
                return refused(); // uncovered(allocator-invariant)
            assign(child, *best);
            if (keepsToTheRead && use < original)
                spillWithWindows(reg, use, original);
            return {};
        }

        machine::ITarget const& _target;
        Facts const& _facts;
        Layout const& _layout;
        CompileCancellation const& _cancellation;
        std::pmr::vector<Child> _children;
        std::pmr::vector<std::pair<uint32_t, uint32_t>> _queue; ///< (first live position, child), a min-heap.
        std::pmr::vector<Holding> _holdings;                    ///< Per register index.
        std::pmr::vector<uint32_t> _hints; ///< Per virtual register, the register index it last took.
        std::size_t _childBound = 0;
    };

    /// The scan's result as locations: per virtual register its children in order, and their places.
    class Placements
    {
      public:
        Placements(std::span<Child const> children,
                   Facts const& facts,
                   uint32_t registers,
                   uint32_t slotsFrom,
                   std::pmr::memory_resource& memory):
            _facts { facts }, _children { memory }, _slots(registers, None, &memory)
        {
            auto keyed = std::pmr::vector<std::pair<uint32_t, Child>> { &memory };
            for (auto const& child: children)
                if (child.from < child.to && child.placement != Placement::Unplaced)
                    keyed.emplace_back(child.reg, child);
            std::ranges::stable_sort(
                keyed, {}, [](auto const& entry) { return std::pair { entry.first, entry.second.from }; });
            _children.build(keyed, registers);

            // One slot per spilled register, reused once the register's lifetime is over (packSpillSlots()).
            auto spilled = std::pmr::vector<uint32_t> { &memory };
            auto lifetimes = std::pmr::vector<SlotLifetime> { &memory };
            for (auto const reg: std::views::iota(uint32_t { 0 }, registers))
            {
                auto const parts = _children.of(reg);
                auto const ranges = facts.ranges.of(reg);
                if (ranges.empty() || std::ranges::none_of(parts, [](Child const& part) {
                        return part.placement == Placement::Slot;
                    }))
                    continue;
                spilled.push_back(reg);
                lifetimes.push_back(SlotLifetime { .start = ranges.front().start, .end = ranges.back().end });
            }
            auto const packing = packSpillSlots(lifetimes, memory);
            _slotCount = packing.slotCount;
            for (auto const index: std::views::iota(std::size_t { 0 }, spilled.size()))
                _slots[spilled[index]] = slotsFrom + (packing.slots[index] * SlotBytes);
        }

        [[nodiscard]] uint32_t slotCount() const noexcept { return _slotCount; }

        /// Where @p reg lives at @p position; kind None where it has no child.
        [[nodiscard]] Location at(uint32_t reg, uint32_t position) const noexcept
        {
            auto const parts = _children.of(reg);
            auto const after = std::ranges::upper_bound(parts, position, {}, &Child::from);
            if (after == parts.begin() || position >= std::prev(after)->to)
                return {};
            return locationOf(*std::prev(after));
        }

        /// Where @p part lives.
        [[nodiscard]] Location locationOf(Child const& part) const noexcept
        {
            if (part.placement == Placement::Register)
                return Location::of(part.physical);
            return Location::slot(_facts.classes[part.reg], _slots[part.reg]);
        }

        [[nodiscard]] std::span<Child const> children(uint32_t reg) const noexcept
        {
            return _children.of(reg);
        }

      private:
        Facts const& _facts;
        Runs<Child> _children;
        std::pmr::vector<uint32_t> _slots; ///< Per virtual register, its slot's offset, or None.
        uint32_t _slotCount = 0;
    };

    /// Whether @p reg is live at @p position.
    [[nodiscard]] bool isLiveAt(Facts const& facts, uint32_t reg, uint32_t position) noexcept
    {
        auto const ranges = facts.ranges.of(reg);
        auto const found =
            std::ranges::partition_point(ranges, [&](LiveRange range) { return range.end <= position; });
        return found != ranges.end() && found->start <= position;
    }

    /// The phases of one gap's moves, in the order they run.
    enum class Phase : uint8_t
    {
        EdgeIn,  ///< The moves of an edge into a block with one predecessor, at its first gap.
        Split,   ///< Where intervals change place, and the copies out of fixed registers.
        Reads,   ///< The copies into fixed registers, and the stores of values leaving a register at a read.
        EdgeOut, ///< The moves of an edge out of a block with one successor, before its terminator.
    };

    /// One move of a gap, before the gap's moves are sequentialized.
    struct GapMove
    {
        uint32_t gap = 0;
        Phase phase = Phase::Split;
        Move move {};
    };

    /// Whether @p instruction names a register: a register operand, or a memory operand (conservatively, with
    /// or without registers).
    [[nodiscard]] bool namesARegister(MachineFunction const& function, uint32_t instruction) noexcept
    {
        return std::ranges::any_of(function.operands(instruction), [](MachineOperand const& operand) {
            return std::holds_alternative<RegisterOperand>(operand)
                   || std::holds_alternative<MemoryOperand>(operand);
        });
    }

    /// Every gap's moves: where intervals change place, the fixed copies and the edges.
    class GapMoves
    {
      public:
        GapMoves(MachineFunction const& function,
                 Facts const& facts,
                 Layout const& layout,
                 Placements const& placements,
                 BlockOrder const& order,
                 std::pmr::memory_resource& memory):
            _function { function },
            _facts { facts },
            _layout { layout },
            _placements { placements },
            _order { order },
            _memory { memory },
            _moves { &memory }
        {
        }

        /// The moves, sorted by gap and phase; or InternalVerificationFailed for a critical edge, an edge
        /// out of a block whose terminator names a register, or a value without a place where it is live.
        [[nodiscard]] std::expected<std::pmr::vector<GapMove>, CompileError> collect() &&
        {
            if (!fixedCopies() || !edges())
                return refused();
            splits();
            std::ranges::stable_sort(
                _moves, {}, [](GapMove const& move) { return std::pair { move.gap, move.phase }; });
            return std::move(_moves);
        }

      private:
        void add(uint32_t gap, Phase phase, Location from, Location to)
        {
            if (!from.samePlace(to))
                _moves.push_back(
                    GapMove { .gap = gap, .phase = phase, .move = Move { .from = from, .to = to } });
        }

        [[nodiscard]] bool fixedCopies()
        {
            for (auto const& copy: _facts.copiesOut)
                if (auto const to = _placements.at(copy.reg, copy.gap); to.kind != LocationKind::None)
                    add(copy.gap, Phase::Split, Location::of(copy.fixed), to);
            for (auto const& copy: _facts.copiesIn)
            {
                auto const from = _placements.at(copy.reg, copy.gap);
                if (from.kind == LocationKind::None)
                    return false; // uncovered(allocator-invariant)
                add(copy.gap, Phase::Reads, from, Location::of(copy.fixed));
            }
            return true;
        }

        /// Where a register's consecutive children meet at a gap within a block and it is live on both sides,
        /// its value moves; not where a copy out of a fixed register writes it.
        void splits()
        {
            for (auto const reg: std::views::iota(uint32_t { 0 }, _function.virtualRegisterCount()))
            {
                auto const parts = _placements.children(reg);
                auto const copied = std::ranges::equal_range(_facts.copiesOut, reg, {}, &FixedCopy::reg);
                for (auto const index: std::views::iota(std::min<std::size_t>(1, parts.size()), parts.size()))
                {
                    auto const& before = parts[index - 1];
                    auto const& after = parts[index];
                    auto const boundary = after.from;
                    // A value an instruction only reads may leave its register at the read (Scan::evict()):
                    // it is stored in the gap before, and the instruction reads the register still.
                    auto const atRead = _layout.isInstruction(boundary);
                    auto const gap = atRead ? boundary - 1 : boundary;
                    // That store runs after the gap's splits and copies out, which may write the register.
                    auto const isCopiedOut =
                        !atRead && std::ranges::any_of(copied, [&](FixedCopy const& copy) {
                            return copy.gap == gap;
                        });
                    if (before.to == boundary && (atRead || _layout.isGap(boundary))
                        && isLiveAt(_facts, reg, boundary - 1) && isLiveAt(_facts, reg, boundary)
                        && !isCopiedOut)
                        add(gap,
                            atRead ? Phase::Reads : Phase::Split,
                            _placements.locationOf(before),
                            _placements.locationOf(after));
                }
            }
        }

        /// Per laid-out block, the values live into it, its parameters left out.
        [[nodiscard]] Runs<uint32_t> liveIns() const
        {
            auto const registers = _function.virtualRegisterCount();
            auto parameterOf = std::pmr::vector<uint32_t>(registers, None, &_memory);
            for (auto const slot: std::views::iota(uint32_t { 0 }, _layout.blockCount()))
                for (auto const parameter: _function.parameters(_layout.block(slot)))
                    parameterOf[std::to_underlying(parameter)] = slot;
            auto liveIns = std::pmr::vector<std::pair<uint32_t, uint32_t>> { &_memory };
            auto const starts = _layout.starts();
            for (auto const reg: std::views::iota(uint32_t { 0 }, registers))
                for (auto const& range: _facts.ranges.of(reg))
                {
                    auto const first = std::ranges::lower_bound(starts, range.start) - starts.begin();
                    auto const last = std::ranges::lower_bound(starts, range.end) - starts.begin();
                    for (auto const slot:
                         std::views::iota(static_cast<uint32_t>(first), static_cast<uint32_t>(last)))
                        if (parameterOf[reg] != slot)
                            liveIns.emplace_back(slot, reg);
                }
            std::ranges::sort(liveIns);
            auto runs = Runs<uint32_t> { _memory };
            runs.build(liveIns, _layout.blockCount());
            return runs;
        }

        /// Where the moves of an edge out of the block at layout slot @p slot go: before its terminator when
        /// it has one successor, else in the target's first gap, which then has one predecessor.
        [[nodiscard]] std::optional<std::pair<uint32_t, Phase>> edgeGap(
            uint32_t slot, machine::MachineEdge const& edge, std::span<uint32_t const> predecessors) const
        {
            auto const block = _layout.block(slot);
            if (_function.edges(block).size() == 1)
            {
                auto const instructions = _function.instructions(block);
                if (instructions.empty() || namesARegister(_function, instructions.back()))
                    return std::nullopt;
                return std::pair { _layout.terminator(slot) - 1, Phase::EdgeOut };
            }
            if (predecessors[std::to_underlying(edge.target)] != 1
                || _function.instructions(edge.target).empty())
                return std::nullopt; // a critical edge: the selector splits them
            return std::pair { _layout.start(*_order.position(edge.target)) + 1, Phase::EdgeIn };
        }

        /// Every edge's moves: each value live into its target from its place at the source's end to its
        /// place at the target's entry, and each argument to its parameter's place.
        [[nodiscard]] bool edges()
        {
            auto const live = liveIns();
            auto predecessors = std::pmr::vector<uint32_t>(_function.blockCount(), 0, &_memory);
            for (auto const slot: std::views::iota(uint32_t { 0 }, _layout.blockCount()))
                for (auto const& edge: _function.edges(_layout.block(slot)))
                    ++predecessors[std::to_underlying(edge.target)];
            for (auto const slot: std::views::iota(uint32_t { 0 }, _layout.blockCount()))
                for (auto const& edge: _function.edges(_layout.block(slot)))
                {
                    auto const gap = edgeGap(slot, edge, predecessors);
                    if (!gap || !edgeMoves(slot, edge, live, *gap))
                        return false;
                }
            return true;
        }

        [[nodiscard]] bool edgeMoves(uint32_t slot,
                                     machine::MachineEdge const& edge,
                                     Runs<uint32_t> const& live,
                                     std::pair<uint32_t, Phase> gap)
        {
            auto const end = _layout.terminator(slot);
            auto const target = *_order.position(edge.target);
            auto const entry = _layout.start(target);
            for (auto const reg: live.of(target))
            {
                auto const from = _placements.at(reg, end);
                auto const to = _placements.at(reg, entry);
                if (from.kind == LocationKind::None || to.kind == LocationKind::None)
                    return false; // uncovered(allocator-invariant)
                add(gap.first, gap.second, from, to);
            }
            auto const arguments = _function.arguments(edge);
            auto const parameters = _function.parameters(edge.target);
            for (auto const index: std::views::iota(std::size_t { 0 }, arguments.size()))
            {
                auto const to = _placements.at(std::to_underlying(parameters[index]), entry);
                auto const from = _placements.at(std::to_underlying(arguments[index]), end);
                if (from.kind == LocationKind::None || to.kind == LocationKind::None)
                    return false; // uncovered(allocator-invariant)
                add(gap.first, gap.second, from, to);
            }
            return true;
        }

        MachineFunction const& _function;
        Facts const& _facts;
        Layout const& _layout;
        Placements const& _placements;
        BlockOrder const& _order;
        std::pmr::memory_resource& _memory;
        std::pmr::vector<GapMove> _moves;
    };

    /// Per instruction, the moves before it: each gap's phases in order, each phase's parallel moves
    /// sequentialized through @p scratch.
    struct SequentialMoves
    {
        std::pmr::vector<std::pair<uint32_t, std::pmr::vector<Move>>> runs; ///< (instruction, its moves)
        bool usesScratch = false; ///< Whether a cycle was broken through the scratch slot.
    };

    [[nodiscard]] SequentialMoves sequentialize(std::span<GapMove const> moves,
                                                std::span<uint32_t const> instructionAt,
                                                Location scratch,
                                                std::pmr::memory_resource& memory)
    {
        auto result = SequentialMoves { .runs = decltype(SequentialMoves::runs) { &memory } };
        auto parallel = std::pmr::vector<Move> { &memory };
        for (auto const index: std::views::iota(std::size_t { 0 }, moves.size()))
        {
            auto const& move = moves[index];
            parallel.push_back(move.move);
            if (index + 1 != moves.size() && moves[index + 1].gap == move.gap
                && moves[index + 1].phase == move.phase)
                continue;
            auto const instruction = instructionAt[(move.gap + 1) / 2];
            Require(instruction != None); // every move's gap is followed by an instruction (Layout::isGap())
            if (result.runs.empty() || result.runs.back().first != instruction)
                result.runs.emplace_back(instruction, std::pmr::vector<Move> { &memory });
            for (auto const& sequential: sequentializeParallelMoves(parallel, scratch, memory))
            {
                result.usesScratch = result.usesScratch || sequential.from.samePlace(scratch)
                                     || sequential.to.samePlace(scratch);
                result.runs.back().second.push_back(sequential);
            }
            parallel.clear();
        }
        return result;
    }

    /// The frame from its bottom: the outgoing area, the spill slots, the saved registers, the caller's frame
    /// pointer, the return address.
    [[nodiscard]] FrameLayout frameOf(machine::ITarget const& target,
                                      RegisterSet written,
                                      uint64_t outgoing,
                                      uint64_t spillBytes)
    {
        auto const& host = target.conventionRow(machine::hostConvention(target.description()));
        auto const& internal = target.conventionRow(machine::Convention::WasmInternal);
        auto const stack = target.stackRule();
        auto saved = RegisterSet {};
        for (auto const registerClass: { RegisterClass::General, RegisterClass::Float })
            for (auto const number: std::views::iota(uint8_t { 0 }, machine::MaximumRegistersPerClass))
                if (auto const reg = PhysicalRegister { .registerClass = registerClass, .number = number };
                    written.contains(reg) && internal.calleeSaved.contains(reg))
                    saved = saved.with(reg);
        // The frame pointer's save counts before the frame is aligned: a prologue's push of it keeps every
        // call aligned.
        auto const above = stack.savedBytes(saved) + stack.framePointerBytes + stack.returnAddressBytes;
        auto const alignment = std::max<uint64_t>({ stack.alignment, host.stackAlignment, 1 });
        return FrameLayout {
            .totalBytes = alignUp(outgoing + spillBytes + above, alignment),
            .calleeSaved = saved,
            .outgoingBytes = outgoing,
            .spillOffset = outgoing,
            .spillBytes = spillBytes,
        };
    }

    /// Rewrites every register operand of @p function's laid-out instructions to its place: a fixed one to
    /// its fixed register, a read to where its value is in the gap before, a write to where it goes.
    [[nodiscard]] std::expected<void, CompileError> rewriteOperands(MachineFunction& function,
                                                                    machine::ITarget const& target,
                                                                    LiveIntervals const& live,
                                                                    Placements const& placements)
    {
        auto placed = true; // whether every operand found its value in a register: an allocator invariant
        auto const place = [&](RegisterName& name, uint32_t at) {
            if (name.kind != RegisterKind::Virtual)
                return;
            auto const location = placements.at(name.number, at);
            placed = placed && location.kind == LocationKind::Register;
            name = RegisterName::of(location.physicalRegister());
        };
        for (auto const index: std::views::iota(uint32_t { 0 }, function.instructionCount()))
        {
            auto const position = live.instructionPositions[index];
            if (position == intermediate::Unnumbered)
                continue; // a block linearize() left out: unreachable, never encoded
            auto const rows = target.constraints(function.instruction(index).opcode);
            auto const operands = function.operands(index);
            for (auto const operandIndex: std::views::iota(std::size_t { 0 }, operands.size()))
            {
                if (auto* address = std::get_if<MemoryOperand>(&operands[operandIndex]))
                {
                    place(address->base, position - 1);
                    place(address->index, position - 1);
                    continue;
                }
                auto* reg = std::get_if<RegisterOperand>(&operands[operandIndex]);
                if (reg == nullptr || reg->name.kind != RegisterKind::Virtual)
                    continue;
                auto const& row = rowOf(rows, operandIndex);
                auto const reads = reg->role == OperandRole::Use || reg->role == OperandRole::UseDef;
                if (row.fixed)
                    reg->name = RegisterName::of(
                        PhysicalRegister { .registerClass = row.registerClass, .number = *row.fixed });
                else
                    place(reg->name, reads ? position - 1 : position);
            }
        }
        if (placed)
            return {};
        return refused(); // uncovered(allocator-invariant)
    }
} // namespace

std::expected<FrameLayout, CompileError> LinearScanAllocator::allocate(
    MachineFunction& function,
    machine::ITarget const& target,
    uint64_t frameCost,
    CompileCancellation const& cancellation,
    std::pmr::memory_resource& memory) const
{
    if (auto verified = machine::verifyMachineFunction(function, target); !verified)
        return std::unexpected { verified.error() };
    auto const graph = machine::controlFlowGraphOf(function, memory);
    auto const dominators = intermediate::DominatorTree { graph, memory };
    auto const loops = intermediate::LoopNesting { graph, dominators, memory };
    auto order = machine::linearize(function, loops, memory);
    if (!order)
        return std::unexpected { order.error() }; // uncovered(allocator-invariant)
    auto const live = machine::computeLiveIntervals(function, *order, loops, _limits, cancellation, memory);
    if (!live)
        return std::unexpected { live.error() };
    auto const layout = Layout { *order, *live, memory };
    auto const facts = FactsReader { function, target, *live, layout, memory }.read();
    if (!facts)
        return std::unexpected { facts.error() };

    auto scan = Scan { function, target, *facts, layout, cancellation, memory };
    if (auto ran = scan.run(); !ran)
        return std::unexpected { ran.error() };

    auto const tooLarge = [&](uint64_t requested) {
        return std::unexpected { CompileError {
            .code = CompileErrorCode::FrameTooLarge,
            .requested = requested,
            .limit = machine::maximumFrameBytes(target.frameRule(), frameCost) } };
    };
    auto outgoing =
        uint64_t { target.conventionRow(machine::hostConvention(target.description())).shadowSpaceBytes };
    for (auto const& site: function.callSites())
        outgoing = std::max(outgoing, uint64_t { site.outgoingBytes });
    outgoing = alignUp(outgoing, SlotBytes);
    if (outgoing > std::numeric_limits<uint32_t>::max())
        return tooLarge(outgoing);
    auto const placements = Placements {
        scan.children(), *facts, function.virtualRegisterCount(), static_cast<uint32_t>(outgoing), memory
    };
    // The spill slots' offsets, and the scratch slot's past them, are 32 bits wide: past that no frame keeps
    // a frame rule anyway.
    auto const scratchOffset = outgoing + (uint64_t { placements.slotCount() } * SlotBytes);
    if (scratchOffset + SlotBytes > std::numeric_limits<uint32_t>::max())
        return tooLarge(scratchOffset + SlotBytes);
    auto const scratch = Location::slot(RegisterClass::General, static_cast<uint32_t>(scratchOffset));

    auto moves = GapMoves { function, *facts, layout, placements, *order, memory }.collect();
    if (!moves)
        return std::unexpected { moves.error() };
    auto instructionAt = std::pmr::vector<uint32_t>((layout.end() / 2) + 1, None, &memory);
    for (auto const index: std::views::iota(uint32_t { 0 }, function.instructionCount()))
        if (auto const position = live->instructionPositions[index]; position != intermediate::Unnumbered)
            instructionAt[position / 2] = index;
    auto const sequential = sequentialize(*moves, instructionAt, scratch, memory);

    auto written = facts->written;
    for (auto const& child: scan.children())
        if (child.placement == Placement::Register && child.from < child.to)
            written = written.with(child.physical);
    auto const spillBytes =
        (uint64_t { placements.slotCount() } + (sequential.usesScratch ? 1 : 0)) * SlotBytes;
    auto const frame = frameOf(target, written, outgoing, spillBytes);
    auto const& host = target.conventionRow(machine::hostConvention(target.description()));
    if (auto checked =
            machine::checkFrame(target.frameRule(), target.stackRule(), host, frameCost, frame).and_then([&] {
                return machine::checkOutgoingArea(function, frame);
            });
        !checked)
        return std::unexpected { checked.error() };

    for (auto const& [instruction, run]: sequential.runs)
        function.setMovesBefore(instruction, run);
    return rewriteOperands(function, target, *live, placements).transform([&] { return frame; });
}

} // namespace isoline::compiler::registerallocation
