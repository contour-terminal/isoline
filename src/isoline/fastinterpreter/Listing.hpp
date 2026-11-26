// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

#include <isoline/fastinterpreter/Code.hpp>

namespace isoline::fastinterpreter
{

/// The listing of @p function's code, one operation per line: its text, then its slot operands as
/// `s<slot>` and its immediates as `#<value>`, comma-separated, as in `i32.add.imm s3, s0, #5`.
///
/// What lowering tests compare. Precondition (checked): the code is a sequence of whole operations,
/// each header word an OperationId.
/// @param function A lowered function.
/// @return The listing, each line ending with a newline.
[[nodiscard]] std::string describe(LoweredFunction const& function);

} // namespace isoline::fastinterpreter
