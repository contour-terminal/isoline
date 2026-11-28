// SPDX-License-Identifier: Apache-2.0
#pragma once

// x86-64's machine opcodes: what a MachineInstruction's opcode names on this target. The first MnemonicCount
// are the encoding table's rows, one instruction form each, whose operand constraints follow from the row:
// its explicit operands in its form's order, then the fixed registers it reads or writes without naming them
// (div's rdx:rax, a shift's cl), each a register operand fixed to its register. The stack pointer a push,
// pop, call or ret moves is no operand: no instruction of a function's body moves it (FrameLayout).
//
// The rest are pseudo-instructions: what the encoder expands into encoding rows once registers and the frame
// are known (Lowering.hpp), because they need code out of line (a trap site, a poll's slow path, a call's
// host path, a jump table) or the frame (an incoming stack argument, the return's epilogue). A call's rows
// hold one position per argument register of its convention, in the convention's order (a call that passes
// fewer puts an Immediate in the others, a position whose row is not read), then a definition fixed to every
// register the convention does not preserve, derived from its row (forEachCallerSaved()): a result is the
// definition at its register's position, every other one a clobber. So every call's constraints are static
// rows, whatever it passes.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include <isoline/compiler/machine/CallingConvention.hpp>
#include <isoline/compiler/machine/Registers.hpp>
#include <isoline/compiler/machine/Target.hpp>
#include <isoline/compiler/x86_64/Encoder.hpp>
#include <isoline/compiler/x86_64/Registers.hpp>

