// SPDX-License-Identifier: Apache-2.0
#include <isoline/fastinterpreter/Dispatch.hpp>

#include <span>

#include <isoline/fastinterpreter/DispatchAvailability.hpp>

namespace isoline::fastinterpreter
{

std::span<Dispatch const> availableDispatches() noexcept
{
    return configured::AvailableDispatches;
}

Dispatch bestDispatch() noexcept
{
    return configured::AvailableDispatches.back();
}

} // namespace isoline::fastinterpreter
