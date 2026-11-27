// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace isoline::testing
{

/// The bytes the replaced global operator new (AllocationCounter.cpp) has handed out and not taken
/// back, the most it has held at once, and all it has handed out: the counts every allocation through
/// new and delete keeps, in the one binary that links that translation unit.
///
/// The counts are plain, unsynchronized integers: the suite that links them allocates on one thread.
struct AllocationCounts
{
    std::size_t liveBytes = 0; ///< The bytes allocated and not yet freed.
    std::size_t peakBytes = 0; ///< The most liveBytes has been since the last resetPeak().
    /// The bytes allocated in all, freed or not: what a stretch of code allocated whatever it freed in
    /// between, so work that holds little at once still counts.
    std::size_t allocatedBytes = 0;
};

/// The current counts.
/// @return The counts as they stand.
[[nodiscard]] AllocationCounts allocationCounts() noexcept;

/// Sets the peak to the bytes live now, so that it measures what follows.
void resetAllocationPeak() noexcept;

/// What the throwing operator new throws for an allocation an AllocationLimit refuses.
enum class RefusalException : uint8_t
{
    BadAlloc,    ///< std::bad_alloc: the host is out of memory.
    LengthError, ///< std::length_error: what a container throws for a size past its max_size().
};

/// A budget on what one stretch of code may allocate: while it lives, operator new refuses (throws
/// std::bad_alloc, or what the limit names; the nothrow forms return null) any allocation that would
/// leave more than the budget allocated beyond what was live when it began. Its destructor lifts the
/// budget again.
class AllocationLimit
{
  public:
    /// Starts the budget.
    /// @param extraBytes The most the code may hold beyond what is live now.
    /// @param refusal What the throwing operator new throws for a refused allocation.
    explicit AllocationLimit(std::size_t extraBytes,
                             RefusalException refusal = RefusalException::BadAlloc) noexcept;
    ~AllocationLimit();
    AllocationLimit(AllocationLimit const&) = delete;
    AllocationLimit& operator=(AllocationLimit const&) = delete;
    AllocationLimit(AllocationLimit&&) = delete;
    AllocationLimit& operator=(AllocationLimit&&) = delete;

  private:
    std::size_t _previous;             ///< The limit before this one, restored by the destructor.
    RefusalException _previousRefusal; ///< The refusal before this one, restored by the destructor.
};

/// What one stretch of code allocated: construct it just before the code, read it just after.
///
/// A scope given a budget also refuses every allocation that would take the code past it (an
/// AllocationLimit), so code that would hold far more fails at once, small, instead of exhausting the
/// host first.
class AllocationScope
{
  public:
    AllocationScope() noexcept:
        _baseline { allocationCounts().liveBytes },
        _allocatedBaseline { allocationCounts().allocatedBytes },
        _limit { std::nullopt }
    {
        resetAllocationPeak();
    }

    /// A scope whose code may hold at most @p budgetBytes beyond what is live now; more is refused.
    /// @param budgetBytes The bytes the code may hold at once.
    explicit AllocationScope(std::size_t budgetBytes) noexcept:
        _baseline { allocationCounts().liveBytes },
        _allocatedBaseline { allocationCounts().allocatedBytes },
        _limit { std::in_place, budgetBytes }
    {
        resetAllocationPeak();
    }

    /// The most the code held at once beyond what was live when the scope began.
    /// @return The peak's growth, in bytes.
    [[nodiscard]] std::size_t peakBytes() const noexcept { return allocationCounts().peakBytes - _baseline; }

    /// What the code allocated and still holds.
    /// @return The live bytes' growth, in bytes; zero when the code freed more than it kept.
    [[nodiscard]] std::size_t retainedBytes() const noexcept
    {
        auto const live = allocationCounts().liveBytes;
        return live > _baseline ? live - _baseline : 0;
    }

    /// What the code allocated in all, whatever it freed in between.
    /// @return The bytes allocated since the scope began.
    [[nodiscard]] std::size_t allocatedBytes() const noexcept
    {
        return allocationCounts().allocatedBytes - _allocatedBaseline;
    }

  private:
    std::size_t _baseline;
    std::size_t _allocatedBaseline;        ///< The bytes allocated in all when the scope began.
    std::optional<AllocationLimit> _limit; ///< The budget, lifted when the scope ends; none refuses nothing.
};

} // namespace isoline::testing
