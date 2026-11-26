// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>

namespace isoline::testing
{

/// The bytes the replaced global operator new (AllocationCounter.cpp) has handed out and not taken
/// back, and the most it has held at once: the counts every allocation through new and delete
/// keeps, in the one binary that links that translation unit.
///
/// The counts are plain, unsynchronized integers: the suite that links them allocates on one thread.
struct AllocationCounts
{
    std::size_t liveBytes = 0; ///< The bytes allocated and not yet freed.
    std::size_t peakBytes = 0; ///< The most liveBytes has been since the last resetPeak().
};

/// The current counts.
/// @return The counts as they stand.
[[nodiscard]] AllocationCounts allocationCounts() noexcept;

/// Sets the peak to the bytes live now, so that it measures what follows.
void resetAllocationPeak() noexcept;

/// What one stretch of code allocated: construct it just before the code, read it just after.
class AllocationScope
{
  public:
    AllocationScope() noexcept: _baseline { allocationCounts().liveBytes } { resetAllocationPeak(); }

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

  private:
    std::size_t _baseline;
};

} // namespace isoline::testing
