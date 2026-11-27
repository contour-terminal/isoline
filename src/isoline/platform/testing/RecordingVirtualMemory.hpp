// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

#include <isoline/platform/Platform.hpp>
#include <isoline/platform/VirtualMemory.hpp>

namespace isoline::platform::testing
{

/// Which IVirtualMemory call a RecordedCall is.
enum class VirtualMemoryCall : uint8_t
{
    Reserve, ///< reserve().
    Protect, ///< protect().
    Release, ///< release().
};

/// Whether a recorded call reached the wrapped memory.
enum class CallOutcome : uint8_t
{
    Forwarded, ///< It was passed on; its result is the wrapped memory's.
    Refused,   ///< It was refused with the configured failure, and not passed on.
};

/// One call a RecordingVirtualMemory saw.
struct RecordedCall
{
    VirtualMemoryCall call = VirtualMemoryCall::Reserve; ///< Which call.
    /// The region: protect()'s and release()'s argument, or the one reserve() returned (base null when
    /// it failed, bytes the size asked for).
    Region region {};
    Protection protection = Protection::None;     ///< protect()'s protection; None for the others.
    CallOutcome outcome = CallOutcome::Forwarded; ///< Whether it was passed on.
    /// How the pages were decommitted, for a protect() to Protection::None: the wrapped memory's
    /// mechanism, which the same call on another platform row would use instead.
    std::optional<DecommitKind> decommit = std::nullopt;
    /// What it failed with, the configured failure or the wrapped memory's; none when it succeeded.
    std::optional<VirtualMemoryFailure> failure = std::nullopt;
};

/// A call to refuse: the @p call-th call, counted from 1 over every call, fails with @p failure.
struct CallRefusal
{
    std::size_t call = 1;            ///< Which call, 1 for the first.
    VirtualMemoryFailure failure {}; ///< What it fails with.
};

/// Wraps any IVirtualMemory: records every call, the (region, protection) transitions among them and
/// their results, and refuses the one call it is configured to. Every call, refused or not, checks
/// the interface's preconditions first; release() also checks that its region is a whole reservation
/// this memory made, which the system can tell on Windows only.
class RecordingVirtualMemory final: public IVirtualMemory
{
  public:
    /// @param inner The memory calls are passed on to; must outlive this.
    /// @param refusal The call to refuse instead of passing it on, if any.
    RecordingVirtualMemory(IVirtualMemory& inner, std::optional<CallRefusal> refusal) noexcept:
        _inner { &inner }, _refusal { refusal }
    {
    }

    /// The calls seen so far, in order.
    /// @return Them.
    [[nodiscard]] std::span<RecordedCall const> calls() const noexcept { return _calls; }

    /// @copydoc IVirtualMemory::pageSize
    [[nodiscard]] uint64_t pageSize() const noexcept override { return _inner->pageSize(); }

    /// @copydoc IVirtualMemory::allocationGranularity
    [[nodiscard]] uint64_t allocationGranularity() const noexcept override
    {
        return _inner->allocationGranularity();
    }

    /// @copydoc IVirtualMemory::decommitKind
    [[nodiscard]] DecommitKind decommitKind() const noexcept override { return _inner->decommitKind(); }

    /// @copydoc IVirtualMemory::reserve
    [[nodiscard]] std::expected<Region, VirtualMemoryFailure> reserve(uint64_t bytes) noexcept override
    {
        detail::requireReservationSize(bytes, allocationGranularity());
        auto call = RecordedCall { .call = VirtualMemoryCall::Reserve, .region = { .bytes = bytes } };
        auto result = refusesThisCall() ? refused<Region>() : _inner->reserve(bytes);
        call.outcome = _takenRefusal ? CallOutcome::Refused : CallOutcome::Forwarded;
        if (result)
        {
            call.region = *result;
            _reservations.push_back(*result);
        }
        else
            call.failure = result.error();
        record(call);
        return result;
    }

    /// @copydoc IVirtualMemory::protect
    [[nodiscard]] std::expected<void, VirtualMemoryFailure> protect(Region region,
                                                                    Protection protection) noexcept override
    {
        detail::requirePageRange(region, pageSize());
        auto call =
            RecordedCall { .call = VirtualMemoryCall::Protect, .region = region, .protection = protection };
        if (protectionRow(protection).commitment == Commitment::Decommit)
            call.decommit = decommitKind();
        auto result = refusesThisCall() ? refused<void>() : _inner->protect(region, protection);
        finish(call, result);
        return result;
    }

    /// @copydoc IVirtualMemory::release
    [[nodiscard]] std::expected<void, VirtualMemoryFailure> release(Region region) noexcept override
    {
        detail::requirePageRange(region, pageSize());
        auto const reservation = std::ranges::find_if(_reservations, [&region](Region const& reserved) {
            return region.base >= reserved.base && region.base < reserved.base + reserved.bytes;
        });
        detail::requireWholeReservation(region,
                                        reservation == _reservations.end() ? Region {} : *reservation);
        auto call = RecordedCall { .call = VirtualMemoryCall::Release, .region = region };
        auto result = refusesThisCall() ? refused<void>() : _inner->release(region);
        if (result)
            _reservations.erase(reservation);
        finish(call, result);
        return result;
    }

  private:
    /// Counts this call, and remembers the failure to refuse it with if it is the configured one.
    /// @return Whether to refuse it.
    [[nodiscard]] bool refusesThisCall() noexcept
    {
        ++_callCount;
        _takenRefusal = std::nullopt;
        if (_refusal && _refusal->call == _callCount)
            _takenRefusal = _refusal->failure;
        return _takenRefusal.has_value();
    }

    /// The current call's refusal, as the result of a call returning @p Value.
    /// @return The refusal.
    template <typename Value>
    [[nodiscard]] std::expected<Value, VirtualMemoryFailure> refused() const noexcept
    {
        return std::unexpected { *_takenRefusal };
    }

    /// Completes @p call with @p result and this call's outcome, and records it.
    /// @param call The call.
    /// @param result Its result.
    void finish(RecordedCall call, std::expected<void, VirtualMemoryFailure> const& result) noexcept
    {
        call.outcome = _takenRefusal ? CallOutcome::Refused : CallOutcome::Forwarded;
        if (!result)
            call.failure = result.error();
        record(call);
    }

    /// Appends @p call to the record.
    /// @param call The call.
    void record(RecordedCall const& call) noexcept { _calls.push_back(call); }

    IVirtualMemory* _inner;                            ///< The wrapped memory.
    std::optional<CallRefusal> _refusal;               ///< The call to refuse.
    std::size_t _callCount = 0;                        ///< Calls seen so far.
    std::optional<VirtualMemoryFailure> _takenRefusal; ///< The current call's refusal, if it is refused.
    std::vector<Region> _reservations;                 ///< Reservations made and not yet released.
    std::vector<RecordedCall> _calls;                  ///< Their record.
};

} // namespace isoline::platform::testing
