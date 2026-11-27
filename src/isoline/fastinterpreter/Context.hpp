// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include <isoline/fastinterpreter/Code.hpp>
#include <isoline/runtime/Abrupt.hpp>
#include <isoline/runtime/PollGate.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Trap.hpp>

// What the dispatch backends and the executor share while lowered code runs: the handler signature,
// the registers an operation works on, the cold state behind them, and the executor's entry points that
// operations reach out of line (calls and returns). Internal to the fast interpreter: only
// FastInterpreter.cpp constructs a Context, and only the backends run code with one.

namespace isoline::fastinterpreter
{

struct Context;

/// The frames and slot chunks of one invocation (FastInterpreter.cpp), which the calls and returns of its
/// code enter and leave.
class Invocation;

/// How running lowered code ended, as a handler or the switch loop returns it to the executor.
enum class Exit : uint8_t
{
    Returned, ///< The invocation's function returned; its results are at Context::results.
    Trapped,  ///< It ended abruptly: a trap, or a host function it called failed (Context::abrupt).
    Thrown,   ///< An exception escaped it.
};

/// What an operation's body tells the dispatch: go on to the operation at the new pc, or stop.
enum class Flow : uint8_t
{
    Continue, ///< Run the operation the pc points at now.
    Returned, ///< Stop: the invocation's function returned (Exit::Returned).
    Trapped,  ///< Stop: it ended abruptly (Exit::Trapped).
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
    /// The context of @p invocation on @p store, polling through a gate of its own.
    /// @param store The store; must outlive the context.
    /// @param invocation The invocation's frames; must outlive the context.
    Context(runtime::Store& store, Invocation& invocation) noexcept:
        store { store }, gate { store }, invocation { invocation }
    {
    }

    runtime::Store& store;  ///< The store the invocation runs on.
    runtime::PollGate gate; ///< Its polls, whose countdown is State::stepsLeft.
    Invocation& invocation; ///< Its frames, which calls and returns go through.

    /// The current frame's instance, through which code shared by every instance reaches its own entities.
    runtime::ModuleInstance const* instance = nullptr;

    /// The current function's code: branch targets are word offsets from it.
    Word const* code = nullptr;

    /// The store's memoryGeneration() when the registers' memory 0 (State::memory0) was last read: while
    /// it is the store's still, they are memory 0's bytes and size.
    uint64_t memoryGeneration = 0;
    /// Where the invocation's function left its results: the first of their slots (Exit::Returned).
    Slot const* results = nullptr;

    /// How execution ended abruptly (Exit::Trapped): a trap, or a host function's failure.
    std::optional<runtime::Abrupt> abrupt;
};

/// What out-of-line code leaves: the registers, and whether to go on.
struct OutOfLineStep
{
    State state;                ///< The registers after it.
    Flow flow = Flow::Continue; ///< Whether to go on.
};

/// One call, as its operation names it (CallTable).
struct CallSite
{
    CalleeKind callee = CalleeKind::Internal; ///< What names the callee.
    CallMode mode = CallMode::Call;           ///< Whether the callee replaces the caller's frame.
    uint32_t words = 0;        ///< The operation's code words: where the caller goes on after the call.
    uint32_t argumentSlot = 0; ///< The slot of the first argument, where the callee's frame begins.
    uint32_t operandSlot = 0;  ///< Table: the slot of the table index; Reference: of the reference.
    uint64_t index = 0;        ///< Internal: the code index; Import: the function index; Table and
                               ///< Reference: the type index the callee must have, as the module names it.
    uint64_t table = 0;        ///< Table: the table's index in the module.
};

/// Calls the function @p site names, out of line, from the frame the registers @p state are of: the
/// executor resolves the callee (a table element or a reference is checked, and traps, before anything
/// else), then enters it through its one frame-entry routine, or runs a host function through
/// runtime::callHostFunction(). A Wasm callee's frame begins at the arguments; a tail call's replaces the
/// caller's.
/// @param state The registers, pc at the call operation.
/// @param site The call.
/// @return The registers of the callee's first operation, or of the operation after the call once a host
///         function returned; or how execution stops.
[[nodiscard]] OutOfLineStep callFunction(State state, CallSite const& site) noexcept;

/// Returns from the current frame, out of line: its results, at @p resultSlot, go to where its caller
/// passed the arguments, and the caller goes on after its call; the invocation's own function stops
/// execution instead (Flow::Returned, Context::results).
/// @param state The registers of the returning frame.
/// @param resultSlot The first slot of its results.
/// @return The registers of the caller, or how execution stops.
[[nodiscard]] OutOfLineStep returnFromFunction(State state, uint32_t resultSlot) noexcept;

/// Runs memory.grow of memory @p memory of the current frame's instance, out of line: grows it through the
/// store (runtime::Store::growMemory()) by the pages in slot @p deltaSlot, writes the old size in pages, or
/// -1 of the memory's address type where the grow was refused, to slot @p resultSlot, and reads memory 0 into
/// the registers again where any memory grew. The pc stays at the operation.
/// @param state The registers.
/// @param resultSlot The slot of the result.
/// @param deltaSlot The slot of the pages to add.
/// @param memory The memory's index in the module.
/// @return The registers after it; it always goes on.
[[nodiscard]] OutOfLineStep growMemory(State state,
                                       uint32_t resultSlot,
                                       uint32_t deltaSlot,
                                       uint32_t memory) noexcept;

/// Stops execution with @p trap, out of line: commits the countdown to the store, as every way out of
/// running code does, and records the trap.
/// @param context The invocation.
/// @param stepsLeft The countdown.
/// @param trap Why it stops.
/// @return Flow::Trapped.
[[nodiscard]] Flow leaveTrapped(Context& context, uint64_t stepsLeft, runtime::Trap trap) noexcept;

/// Stops execution with @p abrupt, out of line: commits the countdown to the store, as every way out of
/// running code does, and records how it ended.
/// @param context The invocation.
/// @param stepsLeft The countdown.
/// @param abrupt How it ends: a trap, or a host function's failure.
/// @return Flow::Trapped.
[[nodiscard]] Flow leaveAbruptly(Context& context, uint64_t stepsLeft, runtime::Abrupt abrupt) noexcept;

/// Stops execution as the invocation's function returns, out of line: commits the countdown to the store
/// and records where its results are.
/// @param context The invocation.
/// @param stepsLeft The countdown.
/// @param results The first slot of its results.
/// @return Flow::Returned.
[[nodiscard]] Flow leaveReturning(Context& context, uint64_t stepsLeft, Slot const* results) noexcept;

} // namespace isoline::fastinterpreter
