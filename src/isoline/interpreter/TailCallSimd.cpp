// SPDX-License-Identifier: Apache-2.0
#include <array>

#include <isoline/interpreter/TailCall.hpp>

namespace isoline::interpreter
{

std::array<Handler, OperationCount> const& simdHandlers() noexcept
{
    static constexpr auto Handlers = familyHandlers<OperationFamily::Simd>();
    return Handlers;
}

} // namespace isoline::interpreter
