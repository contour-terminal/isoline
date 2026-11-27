// SPDX-License-Identifier: Apache-2.0
#include <isoline/fastinterpreter/Backends.hpp>
#include <isoline/fastinterpreter/Operations.hpp>

namespace isoline::fastinterpreter
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

Exit runSwitch(State state) noexcept
{
    while (true)
    {
        auto flow = Flow::Continue;
        switch (static_cast<OperationId>(*state.pc))
        {
#define ISOLINE_FAST_OPERATION(Name, Text, Form, Slots, Immediates, Family) \
    case OperationId::Name: flow = switchCase<OperationId::Name>(state); break;
#include <isoline/fastinterpreter/Operations.def>
#undef ISOLINE_FAST_OPERATION
        }
        if (flow != Flow::Continue) [[unlikely]]
            return exitOf(flow);
    }
}

} // namespace isoline::fastinterpreter
