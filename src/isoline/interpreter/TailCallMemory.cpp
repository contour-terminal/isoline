// SPDX-License-Identifier: Apache-2.0
#include <array>

#include <isoline/interpreter/TailCall.hpp>

namespace isoline::interpreter
{

std::array<Handler, OperationCount> const& memoryHandlers() noexcept
{
    static constexpr auto Handlers = familyHandlers<OperationFamily::Variable, OperationFamily::Memory>();
    return Handlers;
}

} // namespace isoline::interpreter
