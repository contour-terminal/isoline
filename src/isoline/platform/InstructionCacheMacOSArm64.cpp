// SPDX-License-Identifier: Apache-2.0
// macOS arm64's row: sys_icache_invalidate and isb.
#include <cstddef>
#include <expected>
#include <memory>
#include <span>

#include <isoline/platform/InstructionCache.hpp>
#include <isoline/platform/Platform.hpp>
#include <isoline/platform/ThisPlatform.hpp>
#include <libkern/OSCacheControl.h>

namespace isoline::platform
{

static_assert(ThisPlatform.instructionCache == InstructionCacheKind::MacOSArm64,
              "InstructionCacheMacOSArm64.cpp is the MacOSArm64 row");
static_assert(instructionCacheRow(InstructionCacheKind::MacOSArm64).invalidation
                  == InstructionCacheInvalidation::AppleInvalidate,
              "InstructionCacheMacOSArm64.cpp invalidates as its row says");

namespace
{

    /// The MacOSArm64 row's cache maintenance.
    class SystemMaintenance final: public IInstructionCacheMaintenance
    {
      public:
        [[nodiscard]] std::expected<void, int> invalidate(std::span<std::byte const> code) noexcept override
        {
            sys_icache_invalidate(const_cast<std::byte*>(code.data()), code.size());
            return {};
        }

        void synchronizeContext() noexcept override { asm volatile("isb" ::: "memory"); }
    };

} // namespace

std::unique_ptr<IInstructionCacheMaintenance> makeSystemInstructionCacheMaintenance()
{
    return std::make_unique<SystemMaintenance>();
}

} // namespace isoline::platform
