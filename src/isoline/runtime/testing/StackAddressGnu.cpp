// SPDX-License-Identifier: Apache-2.0
#include <bit>
#include <cstdint>

#include <isoline/runtime/testing/StackAddress.hpp>

namespace isoline::runtime::testing
{

std::uintptr_t stackAddress() noexcept
{
    return std::bit_cast<std::uintptr_t>(__builtin_frame_address(0));
}

} // namespace isoline::runtime::testing
