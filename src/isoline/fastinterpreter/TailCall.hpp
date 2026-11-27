// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <utility>

#include <isoline/fastinterpreter/Code.hpp>
#include <isoline/fastinterpreter/Context.hpp>
#include <isoline/fastinterpreter/DispatchAvailability.hpp>
#include <isoline/fastinterpreter/Operations.hpp>

// The tail-call dispatch's handlers: one per operation, each running the operation's body and ending in a
// guaranteed tail call of the next operation's handler, whose address is that operation's header word.
// Included only by the TailCall*.cpp family files, which CMake compiles where a probe row of
// DispatchAvailability.hpp passed: ISOLINE_FAST_MUSTTAIL is the spelling of the row that did.

namespace isoline::fastinterpreter
{

static_assert(sizeof(Handler) == sizeof(Word), "a handler's address is a code word");

/// The handler of operation @p Id: its body, then a guaranteed tail call of the next operation's handler.
///
/// Like Handler, not noexcept: a guaranteed tail call may skip over no cleanup (Clang: "cannot compile this
/// tail call skipping over cleanups"), and a noexcept function's terminate scope is one. The body's registers
/// live in an inner scope and go back into the parameters, so that no local whose address the body took is
/// alive at the tail call: GCC refuses one (-Wmaybe-musttail-local-addr) wherever the body is not inlined.
template <OperationId Id>
[[nodiscard]] Exit handler(
    Word const* pc, Slot* fp, Context* context, std::byte* memory0, uint64_t memory0Size, uint64_t stepsLeft)
{
    // A body that could throw would leave the handler, which is not noexcept, by an exception.
    static_assert(noexcept(operation<Id>(std::declval<State&>())));
    {
        auto state = State { .pc = pc,
                             .fp = fp,
                             .context = context,
                             .memory0 = memory0,
                             .memory0Size = memory0Size,
                             .stepsLeft = stepsLeft };
        if (auto const flow = operation<Id>(state); flow != Flow::Continue) [[unlikely]]
            return exitOf(flow);
        pc = state.pc;
        fp = state.fp;
        memory0 = state.memory0;
        memory0Size = state.memory0Size;
        stepsLeft = state.stepsLeft;
    }
    ISOLINE_FAST_MUSTTAIL return std::bit_cast<Handler>(*pc)(
        pc, fp, context, memory0, memory0Size, stepsLeft);
}

/// The handlers of one family file: by OperationId, the handler of each operation of @p Families, null for
/// every other.
template <OperationFamily... Families>
[[nodiscard]] consteval std::array<Handler, OperationCount> familyHandlers() noexcept
{
    return []<std::size_t... Index>(std::index_sequence<Index...>) {
        return std::array<Handler, OperationCount> { [] {
            constexpr auto Id = static_cast<OperationId>(Index);
            if constexpr (((operationRow(Id).family == Families) || ...))
                return Handler { &handler<Id> };
            else
                return Handler { nullptr };
        }()... };
    }(std::make_index_sequence<OperationCount> {});
}

/// The handlers of the control family (TailCallControl.cpp).
[[nodiscard]] std::array<Handler, OperationCount> const& controlHandlers() noexcept;

/// The handlers of the numeric family (TailCallNumeric.cpp).
[[nodiscard]] std::array<Handler, OperationCount> const& numericHandlers() noexcept;

/// The handlers of the variable and memory families (TailCallMemory.cpp).
[[nodiscard]] std::array<Handler, OperationCount> const& memoryHandlers() noexcept;

/// The handlers of the vector family (TailCallSimd.cpp).
[[nodiscard]] std::array<Handler, OperationCount> const& simdHandlers() noexcept;

/// The handlers of the exceptions family (TailCallExceptions.cpp).
[[nodiscard]] std::array<Handler, OperationCount> const& exceptionHandlers() noexcept;

} // namespace isoline::fastinterpreter
