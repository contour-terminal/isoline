// SPDX-License-Identifier: Apache-2.0
#pragma once

// The machine verifier: what a selected function must be for the layers below selection to take it. Each
// register operand has the role and class its opcode's constraint row gives, and a fixed physical register
// where the row fixes one; a memory operand stands only where the row allows one; every edge passes one
// argument of the right class per parameter of its target; and no value is defined in two blocks. A failure
// is a compiler bug (in the selector, or a pass after it), refused as InternalVerificationFailed, never run.
// The layout's contiguity is checked by verifyContiguousLoops(), which linearize() and computeLiveIntervals()
// run.

#include <expected>

#include <isoline/compiler/CompileError.hpp>
#include <isoline/compiler/machine/MachineFunction.hpp>
#include <isoline/compiler/machine/Target.hpp>

namespace isoline::compiler::machine
{

/// Checks @p function against @p target's constraints and the machine function's rules.
/// @param function The function, as selection built it.
/// @param target The target whose opcodes it uses.
/// @return Nothing; or InternalVerificationFailed (pass "machine-verifier").
[[nodiscard]] std::expected<void, CompileError> verifyMachineFunction(MachineFunction const& function,
                                                                      ITarget const& target);

} // namespace isoline::compiler::machine
