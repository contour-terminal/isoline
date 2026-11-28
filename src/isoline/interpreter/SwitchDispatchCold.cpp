// SPDX-License-Identifier: Apache-2.0
#include <core/Assert.hpp>

#include <isoline/interpreter/Backends.hpp>
#include <isoline/interpreter/Operations.hpp>

// The switch dispatch's cold families (SwitchPlacement::OutOfLine), in a translation unit of their own:
// their bodies stay out of the loop's frame and code.

namespace isoline::interpreter
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

// The case of every row of Operations.def, defined outside the switch so that only the rows are inside it.
#define ISOLINE_INTERPRETER_OPERATION(Name, Text, Form, Slots, Immediates, Family, Result) \
    case OperationId::Name: flow = outOfLineCase<OperationId::Name>(state); break;

OutOfLineStep runOutOfLine(State state) noexcept
{
    auto flow = Flow::Continue;
    switch (static_cast<OperationId>(*state.pc))
    {
#include <isoline/interpreter/Operations.def>
    }
    return { .state = state, .flow = flow };
}

#undef ISOLINE_INTERPRETER_OPERATION

} // namespace isoline::interpreter
