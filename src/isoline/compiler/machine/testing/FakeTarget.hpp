// SPDX-License-Identifier: Apache-2.0
#pragma once

// A target for testing the machine layer and the register allocator without an instruction set: two
// registers per class, a handful of opcodes whose rows exercise each kind of constraint (plain, tied,
// fixed, early clobber, memory-capable, a call that clobbers every register), a convention row and a
// frame rule. It selects and encodes nothing: tests build their machine functions by hand.

#include <core/Assert.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory_resource>
#include <optional>
#include <span>
#include <tuple>
#include <vector>

#include <isoline/compiler/intermediate/Dominators.hpp>
#include <isoline/compiler/machine/CallingConvention.hpp>
#include <isoline/compiler/machine/MachineFunction.hpp>
#include <isoline/compiler/machine/Target.hpp>

namespace isoline::compiler::machine::testing
{

/// FakeTarget's opcodes; each is the index of its row of FakeConstraintTable.
enum class FakeOpcode : uint8_t
{
    Constant,     ///< Def ← immediate.
    Copy,         ///< Def ← Use (General).
    CopyFloat,    ///< Def ← Use (Float).
    Add,          ///< Def ← Use + Use; the second operand may be memory.
    AddTied,      ///< UseDef += Use: a two-address form.
    Divide,       ///< Def (fixed g0) ← Use (fixed g0) / Use: x86-64's div in rax.
    Shift,        ///< UseDef <<= Use (fixed g1): x86-64's shift count in rcx.
    EarlyClobber, ///< Early Def ← Use: the result is written before the operand is read.
    Load,         ///< Def ← memory (a MemoryOperand).
    Call,         ///< A Wasm call: Defs fixed to every register FakeConvention does not preserve (all four).
    Jump,         ///< To a label.
    Branch,       ///< On a Use, to one of two labels.
    Return,       ///< Returns its Use (fixed g0).
    Store, ///< Stores its Use to memory (a MemoryOperand): a call's stack argument, a host call's scratch.
    Poll,  ///< A poll: its slow path calls the runtime, so Defs fixed to the host convention's caller-saved
           ///< set.
    CallIndirect,         ///< Calls through its Use, clobbering what Call does: x86-64's call r64.
    PassArguments,        ///< Reads two Uses fixed to g0 and g1: a call's argument registers.
    PassArgumentsSwapped, ///< PassArguments with its operands the other way round: g1, then g0.
    ConstantFloat,        ///< Def (Float) ← immediate.
};

/// The last FakeOpcode enumerator.
inline constexpr auto LastFakeOpcode = FakeOpcode::ConstantFloat;

/// FakeTarget's General registers.
inline constexpr auto FakeGeneralRegisters = std::to_array<PhysicalRegister>({
    { .registerClass = RegisterClass::General, .number = 0 },
    { .registerClass = RegisterClass::General, .number = 1 },
});

/// FakeTarget's Float registers.
inline constexpr auto FakeFloatRegisters = std::to_array<PhysicalRegister>({
    { .registerClass = RegisterClass::Float, .number = 0 },
    { .registerClass = RegisterClass::Float, .number = 1 },
});

/// FakeTarget's internal convention: both registers of each class carry arguments, the first of each a
/// result, the rest of the results in the caller's area; nothing is callee-saved.
inline constexpr auto FakeConvention = ConventionRow {
    .convention = Convention::WasmInternal,
    .integerArguments = FakeGeneralRegisters,
    .floatArguments = FakeFloatRegisters,
    .integerResults = std::span { FakeGeneralRegisters }.first(1),
    .floatResults = std::span { FakeFloatRegisters }.first(1),
    .calleeSaved = RegisterSet {},
    .slots = ArgumentSlots::Independent,
    .shadowSpaceBytes = 0,
    .stackSlotBytes = 8,
    .stackAlignment = 16,
    .aggregateResults = AggregateResults::SplitAcrossRegisters,
    .aggregateRegisterBytes = 0,
    .resultAreaPointer = std::nullopt,
    .pointerReturned = PointerReturned::No,
};

/// FakeTarget's internal convention with g1 callee-saved, for the tests of a frame's saved registers.
inline constexpr auto FakeSavingConvention = ConventionRow {
    .convention = Convention::WasmInternal,
    .integerArguments = FakeGeneralRegisters,
    .floatArguments = FakeFloatRegisters,
    .integerResults = std::span { FakeGeneralRegisters }.first(1),
    .floatResults = std::span { FakeFloatRegisters }.first(1),
    .calleeSaved = RegisterSet::of({ FakeGeneralRegisters[1] }),
    .slots = ArgumentSlots::Independent,
    .shadowSpaceBytes = 0,
    .stackSlotBytes = 8,
    .stackAlignment = 16,
    .aggregateResults = AggregateResults::SplitAcrossRegisters,
    .aggregateRegisterBytes = 0,
    .resultAreaPointer = std::nullopt,
    .pointerReturned = PointerReturned::No,
};

/// Which internal convention a FakeTarget has.
enum class FakeSaves : uint8_t
{
    Nothing,       ///< FakeConvention: nothing callee-saved.
    SecondGeneral, ///< FakeSavingConvention: g1 callee-saved.
};

/// FakeTarget's host convention, of System V's shape over its registers: g1 callee-saved, two words of
/// results in registers, a larger aggregate through a hidden pointer in g0, given back in g0.
inline constexpr auto FakeHostConvention = ConventionRow {
    .convention = Convention::SystemV,
    .integerArguments = FakeGeneralRegisters,
    .floatArguments = FakeFloatRegisters,
    .integerResults = FakeGeneralRegisters,
    .floatResults = FakeFloatRegisters,
    .calleeSaved = RegisterSet::of({ FakeGeneralRegisters[1] }),
    .slots = ArgumentSlots::Independent,
    .shadowSpaceBytes = 0,
    .stackSlotBytes = 8,
    .stackAlignment = 16,
    .aggregateResults = AggregateResults::HiddenPointerBeyond,
    .aggregateRegisterBytes = 16,
    .resultAreaPointer = std::nullopt,
    .pointerReturned = PointerReturned::Yes,
};

/// FakeTarget's stack rule: x86-64's.
inline constexpr auto FakeStackRule = StackRule {
    .alignment = 16, .returnAddressBytes = 8, .savedRegisterBytes = { 8, 16 }, .framePointerBytes = 8
};

/// FakeTarget's frame rule unless a test asks for another: x86-64's numbers.
inline constexpr auto FakeFrameRule = FrameRule { .bytesPerStackValue = 32, .frameOverhead = 256 };

namespace fake
{
    /// A General operand of @p role.
    constexpr OperandConstraint general(OperandRole role) noexcept
    {
        return OperandConstraint { .role = role, .registerClass = RegisterClass::General };
    }

