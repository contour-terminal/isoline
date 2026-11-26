// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

#include <isoline/fastinterpreter/Code.hpp>

namespace isoline::fastinterpreter
{

/// The listing of @p function's code, one operation per line: its text, then its slot operands as
/// `s<slot>` and its immediates as `#<value>`, comma-separated, as in `i32.add.imm s3, s0, #5`.
///
/// A branch ends with its target as `→ @<offset>`, a BranchMove's after its values' slot count
/// (`br_if.move s4, s0, s2, #2 → @9`); a br_table ends with one `@<offset> s<destination>` per label, the
/// default last, `@<offset>.loop` where the label is a loop's (`br_table s3, s1, #1, #1 → @12 s0, @0.loop
/// s1`).
///
/// What lowering tests compare. Precondition (checked): the code is a sequence of whole operations,
/// each header word an OperationId.
/// @param function A lowered function.
/// @return The listing, each line ending with a newline.
[[nodiscard]] std::string describe(LoweredFunction const& function);

} // namespace isoline::fastinterpreter
