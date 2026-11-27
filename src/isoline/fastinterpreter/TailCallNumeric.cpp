// SPDX-License-Identifier: Apache-2.0
#include <array>

#include <isoline/fastinterpreter/TailCall.hpp>

namespace isoline::fastinterpreter
{

std::array<Handler, OperationCount> const& numericHandlers() noexcept
{
    static constexpr auto Handlers = familyHandlers<OperationFamily::Numeric>();
    return Handlers;
}

} // namespace isoline::fastinterpreter
