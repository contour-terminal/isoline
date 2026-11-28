// SPDX-License-Identifier: Apache-2.0
// ThreadStackBounds on Windows: the calling thread's reservation as GetCurrentThreadStackLimits reports it,
// its lowest pages kept out for the guard page and the stack-overflow guarantee.

#include <cstdint>
#include <expected>

#include <windows.h>

#include <isoline/execution/StackBounds.hpp>

namespace isoline::execution
{

namespace
{
    /// What the bottom of a Windows thread stack keeps from the thread: the page that never commits, the
    /// guard page above it and the guarantee the stack-overflow exception runs on (SetThreadStackGuarantee),
    /// each a few pages; 64 KiB covers them on every Windows.
    constexpr auto ReservationGuardBytes = std::uintptr_t { 64 } * 1024;
} // namespace

std::expected<std::uintptr_t, StackBoundsError> ThreadStackBounds::lowestUsableAddress() const noexcept
{
    auto low = ULONG_PTR {};
    auto high = ULONG_PTR {};
    ::GetCurrentThreadStackLimits(&low, &high);
    return static_cast<std::uintptr_t>(low) + ReservationGuardBytes;
}

} // namespace isoline::execution
