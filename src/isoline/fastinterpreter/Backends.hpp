// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <span>

#include <isoline/fastinterpreter/Code.hpp>
#include <isoline/fastinterpreter/Context.hpp>

// The entry points of the dispatch backends, which the executor and lowering reach them through. Internal
// to the fast interpreter.

namespace isoline::fastinterpreter
{

/// Runs Switch code from @p state until it returns or traps: a loop over a switch on each operation's
/// OperationId (SwitchDispatch.cpp).
/// @param state The registers: pc at the function's first operation.
/// @return How it stopped.
[[nodiscard]] Exit runSwitch(State state) noexcept;

/// Runs the operation at State::pc whose family the switch dispatch places out of line
/// (SwitchPlacement::OutOfLine), in a translation unit of its own (SwitchDispatchCold.cpp), so that its
/// body adds nothing to the loop's frame or code.
/// Precondition: the operation is one of those.
/// @param state The registers, by value, so that the loop's never have their address taken.
/// @return The registers after it, and whether to go on.
[[nodiscard]] OutOfLineStep runOutOfLine(State state) noexcept;

/// Runs TailCall code from @p state until it returns or traps: calls the first operation's handler, each
/// of which tail-calls the next one's (TailCallControl.cpp).
/// Precondition: TailCall is available.
/// @param state The registers: pc at the function's first operation.
/// @return How it stopped.
[[nodiscard]] Exit runTailCall(State state) noexcept;

/// The header words of TailCall code, by OperationId: each operation's handler's address, every one of
/// them distinct; empty where TailCall is not available.
/// @return The words, OperationCount of them or none.
[[nodiscard]] std::span<Word const> tailCallHeaderWords() noexcept;

} // namespace isoline::fastinterpreter
