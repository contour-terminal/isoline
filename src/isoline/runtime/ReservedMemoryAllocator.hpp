// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <memory>

#include <isoline/platform/VirtualMemory.hpp>
#include <isoline/runtime/Limits.hpp>
#include <isoline/runtime/LinearMemory.hpp>
#include <isoline/runtime/MemoryLayout.hpp>

namespace isoline::runtime
{

/// Guard-page memories: each reserves max(reservation, maximumBytes) + guard, commits what it spans, grows
/// in place, never moves.
///
/// Every memory reserves, as inaccessible address space, its layout's reservation or its request's
/// maximumBytes, whichever is larger, rounded up to whole pages, and the layout's guard after it, the sum
/// rounded up to the allocation granularity. It commits the pages its bytes span (zeroed when first
/// committed), commits more in place on every grow, and releases the reservation when it is destroyed. Its
/// base therefore never moves. For a memory of 64 KiB pages, guardBytes() is the reservation past the
/// committed pages: at least the layout's guard at every size up to the maximum, and bytes().size() +
/// guardBytes() at least reservation + guard, so it satisfies memoryRequirementsFor() after every grow. A
/// memory64 or custom-page request, whose requirements are {0, 0}, is served by the same rule, so one
/// allocator serves a mixed module; a custom-page memory whose size ends mid-page declares a guard of 0,
/// since the committed rest of that page follows its bytes.
///
/// A size whose reservation is no u64, a refused reservation and a refused commit are each HostMemory;
/// a refused grow leaves the memory as it was.
class ReservedMemoryAllocator final: public IMemoryAllocator
{
  public:
    /// @param virtualMemory The address space memories are reserved in; must outlive the allocator and
    ///        every memory it allocates.
    /// @param layout The engine's layout; well-formed (checked: isWellFormed()).
    ReservedMemoryAllocator(platform::IVirtualMemory& virtualMemory, MemoryLayout layout) noexcept;

    /// Reserves and commits one memory of @p request's initialBytes, zeroed.
    /// @param request What the memory is and may become.
    /// @return The memory; or HostMemory when its reservation is no u64, or the system refuses to reserve
    ///         it or to commit its initial bytes (nothing stays reserved then).
    [[nodiscard]] std::expected<std::unique_ptr<ILinearMemory>, Refusal> allocate(
        MemoryRequest const& request) noexcept override;

  private:
    platform::IVirtualMemory* _virtualMemory; ///< Where memories are reserved.
    MemoryLayout _layout;                     ///< The engine's layout.
};

} // namespace isoline::runtime
