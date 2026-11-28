// SPDX-License-Identifier: Apache-2.0
// ThreadStackBounds on macOS: the calling thread's stack top and size as pthread reports them, one page kept
// out for the guard below it.

#include <cstdint>
#include <expected>

#include <pthread.h>
#include <unistd.h>

#include <isoline/execution/StackBounds.hpp>

namespace isoline::execution
{

std::expected<std::uintptr_t, StackBoundsError> ThreadStackBounds::lowestUsableAddress() const noexcept
{
    auto const self = ::pthread_self();
    auto const top = reinterpret_cast<std::uintptr_t>(::pthread_get_stackaddr_np(self));
    auto const bytes = static_cast<std::uintptr_t>(::pthread_get_stacksize_np(self));
    return top - bytes + static_cast<std::uintptr_t>(::getpagesize());
}

} // namespace isoline::execution
