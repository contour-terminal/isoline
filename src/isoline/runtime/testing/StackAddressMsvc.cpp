// SPDX-License-Identifier: Apache-2.0
#include <bit>
#include <cstdint>

#include <intrin.h>

#include <isoline/runtime/testing/StackAddress.hpp>

namespace isoline::runtime::testing
{

std::uintptr_t stackAddress() noexcept
{
    return std::bit_cast<std::uintptr_t>(_AddressOfReturnAddress());
}

} // namespace isoline::runtime::testing
