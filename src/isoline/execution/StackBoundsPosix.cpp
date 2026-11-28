// SPDX-License-Identifier: Apache-2.0
// ThreadStackBounds on Linux: the calling thread's stack as pthread_getattr_np reports it, its guard kept
// out.

#include <cstddef>
#include <cstdint>
#include <expected>

#include <pthread.h>

#include <isoline/execution/StackBounds.hpp>

namespace isoline::execution
{

std::expected<std::uintptr_t, StackBoundsError> ThreadStackBounds::lowestUsableAddress() const noexcept
{
    // glibc reads the main thread's stack from /proc/self/maps: a sandbox without /proc fails the query.
    auto attributes = pthread_attr_t {};
    if (::pthread_getattr_np(::pthread_self(), &attributes) != 0)
        return std::unexpected { StackBoundsError::Unavailable }; // uncovered(stack-query-refused)
    void* lowest = nullptr;
    auto bytes = std::size_t { 0 };
    auto guard = std::size_t { 0 };
    auto const described = ::pthread_attr_getstack(&attributes, &lowest, &bytes) == 0
                           && ::pthread_attr_getguardsize(&attributes, &guard) == 0;
    ::pthread_attr_destroy(&attributes);
    if (!described)
        return std::unexpected { StackBoundsError::Unavailable }; // uncovered(stack-query-refused)
    // The reported stack excludes the guard already on glibc; keeping one more guard's bytes out costs a page
    // and holds wherever a C library counts it in.
    return reinterpret_cast<std::uintptr_t>(lowest) + guard;
}

} // namespace isoline::execution
