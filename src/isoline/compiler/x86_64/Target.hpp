// SPDX-License-Identifier: Apache-2.0
#pragma once

// The x86-64 target: machine::ITarget over x86-64's tables. Its register file, conventions, frame and stack
// rules are Registers.hpp's rows; its operand constraints are the encoding table's (Instructions.hpp) and the
// pseudo-instructions' under its host convention; selection is the Selector over Selection.def, and encoding
// the Lowering sequences over Encodings.def. Constructed for one TargetDescription by create(), which refuses
// a description it cannot serve; stateless thereafter, so one target serves every compile for its
// description.

#include <expected>
#include <span>

#include <isoline/compiler/CompileError.hpp>
#include <isoline/compiler/machine/CallingConvention.hpp>
#include <isoline/compiler/machine/Target.hpp>
#include <isoline/compiler/machine/TargetDescription.hpp>
#include <isoline/compiler/x86_64/Lowering.hpp>

namespace isoline::compiler::x86_64
{

/// The x86-64 code generator for one target description.
class X64Target final: public machine::ITarget
{
  public:
    /// The target for @p description.
    /// @param description What to generate code for: x86-64, an ABI with a host convention row, any features.
    /// @return The target; or UnsupportedTarget for another architecture, an ABI without a host convention,
    /// or
    ///         a feature of another architecture (arm64's Cssc).
    [[nodiscard]] static std::expected<X64Target, CompileError> create(
        machine::TargetDescription description);

    [[nodiscard]] machine::TargetDescription const& description() const noexcept override
    {
        return _description;
    }

    [[nodiscard]] machine::FrameRule frameRule() const noexcept override;

    [[nodiscard]] machine::StackRule stackRule() const noexcept override;

    [[nodiscard]] uint64_t codeLimit() const noexcept override;

    [[nodiscard]] machine::ConventionRow const& conventionRow(machine::Convention convention) const override;

    [[nodiscard]] std::span<machine::PhysicalRegister const> allocatable(
        machine::RegisterClass registerClass) const noexcept override;

    [[nodiscard]] std::span<machine::OperandConstraint const> constraints(
        uint16_t machineOpcode) const override;

    /// Selects @p function (Require: the compile's target is this one's description).
    [[nodiscard]] std::expected<machine::MachineFunction, CompileError> select(
        intermediate::Function const& function, machine::SelectionContext const& context) const override;

    [[nodiscard]] std::expected<void, CompileError> encode(machine::MachineFunction const& function,
                                                           machine::FrameLayout const& frame,
                                                           machine::ModuleLabels const& labels,
                                                           machine::CodeBuffer& buffer) const override;

    /// The entry adapter of @p type (Trampoline.cpp): called with the function's code in r11 and the
    /// RawValue cells in r10, it keeps the cells' address in its frame, passes the arguments under the
    /// internal convention (stack arguments and a result area in its own frame), calls r11, and writes the
    /// results back into the cells.
    void emitEntryAdapter(binary::FuncType const& type, machine::CodeBuffer& buffer) const override;

    /// The entry adapter's stack arguments and result area (Trampoline.cpp), as emitEntryAdapter() lays them
    /// out below its two slots, rounded to the stack's alignment.
    [[nodiscard]] uint64_t entryAdapterAreaBytes(binary::FuncType const& type) const override;

    /// The module's entry trampoline and its trap exit (Trampoline.cpp), under the description's host
    /// convention: rbp pushed, every other General register the convention preserves pushed, every Float one
    /// saved whole below them (Microsoft x64's xmm6–15), the pinned registers and the adapter's r10 and r11
    /// taken from the arguments, rsp recorded as ExecutionState::entryStackPointer, the adapter called; the
    /// trap exit resets rsp from entryStackPointer, so both ways out restore from the same layout.
    void emitEntryTrampoline(machine::CodeBuffer& buffer, machine::CodeLabel trapExit) const override;

    /// The conventions its calls are selected under: the internal one, and the description's host one.
    [[nodiscard]] CallConventions conventions() const noexcept;

  private:
    X64Target(machine::TargetDescription description, machine::Convention host) noexcept:
        _description { description }, _host { host }
    {
    }

    machine::TargetDescription _description;
    machine::Convention _host;
};

} // namespace isoline::compiler::x86_64
