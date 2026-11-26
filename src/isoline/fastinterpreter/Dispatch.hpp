// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <span>

namespace isoline::fastinterpreter
{

/// How the fast interpreter moves from one operation to the next.
///
/// A module is lowered for one of them, which its code's header words depend on, so the choice is
/// made when the module is loaded. Every dispatch runs every module alike; they differ in speed.
enum class Dispatch : uint8_t
{
    /// A loop over a switch on each operation's OperationId: portable C++, compiled everywhere.
    Switch,
    /// Direct-threaded handlers, each ending in a guaranteed tail call to the next: compiled only where
    /// the compiler guarantees the tail call.
    TailCall,
};

/// The dispatches this build compiled, from the most portable to the fastest: Switch first, always.
///
/// Which compiled is decided when the project is configured (fastinterpreter/CMakeLists.txt).
/// @return The available dispatches.
[[nodiscard]] std::span<Dispatch const> availableDispatches() noexcept;

/// The fastest dispatch this build compiled: TailCall where it is available, otherwise Switch.
/// @return The last of availableDispatches().
[[nodiscard]] Dispatch bestDispatch() noexcept;

} // namespace isoline::fastinterpreter
