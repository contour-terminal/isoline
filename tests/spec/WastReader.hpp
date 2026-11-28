// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <string_view>
#include <vector>

#include "Script.hpp"
#include <isoline/text/TextLimits.hpp>

namespace isoline::spectest
{

/// Parses a `.wast` script of the spec suite (the reference interpreter's script grammar).
///
/// The script is read as S-expressions (text::parseSExpressions, so no nesting reaches the call stack)
/// and each top-level list is one command, read by the row of its keyword in the reader's
/// ScriptCommands table: `module` and `component` (each with `$name`, `definition`, `instance`, `binary`
/// and `quote`), `register`, `invoke`, `get`, `assert_return`, `assert_trap`,
/// `assert_exhaustion`, `assert_exception`, `assert_invalid`, `assert_malformed`, `assert_unlinkable`,
/// `assert_invalid_custom` and `assert_malformed_custom`. Annotations between commands are ignored. A
/// script whose first list is no command is an inline module, one Module command whose text is the
/// whole script, as the reference reads it.
///
/// A module given as text is kept verbatim, `(module …)` and all, for text::compileText; one given as
/// `binary` or `quote` keeps its strings, concatenated; a component likewise. Core values are
/// `(i32.const …)` and its siblings, `(v128.const <shape> …)`, `(ref.null [heap])`, `(ref.extern [N])`,
/// `(ref.func [N])`, and, as expected values only, `(ref.exn)`, `(either …)` and the `nan:canonical` and
/// `nan:arithmetic` patterns of a float or a float lane. A form an expected value may take but Isoline's
/// values cannot hold (the GC proposal's `(ref.i31)` and its siblings, `(ref.host N)`) is a parse error.
/// Component values are the ComponentValueForms rows', read into Command::componentArgs and
/// Command::componentExpected; `(f32.const …)` and `(f64.const …)` read as both layers' values, so a
/// command's Action::arity and expectedCount say which layer has them all.
/// @param script The script's text.
/// @param limits The caps on reading it as text.
/// @return One command per command of the script, in source order: the ordinals an oracle is keyed by.
///         Or the first error, with its line.
[[nodiscard]] std::expected<std::vector<Command>, ParseError> parseWast(std::string_view script,
                                                                        text::TextLimits limits);

} // namespace isoline::spectest
