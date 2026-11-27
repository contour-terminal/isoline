// SPDX-License-Identifier: Apache-2.0
// The POSIX row of backedBytes(): mincore's residency vector.
#include <catch2/catch_test_macros.hpp>

#include <sys/mman.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

#include <isoline/platform/testing/BackedMemory.hpp>

namespace isoline::platform::testing
{

namespace
{

    /// The element type of mincore()'s vector: unsigned char on Linux, char on macOS and FreeBSD.
    template <typename Result, typename Address, typename Length, typename Element>
    Element vectorElementOf(Result (*)(Address, Length, Element*));

    /// mincore()'s vector element on this system.
    using ResidencyByte = decltype(vectorElementOf(&mincore));

} // namespace

uint64_t backedBytes(Region region, uint64_t pageSize)
{
    auto residency = std::vector<ResidencyByte>(static_cast<std::size_t>(region.bytes / pageSize));
    REQUIRE(mincore(region.base, static_cast<std::size_t>(region.bytes), residency.data()) == 0);
    auto const resident =
        std::ranges::count_if(residency, [](ResidencyByte page) { return (page & 1) != 0; });
    return static_cast<uint64_t>(resident) * pageSize;
}

} // namespace isoline::platform::testing