namespace isoline::compiler::x86_64
{

/// x86-64's pseudo-instructions, each expanded by the encoder (Lowering.hpp). Their operands, by position,
/// are PseudoConstraints' rows.
enum class Pseudo : uint8_t
{
    Branch,              ///< Terminator: on a General Use, to its first label if non-zero, else its second.
    CompareBranch,       ///< Terminator: cmp (an Immediate naming the form) of a Use and a Use or an
                         ///< Immediate, then on a condition (an Immediate) to the first label, else the
                         ///< second.
    Switch,              ///< Terminator: on an I32 Use, through a jump table to one of its labels (the
                         ///< fallback first), two Early scratch registers, the count, the Spectre guard.
    Return,              ///< Terminator: the epilogue and ret, reading the internal result registers.
    Trap,                ///< Terminator: jumps to a trap site of its code (an Immediate).
    TrapIf,              ///< On a condition (an Immediate), jumps to a trap site of its code.
    TrapIfSubject,       ///< TrapIf, the site also storing its Use as the trap's subject, zero-extended.
    TrapIfPending,       ///< On a condition, leaves through the trap exit: a stub left its Abrupt pending.
    LoadIncomingGeneral, ///< Defines a General register from an incoming stack argument (its offset).
    LoadIncomingFloat,   ///< Defines a Float register from an incoming stack argument (its offset).
    Poll,                ///< The poll fast path on its countdown (a UseDef), the slow path out of line.
    PollStored,          ///< The poll fast path on the stored countdown, the slow path out of line.
    CallDirect,          ///< A call of a function of the module, by its code index (an Immediate).
    CallDispatched,      ///< A call through a dispatch row: compiled code, or the host-call path.
    CallStub,            ///< A call of a runtime stub, by its row (an Immediate), under the host
                         ///< convention.
    RestoreInstanceContext, ///< r14 from its Use, after a call that may have set another instance's.
    /// The function's entry, first in its entry block, read by the prologue (enterFrame()), which expands it
    /// before the frame is allocated: whether the FunctionEntry poll runs (an Immediate, 0 or 1), whether the
    /// frame is charged (0 or 1), and its cost. It emits nothing where it stands.
    Entry,
};

/// The last Pseudo enumerator.
inline constexpr auto LastPseudo = Pseudo::Entry;

/// How many pseudo-instructions there are.
inline constexpr auto PseudoCount = static_cast<std::size_t>(LastPseudo) + 1;

/// How many machine opcodes x86-64 has: every encoding row, then every pseudo-instruction.
inline constexpr auto MachineOpcodeCount = MnemonicCount + PseudoCount;

/// The machine opcode of @p mnemonic.
[[nodiscard]] constexpr uint16_t opcodeOf(Mnemonic mnemonic) noexcept
{
    return static_cast<uint16_t>(mnemonic);
}

/// The machine opcode of @p pseudo.
[[nodiscard]] constexpr uint16_t opcodeOf(Pseudo pseudo) noexcept
{
    return static_cast<uint16_t>(MnemonicCount + static_cast<std::size_t>(pseudo));
}

/// The encoding row @p opcode names; none for a pseudo-instruction.
[[nodiscard]] constexpr std::optional<Mnemonic> mnemonicOf(uint16_t opcode) noexcept
{
    if (opcode >= MnemonicCount)
        return std::nullopt;
    return static_cast<Mnemonic>(opcode);
}

/// The pseudo-instruction @p opcode names; none for an encoding row or past the last opcode.
[[nodiscard]] constexpr std::optional<Pseudo> pseudoOf(uint16_t opcode) noexcept
{
    if (opcode < MnemonicCount || opcode >= MachineOpcodeCount)
        return std::nullopt;
    return static_cast<Pseudo>(opcode - MnemonicCount);
}

/// The most operands an encoding row's machine instruction has: three explicit, two implicit.
inline constexpr auto MaximumMnemonicOperands = std::size_t { 5 };

/// An encoding row's operand constraints, by machine operand position.
struct MnemonicConstraints
{
    std::array<machine::OperandConstraint, MaximumMnemonicOperands> rows {}; ///< The first count used.
    uint8_t count = 0;                                                       ///< How many operands.
};

/// The register class of an explicit operand of @p kind.
[[nodiscard]] constexpr machine::RegisterClass registerClassOf(OperandKind kind) noexcept
{
    return kind == OperandKind::Vector ? machine::RegisterClass::Float : machine::RegisterClass::General;
}

/// The constraints of @p row's machine instruction: each explicit register its role and class, a memory
/// operand a memory-capable Use, an immediate, condition or label a row that is not read; then each implicit
/// register but the stack pointer, fixed to it.
[[nodiscard]] constexpr MnemonicConstraints constraintsOf(EncodingRow const& row) noexcept
{
    auto constraints = MnemonicConstraints {};
    for (auto const& slot: row.slots)
    {
        if (slot.placement == Placement::Implicit && slot.fixed == Gpr::Rsp)
            continue;
        auto& constraint = constraints.rows[constraints.count++];
        if (slot.placement == Placement::Implicit)
            constraint = machine::OperandConstraint { .role = slot.role,
                                                      .registerClass = machine::RegisterClass::General,
                                                      .fixed = static_cast<uint8_t>(*slot.fixed) };
        else if (isRegisterKind(slot.kind))
            constraint =
                machine::OperandConstraint { .role = slot.role, .registerClass = registerClassOf(slot.kind) };
        else if (isMemoryKind(slot.kind))
            constraint = machine::OperandConstraint { .role = machine::OperandRole::Use,
                                                      .registerClass = machine::RegisterClass::General,
                                                      .memory = machine::AllowsMemory::Yes };
    }
    return constraints;
}

/// Every encoding row's constraints, in Mnemonic order.
inline constexpr auto MnemonicConstraintTable = [] {
    auto table = std::array<MnemonicConstraints, MnemonicCount> {};
    for (auto const& row: EncodingTable)
        table[static_cast<std::size_t>(row.mnemonic)] = constraintsOf(row);
    return table;
}();

/// The constraints of @p mnemonic's machine instruction.
[[nodiscard]] constexpr std::span<machine::OperandConstraint const> constraintsOf(Mnemonic mnemonic) noexcept
{
    auto const& constraints = MnemonicConstraintTable[static_cast<std::size_t>(mnemonic)];
    return std::span { constraints.rows }.first(constraints.count);
}

namespace detail
{
    /// A row at a position that is not read: an Immediate's or a label's.
    inline constexpr auto NotRead = machine::OperandConstraint {};

    /// A General operand of @p role.
    constexpr machine::OperandConstraint general(machine::OperandRole role) noexcept
    {
        return machine::OperandConstraint { .role = role, .registerClass = machine::RegisterClass::General };
    }

    /// An operand of @p role fixed to @p reg.
    constexpr machine::OperandConstraint fixedTo(machine::OperandRole role,
                                                 machine::PhysicalRegister reg) noexcept
    {
        return machine::OperandConstraint { .role = role,
                                            .registerClass = reg.registerClass,
                                            .fixed = reg.number };
    }

    /// How many registers a call under @p row clobbers.
    constexpr std::size_t clobberCount(machine::ConventionRow const& row) noexcept
    {
        auto count = std::size_t { 0 };
        machine::forEachCallerSaved(
            row, AllocatableGeneral, AllocatableFloat, [&](machine::PhysicalRegister) { ++count; });
        return count;
    }