    /// A General operand of @p role fixed to register @p number.
    constexpr OperandConstraint fixedGeneral(OperandRole role, uint8_t number) noexcept
    {
        return OperandConstraint { .role = role, .registerClass = RegisterClass::General, .fixed = number };
    }

    /// A Float operand of @p role fixed to register @p number.
    constexpr OperandConstraint fixedFloat(OperandRole role, uint8_t number) noexcept
    {
        return OperandConstraint { .role = role, .registerClass = RegisterClass::Float, .fixed = number };
    }

    inline constexpr auto Constant = std::to_array({ general(OperandRole::Def), general(OperandRole::Use) });
    inline constexpr auto Copy = std::to_array({ general(OperandRole::Def), general(OperandRole::Use) });
    inline constexpr auto CopyFloat = std::to_array({
        OperandConstraint { .role = OperandRole::Def, .registerClass = RegisterClass::Float },
        OperandConstraint { .role = OperandRole::Use, .registerClass = RegisterClass::Float },
    });
    inline constexpr auto Add = std::to_array({
        general(OperandRole::Def),
        general(OperandRole::Use),
        OperandConstraint {
            .role = OperandRole::Use, .registerClass = RegisterClass::General, .memory = AllowsMemory::Yes },
    });
    inline constexpr auto AddTied =
        std::to_array({ general(OperandRole::UseDef), general(OperandRole::Use) });
    inline constexpr auto Divide = std::to_array({
        fixedGeneral(OperandRole::Def, 0),
        fixedGeneral(OperandRole::Use, 0),
        general(OperandRole::Use),
    });
    inline constexpr auto Shift = std::to_array({
        general(OperandRole::UseDef),
        fixedGeneral(OperandRole::Use, 1),
    });
    inline constexpr auto EarlyClobber =
        std::to_array({ general(OperandRole::Early), general(OperandRole::Use) });
    inline constexpr auto Load = std::to_array({
        general(OperandRole::Def),
        OperandConstraint {
            .role = OperandRole::Use, .registerClass = RegisterClass::General, .memory = AllowsMemory::Yes },
    });
    /// How many of FakeTarget's registers a call under @p row clobbers: those it does not preserve.
    constexpr std::size_t clobberCount(ConventionRow const& row) noexcept
    {
        auto count = std::size_t { 0 };
        for (auto const& files: { std::span { FakeGeneralRegisters }, std::span { FakeFloatRegisters } })
            for (auto const reg: files)
                if (!row.calleeSaved.contains(reg))
                    ++count;
        return count;
    }

