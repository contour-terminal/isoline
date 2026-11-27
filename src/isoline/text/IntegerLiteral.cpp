// SPDX-License-Identifier: Apache-2.0
#include <isoline/text/IntegerLiteral.hpp>

#include <cstdint>
#include <expected>
#include <limits>
#include <string_view>

#include <isoline/text/Digits.hpp>
#include <isoline/text/LiteralError.hpp>
#include <isoline/text/TextLimits.hpp>

namespace isoline::text
{

std::expected<uint64_t, LiteralError> parseInteger(std::string_view text,
                                                   IntegerWidth width,
                                                   IntegerSyntax syntax,
                                                   TextLimits const& limits)
{
    if (text.size() > limits.maxNumberLength)
        return std::unexpected { LiteralError::TooLong };

    auto const isNegative = text.starts_with('-');
    if (isNegative || text.starts_with('+'))
    {
        if (syntax == IntegerSyntax::Unsigned)
            return std::unexpected { LiteralError::NotANumber };
        text.remove_prefix(1);
    }

    auto const radix = text.starts_with("0x") ? Radix::Hexadecimal : Radix::Decimal;
    auto const base = radix == Radix::Hexadecimal ? uint64_t { 16 } : uint64_t { 10 };

    // Once the magnitude exceeds 64 bits it stays above every limit, so it saturates there.
    constexpr auto Saturated = std::numeric_limits<uint64_t>::max();
    auto magnitude = uint64_t { 0 };
    auto overflows = false;
    auto const accumulate = [&](uint32_t digit) {
        if (magnitude > (Saturated - digit) / base)
            overflows = true;
        else
            magnitude = (magnitude * base) + digit;
    };
    auto const end = readDigits(text, radix == Radix::Hexadecimal ? 2U : 0U, radix, accumulate);
    if (end != text.size())
        return std::unexpected { LiteralError::NotANumber };

    auto const bits = static_cast<uint32_t>(width);
    auto const mask = Saturated >> (64U - bits);
    // -2^(N-1) is the lowest signed value; 2^N - 1 the highest unsigned one.
    auto const highest = isNegative ? (mask >> 1U) + 1U : mask;
    if (overflows || magnitude > highest)
        return std::unexpected { LiteralError::OutOfRange };
    return (isNegative ? uint64_t { 0 } - magnitude : magnitude) & mask;
}

} // namespace isoline::text
