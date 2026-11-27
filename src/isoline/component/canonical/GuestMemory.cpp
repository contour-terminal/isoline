// SPDX-License-Identifier: Apache-2.0
#include <isoline/component/canonical/GuestMemory.hpp>

#include <core/Assert.hpp>

#include <bit>
#include <cstdint>
#include <expected>
#include <format>

#include <isoline/component/canonical/Trap.hpp>

namespace isoline::component::canonical
{

Trap outOfBounds(uint32_t address, uint64_t length, uint64_t memorySize)
{
    return trapOf(TrapCode::OutOfBounds,
                  std::format("{} bytes at {} pass its end at {}", length, address, memorySize));
}

std::expected<void, Trap> checkRange(uint32_t address,
                                     uint64_t length,
                                     uint32_t alignment,
                                     uint64_t memorySize)
{
    Require(std::has_single_bit(alignment));
    if (address % alignment != 0)
        return std::unexpected { trapOf(TrapCode::UnalignedPointer, address, alignment) };
    if (length > memorySize || address > memorySize - length)
        return std::unexpected { outOfBounds(address, length, memorySize) };
    return {};
}

std::expected<uint32_t, Trap> reallocate(
    IGuestMemoryAccess& memory, uint32_t old, uint32_t oldSize, uint32_t alignment, uint32_t newSize)
{
    return memory.reallocate(old, oldSize, alignment, newSize).and_then([&](uint32_t block) {
        return checkRange(block, newSize, alignment, memory.size()).transform([block] { return block; });
    });
}

} // namespace isoline::component::canonical