    /// The rows of an instruction of @p lead leading rows, then a fixed Use per argument register of @p
    /// arguments (General, then Float when @p floats), then a fixed Def per register @p clobbers does not
    /// preserve.
    template <std::size_t Count, std::size_t Lead>
    constexpr std::array<machine::OperandConstraint, Count> callRows(
        std::array<machine::OperandConstraint, Lead> const& lead,
        machine::ConventionRow const* arguments,
        machine::ConventionRow const& clobbers) noexcept
    {
        auto rows = std::array<machine::OperandConstraint, Count> {};
        auto next = std::size_t { 0 };
        for (auto const& constraint: lead)
            rows[next++] = constraint;
        if (arguments != nullptr)
        {
            for (auto const reg: arguments->integerArguments)
                rows[next++] = fixedTo(machine::OperandRole::Use, reg);
            for (auto const reg: arguments->floatArguments)
                rows[next++] = fixedTo(machine::OperandRole::Use, reg);
        }
        machine::forEachCallerSaved(
            clobbers, AllocatableGeneral, AllocatableFloat, [&](machine::PhysicalRegister reg) {
                rows[next++] = fixedTo(machine::OperandRole::Def, reg);
            });
        return rows;
    }

    /// How many argument registers @p row has, both classes.
    constexpr std::size_t argumentCount(machine::ConventionRow const& row) noexcept
    {
        return row.integerArguments.size() + row.floatArguments.size();
    }
} // namespace detail

/// Where a call's operands lie, by position: the leading operands, then one per argument register of its
/// convention (General, then Float), then one per register it clobbers.
struct CallLayout
{
    std::size_t lead = 0;      ///< How many leading operands (the target, the row, the address).
    std::size_t arguments = 0; ///< How many argument positions follow them.
    std::size_t clobbers = 0;  ///< How many clobber positions follow those.

    /// The position of argument register @p index (General first, then Float).
    [[nodiscard]] constexpr std::size_t argument(std::size_t index) const noexcept { return lead + index; }

    /// The position of the clobber with index @p index.
    [[nodiscard]] constexpr std::size_t clobber(std::size_t index) const noexcept
    {
        return lead + arguments + index;
    }

    /// How many operands the call has before any trailing Immediates.
    [[nodiscard]] constexpr std::size_t size() const noexcept { return lead + arguments + clobbers; }
};

/// The position of @p reg among the registers a call under @p row clobbers; none when it preserves it.
[[nodiscard]] constexpr std::optional<std::size_t> clobberIndexOf(machine::ConventionRow const& row,
                                                                  machine::PhysicalRegister reg) noexcept
{
    auto index = std::size_t { 0 };
    auto found = std::optional<std::size_t> {};
    machine::forEachCallerSaved(
        row, AllocatableGeneral, AllocatableFloat, [&](machine::PhysicalRegister clobbered) {
            if (clobbered == reg)
                found = index;
            ++index;
        });
    return found;
}

/// The pseudo-instructions' rows when the host convention is @p Host.
template <machine::ConventionRow const& Host>
struct PseudoConstraints
{
    /// A Wasm call's layout: its leading operands, the internal convention's arguments and clobbers.
    static constexpr auto wasmCall(std::size_t lead) noexcept
    {
        return CallLayout { .lead = lead,
                            .arguments = detail::argumentCount(WasmInternalConvention),
                            .clobbers = detail::clobberCount(WasmInternalConvention) };
    }

    /// CallDirect's layout: the code index, then a Wasm call's.
    static constexpr auto DirectLayout = wasmCall(1);
    /// CallDispatched's layout: the row (r10) and the function address (rax), then a Wasm call's.
    static constexpr auto DispatchedLayout = wasmCall(2);
    /// CallStub's layout: the stub, the host convention's General arguments, its clobbers.
    static constexpr auto StubLayout = CallLayout { .lead = 1,
                                                    .arguments = Host.integerArguments.size(),
                                                    .clobbers = detail::clobberCount(Host) };
    /// Poll's layout: its countdown and site, the host convention's clobbers.
    static constexpr auto PollLayout =
        CallLayout { .lead = 2, .arguments = 0, .clobbers = detail::clobberCount(Host) };
    /// PollStored's layout: its site, the host convention's clobbers.
    static constexpr auto PollStoredLayout =
        CallLayout { .lead = 1, .arguments = 0, .clobbers = detail::clobberCount(Host) };

