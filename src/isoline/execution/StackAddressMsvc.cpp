// SPDX-License-Identifier: Apache-2.0
#include <bit>
#include <cstdint>

#include <intrin.h>

#include <isoline/execution/StackBounds.hpp>

namespace isoline::execution
{

std::uintptr_t currentStackAddress() noexcept
{
    return std::bit_cast<std::uintptr_t>(_AddressOfReturnAddress());
}

} // namespace isoline::execution
