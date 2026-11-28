// SPDX-License-Identifier: Apache-2.0
// x86-64's entry code: the module's entry trampoline with its trap exit, and the entry adapters (P5 Decisions
// "Calling conventions are rows" and "Traps never skip a C++ frame"). Both are data over the convention rows:
// the trampoline saves what the host convention's row preserves, the adapter places what assignCall() places
// under the internal one, so that a new host ABI is a row, not new code here.

#include <core/Assert.hpp>
#include <core/Utils.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <ranges>
#include <utility>
#include <variant>
#include <vector>

#include <isoline/compiler/ContextLayout.hpp>
#include <isoline/compiler/machine/CallingConvention.hpp>
#include <isoline/compiler/x86_64/Lowering.hpp>
#include <isoline/compiler/x86_64/Registers.hpp>
#include <isoline/compiler/x86_64/Target.hpp>
#include <isoline/runtime/RawValue.hpp>

namespace isoline::compiler::x86_64
{

namespace
{
    using intermediate::Type;
    using machine::MachineOperand;
    using machine::PhysicalRegister;
    using machine::RegisterClass;

    /// Where the trampoline puts each of EntryTrampoline's parameters, in their order: the execution state
    /// and the instance context in their pinned registers, the adapter in rax (called at once), the function
    /// in r11 and the cells in r10 (the adapter's two inputs). None is an argument register of either host
    /// convention, so the moves cannot overwrite a parameter not yet read.
    constexpr auto EntryParameterTargets =
        std::to_array<Gpr>({ Gpr::R15, Gpr::R14, Gpr::Rax, Gpr::R11, Gpr::R10 });

    /// The register the adapter finds the function's code in (EntryParameterTargets' fourth).
    constexpr auto AdapterFunction = Gpr::R11;

    /// The register the adapter finds the RawValue cells in (EntryParameterTargets' fifth).
    constexpr auto AdapterValues = Gpr::R10;

    /// A RawValue cell's bytes, and a word's.
    constexpr auto CellBytes = static_cast<int64_t>(sizeof(runtime::RawValue));
    constexpr auto WordBytes = int64_t { 8 };

    /// The bytes an XMM register's save takes, whole: Microsoft x64 preserves all 128 bits of xmm6–15.
    constexpr auto FloatSaveBytes = int64_t { 16 };

    /// The stack's alignment at every call.
    constexpr auto Alignment = static_cast<int64_t>(X64StackRule.alignment);

    [[nodiscard]] constexpr MachineOperand gpr(Gpr reg) noexcept
    {
        return physicalOperand(reg);
    }

    [[nodiscard]] constexpr MachineOperand xmm(Xmm reg) noexcept
    {
        return physicalOperand(reg);
    }

    [[nodiscard]] constexpr MachineOperand operandOf(PhysicalRegister reg) noexcept
    {
        return machine::RegisterOperand { .name = machine::RegisterName::of(reg),
                                          .role = machine::OperandRole::Use };
    }

    [[nodiscard]] constexpr int64_t roundUp(int64_t bytes) noexcept
    {
        return (bytes + Alignment - 1) / Alignment * Alignment;
    }

    /// What the trampoline saves of the host's registers, from its convention row: every General register
    /// it preserves but rbp (pushed first, as the frame pointer), in register order; and every Float one.
    struct HostSaves
    {
        std::pmr::vector<Gpr> general;
        std::pmr::vector<Xmm> floats;

        /// The bytes below the pushes: the Float saves, then what keeps rsp 16-aligned at the adapter's call
        /// (the call into the trampoline pushed 8 bytes, rbp 8 more, each General save 8).
        [[nodiscard]] int64_t floatAreaBytes() const noexcept
        {
            auto const pushed = WordBytes * static_cast<int64_t>(general.size());
            return roundUp(pushed + (FloatSaveBytes * static_cast<int64_t>(floats.size()))) - pushed;
        }
    };