    /// The rows of a call under @p row: one Def fixed to each register it clobbers, General then Float, after
    /// @p Lead leading rows of @p lead. Derived from the convention row, never listed by hand.
    template <std::size_t Count, std::size_t Lead = 0>
    constexpr std::array<OperandConstraint, Count + Lead> clobberRows(
        ConventionRow const& row, std::array<OperandConstraint, Lead> const& lead = {}) noexcept
    {
        auto rows = std::array<OperandConstraint, Count + Lead> {};
        auto next = std::size_t { 0 };
        for (auto const& constraint: lead)
            rows[next++] = constraint;
        for (auto const& files: { std::span { FakeGeneralRegisters }, std::span { FakeFloatRegisters } })
            for (auto const reg: files)
                if (!row.calleeSaved.contains(reg))
                    rows[next++] = OperandConstraint { .role = OperandRole::Def,
                                                       .registerClass = reg.registerClass,
                                                       .fixed = reg.number };
        return rows;
    }

    inline constexpr auto Call = clobberRows<clobberCount(FakeConvention)>(FakeConvention);
    inline constexpr auto Jump = std::to_array({ general(OperandRole::Use) });
    inline constexpr auto Branch = std::to_array({
        general(OperandRole::Use),
        general(OperandRole::Use),
        general(OperandRole::Use),
    });
    inline constexpr auto Return = std::to_array({ fixedGeneral(OperandRole::Use, 0) });
    inline constexpr auto Store = std::to_array({
        general(OperandRole::Use),
        OperandConstraint {
            .role = OperandRole::Use, .registerClass = RegisterClass::General, .memory = AllowsMemory::Yes },
    });
    inline constexpr auto Poll = clobberRows<clobberCount(FakeHostConvention)>(FakeHostConvention);
    inline constexpr auto CallIndirect =
        clobberRows<clobberCount(FakeConvention), 1>(FakeConvention, { general(OperandRole::Use) });
    inline constexpr auto PassArguments =
        std::to_array({ fixedGeneral(OperandRole::Use, 0), fixedGeneral(OperandRole::Use, 1) });
    inline constexpr auto PassArgumentsSwapped =
        std::to_array({ fixedGeneral(OperandRole::Use, 1), fixedGeneral(OperandRole::Use, 0) });
    inline constexpr auto ConstantFloat = std::to_array({
        OperandConstraint { .role = OperandRole::Def, .registerClass = RegisterClass::Float },
        general(OperandRole::Use),
    });
} // namespace fake

/// One FakeOpcode's operand constraints.
struct FakeConstraintRow
{
    FakeOpcode opcode;                           ///< The opcode; equal to the row's index.
    std::span<OperandConstraint const> operands; ///< Its operands' constraints, by position.
};

/// Every FakeOpcode's constraints, in enumerator order.
inline constexpr auto FakeConstraintTable = std::to_array<FakeConstraintRow>({
    { .opcode = FakeOpcode::Constant, .operands = fake::Constant },
    { .opcode = FakeOpcode::Copy, .operands = fake::Copy },
    { .opcode = FakeOpcode::CopyFloat, .operands = fake::CopyFloat },
    { .opcode = FakeOpcode::Add, .operands = fake::Add },
    { .opcode = FakeOpcode::AddTied, .operands = fake::AddTied },
    { .opcode = FakeOpcode::Divide, .operands = fake::Divide },
    { .opcode = FakeOpcode::Shift, .operands = fake::Shift },
    { .opcode = FakeOpcode::EarlyClobber, .operands = fake::EarlyClobber },
    { .opcode = FakeOpcode::Load, .operands = fake::Load },
    { .opcode = FakeOpcode::Call, .operands = fake::Call },
    { .opcode = FakeOpcode::Jump, .operands = fake::Jump },
    { .opcode = FakeOpcode::Branch, .operands = fake::Branch },
    { .opcode = FakeOpcode::Return, .operands = fake::Return },
    { .opcode = FakeOpcode::Store, .operands = fake::Store },
    { .opcode = FakeOpcode::Poll, .operands = fake::Poll },
    { .opcode = FakeOpcode::CallIndirect, .operands = fake::CallIndirect },
    { .opcode = FakeOpcode::PassArguments, .operands = fake::PassArguments },
    { .opcode = FakeOpcode::PassArgumentsSwapped, .operands = fake::PassArgumentsSwapped },
    { .opcode = FakeOpcode::ConstantFloat, .operands = fake::ConstantFloat },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: FakeConstraintTable)
        {
            if (static_cast<std::size_t>(row.opcode) != index)
                return false;
            ++index;
        }
        return index == static_cast<std::size_t>(LastFakeOpcode) + 1;
    }(),
    "FakeConstraintTable must hold one row per FakeOpcode, at the index of its enumerator");

