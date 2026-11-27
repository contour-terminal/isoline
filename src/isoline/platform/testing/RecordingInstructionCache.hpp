// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

#include <isoline/platform/InstructionCache.hpp>

namespace isoline::platform::testing
{

/// Which IInstructionCache call a RecordingInstructionCache saw.
enum class InstructionCacheCall : uint8_t
{
    Synchronize,           ///< synchronize().
    SynchronizeThisThread, ///< synchronizeThisThread().
};

/// One synchronize() to refuse: the @p call-th, counted from 1 over synchronize() calls only.
struct SynchronizeRefusal
{
    std::size_t call = 1; ///< Which synchronize(), 1 for the first.
    /// What it fails with: a refused synchronization, unless a test names another.
    InstructionCacheFailure failure { .error = InstructionCacheError::SynchronizationRefused };
};

/// Wraps any IInstructionCache: records every call, in order, with the code synchronize() was given,
/// and refuses the one synchronize() it is configured to without passing it on. What code memory and
/// the compiled executor call is then a test's to check: the executor's synchronizeThisThread() per
/// prepared instance, the code memory's synchronize() before the final protection.
class RecordingInstructionCache final: public IInstructionCache
{
  public:
    /// @param inner The cache calls are passed on to; must outlive this.
    /// @param refusal The synchronize() to refuse, if any.
    RecordingInstructionCache(IInstructionCache& inner, std::optional<SynchronizeRefusal> refusal) noexcept:
        _inner { &inner }, _refusal { refusal }
    {
    }

    /// The calls seen so far, in order.
    /// @return Them.
    [[nodiscard]] std::span<InstructionCacheCall const> calls() const noexcept { return _calls; }

    /// The code of every synchronize() seen so far, refused ones included, in order.
    /// @return Them.
    [[nodiscard]] std::span<std::span<std::byte const> const> synchronizedCode() const noexcept
    {
        return _synchronizedCode;
    }

    /// @copydoc IInstructionCache::synchronize
    [[nodiscard]] std::expected<void, InstructionCacheFailure> synchronize(
        std::span<std::byte const> code) noexcept override
    {
        _calls.push_back(InstructionCacheCall::Synchronize);
        _synchronizedCode.push_back(code);
        if (_refusal && _synchronizedCode.size() == _refusal->call)
            return std::unexpected { _refusal->failure };
        return _inner->synchronize(code);
    }

    /// @copydoc IInstructionCache::synchronizeThisThread
    void synchronizeThisThread() noexcept override
    {
        _calls.push_back(InstructionCacheCall::SynchronizeThisThread);
        _inner->synchronizeThisThread();
    }

  private:
    IInstructionCache* _inner;                                 ///< Where calls go.
    std::optional<SynchronizeRefusal> _refusal;                ///< The synchronize() to refuse.
    std::vector<InstructionCacheCall> _calls;                  ///< Every call, in order.
    std::vector<std::span<std::byte const>> _synchronizedCode; ///< Every synchronize()'s code.
};

} // namespace isoline::platform::testing
