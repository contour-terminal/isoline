// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace isoline::runtime
{

/// Why execution trapped.
///
/// The enumerators are the rows of TrapTable, in the same order; adding a trap is adding an
/// enumerator here and a row there.
enum class TrapCode : uint8_t
{
    Unreachable,
    IntegerDivideByZero,
    IntegerOverflow,
    InvalidConversionToInteger,
    OutOfBoundsMemoryAccess,
    OutOfBoundsTableAccess,
    UndefinedElement,
    UninitializedElement,
    IndirectCallTypeMismatch,
    CallStackExhausted,
    HostTrap,
};

/// The last TrapCode enumerator, for checking TrapTable's size.
inline constexpr auto LastTrapCode = TrapCode::HostTrap;

/// What the reference interpreter appends to a trap's message.
enum class TrapDetail : uint8_t
{
    None,  ///< Nothing: the message is the row's text.
    Index, ///< The index the trap names (Trap::subject), after a space, signed as Int32.to_string spells it.
};

/// One trap: its code and the message the spec test suite expects for it.
struct TrapRow
{
    TrapCode code;                ///< The trap.
    std::string_view specMessage; ///< The spec suite's message; HostTrap's is ours, the spec has none.
    TrapDetail detail = TrapDetail::None; ///< What the interpreter appends to it.
};

/// Every trap, one row each, in TrapCode order.
inline constexpr auto TrapTable = std::to_array<TrapRow>({
    { .code = TrapCode::Unreachable, .specMessage = "unreachable" },
    { .code = TrapCode::IntegerDivideByZero, .specMessage = "integer divide by zero" },
    { .code = TrapCode::IntegerOverflow, .specMessage = "integer overflow" },
    { .code = TrapCode::InvalidConversionToInteger, .specMessage = "invalid conversion to integer" },
    { .code = TrapCode::OutOfBoundsMemoryAccess, .specMessage = "out of bounds memory access" },
    { .code = TrapCode::OutOfBoundsTableAccess, .specMessage = "out of bounds table access" },
    // call_indirect's: the interpreter appends the table element's index.
    { .code = TrapCode::UndefinedElement, .specMessage = "undefined element", .detail = TrapDetail::Index },
    { .code = TrapCode::UninitializedElement,
      .specMessage = "uninitialized element",
      .detail = TrapDetail::Index },
    { .code = TrapCode::IndirectCallTypeMismatch, .specMessage = "indirect call type mismatch" },
    { .code = TrapCode::CallStackExhausted, .specMessage = "call stack exhausted" },
    { .code = TrapCode::HostTrap, .specMessage = "host trap" },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: TrapTable)
        {
            if (static_cast<std::size_t>(row.code) != index)
                return false;
            ++index;
        }
        return index == static_cast<std::size_t>(LastTrapCode) + 1;
    }(),
    "TrapTable must hold one row per TrapCode, at the index of its enumerator");

/// A trap: execution stopped, and why.
struct Trap
{
    /// Builds a trap; there is no default code, so a trap that forgets to name one does not compile.
    /// @param code Why it trapped.
    /// @param subject The index it names, if any.
    constexpr explicit Trap(TrapCode code, std::optional<uint32_t> subject = {}) noexcept:
        code { code }, subject { subject }
    {
    }

    /// Why it trapped.
    TrapCode code;
    /// The index it names, such as call_indirect's table element; empty when it names none.
    std::optional<uint32_t> subject;

    bool operator==(Trap const&) const = default;
};

/// A trap that names no index, as the error of a fallible step.
/// @param code Why it trapped.
/// @return The trap, ready to return as a std::expected's error.
[[nodiscard]] constexpr std::unexpected<Trap> trap(TrapCode code) noexcept
{
    return std::unexpected { Trap { code } };
}

/// The message the spec test suite expects for @p code.
/// @param code The trap to describe.
/// @return Its row's message in TrapTable.
[[nodiscard]] constexpr std::string_view specMessage(TrapCode code) noexcept
{
    return TrapTable[static_cast<std::size_t>(code)].specMessage;
}

/// The message the reference interpreter reports for @p trap: its row's message, followed by the
/// index it names when the row's detail is Index.
/// @param trap The trap to describe.
/// @return The message the spec suite's assertions match as a prefix.
[[nodiscard]] inline std::string specMessage(Trap const& trap)
{
    auto const& row = TrapTable[static_cast<std::size_t>(trap.code)];
    if (row.detail == TrapDetail::Index && trap.subject)
        return std::format("{} {}", row.specMessage, static_cast<int32_t>(*trap.subject));
    return std::string { row.specMessage };
}

} // namespace isoline::runtime
