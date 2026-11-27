// SPDX-License-Identifier: Apache-2.0
#include <array>

#include <isoline/fastinterpreter/TailCall.hpp>

namespace isoline::fastinterpreter
{

std::array<Handler, OperationCount> const& memoryHandlers() noexcept
{
    static constexpr auto Handlers = familyHandlers<OperationFamily::Variable, OperationFamily::Memory>();
    return Handlers;
}

} // namespace isoline::fastinterpreter
