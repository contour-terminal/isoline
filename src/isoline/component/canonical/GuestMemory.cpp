// SPDX-License-Identifier: Apache-2.0
#include <isoline/component/canonical/GuestMemory.hpp>

#include <core/Assert.hpp>

#include <bit>
#include <cstdint>
#include <expected>

#include <isoline/component/canonical/Trap.hpp>

namespace isoline::component::canonical
{

std::expected<void, Trap> checkRange(uint32_t address,
                                     uint64_t length,
                                     uint32_t alignment,
                                     uint64_t memorySize)
{
    Require(std::has_single_bit(alignment));
    if (address % alignment != 0)
        return std::unexpected { trapOf(TrapCode::UnalignedPointer, address, alignment) };
    if (length > memorySize || address > memorySize - length)
        return std::unexpected { trapOf(TrapCode::OutOfBounds, length, address, memorySize) };
    return {};
}

} // namespace isoline::component::canonical
