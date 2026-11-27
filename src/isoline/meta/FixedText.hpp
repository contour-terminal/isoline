// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <string_view>

// A string literal held by value, so that a text can be a template argument or an annotation's value:
// both need a structural type, whose every member is public and itself structural. A character array
// is; std::string_view is not (its members are private), and GCC 16.2.1 rejects an annotation whose
// value holds one (measured, docs/language-features.md). C++23, on every compiler, and installed with
// isoline_core's headers (isoline::meta is internal to module isoline), so the header check holds it to
// C++23 before annotations become the tables' source (P19 Task 15).

namespace isoline::meta
{

namespace detail
{
    /// Not constexpr, so that calling it ends a constant evaluation, naming why: a FixedText was given a
    /// character array that does not end in a NUL, which view() would cut short by one character.
    inline void fixedTextNeedsANulTerminatedLiteral() noexcept
    { // uncovered(constant-evaluation-only)
    } // uncovered(constant-evaluation-only)
} // namespace detail

/// A string literal held by value. It is structural, so it can be an annotation value or a template
/// argument; std::string_view is neither.
template <std::size_t N>
struct FixedText
{
    char data[N] {}; ///< The literal's characters, its terminating NUL included.

    /// Copies @p text. Implicit on purpose: Tag<"x"> converts a literal (no google-/hicpp- explicit
    /// check is enabled). In constant evaluation, an array whose last character is not a NUL does not
    /// compile.
    /// @param text A string literal.
    constexpr FixedText(char const (&text)[N]) noexcept
    {
        if consteval
        {
            if (text[N - 1] != '\0')
                detail::fixedTextNeedsANulTerminatedLiteral();
        }
        std::ranges::copy(text, data);
    }

    /// The text, without the terminating NUL.
    /// @return A view of data, valid as long as this object.
    [[nodiscard]] constexpr std::string_view view() const noexcept { return { data, N - 1 }; }
};

} // namespace isoline::meta