/// @p opcode as a machine opcode.
[[nodiscard]] constexpr uint16_t opcodeOf(FakeOpcode opcode) noexcept
{
    return static_cast<uint16_t>(opcode);
}

/// A register operand reading @p reg of @p function.
[[nodiscard]] inline MachineOperand use(MachineFunction const& function, VirtualRegister reg)
{
    return RegisterOperand { .name = function.name(reg), .role = OperandRole::Use };
}

/// A register operand writing @p reg of @p function.
[[nodiscard]] inline MachineOperand def(MachineFunction const& function, VirtualRegister reg)
{
    return RegisterOperand { .name = function.name(reg), .role = OperandRole::Def };
}

/// A register operand reading and writing @p reg of @p function (tied).
[[nodiscard]] inline MachineOperand useDef(MachineFunction const& function, VirtualRegister reg)
{
    return RegisterOperand { .name = function.name(reg), .role = OperandRole::UseDef };
}

/// A register operand writing @p reg of @p function before the uses are read.
[[nodiscard]] inline MachineOperand early(MachineFunction const& function, VirtualRegister reg)
{
    return RegisterOperand { .name = function.name(reg), .role = OperandRole::Early };
}

/// A label operand naming @p block.
[[nodiscard]] inline MachineOperand label(BlockId block)
{
    return LabelOperand { .block = block };
}

