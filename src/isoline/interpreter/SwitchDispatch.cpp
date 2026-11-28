// SPDX-License-Identifier: Apache-2.0
#include <isoline/interpreter/Backends.hpp>
#include <isoline/interpreter/Operations.hpp>

namespace isoline::interpreter
{

namespace
{
    /// The case of operation @p Id: its body inline, or a call out of line for a family the switch places
    /// there (OperationFamilyTable).
    template <OperationId Id>
    [[nodiscard]] Flow switchCase(State& state) noexcept
    {
        if constexpr (familyRow(operationRow(Id).family).placement == SwitchPlacement::Inline)
            return operation<Id>(state);
        else
        {
            auto const step = runOutOfLine(state);
            state = step.state;
            return step.flow;
        }
    }
} // namespace

// The case of every row of Operations.def, defined outside the loop so that only the rows are inside it.
#define ISOLINE_INTERPRETER_OPERATION(Name, Text, Form, Slots, Immediates, Family, Result) \
    case OperationId::Name: flow = switchCase<OperationId::Name>(state); break;

Exit runSwitch(State state) noexcept
{
    while (true)
    {
        auto flow = Flow::Continue;
        switch (static_cast<OperationId>(*state.pc))
        {
#include <isoline/interpreter/Operations.def>
        }
        if (flow != Flow::Continue) [[unlikely]]
            return exitOf(flow);
    }
}

#undef ISOLINE_INTERPRETER_OPERATION

} // namespace isoline::interpreter
