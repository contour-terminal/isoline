// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <sys/mman.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <isoline/binary/Types.hpp>
#include <isoline/platform/VirtualMemory.hpp>
#include <isoline/runtime/LinearMemory.hpp>
#include <isoline/runtime/MemoryLayout.hpp>
#include <isoline/runtime/ReservedMemoryAllocator.hpp>

namespace
{

/// The element type of mincore()'s vector: unsigned char on Linux, char on macOS and FreeBSD.
template <typename Function>
struct MincoreVector;

template <typename Result, typename Address, typename Length, typename Element>
struct MincoreVector<Result (*)(Address, Length, Element*)>
{
    using ElementType = Element; ///< The element type.
};

template <typename Result, typename Address, typename Length, typename Element>
struct MincoreVector<Result (*)(Address, Length, Element*) noexcept>
{
    using ElementType = Element; ///< The element type.
};

/// The resident bytes of the @p bytes from @p base, as the kernel reports them page by page.
[[nodiscard]] uint64_t residentBytes(std::byte* base, uint64_t bytes, uint64_t pageSize)
{
    using Element = MincoreVector<decltype(&::mincore)>::ElementType;
    auto pages = std::vector<Element>(static_cast<std::size_t>(bytes / pageSize));
    REQUIRE(::mincore(base, static_cast<std::size_t>(bytes), pages.data()) == 0);
    auto resident = uint64_t { 0 };
    for (auto const page: pages)
        if ((static_cast<unsigned>(page) & 1U) != 0)
            resident += pageSize;
    return resident;
}

} // namespace

TEST_CASE("ReservedMemory.wasmtimeParityStaysSmallInMemory", "[guardpages]")
{
    // A memory of the parity layout reaches 4 GiB + 32 MiB, but only what it spans is ever memory: its
    // first page, written whole, and nothing of the rest.
    auto system = isoline::platform::SystemVirtualMemory {};
    auto const parity = isoline::runtime::layoutOf(
        isoline::runtime::LayoutRow::WasmtimeParity, 4ULL << 30, system.pageSize());
    auto allocator = isoline::runtime::ReservedMemoryAllocator { system, parity };
    auto const page = isoline::binary::pageSize(isoline::binary::MemoryType {});
    auto const type = isoline::binary::MemoryType { .limits = { .min = 1, .max = 65'536 } };
    auto memory = allocator.allocate(isoline::runtime::MemoryRequest {
        .type = type, .initialBytes = page, .maximumBytes = 65'536 * page });
    REQUIRE(memory.has_value());
    auto& bytes = **memory;
    for (auto& value: bytes.bytes())
        value = std::byte { 1 };

    auto const reach = bytes.bytes().size() + bytes.guardBytes();
    REQUIRE(reach == (4ULL << 30) + (32ULL << 20));
    auto const resident = residentBytes(bytes.bytes().data(), reach, system.pageSize());
    CHECK(resident >= page);
    CHECK(resident < (2ULL << 20));
}
