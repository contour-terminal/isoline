// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

#include <isoline/runtime/Trap.hpp>

namespace isoline::differential
{

/// How Wasmtime words one of its traps (`wasm trap: <message>`), and the TrapCodes that agree with it.
///
/// Wasmtime has one trap where Isoline has several: a table index out of bounds is
/// OutOfBoundsTableAccess for table.get and UndefinedElement for call_indirect, and one null reference
/// covers call_ref's, ref.as_non_null's and throw_ref's. Such a row lists each.
struct TrapMappingRow
{
    std::string_view message;               ///< Wasmtime's message, after `wasm trap: `.
    std::array<runtime::TrapCode, 3> codes; ///< The TrapCodes that agree, the first `count` of them.
    std::size_t count = 1;                  ///< How many of `codes` apply.

    /// The codes that agree.
    [[nodiscard]] constexpr std::span<runtime::TrapCode const> agreeing() const noexcept
    {
        return std::span { codes }.first(count);
    }
};

/// Every trap of Wasmtime 50 (scripts/tools.lock) that an import-free module whose exports take and return
/// numbers can raise under the oracle's flags. Wasmtime's own limits are no row: its `interrupt` (the
/// oracle's timeout) is reported as an exhausted oracle, never compared.
inline constexpr auto TrapMappingRows = std::to_array<TrapMappingRow>({
    { .message = "wasm `unreachable` instruction executed", .codes = { runtime::TrapCode::Unreachable } },
    { .message = "integer divide by zero", .codes = { runtime::TrapCode::IntegerDivideByZero } },
    { .message = "integer overflow", .codes = { runtime::TrapCode::IntegerOverflow } },
    { .message = "invalid conversion to integer",
      .codes = { runtime::TrapCode::InvalidConversionToInteger } },
    { .message = "out of bounds memory access", .codes = { runtime::TrapCode::OutOfBoundsMemoryAccess } },
    { .message = "undefined element: out of bounds table access",
      .codes = { runtime::TrapCode::OutOfBoundsTableAccess, runtime::TrapCode::UndefinedElement },
      .count = 2 },
    { .message = "uninitialized element", .codes = { runtime::TrapCode::UninitializedElement } },
    { .message = "indirect call type mismatch", .codes = { runtime::TrapCode::IndirectCallTypeMismatch } },
    { .message = "null reference",
      .codes = { runtime::TrapCode::NullFunctionReference,
                 runtime::TrapCode::NullReference,
                 runtime::TrapCode::NullExceptionReference },
      .count = 3 },
    { .message = "call stack exhausted", .codes = { runtime::TrapCode::CallStackExhausted } },
});

/// The row of Wasmtime's trap message @p message.
/// @param message The message, after `wasm trap: `.
/// @return The row, or null when no row has it.
[[nodiscard]] constexpr TrapMappingRow const* trapMappingOf(std::string_view message) noexcept
{
    for (auto const& row: TrapMappingRows)
    {
        if (row.message == message)
            return &row;
    }
    return nullptr;
}

} // namespace isoline::differential
