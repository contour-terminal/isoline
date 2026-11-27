// SPDX-License-Identifier: Apache-2.0
#pragma once

// The assignment checker: an allocation's independent proof. It re-simulates an allocated function on its
// places (registers and slots), each holding the set of values known to be there, and checks that every
// operand reads the value its definition wrote: the definition the unallocated function reaches the read
// with. It knows nothing of how the allocator decided: only the function before allocation, the function
// after (the same instructions, every register operand physical, the moves before each instruction), and the
// frame. So a dropped clobber, a forgotten reload, a misordered parallel move or a wrong edge move shows as a
// read that finds another value.
//
// A place's set is a forward dataflow fact over the control-flow graph, joined by intersection and iterated
// to a fixpoint in reverse postorder; on an edge, the places holding an argument also hold the target's
// parameter. Every definition is a value, a definition in a loop the same value on every iteration: it is
// removed from every place before it is written again. A physical register read with no definition before it
// in its block reads what it held at the function's entry (in the entry block) and is not checked elsewhere.
// Test support (reused by Task 15 on whole modules), linked into no shipped binary.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <limits>
#include <memory_resource>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/compiler/machine/FrameRule.hpp>
#include <isoline/compiler/machine/Linearize.hpp>
#include <isoline/compiler/machine/MachineFunction.hpp>
#include <isoline/compiler/machine/MachineVerifier.hpp>
#include <isoline/compiler/machine/Target.hpp>

namespace isoline::compiler::registerallocation::testing
{

/// What the checker found wrong with an allocation.
enum class AssignmentFaultKind : uint8_t
{
    ShapeChanged,     ///< The allocated function's instructions or operands differ beyond their registers.
    VirtualLeft,      ///< A register operand still names a virtual register.
    BadPlace,         ///< A move or an operand names a place outside the register files or the spill area.
    WrongValue,       ///< A read finds another value than the definition the function reaches it with.
    ConstraintBroken, ///< The allocated function breaks its target's constraint rows
                      ///< (verifyMachineFunction()).
    FrameRefused,     ///< The frame breaks the target's frame or stack rule (checkFrame()).
    OutgoingTooSmall, ///< The frame's outgoing area does not cover a call site (checkOutgoingArea()).
};

/// A fault, and where: the instruction and operand, when it is at one.
struct AssignmentFault
{
    AssignmentFaultKind kind = AssignmentFaultKind::ShapeChanged; ///< What is wrong.
    std::optional<uint32_t> instruction {};                       ///< The instruction, by index.
    std::optional<uint32_t> operand {};                           ///< Its operand, by position.

    /// Whether both describe the same fault.
    [[nodiscard]] bool operator==(AssignmentFault const&) const noexcept = default;
};

namespace detail
{
    /// No value, no index.
    inline constexpr auto NoValue = std::numeric_limits<uint32_t>::max();

    /// The places of the simulation: per class its MaximumRegistersPerClass registers, then the spill slots.
    inline constexpr auto RegisterPlaces =
        static_cast<uint32_t>(machine::RegisterClassCount) * machine::MaximumRegistersPerClass;

    /// The checker's run over one allocation.
    class Checker
    {
      public:
        Checker(machine::MachineFunction const& before,
                machine::MachineFunction const& after,
                machine::FrameLayout const& frame,
                std::pmr::memory_resource& memory):
            _before { before },
            _after { after },
            _frame { frame },
            _memory { memory },
            _slots { static_cast<uint32_t>(frame.spillBytes / 8) },
            _parameterBase(before.blockCount(), NoValue, &memory),
            _firstDefinition(before.instructionCount(), NoValue, &memory),
            _outValue(before.virtualRegisterCount(), NoValue, &memory),
            _local(before.virtualRegisterCount(), NoValue, &memory),
            _localStamp(before.virtualRegisterCount(), NoValue, &memory),
            _physicalLocal(RegisterPlaces, NoValue, &memory),
            _physicalStamp(RegisterPlaces, NoValue, &memory)
        {
            // Value ids: the registers' entry values first, then each block's parameters and each written
            // operand, in block and instruction order.
            auto next = RegisterPlaces;
            for (auto const block: std::views::iota(uint32_t { 0 }, before.blockCount()))
            {
                _parameterBase[block] = next;
                for (auto const parameter: before.parameters(machine::BlockId { block }))
                    _outValue[std::to_underlying(parameter)] = next++;
                for (auto const instruction: before.instructions(machine::BlockId { block }))
                {
                    _firstDefinition[instruction] = next;
                    for (auto const& operand: before.operands(instruction))
                        if (auto const* reg = std::get_if<machine::RegisterOperand>(&operand);
                            reg != nullptr && reg->role != machine::OperandRole::Use)
                        {
                            if (reg->name.kind == machine::RegisterKind::Virtual)
                                _outValue[reg->name.number] = next;
                            ++next;
                        }
                }
            }
        }

