// SPDX-License-Identifier: Apache-2.0
// Windows arm64's row: FlushInstructionCache, documented to cover every processor, and isb.
#include <cstddef>
#include <expected>
#include <memory>
#include <span>

// windows.h defines min and max as macros, which break std::numeric_limits<…>::max() and std::min
// wherever they follow it; NOMINMAX leaves them out.
#define NOMINMAX
#include <intrin.h>
#include <windows.h>

#include <isoline/platform/InstructionCache.hpp>
#include <isoline/platform/Platform.hpp>
#include <isoline/platform/ThisPlatform.hpp>

namespace isoline::platform
{

static_assert(ThisPlatform.instructionCache == InstructionCacheKind::WindowsArm64,
              "InstructionCacheWindowsArm64.cpp is the WindowsArm64 row");
static_assert(instructionCacheRow(InstructionCacheKind::WindowsArm64).invalidation
                  == InstructionCacheInvalidation::WindowsFlush,
              "InstructionCacheWindowsArm64.cpp invalidates as its row says");

namespace
{

    /// The WindowsArm64 row's cache maintenance.
    class SystemMaintenance final: public IInstructionCacheMaintenance
    {
      public:
        [[nodiscard]] std::expected<void, int> invalidate(std::span<std::byte const> code) noexcept override
        {
            if (FlushInstructionCache(GetCurrentProcess(), code.data(), code.size()) == 0)
                return std::unexpected { static_cast<int>(GetLastError()) };
            return {};
        }

        void synchronizeContext() noexcept override { __isb(_ARM64_BARRIER_SY); }
    };

} // namespace

std::unique_ptr<IInstructionCacheMaintenance> makeSystemInstructionCacheMaintenance()
{
    return std::make_unique<SystemMaintenance>();
}

} // namespace isoline::platform
