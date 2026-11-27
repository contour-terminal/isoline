// SPDX-License-Identifier: Apache-2.0
#include <array>

#include <isoline/fastinterpreter/TailCall.hpp>

namespace isoline::fastinterpreter
{

std::array<Handler, OperationCount> const& exceptionHandlers() noexcept
{
    static constexpr auto Handlers = familyHandlers<OperationFamily::Exceptions>();
    return Handlers;
}

} // namespace isoline::fastinterpreter