/// Ends @p block with @p edges and a terminator that names their targets: Return (of @p value, if given)
/// for none, Jump for one, and Branch on @p value (Require: given) to the first two for two.
inline void terminate(MachineFunction& function,
                      BlockId block,
                      std::span<EdgeTo const> edges,
                      std::optional<VirtualRegister> value = std::nullopt)
{
    Require(edges.size() <= 2);
    function.setEdges(block, edges);
    auto operands = std::vector<MachineOperand> {};
    if (edges.empty())
    {
        if (value)
            operands.push_back(use(function, *value));
        std::ignore = function.append(block, opcodeOf(FakeOpcode::Return), operands);
        return;
    }
    if (edges.size() == 2)
    {
        Require(value.has_value());
        operands.push_back(use(function, *value));
    }
    for (auto const& edge: edges)
        operands.push_back(label(edge.target));
    std::ignore =
        function.append(block, opcodeOf(edges.size() == 1 ? FakeOpcode::Jump : FakeOpcode::Branch), operands);
}

/// The loops of a machine function: its control-flow graph, dominator tree and loop nesting, built in
/// that order, as linearize() and computeLiveIntervals() take them.
struct LoopsOf
{
    /// The loops of @p function.
    LoopsOf(MachineFunction const& function, std::pmr::memory_resource& memory):
        graph { controlFlowGraphOf(function, memory) },
        dominators { graph, memory },
        loops { graph, dominators, memory }
    {
    }

    intermediate::ControlFlowGraph graph;   ///< The control-flow graph.
    intermediate::DominatorTree dominators; ///< Its dominator tree.
    intermediate::LoopNesting loops;        ///< Its loops.
};

/// The fake target: two registers per class and FakeConstraintTable's opcodes.
class FakeTarget final: public ITarget
{
  public:
    /// A fake target bounding frames by @p rule.
    /// @param rule Its frame rule; a test of the frame bound passes a tighter one.
    /// @param saves Which internal convention it has.
    explicit FakeTarget(FrameRule rule = FakeFrameRule, FakeSaves saves = FakeSaves::Nothing) noexcept:
        _rule { rule }, _saves { saves }
    {
    }

    [[nodiscard]] TargetDescription const& description() const noexcept override { return _description; }

    [[nodiscard]] FrameRule frameRule() const noexcept override { return _rule; }

    [[nodiscard]] StackRule stackRule() const noexcept override { return FakeStackRule; }

    [[nodiscard]] ConventionRow const& conventionRow(Convention convention) const override
    {
        Require(convention == Convention::WasmInternal || convention == Convention::SystemV);
        if (convention != Convention::WasmInternal)
            return FakeHostConvention;
        return _saves == FakeSaves::Nothing ? FakeConvention : FakeSavingConvention;
    }

    [[nodiscard]] std::span<PhysicalRegister const> allocatable(
        RegisterClass registerClass) const noexcept override
    {
        return registerClass == RegisterClass::General ? std::span { FakeGeneralRegisters }
                                                       : std::span { FakeFloatRegisters };
    }

    [[nodiscard]] std::span<OperandConstraint const> constraints(uint16_t machineOpcode) const override
    {
        Require(machineOpcode < FakeConstraintTable.size());
        return FakeConstraintTable[machineOpcode].operands;
    }

    /// Selects nothing: an empty function. Tests build machine functions by hand.
    [[nodiscard]] std::expected<MachineFunction, CompileError> select(
        intermediate::Function const& /*function*/, SelectionContext const& context) const override
    {
        return MachineFunction { context.memory };
    }

    /// Encodes nothing.
    [[nodiscard]] std::expected<void, CompileError> encode(MachineFunction const& /*function*/,
                                                           FrameLayout const& /*frame*/,
                                                           CodeBuffer& /*buffer*/) const override
    {
        return {};
    }

    /// Emits nothing.
    void emitEntryAdapter(binary::FuncType const& /*type*/, CodeBuffer& /*buffer*/) const override {}

    /// Emits nothing.
    void emitEntryTrampoline(CodeBuffer& /*buffer*/) const override {}

  private:
    /// A placeholder (x86-64, System V): the fake target generates for no machine.
    TargetDescription _description {};
    FrameRule _rule;
    FakeSaves _saves;
};

} // namespace isoline::compiler::machine::testing
