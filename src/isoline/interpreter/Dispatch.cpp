// SPDX-License-Identifier: Apache-2.0
#include <isoline/interpreter/Dispatch.hpp>

#include <algorithm>
#include <optional>
#include <span>
#include <string_view>

#include <isoline/interpreter/DispatchAvailability.hpp>

namespace isoline::interpreter
{

std::span<Dispatch const> availableDispatches() noexcept
{
    return configured::AvailableDispatches;
}

Dispatch bestDispatch() noexcept
{
    return configured::AvailableDispatches.back();
}

bool isAvailable(Dispatch dispatch) noexcept
{
    return std::ranges::find(configured::AvailableDispatches, dispatch)
           != configured::AvailableDispatches.end();
}

std::optional<Dispatch> dispatchNamed(std::string_view name) noexcept
{
    auto const found = std::ranges::find(configured::DispatchNames, name, &DispatchName::name);
    return found == configured::DispatchNames.end() ? std::nullopt : std::optional { found->dispatch };
}

} // namespace isoline::interpreter
