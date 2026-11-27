// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <string_view>

#include <isoline/text/LiteralError.hpp>
#include <isoline/text/TextLimits.hpp>

namespace isoline::text
{

// Float literals (spec §6.3.2): an optional sign, then `inf`, `nan`, `nan:0x` and a payload, or a decimal
// or `0x` hexadecimal mantissa with an optional exponent (`e` or `p`). A mantissa is rounded to the nearest
// value of the type, ties to even, exactly and directly at the type's width: an f32 is never rounded
// through an f64 first.

/// Reads an f32 literal.
/// @param text The literal's text, as the lexer read it.
/// @param limits The caps on reading text; the text may be at most TextLimits::maxNumberLength long, and
///               that bounds the exact arithmetic of the conversion.
/// @return The value's bits; or NotANumber, OutOfRange (a finite literal that rounds to infinity, or a
///         NaN payload that is zero or does not fit the significand) or TooLong.
[[nodiscard]] std::expected<uint32_t, LiteralError> parseF32(std::string_view text, TextLimits const& limits);

/// Reads an f64 literal.
/// @param text The literal's text, as the lexer read it.
/// @param limits The caps on reading text, as parseF32's.
/// @return The value's bits; or NotANumber, OutOfRange or TooLong, as parseF32's.
[[nodiscard]] std::expected<uint64_t, LiteralError> parseF64(std::string_view text, TextLimits const& limits);

} // namespace isoline::text
