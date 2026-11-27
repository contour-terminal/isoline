// SPDX-License-Identifier: Apache-2.0
// The row of every platform without membarrier(2): both calls refuse, as a kernel without the system
// call would. No instruction-cache row of these platforms calls them.
#include <cerrno>
#include <expected>
#include <memory>

#include <isoline/platform/Membarrier.hpp>
#include <isoline/platform/Platform.hpp>
#include <isoline/platform/ThisPlatform.hpp>

namespace isoline::platform
{

static_assert(ThisPlatform.system != OperatingSystem::Linux,
              "MembarrierNone.cpp is the row of the other systems");

namespace
{

    /// A platform without membarrier(2).
    class NoMembarrier final: public IMembarrier
    {
      public:
        [[nodiscard]] std::expected<void, int> registerSynchronizeCore() noexcept override
        {
            return std::unexpected { ENOSYS };
        }

        [[nodiscard]] std::expected<void, int> synchronizeCore() noexcept override
        {
            return std::unexpected { ENOSYS };
        }
    };

} // namespace

std::unique_ptr<IMembarrier> makeSystemMembarrier()
{
    return std::make_unique<NoMembarrier>();
}

} // namespace isoline::platform
