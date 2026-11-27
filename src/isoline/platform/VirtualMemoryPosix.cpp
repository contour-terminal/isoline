// SPDX-License-Identifier: Apache-2.0
// The POSIX row of SystemVirtualMemory (Linux, macOS, FreeBSD): mmap, mprotect, madvise and munmap.
#include <sys/mman.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <utility>

#include <unistd.h>

#include <isoline/meta/Check.hpp>
#include <isoline/platform/Platform.hpp>
#include <isoline/platform/ThisPlatform.hpp>
#include <isoline/platform/VirtualMemory.hpp>

namespace isoline::platform
{

static_assert(ThisPlatform.system != OperatingSystem::Windows, "VirtualMemoryPosix.cpp is the POSIX row");

namespace
{

    /// This build's branch-target flag.
    constexpr int ThisBranchTargetFlag =
        BranchTargetProtectionFlags[std::to_underlying(ThisPlatform.branchTargets)].flag;

    /// The mmap/mprotect flags of @p row (which is never writable and executable at once), but for
    /// branch-target enforcement. Write and execute are exclusive branches, so that no path combines them.
    /// @param row The protection's row.
    /// @return Its PROT_* flags.
    [[nodiscard]] constexpr int accessFlagsOf(ProtectionRow const& row) noexcept
    {
        auto const read = row.read == PageAccess::Allowed ? PROT_READ : PROT_NONE;
        if (row.execute == PageAccess::Allowed)
            return read | PROT_EXEC;
        if (row.write == PageAccess::Allowed)
            return read | PROT_WRITE;
        return read;
    }

    /// A failure of @p error with the calling thread's errno.
    /// @param error Why.
    /// @return The failure.
    [[nodiscard]] std::unexpected<VirtualMemoryFailure> systemFailure(VirtualMemoryError error) noexcept
    {
        return std::unexpected { VirtualMemoryFailure { .error = error, .systemCode = errno } };
    }

    /// Decommits @p bytes at @p base by @p decommit, a POSIX row's mechanism (the constructor refuses
    /// Windows').
    /// @param base The first page.
    /// @param bytes The size, whole pages.
    /// @param decommit The mechanism.
    /// @return Nothing; or ProtectRefused with errno.
    [[nodiscard]] std::expected<void, VirtualMemoryFailure> decommitPages(std::byte* base,
                                                                          std::size_t bytes,
                                                                          DecommitKind decommit) noexcept
    {
        if (decommit == DecommitKind::MapFixedAnonymous)
        {
            // A fresh mapping in place replaces the pages, and their contents with them.
            auto* const mapped = mmap(base, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
            if (mapped == MAP_FAILED)
                return systemFailure(VirtualMemoryError::ProtectRefused);
            return {};
        }
        // MadviseDontNeed: on a private anonymous mapping it drops the pages, so the next commit reads
        // zeroes.
        if (mprotect(base, bytes, PROT_NONE) != 0)
            return systemFailure(VirtualMemoryError::ProtectRefused);
        if (madvise(base, bytes, MADV_DONTNEED) != 0)
            return systemFailure(VirtualMemoryError::ProtectRefused); // uncovered(posix-madvise-refused)
        return {};
    }

} // namespace

SystemVirtualMemory::SystemVirtualMemory() noexcept: SystemVirtualMemory { ThisPlatform.decommit }
{
}

SystemVirtualMemory::SystemVirtualMemory(DecommitKind decommit) noexcept:
    _pageSize { static_cast<uint64_t>(sysconf(_SC_PAGESIZE)) },
    _allocationGranularity { _pageSize },
    _decommit { decommit }
{
    ISOLINE_PRECONDITION(decommitsOnThisSystem(decommit));
}

std::expected<Region, VirtualMemoryFailure> SystemVirtualMemory::reserve(uint64_t bytes) noexcept
{
    detail::requireReservationSize(bytes, _allocationGranularity);
    return detail::systemSize<std::size_t>(bytes, VirtualMemoryError::ReserveRefused)
        .and_then([bytes](std::size_t size) -> std::expected<Region, VirtualMemoryFailure> {
            // A private PROT_NONE mapping is charged to no commit limit: only pages made writable are.
            auto* const base = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (base == MAP_FAILED)
                return systemFailure(VirtualMemoryError::ReserveRefused);
            return Region { .base = static_cast<std::byte*>(base), .bytes = bytes };
        });
}

std::expected<void, VirtualMemoryFailure> SystemVirtualMemory::protect(Region region,
                                                                       Protection protection) noexcept
{
    detail::requirePageRange(region, _pageSize);
    return detail::systemSize<std::size_t>(region.bytes, VirtualMemoryError::ProtectRefused)
        .and_then([this, region, protection](std::size_t size) -> std::expected<void, VirtualMemoryFailure> {
            auto const& row = protectionRow(protection);
            if (row.commitment == Commitment::Decommit)
                return decommitPages(region.base, size, _decommit);
            auto const enforcesBranchTargets = row.branchTargets == BranchTargetEnforcement::On;
            if (enforcesBranchTargets && ThisBranchTargetFlag == 0)
                return std::unexpected { VirtualMemoryFailure {
                    .error = VirtualMemoryError::BranchTargetsUnsupported } };
            auto const flags = accessFlagsOf(row) | (enforcesBranchTargets ? ThisBranchTargetFlag : 0);
            if (mprotect(region.base, size, flags) != 0)
            {
                // A kernel without BTI support refuses PROT_BTI as an unknown flag.
                auto const error = enforcesBranchTargets && errno == EINVAL
                                       ? VirtualMemoryError::BranchTargetsUnsupported
                                       : VirtualMemoryError::ProtectRefused;
                return systemFailure(error);
            }
            return {};
        });
}

std::expected<void, VirtualMemoryFailure> SystemVirtualMemory::release(Region region) noexcept
{
    detail::requirePageRange(region, _pageSize);
    return detail::systemSize<std::size_t>(region.bytes, VirtualMemoryError::ReleaseRefused)
        .and_then([region](std::size_t size) -> std::expected<void, VirtualMemoryFailure> {
            if (munmap(region.base, size) != 0)
                return systemFailure(VirtualMemoryError::ReleaseRefused);
            return {};
        });
}

} // namespace isoline::platform
