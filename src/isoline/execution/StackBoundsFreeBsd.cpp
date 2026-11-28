// SPDX-License-Identifier: Apache-2.0
// ThreadStackBounds on FreeBSD: the calling thread's stack as pthread_attr_get_np reports it (<pthread_np.h>,
// into attributes pthread_attr_init made), its guard kept out; Unavailable where the query fails.

#include <cstddef>
#include <cstdint>
#include <expected>

#include <pthread.h>
#include <pthread_np.h>

#include <isoline/execution/StackBounds.hpp>

namespace isoline::execution
{

std::expected<std::uintptr_t, StackBoundsError> ThreadStackBounds::lowestUsableAddress() const noexcept
{
    auto attributes = pthread_attr_t {};
    if (::pthread_attr_init(&attributes) != 0)
        return std::unexpected { StackBoundsError::Unavailable };
    void* lowest = nullptr;
    auto bytes = std::size_t { 0 };
    auto guard = std::size_t { 0 };
    auto const described = ::pthread_attr_get_np(::pthread_self(), &attributes) == 0
                           && ::pthread_attr_getstack(&attributes, &lowest, &bytes) == 0
                           && ::pthread_attr_getguardsize(&attributes, &guard) == 0;
    ::pthread_attr_destroy(&attributes);
    if (!described)
        return std::unexpected { StackBoundsError::Unavailable };
    return reinterpret_cast<std::uintptr_t>(lowest) + guard;
}

} // namespace isoline::execution
