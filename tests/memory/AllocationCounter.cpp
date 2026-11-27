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
#include <stdexcept>
#include <utility>

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

    /// The most live bytes an allocation may leave; the default refuses nothing.
    std::size_t& liveLimit() noexcept
    {
        static auto limit = std::numeric_limits<std::size_t>::max();
        return limit;
    }

    /// What the throwing operator new throws for a refused allocation.
    RefusalException& refusalException() noexcept
    {
        static auto refusal = RefusalException::BadAlloc;
        return refusal;
    }

    void* allocate(std::size_t size) noexcept
    {
        if (size > std::numeric_limits<std::size_t>::max() - HeaderBytes)
            return nullptr;
        if (size > liveLimit() - std::min(liveLimit(), counts().liveBytes))
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

    /// The std::length_error a refused allocation throws. Its message is allocated, past the budget,
    /// so the budget is lifted while it is built, or building it would recurse.
    std::length_error lengthError()
    {
        auto const limit = std::exchange(liveLimit(), std::numeric_limits<std::size_t>::max());
        auto error = std::length_error { "allocation refused" };
        liveLimit() = limit;
        return error;
    }

    void* allocateOrThrow(std::size_t size)
    {
        if (auto* const pointer = allocate(size))
            return pointer;
        if (refusalException() == RefusalException::LengthError)
            throw lengthError();
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

AllocationLimit::AllocationLimit(std::size_t extraBytes, RefusalException refusal) noexcept:
    _previous { liveLimit() }, _previousRefusal { refusalException() }
{
    refusalException() = refusal;
    auto const live = counts().liveBytes;
    liveLimit() = extraBytes > std::numeric_limits<std::size_t>::max() - live
                      ? std::numeric_limits<std::size_t>::max()
                      : live + extraBytes;
}

AllocationLimit::~AllocationLimit()
{
    liveLimit() = _previous;
    refusalException() = _previousRefusal;
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
