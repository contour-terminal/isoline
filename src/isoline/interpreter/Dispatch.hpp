// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace isoline::interpreter
{

/// How the interpreter moves from one operation to the next.
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

/// The last Dispatch enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastDispatch = Dispatch::TailCall;

/// A dispatch and how command lines and test names spell it (DispatchNames).
struct DispatchName
{
    Dispatch dispatch;     ///< The dispatch.
    std::string_view name; ///< Its name: switch or tailcall.
};

/// The dispatches this build compiled, from the most portable to the fastest: Switch first, always.
///
/// Which compiled is decided when the project is configured (interpreter/CMakeLists.txt).
/// @return The available dispatches.
[[nodiscard]] std::span<Dispatch const> availableDispatches() noexcept;

/// The fastest dispatch this build compiled: TailCall where it is available, otherwise Switch.
/// @return The last of availableDispatches().
[[nodiscard]] Dispatch bestDispatch() noexcept;

/// Whether this build compiled @p dispatch: whether it is one of availableDispatches().
/// @param dispatch The dispatch; any value of the type.
/// @return Whether code lowered for it can run.
[[nodiscard]] bool isAvailable(Dispatch dispatch) noexcept;

/// The dispatch named @p name, compiled or not.
/// @param name A name, as nameOf() spells it.
/// @return The dispatch, or nothing when no dispatch has that name.
[[nodiscard]] std::optional<Dispatch> dispatchNamed(std::string_view name) noexcept;

} // namespace isoline::interpreter
