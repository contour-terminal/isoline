// SPDX-License-Identifier: Apache-2.0
#pragma once

// The machine layer's target seam: what a backend provides, so that everything between the optimized
// intermediate code and the bytes (layout, liveness, register allocation, the compile pipeline) is
// target-independent. x86-64 implements it (P5), arm64 too (P6). A target is data first: its register
// files (allocatable()), the operand constraints of its opcodes (constraints()), its frame rule and its
// calling conventions (CallingConvention.hpp) are rows; selection, encoding and the entry code are the
// code a new target brings. What code is generated for, TargetDescription, lives in its own data-only
// header, so that what names a target (CompileConfig, an artifact's key) needs nothing of the backend.

#include <cstdint>
#include <expected>
#include <initializer_list>
#include <memory_resource>
#include <span>
#include <vector>

#include <isoline/compiler/CompileConfig.hpp>
#include <isoline/compiler/CompileError.hpp>
#include <isoline/compiler/machine/CallingConvention.hpp>
#include <isoline/compiler/machine/CodeBuffer.hpp>
#include <isoline/compiler/machine/FrameRule.hpp>
#include <isoline/compiler/machine/MachineFunction.hpp>
#include <isoline/compiler/machine/Registers.hpp>
#include <isoline/compiler/machine/TargetDescription.hpp>

namespace isoline::binary
{
struct FuncType;
} // namespace isoline::binary

namespace isoline::validate
{
class ValidatedModule;
} // namespace isoline::validate

namespace isoline::compiler::machine
{

/// What instruction selection is given beside the function: the compile's configuration, the module the
/// function belongs to, its index in the code section and the module's imported functions, the compile's
/// cancellation, and the arena the machine function allocates from.
struct SelectionContext
{
    CompileConfig const& config;             ///< The compile's configuration (target, mode, guard).
    validate::ValidatedModule const& module; ///< The module.
    uint32_t codeIndex;                      ///< The function's index in the code section.
    /// How many functions the module imports: a direct call's function index less this is its code index.
    /// Counted once per module by its compile, so that no function scans the imports.
    uint32_t importedFunctions;
    CompileCancellation const& cancellation; ///< Polled as selection proceeds.
    std::pmr::memory_resource& memory;       ///< What the machine function allocates from.
};

/// What a function's code reaches beyond itself in the module's code buffer: every function of the module,
/// by code index, for a direct call's rel32, and the entry trampoline's trap exit, which every trap site
/// jumps to (P5 Decision "Traps never skip a C++ frame"). The labels belong to the module's CodeBuffer; the
/// module's compile binds them as it emits the functions and the trampoline.
struct ModuleLabels
{
    std::span<CodeLabel const> functions {}; ///< Per code index, the label of the function's entry.
    CodeLabel trapExit {};                   ///< The trampoline's trap exit.
};

/// A code generator for one instruction set and operating-system ABI.
///
/// Stateless once constructed, and safe to share between compiles: every method is const. A target is
/// constructed for one TargetDescription, through a static factory that refuses a description it cannot
/// serve, and has no setters.
class ITarget
{
  public:
    virtual ~ITarget() = default;

    /// What the target generates code for.
    [[nodiscard]] virtual TargetDescription const& description() const noexcept = 0;

    /// The target's bound on a native frame (its FrameRule row).
    [[nodiscard]] virtual FrameRule frameRule() const noexcept = 0;

    /// The target's stack rule: the alignment every call leaves with, and what a call pushes.
    [[nodiscard]] virtual StackRule stackRule() const noexcept = 0;

    /// The target's row of @p convention: its internal convention's, or its host's
    /// (hostConvention(description())), which the stubs and the entry trampoline use.
    /// @param convention WasmInternal or the host convention (Require).
    [[nodiscard]] virtual ConventionRow const& conventionRow(Convention convention) const = 0;

    /// The registers of @p registerClass the allocator may assign, in its order of preference.
    [[nodiscard]] virtual std::span<PhysicalRegister const> allocatable(
        RegisterClass registerClass) const noexcept = 0;

