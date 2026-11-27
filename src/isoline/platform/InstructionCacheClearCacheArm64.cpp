// SPDX-License-Identifier: Apache-2.0
// The cache maintenance of the arm64 rows that invalidate with the compiler's runtime, Linux's and
// FreeBSD's: __builtin___clear_cache and isb. What differs between them, membarrier on Linux, is their
// InstructionCacheTable rows' other-thread step (InstructionCache.cpp), not this file.
#include <cstddef>
#include <expected>
#include <memory>
#include <span>

#include <isoline/platform/InstructionCache.hpp>
#include <isoline/platform/Platform.hpp>
#include <isoline/platform/ThisPlatform.hpp>

namespace isoline::platform
{

static_assert(instructionCacheRow(ThisPlatform.instructionCache).invalidation
                  == InstructionCacheInvalidation::CompilerClearCache,
              "InstructionCacheClearCacheArm64.cpp is the row of platforms that invalidate with "
              "__builtin___clear_cache");
static_assert(instructionCacheRow(ThisPlatform.instructionCache).thisThread
                  == ThisThreadSynchronization::InstructionSynchronizationBarrier,
              "InstructionCacheClearCacheArm64.cpp synchronizes the calling thread with isb");

namespace
{

    /// __builtin___clear_cache and isb.
    class SystemMaintenance final: public IInstructionCacheMaintenance
    {
      public:
        [[nodiscard]] std::expected<void, int> invalidate(std::span<std::byte const> code) noexcept override
        {
            // dc cvau; dsb ish; ic ivau; dsb ish; isb over the range, by the compiler's runtime.
            auto* const begin = const_cast<char*>(reinterpret_cast<char const*>(code.data()));
            __builtin___clear_cache(begin, begin + code.size());
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
