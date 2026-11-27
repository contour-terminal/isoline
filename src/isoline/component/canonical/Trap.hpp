// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Assert.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <isoline/api/Types.hpp>
#include <isoline/component/Limits.hpp>

namespace isoline::component::canonical
{

/// Why a component call trapped: the canonical ABI's trap_if conditions, the caps that end a call, and
/// a core trap that reached the component layer.
enum class TrapCode : uint8_t
{
    InvalidChar,         ///< A `char` of 0x110000 or more, or a surrogate.
    InvalidDiscriminant, ///< A variant's, option's, result's or enum's case past its last.
    UnalignedPointer,    ///< A pointer not aligned to what it points at.
    OutOfBounds,         ///< A range of guest memory past the memory's end.
    InvalidEncoding,     ///< A string's bytes not text in their encoding.
    LiveValueBytes,      ///< The store's live value bytes would pass ComponentLimits::maxLiveValueBytes.
    Handles,             ///< The store's handles would pass ComponentLimits::maxHandles.
    ResourceTypes,       ///< The store's resource types would pass ComponentLimits::maxResourceTypes.
    Interrupted,         ///< The store's interrupter stopped the call: always this code, never Core (R13).
    CannotLeave,         ///< A component instance left while its `may_leave` was cleared.
    CannotEnter,         ///< A component instance entered while it may not be: locked down, or re-entered.
    UnknownHandle,       ///< A handle index no handle of the table has.
    WrongHandleType,     ///< A handle of another resource type than the one expected.
    BorrowOutstanding,   ///< A call returned while a handle it borrowed is still held.
    LendOutstanding,     ///< An own handle removed while it is lent.
    OwnExpected,         ///< A borrow handle lifted where an own is expected.
    WrongResource,       ///< A resource lowered as another resource type than its own.
    UncaughtException,   ///< A core exception no handler caught reached the component boundary.
    Core,                ///< A core trap (Trap::core), its message the core's.
};

/// The last TrapCode enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastTrapCode = TrapCode::Core;

/// The number of TrapCode enumerators, as LastTrapCode counts them.
inline constexpr auto TrapCodeCount = static_cast<std::size_t>(LastTrapCode) + 1;

/// One row of TrapCodeTable: a trap and its message.
struct TrapCodeRow
{
    TrapCode code; ///< The trap; equal to the row's index.
    /// Its message: each `{}` takes the trap's next argument (Trap::arguments), in order.
    std::string_view format;
};

/// Every trap's message, one row per TrapCode enumerator, in enumerator order.
///
/// One wording per trap: the component-model tests' (test/ at the pin) where they assert one, as a
/// prefix the values follow, else Isoline's own; the caps read as their LimitTable rows do. Other
/// suites word some traps differently; the wast runner maps each TrapCode to the texts it accepts
/// (Task 19), as CoreTrapWording does for core traps.
inline constexpr auto TrapCodeTable = std::to_array<TrapCodeRow>({
    { .code = TrapCode::InvalidChar, .format = "invalid `char` bit pattern {}" },
    { .code = TrapCode::InvalidDiscriminant, .format = "invalid variant discriminant {} of {} cases" },
    { .code = TrapCode::UnalignedPointer, .format = "unaligned pointer {} for an alignment of {}" },
    { .code = TrapCode::OutOfBounds, .format = "out of bounds of memory: {}" },
    { .code = TrapCode::InvalidEncoding, .format = "invalid {}" },
    { .code = TrapCode::LiveValueBytes,
      .format = LimitTable[static_cast<std::size_t>(ComponentLimit::LiveValueBytes)].message },
    { .code = TrapCode::Handles,
      .format = LimitTable[static_cast<std::size_t>(ComponentLimit::Handles)].message },
    { .code = TrapCode::ResourceTypes,
      .format = LimitTable[static_cast<std::size_t>(ComponentLimit::ResourceTypes)].message },
    { .code = TrapCode::Interrupted, .format = "interrupted" },
    { .code = TrapCode::CannotLeave, .format = "cannot leave component instance" },
    { .code = TrapCode::CannotEnter, .format = "cannot enter component instance" },
    { .code = TrapCode::UnknownHandle, .format = "unknown handle index {}" },
    { .code = TrapCode::WrongHandleType,
      .format = "handle index {} used with the wrong type, expected {} but found {}" },
    { .code = TrapCode::BorrowOutstanding, .format = "borrow handles still remain at the end of the call" },
    { .code = TrapCode::LendOutstanding, .format = "cannot remove owned resource while borrowed" },
    { .code = TrapCode::OwnExpected, .format = "handle index {} is a borrow where an own is expected" },
    { .code = TrapCode::WrongResource, .format = "resource {} lowered as another resource type" },
    { .code = TrapCode::UncaughtException, .format = "uncaught exception" },
    { .code = TrapCode::Core, .format = "{}" },
});

/// The number of `{}` in @p format.
/// @param format A TrapCodeTable format.
/// @return How many arguments it takes.
[[nodiscard]] constexpr std::size_t placeholderCount(std::string_view format) noexcept
{
    // Split at each placeholder: one piece more than placeholders, and none for an empty format.
    auto pieces = std::size_t { 0 };
    for ([[maybe_unused]] auto const piece: std::views::split(format, std::string_view { "{}" }))
        ++pieces;
    return pieces == 0 ? 0 : pieces - 1;
}

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: TrapCodeTable)
        {
            if (static_cast<std::size_t>(row.code) != index)
                return false;
            // Every brace is half of a placeholder, so that message() substitutes nothing else.
            auto braces = std::size_t { 0 };
            for (auto const character: row.format)
                braces += character == '{' || character == '}' ? 1 : 0;
            if (braces != 2 * placeholderCount(row.format))
                return false;
            ++index;
        }
        return index == TrapCodeCount;
    }(),
    "TrapCodeTable must hold one row per TrapCode, at the index of its enumerator, with only {} braces");

