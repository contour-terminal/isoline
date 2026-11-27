// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

#include <isoline/binary/Module.hpp>

namespace isoline::binary
{

/// The decoded body units of @p function: its instructions, br_table labels, catch clauses, typed
/// select types and local runs, what every engine's bound on the code it prepares from a body is counted
/// in (the fast interpreter's lowered words, the compiler's CompileLimits::maxFunctionBodyUnits).
///
/// The decoder keeps no body length, but each unit took at least one byte of the body, so a bound per
/// unit is at most the same bound per body byte.
/// @param function A decoded function.
/// @return The number of its units.
[[nodiscard]] constexpr uint64_t bodyUnits(Function const& function) noexcept
{
    return uint64_t { function.body.size() } + function.brTableLabels.size() + function.catchClauses.size()
           + function.selectTypes.size() + function.locals.size();
}

} // namespace isoline::binary