    static constexpr auto Branch =
        std::to_array({ detail::general(machine::OperandRole::Use), detail::NotRead, detail::NotRead });
    static constexpr auto CompareBranch = std::to_array({ detail::NotRead,
                                                          detail::NotRead,
                                                          detail::general(machine::OperandRole::Use),
                                                          detail::general(machine::OperandRole::Use),
                                                          detail::NotRead,
                                                          detail::NotRead });
    static constexpr auto Switch = std::to_array({ detail::general(machine::OperandRole::Use),
                                                   detail::general(machine::OperandRole::Early),
                                                   detail::general(machine::OperandRole::Early),
                                                   detail::NotRead,
                                                   detail::NotRead,
                                                   detail::NotRead });
    static constexpr auto Return = std::to_array({
        detail::fixedTo(machine::OperandRole::Use, WasmInternalConvention.integerResults[0]),
        detail::fixedTo(machine::OperandRole::Use, WasmInternalConvention.integerResults[1]),
        detail::fixedTo(machine::OperandRole::Use, WasmInternalConvention.floatResults[0]),
        detail::fixedTo(machine::OperandRole::Use, WasmInternalConvention.floatResults[1]),
    });
    static constexpr auto Trap = std::to_array({ detail::NotRead });
    static constexpr auto TrapIf = std::to_array({ detail::NotRead, detail::NotRead });
    static constexpr auto TrapIfSubject =
        std::to_array({ detail::NotRead, detail::NotRead, detail::general(machine::OperandRole::Use) });
    static constexpr auto TrapIfPending = std::to_array({ detail::NotRead });
    static constexpr auto LoadIncomingGeneral =
        std::to_array({ detail::general(machine::OperandRole::Def), detail::NotRead });
    static constexpr auto LoadIncomingFloat =
        std::to_array({ machine::OperandConstraint { .role = machine::OperandRole::Def,
                                                     .registerClass = machine::RegisterClass::Float },
                        detail::NotRead });
    static constexpr auto Poll = detail::callRows<PollLayout.size()>(
        std::array { detail::general(machine::OperandRole::UseDef), detail::NotRead }, nullptr, Host);
    static constexpr auto PollStored =
        detail::callRows<PollStoredLayout.size()>(std::array { detail::NotRead }, nullptr, Host);
    static constexpr auto CallDirect = detail::callRows<DirectLayout.size()>(
        std::array { detail::NotRead }, &WasmInternalConvention, WasmInternalConvention);
    static constexpr auto CallDispatched = detail::callRows<DispatchedLayout.size()>(
        std::array { detail::fixedTo(machine::OperandRole::Use, physical(Gpr::R10)),
                     detail::fixedTo(machine::OperandRole::Use, physical(Gpr::Rax)) },
        &WasmInternalConvention,
        WasmInternalConvention);
    static constexpr auto CallStub = [] {
        auto rows = std::array<machine::OperandConstraint, StubLayout.size()> {};
        auto next = std::size_t { 0 };
        rows[next++] = detail::NotRead;
        for (auto const reg: Host.integerArguments)
            rows[next++] = detail::fixedTo(machine::OperandRole::Use, reg);
        machine::forEachCallerSaved(
            Host, AllocatableGeneral, AllocatableFloat, [&](machine::PhysicalRegister reg) {
                rows[next++] = detail::fixedTo(machine::OperandRole::Def, reg);
            });
        return rows;
    }();
    static constexpr auto Entry = std::to_array({ detail::NotRead, detail::NotRead, detail::NotRead });
    static constexpr auto RestoreInstanceContext =
        std::to_array({ detail::general(machine::OperandRole::Use) });

    /// Every pseudo-instruction's rows, in Pseudo order.
    static constexpr auto Table = std::to_array<std::span<machine::OperandConstraint const>>({
        Branch,
        CompareBranch,
        Switch,
        Return,
        Trap,
        TrapIf,
        TrapIfSubject,
        TrapIfPending,
        LoadIncomingGeneral,
        LoadIncomingFloat,
        Poll,
        PollStored,
        CallDirect,
        CallDispatched,
        CallStub,
        RestoreInstanceContext,
        Entry,
    });
    static_assert(Table.size() == PseudoCount, "PseudoConstraints::Table must hold one row per Pseudo");
};

/// The name of @p pseudo, for traces and diagnostics.
[[nodiscard]] std::string_view nameOf(Pseudo pseudo) noexcept;

} // namespace isoline::compiler::x86_64
