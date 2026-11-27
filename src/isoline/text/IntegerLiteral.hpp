// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <string_view>

#include <isoline/text/LiteralError.hpp>
#include <isoline/text/TextLimits.hpp>

namespace isoline::text
{

/// The width of an integer literal's type, in bits.
enum class IntegerWidth : uint8_t
{
    I8 = 8,
    I16 = 16,
    I32 = 32,
    I64 = 64,
};

/// Which integers a literal may spell (spec §6.3.1).
enum class IntegerSyntax : uint8_t
{
    Unsigned, ///< `uN`: digits only, no sign.
    Either,   ///< `iN`, which is `sN` ∪ `uN`: a sign is allowed, and so is any unsigned value.
};

/// Reads an integer literal: decimal or `0x` hexadecimal digits with single underscores between them,
/// after an optional sign where @p syntax allows one (spec §6.3.1).
/// @param text The literal's text, as the lexer read it.
/// @param width The width of the literal's type.
/// @param syntax Which integers it may spell. With Either, a negative literal reaches down to
///               -2^(width-1), and a non-negative one up to 2^width - 1, whether or not it
///               carries a `+`. The formal grammar reads `+N` as an `sN`, bounded by 2^(width-1) - 1;
///               the reference interpreter (`ixx.ml`) and wasm-tools accept the whole unsigned range
///               after `+` too, and so does Isoline, deliberately, so that `+255` is an i8's 0xFF.
/// @param limits The caps on reading text; the text may be at most TextLimits::maxNumberLength long.
/// @return The value's two's-complement bits, truncated to @p width; or NotANumber, OutOfRange or
///         TooLong.
[[nodiscard]] std::expected<uint64_t, LiteralError> parseInteger(std::string_view text,
                                                                 IntegerWidth width,
                                                                 IntegerSyntax syntax,
                                                                 TextLimits const& limits);

} // namespace isoline::text