    /// What @p machineOpcode asks of each of its operands, by position: their roles, classes, fixed
    /// registers and whether a memory operand will do. An instruction with more operands than rows (a
    /// variadic tail) takes the last row for the rest. A row at an immediate, label, stub or constant
    /// operand's position is not read.
    /// @param machineOpcode An opcode of the target's (Require).
    [[nodiscard]] virtual std::span<OperandConstraint const> constraints(uint16_t machineOpcode) const = 0;

    /// Selects machine instructions for @p function: its blocks in machine terms, with critical edges
    /// split, so that no block is added after selection.
    /// @param function A verified, optimized function.
    /// @param context The compile's context.
    /// @return The machine function; or why selection refused (Cancelled, a cap).
    [[nodiscard]] virtual std::expected<MachineFunction, CompileError> select(
        intermediate::Function const& function, SelectionContext const& context) const = 0;

    /// Encodes @p function, allocated, with its frame @p frame, into @p buffer, at its current offset: the
    /// prologue, the blocks in the layout linearize() gives, the moves the allocator inserted before each
    /// instruction, and the out-of-line paths (trap sites, slow paths, jump tables) after them.
    /// @param function The allocated function.
    /// @param frame Its frame.
    /// @param labels The module's labels its calls and trap sites reach.
    /// @param buffer The module's code buffer.
    /// @return Nothing; or why encoding refused (an instruction the target cannot encode: a compiler bug).
    [[nodiscard]] virtual std::expected<void, CompileError> encode(MachineFunction const& function,
                                                                   FrameLayout const& frame,
                                                                   ModuleLabels const& labels,
                                                                   CodeBuffer& buffer) const = 0;

    /// Emits the entry adapter of functions of @p type: the host's RawValue arguments and results to and
    /// from the internal convention.
    virtual void emitEntryAdapter(binary::FuncType const& type, CodeBuffer& buffer) const = 0;

    /// Emits the module's entry trampoline: the host's callee-saved registers saved per the host convention,
    /// the context registers set, the entry stack pointer recorded, and the trap exit.
    virtual void emitEntryTrampoline(CodeBuffer& buffer) const = 0;

  protected:
    ITarget() = default;
    ITarget(ITarget const&) = default;
    ITarget(ITarget&&) = default;
    ITarget& operator=(ITarget const&) = default;
    ITarget& operator=(ITarget&&) = default;
};

/// Visits the registers a call under @p row clobbers out of the allocatable @p general then @p floats: each
/// the row does not preserve, in their order. The one derivation of a clobber list: callerSavedRegisters()
/// runs it on a target, and a target's constexpr operand rows run it on the register files the target
/// allocates.
/// @param row The convention row.
/// @param general The allocatable General registers.
/// @param floats The allocatable Float registers.
/// @param visit Called with each clobbered register.
template <typename Visit>
constexpr void forEachCallerSaved(ConventionRow const& row,
                                  std::span<PhysicalRegister const> general,
                                  std::span<PhysicalRegister const> floats,
                                  Visit const& visit)
{
    for (auto const files: { general, floats })
        for (auto const reg: files)
            if (!row.calleeSaved.contains(reg))
                visit(reg);
}

/// The registers a call under @p convention clobbers on @p target: every allocatable register its row does
/// not preserve, General then Float, in the target's order. What a call's, a poll's, a stub's and CallHost's
/// physical definitions are, derived from the convention row and never listed by hand. (Registers the
/// allocator never assigns, such as the pinned ones, are not tracked, so a clobber of one is not listed.)
/// @param target The target.
/// @param convention Its internal convention, or its host's (Require).
/// @param memory What the list allocates from.
[[nodiscard]] inline std::pmr::vector<PhysicalRegister> callerSavedRegisters(
    ITarget const& target, Convention convention, std::pmr::memory_resource& memory)
{
    auto clobbered = std::pmr::vector<PhysicalRegister> { &memory };
    forEachCallerSaved(target.conventionRow(convention),
                       target.allocatable(RegisterClass::General),
                       target.allocatable(RegisterClass::Float),
                       [&](PhysicalRegister reg) { clobbered.push_back(reg); });
    return clobbered;
}

} // namespace isoline::compiler::machine
