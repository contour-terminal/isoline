// SPDX-License-Identifier: Apache-2.0
#include <isoline/component/canonical/Trap.hpp>

#include <core/Assert.hpp>

#include <cstddef>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

#include <isoline/api/Types.hpp>

namespace isoline::component::canonical
{

std::string Trap::message() const
{
    auto const format = trapCodeRow(code).format;
    Require(arguments.size() == placeholderCount(format));
    // The pieces between the placeholders, each followed by the next argument but the last.
    auto text = std::string {};
    auto index = std::size_t { 0 };
    for (auto const piece: std::views::split(format, std::string_view { "{}" }))
    {
        if (index != 0)
            text += arguments[index - 1];
        text.append(piece.begin(), piece.end());
        ++index;
    }
    return text;
}

Trap fromCore(isoline::api::Trap const& trap)
{
    if (trap.code == isoline::api::TrapCode::Interrupted)
        return trapOf(TrapCode::Interrupted);
    auto result = trapOf(TrapCode::Core, isoline::api::specMessage(trap));
    result.core = trap.code;
    return result;
}

void PendingTrap::raise(Trap trap) noexcept
{
    if (!_trap)
        _trap = std::move(trap);
}

std::optional<Trap> PendingTrap::take() noexcept
{
    return std::exchange(_trap, std::nullopt);
}

} // namespace isoline::component::canonical
