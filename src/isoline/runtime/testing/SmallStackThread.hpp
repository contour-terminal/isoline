// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>

/// A thread of a chosen stack size, for tests that bound how deep the engine nests on the host stack: host
/// re-entries nest an invocation each, and must fit a small thread. The platform's half is CMake-selected
/// (SmallStackThreadPosix.cpp, SmallStackThreadWindows.cpp). Tests only.
namespace isoline::runtime::testing
{

/// Why a SmallStackThread could not run.
enum class SmallStackError : uint8_t
{
    Unavailable, ///< The platform refused a thread of the size (too small for it, or out of resources).
};

/// The stack a test thread gets on this build, from tests/CMakeLists.txt's row for its flavour: 512 KiB on an
/// optimized build without sanitizers or coverage, whose dispatch frames -Wframe-larger-than bounds, and more
/// on any other, whose frames carry redzones and counters.
inline constexpr auto SmallStackBytes = std::size_t { ISOLINE_SMALL_STACK_BYTES };

/// Runs @p work on a new thread of @p stackBytes of stack, and returns once it is done: the thread is joined
/// before this returns. @p work must end by itself (a finite guest, a bounded budget).
/// @param stackBytes The thread's stack size.
/// @param work What it runs.
/// @return Nothing once @p work ran, or why no thread could run it.
[[nodiscard]] std::expected<void, SmallStackError> runOnSmallStack(std::size_t stackBytes,
                                                                   std::function<void()> const& work);

} // namespace isoline::runtime::testing
