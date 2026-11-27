// SPDX-License-Identifier: Apache-2.0
#include <core/Assert.hpp>

#include <isoline/fastinterpreter/Backends.hpp>
#include <isoline/fastinterpreter/Operations.hpp>

// The switch dispatch's cold families (SwitchPlacement::OutOfLine), in a translation unit of their own:
// their bodies stay out of the loop's frame and code.

namespace isoline::fastinterpreter
{

namespace
{
    /// The case of operation @p Id: its body, for an operation the switch places out of line.
    template <OperationId Id>
    [[nodiscard]] Flow outOfLineCase(State& state) noexcept
    {
        if constexpr (familyRow(operationRow(Id).family).placement == SwitchPlacement::OutOfLine)
            return operation<Id>(state);
        else
            // The loop runs every other operation inline (runSwitch()).
            core::unreachable(); // uncovered(inline-operation)
    }
} // namespace

OutOfLineStep runOutOfLine(State state) noexcept
{
    auto flow = Flow::Continue;
    switch (static_cast<OperationId>(*state.pc))
    {
#define ISOLINE_FAST_OPERATION(Name, Text, Form, Slots, Immediates, Family) \
    case OperationId::Name: flow = outOfLineCase<OperationId::Name>(state); break;
#include <isoline/fastinterpreter/Operations.def>
#undef ISOLINE_FAST_OPERATION
    }
    return { .state = state, .flow = flow };
}

} // namespace isoline::fastinterpreter
