// SPDX-License-Identifier: Apache-2.0
//
// Must not compile (tests/CMakeLists.txt, compile-fail-fixed-text-unterminated): a FixedText made in
// constant evaluation from a character array without a terminating NUL, whose view() would drop its last
// character. The build names isoline::meta::detail::fixedTextNeedsANulTerminatedLiteral.

#include <cstddef>

#include <isoline/meta/FixedText.hpp>

namespace
{

/// Two characters, no NUL.
constexpr char Unterminated[] = { 'o', 'k' };

/// Refused: the constructor's check ends the constant evaluation.
constexpr auto Text = isoline::meta::FixedText { Unterminated };

} // namespace

/// Uses Text, so that the variable is not unused.
auto fixtureTextSize() -> std::size_t
{
    return Text.view().size();
}
