// SPDX-License-Identifier: Apache-2.0
// The row of every platform whose instruction fetch is coherent with stores (x86-64, and the
// architectures without a backend): no cache maintenance, since its InstructionCacheTable row calls none.
#include <cstddef>
#include <expected>
#include <memory>
#include <span>

#include <isoline/platform/InstructionCache.hpp>
#include <isoline/platform/Platform.hpp>
#include <isoline/platform/ThisPlatform.hpp>

namespace isoline::platform
{

static_assert(ThisPlatform.instructionCache == InstructionCacheKind::Coherent,
              "InstructionCacheCoherent.cpp is the row of coherent platforms");

namespace
{

    /// Cache maintenance where none is needed: both calls do nothing.
    class CoherentMaintenance final: public IInstructionCacheMaintenance
    {
      public:
        [[nodiscard]] std::expected<void, int> invalidate(
            std::span<std::byte const> /*code*/) noexcept override
        {
            return {};
        }

        void synchronizeContext() noexcept override {}
    };

} // namespace

std::unique_ptr<IInstructionCacheMaintenance> makeSystemInstructionCacheMaintenance()
{
    return std::make_unique<CoherentMaintenance>();
}

} // namespace isoline::platform