/// The row of @p code.
/// @param code A trap.
/// @return Its TrapCodeTable row.
[[nodiscard]] constexpr TrapCodeRow const& trapCodeRow(TrapCode code) noexcept
{
    return TrapCodeTable[static_cast<std::size_t>(code)];
}

/// A component trap: why, and the values its message carries.
struct Trap
{
    TrapCode code = TrapCode::Core; ///< Why.
    /// The values its message carries, one per `{}` of its row's format, in order: "5" for
    /// `unknown handle index 5`.
    std::vector<std::string> arguments {};
    /// For Core, the core trap; else empty.
    std::optional<isoline::api::TrapCode> core {};

    /// The trap's text: its row's format, each `{}` replaced by the next argument.
    /// @return It (checked: one argument per placeholder).
    /// @throws std::bad_alloc When the text cannot be stored.
    [[nodiscard]] std::string message() const;

    /// Two traps are equal when every member is.
    bool operator==(Trap const&) const = default;
};

static_assert(std::is_nothrow_move_constructible_v<Trap> && std::is_nothrow_move_assignable_v<Trap>,
              "PendingTrap::raise() moves a Trap in a noexcept function");

/// A trap of @p code whose arguments are @p values, each formatted with `{}`.
/// @param code Why.
/// @param values One per placeholder of its row's format (checked).
/// @return The trap.
/// @throws std::bad_alloc When the arguments cannot be stored.
template <typename... Args>
[[nodiscard]] Trap trapOf(TrapCode code, Args const&... values)
{
    auto trap = Trap { .code = code, .arguments = { std::format("{}", values)... } };
    Require(trap.arguments.size() == placeholderCount(trapCodeRow(code).format));
    return trap;
}

/// The component trap a core trap is: Interrupted for an interrupt (R13), else Core carrying the core
/// trap's code and its spec message.
/// @param trap A core trap that ended a core call the component layer made.
/// @return The component trap.
/// @throws std::bad_alloc When its message cannot be stored.
[[nodiscard]] Trap fromCore(isoline::api::Trap const& trap);

/// The core trap a component trap crosses core frames as (A4): HostTrap, which P3 reserves for the
/// embedder and every engine hands back unchanged.
/// @return Trap{HostTrap}.
[[nodiscard]] constexpr isoline::api::Trap carrierTrap() noexcept
{
    return isoline::api::Trap { isoline::api::TrapCode::HostTrap };
}

/// The carrier (I3): a trap raised in a lowered function or a built-in is stored here while it crosses
/// core frames as carrierTrap(), and the component call that sees the core call fail with it takes it
/// back. One per component store, which runs one call at a time.
class PendingTrap
{
  public:
    /// Stores @p trap, unless a trap is pending already: the first trap raised ends the call, so a later
    /// one (raised while a host unwinds the first) is dropped.
    /// @param trap The trap.
    void raise(Trap trap) noexcept;

    /// Takes the pending trap.
    /// @return It, once; empty when none is pending.
    [[nodiscard]] std::optional<Trap> take() noexcept;

    /// Whether a trap is pending.
    /// @return true between a raise() and the take() that returns it.
    [[nodiscard]] bool isPending() const noexcept { return _trap.has_value(); }

  private:
    std::optional<Trap> _trap {};
};

} // namespace isoline::component::canonical
