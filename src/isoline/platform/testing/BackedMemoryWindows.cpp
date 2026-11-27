// SPDX-License-Identifier: Apache-2.0
// The Windows row of backedBytes(): VirtualQuery's committed regions.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include <isoline/platform/testing/BackedMemory.hpp>

// windows.h defines min and max as macros, which break std::numeric_limits<…>::max() and std::min
// wherever they follow it; NOMINMAX leaves them out.
#define NOMINMAX
#include <windows.h>

namespace isoline::platform::testing
{

uint64_t backedBytes(Region region, uint64_t /*pageSize*/)
{
    auto committed = uint64_t { 0 };
    auto* cursor = region.base;
    auto* const end = region.base + region.bytes;
    while (cursor < end)
    {
        auto info = MEMORY_BASIC_INFORMATION {};
        REQUIRE(VirtualQuery(cursor, &info, sizeof(info)) == sizeof(info));
        auto* const next = std::min(end, static_cast<std::byte*>(info.BaseAddress) + info.RegionSize);
        if (info.State == MEM_COMMIT)
            committed += static_cast<uint64_t>(next - cursor);
        cursor = next;
    }
    return committed;
}

} // namespace isoline::platform::testing