        [[nodiscard]] std::expected<void, AssignmentFault> run()
        {
            auto const graph = machine::controlFlowGraphOf(_after, _memory);
            auto const order = machine::reversePostorderOf(graph, _memory);
            auto in = std::pmr::vector<std::optional<State>>(_after.blockCount(), &_memory);
            auto out = std::pmr::vector<std::optional<State>>(_after.blockCount(), &_memory);
            in[0] = entryState();
            // The fixpoint, unchecked: the sets only shrink once each block is reached.
            auto changed = true;
            while (changed)
            {
                changed = false;
                for (auto const block: order)
                {
                    auto merged = mergedInto(block, graph, out, in[0]);
                    if (!merged)
                        continue;
                    if (in[block] && *in[block] == *merged && out[block])
                        continue;
                    in[block] = *merged;
                    auto result = transfer(block, *merged, Checking::No);
                    if (!result)
                        return std::unexpected { result.error() };
                    if (!out[block] || *out[block] != *result)
                    {
                        out[block] = std::move(*result);
                        changed = true;
                    }
                }
            }
            for (auto const block: order)
                if (in[block])
                    if (auto result = transfer(block, *in[block], Checking::Yes); !result)
                        return std::unexpected { result.error() };
            return {};
        }

      private:
        /// Per place, the values known to be there, sorted.
        using State = std::pmr::vector<std::pmr::vector<uint32_t>>;

        enum class Checking : uint8_t
        {
            No,
            Yes,
        };

        [[nodiscard]] State emptyState() const
        {
            auto state = State { &_memory };
            state.resize(RegisterPlaces + _slots, std::pmr::vector<uint32_t> { &_memory });
            return state;
        }

        [[nodiscard]] State entryState() const
        {
            auto state = emptyState();
            for (auto const place: std::views::iota(uint32_t { 0 }, RegisterPlaces))
                state[place].push_back(place);
            return state;
        }

        /// The place @p location names, or NoValue outside the register files and the spill area.
        [[nodiscard]] uint32_t placeOf(machine::Location const& location) const noexcept
        {
            if (location.kind == machine::LocationKind::Register)
                return location.index < machine::MaximumRegistersPerClass
                           ? (static_cast<uint32_t>(location.registerClass)
                              * machine::MaximumRegistersPerClass)
                                 + location.index
                           : NoValue;
            if (location.kind != machine::LocationKind::Slot || location.index < _frame.spillOffset
                || location.index % 8 != 0 || location.index >= _frame.spillOffset + _frame.spillBytes)
                return NoValue;
            return RegisterPlaces + static_cast<uint32_t>((location.index - _frame.spillOffset) / 8);
        }

        [[nodiscard]] static uint32_t placeOf(machine::RegisterName const& name) noexcept
        {
            return (static_cast<uint32_t>(name.registerClass) * machine::MaximumRegistersPerClass)
                   + name.number;
        }

        static void add(std::pmr::vector<uint32_t>& values, uint32_t value)
        {
            auto const found = std::ranges::lower_bound(values, value);
            if (found == values.end() || *found != value)
                values.insert(found, value);
        }

        [[nodiscard]] static bool holds(std::pmr::vector<uint32_t> const& values, uint32_t value)
        {
            return std::ranges::binary_search(values, value);
        }

        static void kill(State& state, uint32_t value)
        {
            for (auto& values: state)
                if (auto const found = std::ranges::lower_bound(values, value);
                    found != values.end() && *found == value)
                    values.erase(found);
        }

