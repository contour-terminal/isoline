// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <ranges>

namespace isoline::text::testing
{

/// How many runs a measurement takes the fastest of, so that a busy host's noise inflates none.
inline constexpr auto Runs = 3;

/// How much larger the large input of a growth measurement is than the small one.
inline constexpr auto GrowthScale = std::size_t { 4 };

/// The most work on GrowthScale times the input may take, relative to the input: linear work takes
/// about GrowthScale times as long, quadratic work its square, 16. Twice the linear ratio leaves room
/// for noise and for an O(n log n) term.
inline constexpr auto LinearGrowthBound = 2.0 * static_cast<double>(GrowthScale);

/// The fastest of Runs runs of @p work, by the wall clock.
/// @param work What to time.
/// @return Its fastest run.
template <std::invocable Work>
[[nodiscard]] std::chrono::nanoseconds fastestOf(Work const& work)
{
    auto fastest = std::chrono::nanoseconds::max();
    for ([[maybe_unused]] auto const run: std::views::iota(0, Runs))
    {
        auto const start = std::chrono::steady_clock::now();
        work();
        fastest = std::min(
            fastest,
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start));
    }
    return fastest;
}

/// How much longer @p large takes than @p small, each its fastest of Runs runs: a ratio of wall-clock
/// times, which holds across hosts where an absolute time would not.
/// @param small The work on the small input.
/// @param large The work on the large input.
/// @return The ratio.
template <std::invocable Small, std::invocable Large>
[[nodiscard]] double ratioOf(Small const& small, Large const& large)
{
    auto const smallTime = std::max(fastestOf(small), std::chrono::nanoseconds { 1 });
    return static_cast<double>(fastestOf(large).count()) / static_cast<double>(smallTime.count());
}

} // namespace isoline::text::testing
