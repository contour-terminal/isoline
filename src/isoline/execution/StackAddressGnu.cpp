// SPDX-License-Identifier: Apache-2.0
#include <bit>
#include <cstdint>

#include <isoline/execution/StackBounds.hpp>

namespace isoline::execution
{

std::uintptr_t currentStackAddress() noexcept
{
    return std::bit_cast<std::uintptr_t>(__builtin_frame_address(0));
}

} // namespace isoline::execution
