// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include <isoline/fastinterpreter/Code.hpp>
#include <isoline/runtime/PollGate.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Trap.hpp>

// What the dispatch backends and the executor share while lowered code runs: the handler signature,
// the registers an operation works on, and the cold state behind them. Internal to the fast interpreter:
// only FastInterpreter.cpp constructs a Context, and only the backends run code with one.

namespace isoline::fastinterpreter
{

struct Context;

/// How running lowered code ended, as a handler or the switch loop returns it to the executor.
enum class Exit : uint8_t
{
    Returned, ///< The function returned; its results are in its frame (Context::resultSlot).
    Trapped,  ///< It trapped (Context::trap).
    Thrown,   ///< An exception escaped it.
};

/// What an operation's body tells the dispatch: go on to the operation at the new pc, or stop.
enum class Flow : uint8_t
{
    Continue, ///< Run the operation the pc points at now.
    Returned, ///< Stop: the function returned (Exit::Returned).
    Trapped,  ///< Stop: it trapped (Exit::Trapped).
};

/// The Exit a body's stopping Flow ends execution with.
/// Precondition: @p flow is not Flow::Continue.
/// @param flow How the body stopped.
/// @return The Exit.
[[nodiscard]] constexpr Exit exitOf(Flow flow) noexcept
{
    return flow == Flow::Returned ? Exit::Returned : Exit::Trapped;
}

/// A tail-call handler: runs the operation at @p pc, then tail-calls the next one's.
///
/// Six integer arguments, all in registers on SysV x86-64 and AArch64, so that memory 0's bounds check and
/// the step countdown never touch memory; every colder field lives in the Context. Not noexcept, though no
/// handler throws: a noexcept function's implicit terminate scope is a cleanup, which a guaranteed tail call
/// may not skip over.
using Handler = Exit (*)(
    Word const* pc, Slot* fp, Context* context, std::byte* memory0, uint64_t memory0Size, uint64_t stepsLeft);

/// The registers of running code: a handler's arguments, the switch loop's locals.
///
/// An operation's body takes them by reference, and is inlined into its handler or its case, so they stay
/// in registers. Out-of-line code takes them by value and gives them back by value, so that their address
/// never escapes the loop.
struct State
{
    Word const* pc = nullptr;   ///< The current operation's header word.
    Slot* fp = nullptr;         ///< The current frame's first slot.
    Context* context = nullptr; ///< The cold state.
    std::byte* memory0 =
        nullptr;              ///< Memory 0's bytes in the current frame's instance; null when it has none.
    uint64_t memory0Size = 0; ///< Memory 0's size in bytes.
    uint64_t stepsLeft = 0;   ///< The step countdown (runtime::PollGate): the store's while code runs.
};

/// The cold state of one invocation, behind State::context: what operations reach only on their slow
/// paths, and what the executor reads once execution stops.
struct Context
{
    /// The context of an invocation on @p store, polling through a gate of its own.
    /// @param store The store; must outlive the context.
    explicit Context(runtime::Store& store) noexcept: store { store }, gate { store } {}

    runtime::Store& store;  ///< The store the invocation runs on.
    runtime::PollGate gate; ///< Its polls, whose countdown is State::stepsLeft.

    /// The current frame's instance, through which code shared by every instance reaches its own entities.
    runtime::ModuleInstance const* instance = nullptr;

    /// The current function's code: branch targets are word offsets from it.
    Word const* code = nullptr;

    /// Where a Return found the results: the first slot of the frame's results (Exit::Returned).
    uint32_t resultSlot = 0;

    /// The trap execution ended with (Exit::Trapped).
    std::optional<runtime::Trap> trap;
};

/// Stops execution with @p trap, out of line: commits the countdown to the store, as every way out of
/// running code does, and records the trap.
/// @param context The invocation.
/// @param stepsLeft The countdown.
/// @param trap Why it stops.
/// @return Flow::Trapped.
[[nodiscard]] Flow leaveTrapped(Context& context, uint64_t stepsLeft, runtime::Trap trap) noexcept;

/// Stops execution as the function returns, out of line: commits the countdown to the store and records
/// where its results are.
/// @param context The invocation.
/// @param stepsLeft The countdown.
/// @param resultSlot The first slot of its results.
/// @return Flow::Returned.
[[nodiscard]] Flow leaveReturning(Context& context, uint64_t stepsLeft, uint32_t resultSlot) noexcept;

} // namespace isoline::fastinterpreter
