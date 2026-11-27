// SPDX-License-Identifier: Apache-2.0
// The macOS row of SystemJitMemory: mmap(MAP_JIT), pthread_jit_write_protect_np and munmap. A hardened
// runtime needs the com.apple.security.cs.allow-jit entitlement for MAP_JIT, on both architectures.
#include <sys/mman.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <utility>

#include <pthread.h>
#include <unistd.h>

#include <isoline/platform/JitMemory.hpp>
#include <isoline/platform/Platform.hpp>
#include <isoline/platform/ThisPlatform.hpp>
#include <isoline/platform/VirtualMemory.hpp>

namespace isoline::platform
{

static_assert(mapsJit(ThisPlatform.codeMemory), "JitMemoryMacOS.cpp is the row of platforms with MAP_JIT");

namespace
{

    /// A failure of @p error with the calling thread's errno.
    /// @param error Why.
    /// @return The failure.
    [[nodiscard]] std::unexpected<VirtualMemoryFailure> systemFailure(VirtualMemoryError error) noexcept
    {
        return std::unexpected { VirtualMemoryFailure { .error = error, .systemCode = errno } };
    }

    /// The mmap protection of @p mapping. Write and execute are both set only for ReadWriteExecute,
    /// which allowsJitMapping() grants to the per-thread toggle's row alone.
    /// @param mapping The mapping.
    /// @return Its PROT_* flags.
    [[nodiscard]] int protectionOf(JitMapping mapping) noexcept
    {
        auto const& row = JitMappingTable[std::to_underlying(mapping)];
        return PROT_READ | PROT_WRITE | (row.execute == PageAccess::Allowed ? PROT_EXEC : PROT_NONE);
    }

} // namespace

SystemJitMemory::SystemJitMemory() noexcept: _pageSize { static_cast<uint64_t>(sysconf(_SC_PAGESIZE)) }
{
}

std::expected<Region, VirtualMemoryFailure> SystemJitMemory::mapJit(uint64_t bytes,
                                                                    JitMapping mapping) noexcept
{
    detail::requireJitMapping(ThisPlatform.codeMemory, mapping);
    detail::requireReservationSize(bytes, _pageSize);
    return detail::systemSize<std::size_t>(bytes, VirtualMemoryError::ReserveRefused)
        .and_then([bytes, mapping](std::size_t size) -> std::expected<Region, VirtualMemoryFailure> {
            auto* const base =
                mmap(nullptr, size, protectionOf(mapping), MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
            if (base == MAP_FAILED)
                return systemFailure(VirtualMemoryError::ReserveRefused);
            return Region { .base = static_cast<std::byte*>(base), .bytes = bytes };
        });
}

void SystemJitMemory::setThreadAccess(JitAccess access) noexcept
{
    detail::requireJitThreadToggle(ThisPlatform.codeMemory);
    // Write protection on means the thread's view is executable; off, writable. Either way, for the
    // calling thread alone.
    pthread_jit_write_protect_np(access == JitAccess::Execute ? 1 : 0);
}

std::expected<void, VirtualMemoryFailure> SystemJitMemory::release(Region region) noexcept
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