        /// The state on entry to @p block: the intersection of its reached predecessors' states, each renamed
        /// along its edge; none while no predecessor is reached.
        [[nodiscard]] std::optional<State> mergedInto(uint32_t block,
                                                      intermediate::ControlFlowGraph const& graph,
                                                      std::span<std::optional<State> const> out,
                                                      std::optional<State> const& entry) const
        {
            auto merged = std::optional<State> {};
            if (block == 0)
                merged = entry;
            for (auto const predecessor: graph.predecessors(machine::BlockId { block }))
            {
                auto const index = std::to_underlying(predecessor);
                if (!out[index])
                    continue;
                for (auto const& edge: _after.edges(predecessor))
                {
                    if (std::to_underlying(edge.target) != block)
                        continue;
                    auto renamed = rename(*out[index], edge);
                    if (!merged)
                        merged = std::move(renamed);
                    else
                        for (auto const place: std::views::iota(std::size_t { 0 }, merged->size()))
                        {
                            auto kept = std::pmr::vector<uint32_t> { &_memory };
                            std::ranges::set_intersection(
                                (*merged)[place], renamed[place], std::back_inserter(kept));
                            (*merged)[place] = std::move(kept);
                        }
                }
            }
            return merged;
        }

        /// @p state across @p edge: the places holding an argument hold the target's parameter too.
        [[nodiscard]] State rename(State const& state, machine::MachineEdge const& edge) const
        {
            auto renamed = state;
            auto const parameters = _before.parameters(edge.target);
            auto const base = _parameterBase[std::to_underlying(edge.target)];
            for (auto const index: std::views::iota(uint32_t { 0 }, static_cast<uint32_t>(parameters.size())))
                kill(renamed, base + index);
            auto const arguments = _before.arguments(edge);
            for (auto const index: std::views::iota(uint32_t { 0 }, static_cast<uint32_t>(arguments.size())))
            {
                auto const value = _outValue[std::to_underlying(arguments[index])];
                for (auto const place: std::views::iota(std::size_t { 0 }, renamed.size()))
                    if (holds(state[place], value))
                        add(renamed[place], base + index);
            }
            return renamed;
        }

        /// The value a read of @p name in @p block expects; NoValue when unchecked.
        [[nodiscard]] uint32_t expected(machine::RegisterName const& name, uint32_t block) const noexcept
        {
            if (name.kind == machine::RegisterKind::Virtual)
            {
                if (_localStamp[name.number] == _epoch)
                    return _local[name.number];
                return _outValue[name.number];
            }
            auto const place = placeOf(name);
            if (_physicalStamp[place] == _epoch)
                return _physicalLocal[place];
            return block == 0 ? place : NoValue;
        }

        [[nodiscard]] static std::unexpected<AssignmentFault> fault(AssignmentFaultKind kind,
                                                                    std::optional<uint32_t> instruction = {},
                                                                    std::optional<uint32_t> operand = {})
        {
            return std::unexpected { AssignmentFault {
                .kind = kind, .instruction = instruction, .operand = operand } };
        }

        /// @p block's instructions over @p state: their moves, reads and writes.
        [[nodiscard]] std::expected<State, AssignmentFault> transfer(uint32_t block,
                                                                     State state,
                                                                     Checking checking)
        {
            auto const blockId = machine::BlockId { block };
            ++_epoch;
            _block = block;
            _checking = checking;
            auto const parameters = _before.parameters(blockId);
            for (auto const index: std::views::iota(uint32_t { 0 }, static_cast<uint32_t>(parameters.size())))
            {
                auto const reg = std::to_underlying(parameters[index]);
                _local[reg] = _parameterBase[block] + index;
                _localStamp[reg] = _epoch;
            }
            for (auto const instruction: _after.instructions(blockId))
            {
                for (auto const& move: _after.movesBefore(instruction))
                {
                    auto const from = placeOf(move.from);
                    auto const to = placeOf(move.to);
                    if (from == NoValue || to == NoValue)
                        return fault(AssignmentFaultKind::BadPlace, instruction);
                    state[to] = state[from];
                }
                if (auto stepped = step(instruction, state); !stepped)
                    return std::unexpected { stepped.error() };
            }
            return state;
        }

