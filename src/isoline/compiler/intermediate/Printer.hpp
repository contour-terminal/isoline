// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

#include <isoline/compiler/intermediate/Function.hpp>

namespace isoline::compiler::intermediate
{

/// The text of @p function, deterministic for equal functions, one line per block header and per
/// instruction:
///
///     b0(v0:i32):
///       v1 = i32.add v0, v0
///       branch v1, b1(v0), b2()
///     b1(v2:i32) cold:
///       return v2
///
/// An instruction is its results, its row's text, its operands that no target takes, its targets as
/// block(arguments) (switch's fallback first), then its other immediates as [name=value, ...], a type
/// and a context value by their names. A removed instruction is `removed`.
/// @param function A well-formed function (verify()).
/// @return The text, each line ending in a newline.
[[nodiscard]] std::string print(Function const& function);

} // namespace isoline::compiler::intermediate
