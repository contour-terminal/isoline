// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <string_view>

#include <isoline/api/Types.hpp>
#include <isoline/runtime/Trap.hpp>

// How Wasmtime words a core trap (wasmtime-environ v51, src/trap_encoding.rs, `impl Display for Trap`):
// the component suites' assert_trap texts are Wasmtime's, so the runner words a core trap that reached a
// component call, a canonical::Trap of code Core, by its row here before it matches the expected text.
namespace isoline::spectest
{

/// One core trap and Wasmtime's text for it.
struct CoreTrapWordingRow
{
    isoline::api::TrapCode code; ///< The trap; equal to the row's index.
    std::string_view text;       ///< Wasmtime's text; Isoline's own where Wasmtime has no such trap.
};

/// Every core trap's Wasmtime wording, one row per api::TrapCode enumerator, in enumerator order.
inline constexpr auto CoreTrapWording = std::to_array<CoreTrapWordingRow>({
    { .code = isoline::api::TrapCode::Unreachable, .text = "wasm `unreachable` instruction executed" },
    { .code = isoline::api::TrapCode::IntegerDivideByZero, .text = "integer divide by zero" },
    { .code = isoline::api::TrapCode::IntegerOverflow, .text = "integer overflow" },
    { .code = isoline::api::TrapCode::InvalidConversionToInteger, .text = "invalid conversion to integer" },
    { .code = isoline::api::TrapCode::OutOfBoundsMemoryAccess, .text = "out of bounds memory access" },
    // Wasmtime's TableOutOfBounds, which a call_indirect past the table's end raises too.
    { .code = isoline::api::TrapCode::OutOfBoundsTableAccess,
      .text = "undefined element: out of bounds table access" },
    { .code = isoline::api::TrapCode::UndefinedElement,
      .text = "undefined element: out of bounds table access" },
    // Wasmtime's IndirectCallToNull.
    { .code = isoline::api::TrapCode::UninitializedElement, .text = "uninitialized element" },
    { .code = isoline::api::TrapCode::IndirectCallTypeMismatch, .text = "indirect call type mismatch" },
    // Wasmtime raises NullReference for call_ref, ref.as_non_null and throw_ref alike.
    { .code = isoline::api::TrapCode::NullFunctionReference, .text = "null reference" },
    { .code = isoline::api::TrapCode::NullReference, .text = "null reference" },
    { .code = isoline::api::TrapCode::CallStackExhausted, .text = "call stack exhausted" },
    // Isoline's carrier of a component trap across core frames (A4): the component trap is reported.
    { .code = isoline::api::TrapCode::HostTrap, .text = "host trap" },
    { .code = isoline::api::TrapCode::NullExceptionReference, .text = "null reference" },
    // Isoline's own: Wasmtime bounds exceptions by its stores' memory, not by a count.
    { .code = isoline::api::TrapCode::ExceptionLimitExceeded, .text = "exception limit exceeded" },
    { .code = isoline::api::TrapCode::Interrupted, .text = "interrupt" },
    // Wasmtime's OutOfFuel: the step limit is Isoline's fuel.
    { .code = isoline::api::TrapCode::StepLimitExceeded, .text = "all fuel consumed by WebAssembly" },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: CoreTrapWording)
        {
            if (static_cast<std::size_t>(row.code) != index || row.text.empty())
                return false;
            ++index;
        }
        return index == static_cast<std::size_t>(isoline::runtime::LastTrapCode) + 1;
    }(),
    "CoreTrapWording must hold one worded row per api::TrapCode, at the index of its enumerator");

/// Wasmtime's text for the core trap @p code.
/// @param code A core trap.
/// @return Its CoreTrapWording row's text.
[[nodiscard]] constexpr std::string_view coreTrapWording(isoline::api::TrapCode code) noexcept
{
    return CoreTrapWording[static_cast<std::size_t>(code)].text;
}

} // namespace isoline::spectest