        /// Checks a read of @p was, allocated to @p now, by operand @p operand of @p instruction.
        [[nodiscard]] std::expected<void, AssignmentFault> read(State const& state,
                                                                machine::RegisterName const& was,
                                                                machine::RegisterName const& now,
                                                                uint32_t instruction,
                                                                uint32_t operand) const
        {
            if (was.kind == machine::RegisterKind::None && now.kind == machine::RegisterKind::None)
                return {};
            if (now.kind != machine::RegisterKind::Physical)
                return fault(AssignmentFaultKind::VirtualLeft, instruction, operand);
            if (now.number >= machine::MaximumRegistersPerClass)
                return fault(AssignmentFaultKind::BadPlace, instruction, operand);
            auto const value = expected(was, _block);
            if (_checking == Checking::Yes && value != NoValue && !holds(state[placeOf(now)], value))
                return fault(AssignmentFaultKind::WrongValue, instruction, operand);
            return {};
        }

        /// Writes @p value, the definition of @p was, to @p now.
        void write(State& state,
                   machine::RegisterName const& was,
                   machine::RegisterName const& now,
                   uint32_t value)
        {
            kill(state, value);
            state[placeOf(now)] = std::pmr::vector<uint32_t>({ value }, &_memory);
            if (was.kind == machine::RegisterKind::Virtual)
            {
                _local[was.number] = value;
                _localStamp[was.number] = _epoch;
            }
            else
            {
                _physicalLocal[placeOf(was)] = value;
                _physicalStamp[placeOf(was)] = _epoch;
            }
        }

        /// Checks a memory operand's registers (reads) and its unchanged addressing.
        [[nodiscard]] std::expected<void, AssignmentFault> readAddress(
            State const& state,
            machine::MemoryOperand const& was,
            machine::MachineOperand const& allocated,
            uint32_t instruction,
            uint32_t operand) const
        {
            auto const& now = std::get<machine::MemoryOperand>(allocated);
            if (was.scale != now.scale || was.displacement != now.displacement
                || was.extension != now.extension)
                return fault(AssignmentFaultKind::ShapeChanged, instruction, operand);
            return read(state, was.base, now.base, instruction, operand).and_then([&] {
                return read(state, was.index, now.index, instruction, operand);
            });
        }

        /// One instruction: its early definitions are written, then its uses read, then the rest written.
        [[nodiscard]] std::expected<void, AssignmentFault> step(uint32_t instruction, State& state)
        {
            auto const original = _before.operands(instruction);
            auto const allocated = _after.operands(instruction);
            if (_before.instruction(instruction).opcode != _after.instruction(instruction).opcode
                || original.size() != allocated.size())
                return fault(AssignmentFaultKind::ShapeChanged, instruction);
            auto const count = static_cast<uint32_t>(original.size());
            auto values = std::pmr::vector<uint32_t>(count, NoValue, &_memory);
            auto definition = _firstDefinition[instruction];
            for (auto const position: std::views::iota(uint32_t { 0 }, count))
            {
                if (original[position].index() != allocated[position].index())
                    return fault(AssignmentFaultKind::ShapeChanged, instruction, position);
                auto const* reg = std::get_if<machine::RegisterOperand>(&original[position]);
                if (reg == nullptr && !(original[position] == allocated[position])
                    && !std::holds_alternative<machine::MemoryOperand>(original[position]))
                    return fault(AssignmentFaultKind::ShapeChanged, instruction, position);
                if (reg != nullptr
                    && std::get<machine::RegisterOperand>(allocated[position]).role != reg->role)
                    return fault(AssignmentFaultKind::ShapeChanged, instruction, position);
                if (reg != nullptr && reg->role != machine::OperandRole::Use)
                    values[position] = definition++;
            }
            // Early definitions (which), or the others.
            auto const writeAll = [&](machine::OperandRole which) -> std::expected<void, AssignmentFault> {
                for (auto const position: std::views::iota(uint32_t { 0 }, count))
                {
                    auto const* reg = std::get_if<machine::RegisterOperand>(&original[position]);
                    if (reg == nullptr || reg->role == machine::OperandRole::Use
                        || (reg->role == machine::OperandRole::Early)
                               != (which == machine::OperandRole::Early))
                        continue;
                    auto const& now = std::get<machine::RegisterOperand>(allocated[position]).name;
                    if (now.kind != machine::RegisterKind::Physical)
                        return fault(AssignmentFaultKind::VirtualLeft, instruction, position);
                    if (now.number >= machine::MaximumRegistersPerClass)
                        return fault(AssignmentFaultKind::BadPlace, instruction, position);
                    write(state, reg->name, now, values[position]);
                }
                return {};
            };
            if (auto written = writeAll(machine::OperandRole::Early); !written)
                return written;
            for (auto const position: std::views::iota(uint32_t { 0 }, count))
            {
                auto checked = std::expected<void, AssignmentFault> {};
                if (auto const* address = std::get_if<machine::MemoryOperand>(&original[position]))
                    checked = readAddress(state, *address, allocated[position], instruction, position);
                else if (auto const* reg = std::get_if<machine::RegisterOperand>(&original[position]);
                         reg != nullptr
                         && (reg->role == machine::OperandRole::Use
                             || reg->role == machine::OperandRole::UseDef))
                    checked = read(state,
                                   reg->name,
                                   std::get<machine::RegisterOperand>(allocated[position]).name,
                                   instruction,
                                   position);
                if (!checked)
                    return checked;
            }
            return writeAll(machine::OperandRole::Def);
        }

