// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include <isoline/meta/EnumRows.hpp>
#include <isoline/text/TextError.hpp>

namespace isoline::text
{

/// Why a numeric literal has no value.
enum class LiteralError : uint8_t
{
    NotANumber, ///< The text is no literal of the asked kind.
    OutOfRange, ///< The literal is well formed, but its value does not fit the asked type.
    TooLong,    ///< The text is longer than TextLimits::maxNumberLength.
};

/// The last LiteralError enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastLiteralError = LiteralError::TooLong;

/// The text error each LiteralError is reported as, indexed by the LiteralError.
inline constexpr auto LiteralErrorCodes = std::to_array<TextErrorCode>({
    TextErrorCode::UnknownOperator,    // NotANumber: the reference's parser reads no literal there.
    TextErrorCode::ConstantOutOfRange, // OutOfRange
    TextErrorCode::NumberTooLong,      // TooLong
});

static_assert(meta::sizedByEnum<LastLiteralError>(LiteralErrorCodes.size()),
              "LiteralErrorCodes holds one text error per LiteralError");

/// The text error a literal error is reported as.
/// @param error The literal error.
/// @return Its row in LiteralErrorCodes.
[[nodiscard]] constexpr TextErrorCode textErrorCodeOf(LiteralError error) noexcept
{
    return LiteralErrorCodes[static_cast<std::size_t>(error)];
}

} // namespace isoline::text
