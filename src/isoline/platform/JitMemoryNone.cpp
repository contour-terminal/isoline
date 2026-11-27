// SPDX-License-Identifier: Apache-2.0
// The row of every platform without MAP_JIT (Linux, FreeBSD, Windows): its code memory reserves, writes
// and then seals with IVirtualMemory, and never reaches IJitMemory, so every call here is a contract
// violation; should the violation handler return, the call still fails.
#include <cstdint>
#include <expected>

#include <isoline/platform/JitMemory.hpp>
#include <isoline/platform/Platform.hpp>
#include <isoline/platform/ThisPlatform.hpp>
#include <isoline/platform/VirtualMemory.hpp>

namespace isoline::platform
{

static_assert(!mapsJit(ThisPlatform.codeMemory), "JitMemoryNone.cpp is the row of platforms without MAP_JIT");

SystemJitMemory::SystemJitMemory() noexcept = default;

// uncovered-begin(jit-memory-none-violated)
std::expected<Region, VirtualMemoryFailure> SystemJitMemory::mapJit(uint64_t /*bytes*/,
                                                                    JitMapping mapping) noexcept
{
    detail::requireJitMapping(ThisPlatform.codeMemory, mapping);
    return std::unexpected { VirtualMemoryFailure { .error = VirtualMemoryError::ReserveRefused } };
}

void SystemJitMemory::setThreadAccess(JitAccess /*access*/) noexcept
{
    detail::requireJitThreadToggle(ThisPlatform.codeMemory);
}

std::expected<void, VirtualMemoryFailure> SystemJitMemory::release(Region /*region*/) noexcept
{
    // No region came from mapJit() here: there is no MAP_JIT to map one with.
    detail::requireJitMapping(ThisPlatform.codeMemory, JitMapping::ReadWrite);
    return std::unexpected { VirtualMemoryFailure { .error = VirtualMemoryError::ReleaseRefused } };
}
// uncovered-end(jit-memory-none-violated)

} // namespace isoline::platform