        machine::MachineFunction const& _before;
        machine::MachineFunction const& _after;
        machine::FrameLayout const& _frame;
        std::pmr::memory_resource& _memory;
        uint32_t _slots = 0;
        uint32_t _epoch = 0; ///< Counts transfers: a local value is of the transfer that wrote it.
        uint32_t _block = 0; ///< The block of the transfer running.
        Checking _checking = Checking::No;           ///< Whether the transfer running checks its reads.
        std::pmr::vector<uint32_t> _parameterBase;   ///< Per block, its first parameter's value.
        std::pmr::vector<uint32_t> _firstDefinition; ///< Per instruction, its first written operand's value.
        std::pmr::vector<uint32_t> _outValue;        ///< Per virtual register, its last definition's value.
        std::pmr::vector<uint32_t> _local;           ///< Per virtual register, its value within the block.
        std::pmr::vector<uint32_t> _localStamp;      ///< Per virtual register, the transfer _local is of.
        std::pmr::vector<uint32_t> _physicalLocal;   ///< Per register place, its value within the block.
        std::pmr::vector<uint32_t> _physicalStamp; ///< Per register place, the transfer _physicalLocal is of.
    };
} // namespace detail

/// Checks that @p after is a correct allocation of @p before on @p target with @p frame.
/// @param before The function as selected, before allocation.
/// @param after The same function allocated (IRegisterAllocator::allocate()).
/// @param target The target.
/// @param frame The frame the allocator laid out.
/// @param frameCost The Wasm stack values the function charges.
/// @param memory What the simulation allocates from.
/// @return Nothing; or the first fault found.
[[nodiscard]] inline std::expected<void, AssignmentFault> checkAssignment(
    machine::MachineFunction const& before,
    machine::MachineFunction const& after,
    machine::ITarget const& target,
    machine::FrameLayout const& frame,
    uint64_t frameCost,
    std::pmr::memory_resource& memory)
{
    if (before.blockCount() != after.blockCount() || before.instructionCount() != after.instructionCount())
        return std::unexpected { AssignmentFault { .kind = AssignmentFaultKind::ShapeChanged } };
    if (!machine::verifyMachineFunction(after, target))
        return std::unexpected { AssignmentFault { .kind = AssignmentFaultKind::ConstraintBroken } };
    auto const& host = target.conventionRow(machine::hostConvention(target.description()));
    if (!machine::checkFrame(target.frameRule(), target.stackRule(), host, frameCost, frame))
        return std::unexpected { AssignmentFault { .kind = AssignmentFaultKind::FrameRefused } };
    if (!machine::checkOutgoingArea(after, frame))
        return std::unexpected { AssignmentFault { .kind = AssignmentFaultKind::OutgoingTooSmall } };
    return detail::Checker { before, after, frame, memory }.run();
}

} // namespace isoline::compiler::registerallocation::testing
