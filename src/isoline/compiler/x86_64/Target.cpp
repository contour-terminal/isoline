// SPDX-License-Identifier: Apache-2.0
#include <isoline/compiler/x86_64/Target.hpp>

#include <core/Assert.hpp>
#include <core/Utils.hpp>

#include <array>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <ranges>
#include <string_view>
#include <utility>

#include <isoline/compiler/intermediate/Dominators.hpp>
#include <isoline/compiler/machine/Linearize.hpp>
#include <isoline/compiler/x86_64/Instructions.hpp>
#include <isoline/compiler/x86_64/Registers.hpp>
#include <isoline/compiler/x86_64/Selector.hpp>

namespace isoline::compiler::x86_64
{

namespace
{
    /// One pseudo-instruction's name.
    struct PseudoName
    {
        Pseudo pseudo;         ///< The pseudo-instruction; equal to the row's index.
        std::string_view name; ///< Its name.
    };

    constexpr auto PseudoNameTable = std::to_array<PseudoName>({
        { Pseudo::Branch, "branch" },
        { Pseudo::CompareBranch, "compare-branch" },
        { Pseudo::Switch, "switch" },
        { Pseudo::Return, "return" },
        { Pseudo::Trap, "trap" },
        { Pseudo::TrapIf, "trap-if" },
        { Pseudo::TrapIfSubject, "trap-if-subject" },
        { Pseudo::TrapIfPending, "trap-if-pending" },
        { Pseudo::LoadIncomingGeneral, "load-incoming-general" },
        { Pseudo::LoadIncomingFloat, "load-incoming-float" },
        { Pseudo::Poll, "poll" },
        { Pseudo::PollStored, "poll-stored" },
        { Pseudo::CallDirect, "call-direct" },
        { Pseudo::CallDispatched, "call-dispatched" },
        { Pseudo::CallStub, "call-stub" },
        { Pseudo::RestoreInstanceContext, "restore-instance-context" },
        { Pseudo::Entry, "entry" },
    });

    static_assert(
        [] {
            auto index = std::size_t { 0 };
            for (auto const& row: PseudoNameTable)
                if (static_cast<std::size_t>(row.pseudo) != index++)
                    return false;
            return index == PseudoCount;
        }(),
        "PseudoNameTable must hold one row per Pseudo, at the index of its enumerator");

