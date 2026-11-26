// SPDX-License-Identifier: Apache-2.0

// Replaces the global operator new and delete with ones that count bytes, for the one test binary
// that links this file. Every block carries its size in a header ahead of the pointer handed out,
// so the unsized deletes can take it back. The over-aligned (std::align_val_t) forms are left to the
// implementation: they pair with each other, and Isoline allocates nothing over-aligned.

#include "AllocationCounter.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>

namespace isoline::testing
{

namespace
{
    /// The header ahead of each block: room for its size, keeping the block at the alignment operator new
    /// owes (max_align_t is only 8 on MSVC x64 and Apple arm64, where that is 16).
    constexpr auto HeaderBytes = std::size_t { __STDCPP_DEFAULT_NEW_ALIGNMENT__ };
    static_assert(HeaderBytes >= sizeof(std::size_t));

    AllocationCounts& counts() noexcept
    {
        static auto instance = AllocationCounts {};
        return instance;
    }

    void* allocate(std::size_t size) noexcept
    {
        if (size > std::numeric_limits<std::size_t>::max() - HeaderBytes)
            return nullptr;
        auto* const block = static_cast<std::byte*>(std::malloc(size + HeaderBytes));
        if (block == nullptr)
            return nullptr;
        std::memcpy(block, &size, sizeof(size));
        auto& current = counts();
        current.liveBytes += size;
        current.peakBytes = std::max(current.peakBytes, current.liveBytes);
        return block + HeaderBytes;
    }

    void* allocateOrThrow(std::size_t size)
    {
        if (auto* const pointer = allocate(size))
            return pointer;
        throw std::bad_alloc {};
    }

    void release(void* pointer) noexcept
    {
        if (pointer == nullptr)
            return;
        auto* const block = static_cast<std::byte*>(pointer) - HeaderBytes;
        auto size = std::size_t { 0 };
        std::memcpy(&size, block, sizeof(size));
        counts().liveBytes -= size;
        std::free(block);
    }
} // namespace

AllocationCounts allocationCounts() noexcept
{
    return counts();
}

void resetAllocationPeak() noexcept
{
    counts().peakBytes = counts().liveBytes;
}

} // namespace isoline::testing

void* operator new(std::size_t size)
{
    return isoline::testing::allocateOrThrow(size);
}

void* operator new[](std::size_t size)
{
    return isoline::testing::allocateOrThrow(size);
}

void* operator new(std::size_t size, std::nothrow_t const& /*tag*/) noexcept
{
    return isoline::testing::allocate(size);
}

void* operator new[](std::size_t size, std::nothrow_t const& /*tag*/) noexcept
{
    return isoline::testing::allocate(size);
}

void operator delete(void* pointer) noexcept
{
    isoline::testing::release(pointer);
}

void operator delete[](void* pointer) noexcept
{
    isoline::testing::release(pointer);
}

void operator delete(void* pointer, std::size_t /*size*/) noexcept
{
    isoline::testing::release(pointer);
}

void operator delete[](void* pointer, std::size_t /*size*/) noexcept
{
    isoline::testing::release(pointer);
}

void operator delete(void* pointer, std::nothrow_t const& /*tag*/) noexcept
{
    isoline::testing::release(pointer);
}

void operator delete[](void* pointer, std::nothrow_t const& /*tag*/) noexcept
{
    isoline::testing::release(pointer);
}
