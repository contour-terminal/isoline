// SPDX-License-Identifier: Apache-2.0
#include <isoline/platform/InstructionCache.hpp>

#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <utility>

#include <isoline/meta/Check.hpp>
#include <isoline/platform/Membarrier.hpp>
#include <isoline/platform/Platform.hpp>
#include <isoline/platform/ThisPlatform.hpp>

namespace isoline::platform
{

namespace
{

    /// @p error with the system's @p code.
    /// @param error Why.
    /// @param code The system's error code.
    /// @return The failure.
    [[nodiscard]] std::unexpected<InstructionCacheFailure> failure(InstructionCacheError error,
                                                                   int code) noexcept
    {
        return std::unexpected { InstructionCacheFailure { .error = error, .systemCode = code } };
    }

    /// An instruction cache that interprets one InstructionCacheTable row.
    class TableInstructionCache final: public IInstructionCache
    {
      public:
        TableInstructionCache(InstructionCacheRow const& row,
                              std::unique_ptr<IInstructionCacheMaintenance> maintenance,
                              IMembarrier& membarrier) noexcept:
            _row { row }, _maintenance { std::move(maintenance) }, _membarrier { membarrier }
        {
            ISOLINE_PRECONDITION(_maintenance != nullptr);
        }

        [[nodiscard]] std::expected<void, InstructionCacheFailure> synchronize(
            std::span<std::byte const> code) noexcept override
        {
            return invalidate(code).and_then([this] { return synchronizeOtherThreads(); });
        }

        void synchronizeThisThread() noexcept override
        {
            if (_row.thisThread == ThisThreadSynchronization::InstructionSynchronizationBarrier)
                _maintenance->synchronizeContext();
        }

      private:
        /// The row's invalidation of @p code.
        [[nodiscard]] std::expected<void, InstructionCacheFailure> invalidate(
            std::span<std::byte const> code) noexcept
        {
            if (_row.invalidation == InstructionCacheInvalidation::None)
                return {};
            return _maintenance->invalidate(code).transform_error([](int code) {
                return InstructionCacheFailure { .error = InstructionCacheError::SynchronizationRefused,
                                                 .systemCode = code };
            });
        }

        /// The row's synchronization of the other threads.
        [[nodiscard]] std::expected<void, InstructionCacheFailure> synchronizeOtherThreads() noexcept
        {
            if (_row.otherThreads == OtherThreadSynchronization::None)
                return {};
            return _membarrier.synchronizeCore().transform_error([](int code) {
                return InstructionCacheFailure { .error = InstructionCacheError::SynchronizationRefused,
                                                 .systemCode = code };
            });
        }

        InstructionCacheRow _row;                                   ///< What it does.
        std::unique_ptr<IInstructionCacheMaintenance> _maintenance; ///< The cache maintenance.
        IMembarrier& _membarrier;                                   ///< The other threads' synchronization.
    };

} // namespace

std::expected<std::unique_ptr<IInstructionCache>, InstructionCacheFailure> makeInstructionCache(
    InstructionCacheKind kind,
    std::unique_ptr<IInstructionCacheMaintenance> maintenance,
    IMembarrier& membarrier)
{
    auto const& row = instructionCacheRow(kind);
    if (row.otherThreads == OtherThreadSynchronization::MembarrierSynchronizeCore)
    {
        if (auto const registered = membarrier.registerSynchronizeCore(); !registered)
            return failure(InstructionCacheError::RegistrationRefused, registered.error());
    }
    return std::make_unique<TableInstructionCache>(row, std::move(maintenance), membarrier);
}

std::expected<std::unique_ptr<IInstructionCache>, InstructionCacheFailure> makeSystemInstructionCache(
    IMembarrier& membarrier)
{
    return makeInstructionCache(
        ThisPlatform.instructionCache, makeSystemInstructionCacheMaintenance(), membarrier);
}

} // namespace isoline::platform