    /// The pseudo-instructions' rows under @p host.
    [[nodiscard]] std::span<std::span<machine::OperandConstraint const> const> pseudoRowsOf(
        machine::Convention host) noexcept
    {
        if (host == machine::Convention::Windows64)
            return PseudoConstraints<Windows64Convention>::Table;
        return PseudoConstraints<SystemVConvention>::Table;
    }
} // namespace

std::string_view nameOf(Pseudo pseudo) noexcept
{
    return PseudoNameTable[static_cast<std::size_t>(pseudo)].name;
}

std::expected<X64Target, CompileError> X64Target::create(machine::TargetDescription description)
{
    if (description.architecture != machine::Architecture::X64 || !machine::hasHostConvention(description))
        return std::unexpected { CompileError { .code = CompileErrorCode::UnsupportedTarget } };
    return X64Target { description, machine::hostConvention(description) };
}

machine::FrameRule X64Target::frameRule() const noexcept
{
    return X64FrameRule;
}

machine::StackRule X64Target::stackRule() const noexcept
{
    return X64StackRule;
}

machine::ConventionRow const& X64Target::conventionRow(machine::Convention convention) const
{
    Require(convention == machine::Convention::WasmInternal || convention == _host);
    return x86_64::conventionRow(convention);
}

std::span<machine::PhysicalRegister const> X64Target::allocatable(
    machine::RegisterClass registerClass) const noexcept
{
    if (registerClass == machine::RegisterClass::General)
        return AllocatableGeneral;
    return AllocatableFloat;
}

std::span<machine::OperandConstraint const> X64Target::constraints(uint16_t machineOpcode) const
{
    if (auto const mnemonic = mnemonicOf(machineOpcode))
        return constraintsOf(*mnemonic);
    auto const pseudo = pseudoOf(machineOpcode);
    Require(pseudo.has_value());
    return pseudoRowsOf(_host)[static_cast<std::size_t>(*pseudo)];
}

CallConventions X64Target::conventions() const noexcept
{
    return CallConventions { .internal = WasmInternalConvention, .host = x86_64::conventionRow(_host) };
}

std::expected<machine::MachineFunction, CompileError> X64Target::select(
    intermediate::Function const& function, machine::SelectionContext const& context) const
{
    Require(context.config.target == _description);
    auto selector = Selector { _description.features, conventions() };
    return selector.select(function, context);
}

std::expected<void, CompileError> X64Target::encode(machine::MachineFunction const& function,
                                                    machine::FrameLayout const& frame,
                                                    machine::ModuleLabels const& labels,
                                                    machine::CodeBuffer& buffer) const
{
    auto memory = std::pmr::unsynchronized_pool_resource { &function.memory() };
    return encodeFunction(function, frame, conventions(), labels, buffer, nullptr, memory);
}

// uncovered-begin(entry-code-is-task-14)
void X64Target::emitEntryAdapter(binary::FuncType const& /*type*/, machine::CodeBuffer& /*buffer*/) const
{
    core::todo("P5 Task 14: the entry adapters (compiler/x86_64/Trampoline.cpp)");
}

void X64Target::emitEntryTrampoline(machine::CodeBuffer& /*buffer*/) const
{
    core::todo("P5 Task 14: the entry trampoline and its trap exit (compiler/x86_64/Trampoline.cpp)");
}
// uncovered-end(entry-code-is-task-14)

namespace
{
    /// Emits @p move: between registers, between a register and a slot, or between slots by push and pop.
    void emitMove(Assembler& assembler, machine::Move const& move)
    {
        auto const isFloat = move.from.registerClass == machine::RegisterClass::Float;
        auto const registerOperand = [](machine::Location const& location) {
            return machine::MachineOperand { machine::RegisterOperand {
                .name = machine::RegisterName::of(location.physicalRegister()),
                .role = machine::OperandRole::Use } };
        };
        auto const slotOperand = [](machine::Location const& location) {
            return at(Gpr::Rsp, static_cast<int64_t>(location.index));
        };
        auto const fromRegister = move.from.kind == machine::LocationKind::Register;
        auto const toRegister = move.to.kind == machine::LocationKind::Register;
        if (fromRegister && toRegister)
        {
            Require(move.from.registerClass == move.to.registerClass);
            assembler.emit(isFloat ? Mnemonic::MovapsRR : Mnemonic::Mov64RR,
                           { registerOperand(move.to), registerOperand(move.from) });
        }
        else if (toRegister)
            assembler.emit(isFloat ? Mnemonic::MovsdRM : Mnemonic::Mov64RM,
                           { registerOperand(move.to), slotOperand(move.from) });
        else if (fromRegister)
            assembler.emit(isFloat ? Mnemonic::MovsdMR : Mnemonic::Mov64MR,
                           { slotOperand(move.to), registerOperand(move.from) });
        else
        {
            // Without a register of the function's: push reads its operand before rsp moves, pop writes its
            // operand after rsp moved back, so both name their slots as they are.
            assembler.emit(Mnemonic::Push64M, { slotOperand(move.from) });
            assembler.emit(Mnemonic::Pop64M, { slotOperand(move.to) });
        }
    }
} // namespace

std::expected<void, CompileError> encodeFunction(machine::MachineFunction const& function,
                                                 machine::FrameLayout const& frame,
                                                 CallConventions const& conventions,
                                                 machine::ModuleLabels const& labels,
                                                 machine::CodeBuffer& buffer,
                                                 std::pmr::vector<TraceEntry>* trace,
                                                 std::pmr::memory_resource& memory)
{
    auto const graph = machine::controlFlowGraphOf(function, memory);
    auto const dominators = intermediate::DominatorTree { graph, memory };
    auto const loops = intermediate::LoopNesting { graph, dominators, memory };
    auto order = machine::linearize(function, loops, memory);
    if (!order)
        return std::unexpected { order.error() }; // uncovered(layout-breaks-a-loop)
    auto blockLabels = std::pmr::vector<machine::CodeLabel> { &memory };
    blockLabels.reserve(function.blockCount());
    for ([[maybe_unused]] auto const block: std::views::iota(uint32_t { 0 }, function.blockCount()))
        blockLabels.push_back(buffer.createLabel());
    auto assembler = Assembler { buffer, blockLabels, trace };
    auto emitter = FunctionEmitter { assembler, function, frame, conventions, labels, memory };
    emitter.prologue();
    auto const blocks = order->blocks();
    for (auto const [position, block]: core::views::enumerate(blocks))
    {
        auto const next = static_cast<std::size_t>(position) + 1 < blocks.size()
                              ? std::optional { blocks[static_cast<std::size_t>(position) + 1] }
                              : std::nullopt;
        assembler.bind(blockLabels[std::to_underlying(block)]);
        for (auto const index: function.instructions(block))
        {
            for (auto const& move: function.movesBefore(index))
                emitMove(assembler, move);
            auto const opcode = function.instruction(index).opcode;
            auto const mnemonic = mnemonicOf(opcode);
            if (!mnemonic)
            {
                emitter.expand(index, next);
                continue;
            }
            auto const operands = function.operands(index);
            // A jump to the next block in the layout falls through.
            if (*mnemonic == Mnemonic::Jmp && next
                && std::get<machine::LabelOperand>(operands[0]).block == *next)
                continue;
            assembler.emit(*mnemonic, operands.first(formRow(encodingRow(*mnemonic).form).count));
        }
    }
    emitter.finish();
    if (assembler.failure())
        return std::unexpected { CompileError { .code = CompileErrorCode::InternalVerificationFailed,
                                                .pass = "encoder" } };
    return {};
}

} // namespace isoline::compiler::x86_64
