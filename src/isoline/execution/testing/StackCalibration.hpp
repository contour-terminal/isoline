// SPDX-License-Identifier: Apache-2.0
#pragma once

// A recursion of known stack depth, for calibrating a stack painting (StackBounds_test.cpp). Each level calls
// the next through a volatile pointer the caller sets, and keeps its level across the call, so that every
// level is a frame of its own on the real stack: at least 16 bytes (a return address or frame record, and the
// level), and no addressable local, which ASan would move to its fake stack. The levels live in a translation
// unit of their own, which no caller's pointer is visible in: within one, GCC propagates the pointer the
// caller sets into a clone of the level, calls it directly and inlines it into itself, so gcc-release on
// x86-64 took 4'040 bytes for 512 levels. Test support, linked into no shipped binary.

#include <cstdint>

namespace isoline::execution::testing
{

/// The recursion's state: where each level goes next.
struct StackCalibration
{
    /// The next level: stackCalibrationLevel(), set by the caller.
    std::uint32_t (*volatile level)(StackCalibration const&, std::uint32_t);
};

/// The least stack one level of the recursion takes.
inline constexpr auto StackCalibrationLevelBytes = std::uint32_t { 16 };

/// One level of @p calibration, with @p levels more below it.
/// @param calibration Where the levels go next.
/// @param levels How many levels follow this one.
/// @return A value of every level, so that none is dead.
[[nodiscard]] std::uint32_t stackCalibrationLevel(StackCalibration const& calibration, std::uint32_t levels);

} // namespace isoline::execution::testing
