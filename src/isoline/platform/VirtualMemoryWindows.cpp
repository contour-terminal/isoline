// SPDX-License-Identifier: Apache-2.0
// The Windows row of SystemVirtualMemory: VirtualAlloc, VirtualProtect and VirtualFree.
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <utility>

#include <isoline/meta/Check.hpp>
#include <isoline/platform/Platform.hpp>
#include <isoline/platform/ThisPlatform.hpp>
#include <isoline/platform/VirtualMemory.hpp>

// windows.h defines min and max as macros, which break std::numeric_limits<…>::max() and std::min
// wherever they follow it; NOMINMAX leaves them out.
#define NOMINMAX
#include <windows.h>

namespace isoline::platform
{

static_assert(ThisPlatform.system == OperatingSystem::Windows, "VirtualMemoryWindows.cpp is the Windows row");
static_assert(ThisPlatform.branchTargets == BranchTargetProtectionKind::None,
              "Windows has no page protection that enforces branch targets");

namespace
{

    /// The PAGE_* constant of @p row (which is never writable and executable at once).
    /// @param row The protection's row.
    /// @return Its page protection.
    [[nodiscard]] constexpr DWORD pageProtectionOf(ProtectionRow const& row) noexcept
    {
        if (row.execute == PageAccess::Allowed)
            return row.read == PageAccess::Allowed ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
        if (row.write == PageAccess::Allowed)
            return PAGE_READWRITE;
        if (row.read == PageAccess::Allowed)
            return PAGE_READONLY;
        return PAGE_NOACCESS;
    }

    /// A failure of @p error with the calling thread's last error.
    /// @param error Why.
    /// @return The failure.
    [[nodiscard]] std::unexpected<VirtualMemoryFailure> systemFailure(VirtualMemoryError error) noexcept
    {
        return std::unexpected { VirtualMemoryFailure { .error = error,
                                                        .systemCode = static_cast<int>(GetLastError()) } };
    }

    /// The system's page size and allocation granularity.
    /// @return Them.
    [[nodiscard]] SYSTEM_INFO systemInfo() noexcept
    {
        auto info = SYSTEM_INFO {};
        GetSystemInfo(&info);
        return info;
    }

    /// The reservation @p address lies in: its allocation base and the extent of every region of it.
    /// @param address An address.
    /// @return The reservation; none when @p address lies in none.
    [[nodiscard]] std::optional<Region> reservationAt(std::byte* address) noexcept
    {
        auto info = MEMORY_BASIC_INFORMATION {};
        if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info) || info.State == MEM_FREE)
            return std::nullopt;
        auto* const base = static_cast<std::byte*>(info.AllocationBase);
        auto* cursor = base;
        while (VirtualQuery(cursor, &info, sizeof(info)) == sizeof(info) && info.AllocationBase == base)
            cursor = static_cast<std::byte*>(info.BaseAddress) + info.RegionSize;
        return Region { .base = base, .bytes = static_cast<uint64_t>(cursor - base) };
    }

} // namespace

SystemVirtualMemory::SystemVirtualMemory() noexcept: SystemVirtualMemory { ThisPlatform.decommit }
{
}

SystemVirtualMemory::SystemVirtualMemory(DecommitKind decommit) noexcept:
    _pageSize { systemInfo().dwPageSize },
    _allocationGranularity { systemInfo().dwAllocationGranularity },
    _decommit { decommit }
{
    ISOLINE_PRECONDITION(decommitsOnThisSystem(decommit));
}

std::expected<Region, VirtualMemoryFailure> SystemVirtualMemory::reserve(uint64_t bytes) noexcept
{
    detail::requireReservationSize(bytes, _allocationGranularity);
    return detail::systemSize<SIZE_T>(bytes, VirtualMemoryError::ReserveRefused)
        .and_then([bytes](SIZE_T size) -> std::expected<Region, VirtualMemoryFailure> {
            auto* const base = VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS);
            if (base == nullptr)
                return systemFailure(VirtualMemoryError::ReserveRefused);
            return Region { .base = static_cast<std::byte*>(base), .bytes = bytes };
        });
}

std::expected<void, VirtualMemoryFailure> SystemVirtualMemory::protect(Region region,
                                                                       Protection protection) noexcept
{
    detail::requirePageRange(region, _pageSize);
    return detail::systemSize<SIZE_T>(region.bytes, VirtualMemoryError::ProtectRefused)
        .and_then([region, protection](SIZE_T bytes) -> std::expected<void, VirtualMemoryFailure> {
            auto const& row = protectionRow(protection);
            if (row.branchTargets == BranchTargetEnforcement::On)
                return std::unexpected { VirtualMemoryFailure {
                    .error = VirtualMemoryError::BranchTargetsUnsupported } };
            switch (row.commitment)
            {
                case Commitment::Decommit:
                    if (VirtualFree(region.base, bytes, MEM_DECOMMIT) == 0)
                        return systemFailure(VirtualMemoryError::ProtectRefused);
                    return {};
                case Commitment::Commit:
                    if (VirtualAlloc(region.base, bytes, MEM_COMMIT, pageProtectionOf(row)) == nullptr)
                        return systemFailure(VirtualMemoryError::ProtectRefused);
                    return {};
                case Commitment::Retain: {
                    auto previous = DWORD { 0 };
                    if (VirtualProtect(region.base, bytes, pageProtectionOf(row), &previous) == 0)
                        return systemFailure(VirtualMemoryError::ProtectRefused);
                    return {};
                }
            }
            std::unreachable();
        });
}

std::expected<void, VirtualMemoryFailure> SystemVirtualMemory::release(Region region) noexcept
{
    detail::requirePageRange(region, _pageSize);
    // MEM_RELEASE frees the whole reservation region.base begins and takes no size, so a part of one
    // would release all of it: the region must be the whole reservation. An address in none is left
    // to VirtualFree to refuse.
    if (auto const reservation = reservationAt(region.base))
        detail::requireWholeReservation(region, *reservation);
    if (VirtualFree(region.base, 0, MEM_RELEASE) == 0)
        return systemFailure(VirtualMemoryError::ReleaseRefused);
    return {};
}

} // namespace isoline::platform
