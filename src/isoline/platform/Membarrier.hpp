// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <memory>

namespace isoline::platform
{

/// Linux's membarrier(2) system call, its SYNC_CORE commands only: the one way a process makes every
/// other thread of it execute a context-synchronizing event, so that code another thread just wrote is
/// what they fetch. Reached only through this interface, so that a test can inject a refusal.
class IMembarrier
{
  public:
    virtual ~IMembarrier() = default;

    /// Registers the process for MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE, which it must do once
    /// before synchronizeCore(); before any lock-down (seccomp) that would forbid it.
    /// @return Nothing; or the system's errno: ENOSYS without membarrier(2), EINVAL without SYNC_CORE
    ///         on this kernel or architecture, EPERM where a sandbox refuses it.
    [[nodiscard]] virtual std::expected<void, int> registerSynchronizeCore() noexcept = 0;

    /// MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE: returns once every other running thread of the
    /// process has executed a context-synchronizing event.
    /// @return Nothing; or the system's errno: EPERM before registerSynchronizeCore(), EINVAL without
    ///         SYNC_CORE, ENOSYS without membarrier(2).
    [[nodiscard]] virtual std::expected<void, int> synchronizeCore() noexcept = 0;
};

/// This build's membarrier, by its platform row (the CMake-selected Membarrier{Linux,None}.cpp): the
/// system call on Linux; elsewhere there is none, and both calls refuse with ENOSYS (no row but Linux
/// arm64's instruction cache calls it).
/// @return The membarrier.
[[nodiscard]] std::unique_ptr<IMembarrier> makeSystemMembarrier();

} // namespace isoline::platform