    [[nodiscard]] HostSaves hostSavesOf(machine::ConventionRow const& host, std::pmr::memory_resource& memory)
    {
        auto saves = HostSaves { .general = std::pmr::vector<Gpr> { &memory },
                                 .floats = std::pmr::vector<Xmm> { &memory } };
        for (auto const number: std::views::iota(std::size_t { 0 }, RegistersPerFile))
        {
            auto const general = static_cast<Gpr>(number);
            if (general != Gpr::Rbp && host.calleeSaved.contains(physical(general)))
                saves.general.push_back(general);
            auto const floating = static_cast<Xmm>(number);
            if (host.calleeSaved.contains(physical(floating)))
                saves.floats.push_back(floating);
        }
        return saves;
    }
} // namespace

void X64Target::emitEntryTrampoline(machine::CodeBuffer& buffer, machine::CodeLabel trapExit) const
{
    auto memory = std::pmr::unsynchronized_pool_resource {};
    auto assembler = Assembler { buffer, {}, nullptr };
    auto const& host = x86_64::conventionRow(_host);
    auto const saves = hostSavesOf(host, memory);
    auto const floatArea = saves.floatAreaBytes();
    // rbp first: the trampoline is a frame like any other, for profilers and P7's unwind information.
    assembler.emit(Mnemonic::Push64R, { gpr(Gpr::Rbp) });
    assembler.emit(Mnemonic::Mov64RR, { gpr(Gpr::Rbp), gpr(Gpr::Rsp) });
    for (auto const reg: saves.general)
        assembler.emit(Mnemonic::Push64R, { gpr(reg) });
    if (floatArea != 0)
        assembler.emit(Mnemonic::Sub64RI, { gpr(Gpr::Rsp), immediate(floatArea) });
    for (auto const [index, reg]: core::views::enumerate(saves.floats))
        assembler.emit(Mnemonic::MovapsMR,
                       { at(Gpr::Rsp, FloatSaveBytes * static_cast<int64_t>(index)), xmm(reg) });
    // The parameters, where the host convention passed them: a register, or the caller's stack above the
    // return address and the saved rbp.
    auto const parameters = std::pmr::vector<Type>(EntryParameterTargets.size(), Type::Pointer, &memory);
    auto const result = std::array { Type::I32 };
    auto const assignment = machine::assignCall(host, parameters, result, memory);
    Require(!assignment.resultPointer.has_value());
    for (auto const [index, target]: core::views::enumerate(EntryParameterTargets))
    {
        auto const& location = assignment.arguments.locations[static_cast<std::size_t>(index)];
        if (auto const* reg = std::get_if<PhysicalRegister>(&location))
        {
            Require(!std::ranges::contains(host.integerArguments, physical(target)));
            assembler.emit(Mnemonic::Mov64RR, { gpr(target), operandOf(*reg) });
        }
        else
        {
            auto const above = static_cast<int64_t>(X64StackRule.returnAddressBytes)
                               + static_cast<int64_t>(X64StackRule.framePointerBytes);
            auto const offset = above + static_cast<int64_t>(std::get<machine::StackOffset>(location).bytes);
            assembler.emit(Mnemonic::Mov64RM, { gpr(target), at(Gpr::Rbp, offset) });
        }
    }
    // Where the trap exit resets rsp to: below every save, so that both ways out restore from one layout.
    // P7 owns what a reset owes a CET shadow stack (incssp) and the unwind registration of this frame.
    assembler.emit(
        Mnemonic::Mov64MR,
        { at(Gpr::R15, static_cast<int64_t>(layoutOf(ExecutionStateField::EntryStackPointer).offset)),
          gpr(Gpr::Rsp) });
    assembler.emit(Mnemonic::Call64R, { gpr(Gpr::Rax) });
    auto const status = operandOf(host.integerResults[0]);
    assembler.emit(Mnemonic::Xor32RR, { status, status });
    auto const restore = assembler.label();
    assembler.bind(restore);
    for (auto const [index, reg]: core::views::enumerate(saves.floats))
        assembler.emit(Mnemonic::MovapsRM,
                       { xmm(reg), at(Gpr::Rsp, FloatSaveBytes * static_cast<int64_t>(index)) });
    if (floatArea != 0)
        assembler.emit(Mnemonic::Add64RI, { gpr(Gpr::Rsp), immediate(floatArea) });
    for (auto const reg: saves.general | std::views::reverse)
        assembler.emit(Mnemonic::Pop64R, { gpr(reg) });
    assembler.emit(Mnemonic::Pop64R, { gpr(Gpr::Rbp) });
    assembler.emit(Mnemonic::Ret, {});
    // The trap exit: only generated frames lie between it and the trampoline, so resetting rsp skips no C++
    // frame (P5 Decision "Traps never skip a C++ frame"). r15 is pinned, so it still points at the state.
    assembler.bind(trapExit);
    assembler.emit(
        Mnemonic::Mov64RM,
        { gpr(Gpr::Rsp),
          at(Gpr::R15, static_cast<int64_t>(layoutOf(ExecutionStateField::EntryStackPointer).offset)) });
    assembler.emit(Mnemonic::Mov32RI, { status, immediate(std::to_underlying(EntryStatus::Pending)) });
    assembler.jumpTo(restore);
    Require(!assembler.failure().has_value());
}

void X64Target::emitEntryAdapter(binary::FuncType const& type, machine::CodeBuffer& buffer) const
{
    auto memory = std::pmr::unsynchronized_pool_resource {};
    auto assembler = Assembler { buffer, {}, nullptr };
    auto const emit = [&](Mnemonic mnemonic, std::initializer_list<MachineOperand> operands) {
        assembler.emit(mnemonic, operands);
    };
    auto parameters = std::pmr::vector<Type> { &memory };
    for (auto const& parameter: type.params)
        parameters.push_back(typeOf(parameter));
    auto results = std::pmr::vector<Type> { &memory };
    for (auto const& result: type.results)
        results.push_back(typeOf(result));
    auto const assignment = machine::assignCall(WasmInternalConvention, parameters, results, memory);
    auto const hidden = assignment.resultPointer.has_value() ? std::size_t { 1 } : 0;
    // The frame, from rsp up: the stack arguments, the result area, the cells' address, the function's code.
    auto const argumentBytes = static_cast<int64_t>(assignment.arguments.memoryBytes);
    auto const resultArea = argumentBytes;
    auto const valuesSlot = resultArea + static_cast<int64_t>(assignment.results.memoryBytes);
    auto const functionSlot = valuesSlot + WordBytes;
    auto const frameBytes = roundUp(functionSlot + WordBytes);
    emit(Mnemonic::Push64R, { gpr(Gpr::Rbp) });
    emit(Mnemonic::Mov64RR, { gpr(Gpr::Rbp), gpr(Gpr::Rsp) });
    // rax is no input of the adapter's, so it may count the probe loop of a frame past a page.
    allocateStack(assembler, static_cast<uint64_t>(frameBytes), Gpr::Rax);
    emit(Mnemonic::Mov64MR, { at(Gpr::Rsp, valuesSlot), gpr(AdapterValues) });
    emit(Mnemonic::Mov64MR, { at(Gpr::Rsp, functionSlot), gpr(AdapterFunction) });
    // The arguments, each from its cell. rax and r11 are no argument register of the internal convention,
    // nor is xmm15, so they carry what goes to the stack.
    for (auto const [index, parameter]: core::views::enumerate(parameters))
    {
        auto const cell = CellBytes * static_cast<int64_t>(index);
        auto const& location = assignment.arguments.locations[hidden + static_cast<std::size_t>(index)];
        auto const* reg = std::get_if<PhysicalRegister>(&location);
        if (parameter == Type::Ref)
        {
            auto const word = reg != nullptr ? *reg : physical(Gpr::Rax);
            refWordFromCell(
                emit,
                [&](RefRegister which, machine::OperandRole /*role*/) {
                    return which == RefRegister::Word ? operandOf(word) : gpr(Gpr::R11);
                },
                at(AdapterValues, cell),
                at(AdapterValues, cell + WordBytes));
            if (reg == nullptr)
                emit(Mnemonic::Mov64MR,
                     { at(Gpr::Rsp, static_cast<int64_t>(std::get<machine::StackOffset>(location).bytes)),
                       gpr(Gpr::Rax) });
        }
        else if (reg != nullptr)
            emit(valueFormOf(parameter).load, { operandOf(*reg), at(AdapterValues, cell) });
        else
        {
            // A cell's first word holds every other value's bits, zero-extended: copied whole.
            emit(Mnemonic::Mov64RM, { gpr(Gpr::Rax), at(AdapterValues, cell) });
            emit(Mnemonic::Mov64MR,
                 { at(Gpr::Rsp, static_cast<int64_t>(std::get<machine::StackOffset>(location).bytes)),
                   gpr(Gpr::Rax) });
        }
    }
    if (hidden != 0)
    {
        // The internal convention passes the result area's pointer first, in its first argument register.
        auto const* pointer = std::get_if<PhysicalRegister>(&*assignment.resultPointer);
        Require(pointer != nullptr);
        emit(Mnemonic::Lea64RM, { operandOf(*pointer), at(Gpr::Rsp, resultArea) });
    }
    emit(Mnemonic::Call64M, { at(Gpr::Rsp, functionSlot) });
    // The results, each into its cell as it is read, so that a register it came in is free once written.
    // r9 and r11 return no result of the internal convention, nor does xmm15.
    emit(Mnemonic::Mov64RM, { gpr(AdapterValues), at(Gpr::Rsp, valuesSlot) });
    for (auto const [index, location]: core::views::enumerate(assignment.results.locations))
    {
        auto const result = results[static_cast<std::size_t>(index)];
        auto const cell = CellBytes * static_cast<int64_t>(index);
        auto const* reg = std::get_if<PhysicalRegister>(&location);
        auto const scratch = machine::registerClassOf(result) == RegisterClass::Float ? physical(Xmm::Xmm15)
                                                                                      : physical(Gpr::R11);
        auto const source = reg != nullptr ? *reg : scratch;
        if (reg == nullptr)
            emit(valueFormOf(result).load,
                 { operandOf(source),
                   at(Gpr::Rsp,
                      resultArea + static_cast<int64_t>(std::get<machine::StackOffset>(location).bytes)) });
        if (result == Type::Ref)
        {
            refWordToCell(
                emit,
                [&](RefRegister which, machine::OperandRole /*role*/) {
                    return which == RefRegister::Word ? operandOf(source) : gpr(Gpr::R9);
                },
                at(AdapterValues, cell),
                at(AdapterValues, cell + WordBytes));
            continue;
        }
        emit(Mnemonic::Mov64MI32, { at(AdapterValues, cell), immediate(0) });
        emit(Mnemonic::Mov64MI32, { at(AdapterValues, cell + WordBytes), immediate(0) });
        emit(valueFormOf(result).store, { at(AdapterValues, cell), operandOf(source) });
    }
    emit(Mnemonic::Mov64RR, { gpr(Gpr::Rsp), gpr(Gpr::Rbp) });
    emit(Mnemonic::Pop64R, { gpr(Gpr::Rbp) });
    emit(Mnemonic::Ret, {});
    Require(!assembler.failure().has_value());
}

} // namespace isoline::compiler::x86_64
