// SPDX-License-Identifier: Apache-2.0
#include <isoline/platform/VirtualMemory.hpp>

#include <cstdint>

#include <isoline/meta/Check.hpp>

namespace isoline::platform::detail
{

void requireReservationSize(uint64_t bytes, uint64_t granularity) noexcept
{
    ISOLINE_PRECONDITION(bytes > 0);
    ISOLINE_PRECONDITION(bytes % granularity == 0);
}

void requirePageRange(Region const& region, uint64_t pageSize) noexcept
{
    ISOLINE_PRECONDITION(region.bytes > 0);
    ISOLINE_PRECONDITION(region.bytes % pageSize == 0);
    ISOLINE_PRECONDITION(reinterpret_cast<uintptr_t>(region.base) % pageSize == 0);
}

void requireWholeReservation(Region const& region, Region const& reservation) noexcept
{
    ISOLINE_PRECONDITION(region == reservation);
}

} // namespace isoline::platform::detail
