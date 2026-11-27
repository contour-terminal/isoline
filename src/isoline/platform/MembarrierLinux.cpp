// SPDX-License-Identifier: Apache-2.0
// Linux's row of the membarrier seam: the membarrier(2) system call, which glibc does not wrap.
#include <sys/syscall.h>

#include <cerrno>
#include <expected>
#include <memory>

#include <unistd.h>

#include <isoline/platform/Membarrier.hpp>
#include <isoline/platform/Platform.hpp>
#include <isoline/platform/ThisPlatform.hpp>
#include <linux/membarrier.h>

namespace isoline::platform
{

static_assert(ThisPlatform.system == OperatingSystem::Linux, "MembarrierLinux.cpp is Linux's row");

namespace
{

    /// membarrier(@p command, 0, 0).
    /// @param command The MEMBARRIER_CMD_* command.
    /// @return Nothing; or errno.
    [[nodiscard]] std::expected<void, int> membarrier(int command) noexcept
    {
        if (syscall(__NR_membarrier, command, 0, 0) != 0)
            return std::unexpected { errno }; // uncovered(membarrier-refused)
        return {};
    }

    /// The process's own membarrier.
    class SystemMembarrier final: public IMembarrier
    {
      public:
        [[nodiscard]] std::expected<void, int> registerSynchronizeCore() noexcept override
        {
            return membarrier(MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE);
        }

        [[nodiscard]] std::expected<void, int> synchronizeCore() noexcept override
        {
            return membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE);
        }
    };

} // namespace

std::unique_ptr<IMembarrier> makeSystemMembarrier()
{
    return std::make_unique<SystemMembarrier>();
}

} // namespace isoline::platform
