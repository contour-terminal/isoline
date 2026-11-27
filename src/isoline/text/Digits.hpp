// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace isoline::text
{

// The digit sequences of numbers, shared by the lexer, which checks a number's shape, and the literal
// readers, which compute its value.

/// The digits a number is written in.
enum class Radix : uint8_t
{
    Decimal,
    Hexadecimal,
};

/// The value of a digit.
/// @param character The character.
/// @param radix Which digits it may be.
/// @return The digit's value, or nothing when @p character is no digit of @p radix.
constexpr std::optional<uint32_t> digitValue(char character, Radix radix) noexcept
{
    if (character >= '0' && character <= '9')
        return static_cast<uint32_t>(character - '0');
    if (radix == Radix::Decimal)
        return std::nullopt;
    if (character >= 'a' && character <= 'f')
        return static_cast<uint32_t>(character - 'a' + 10);
    if (character >= 'A' && character <= 'F')
        return static_cast<uint32_t>(character - 'A' + 10);
    return std::nullopt;
}

/// Reads the spec's `num` or `hexnum` at an offset: digits, with single underscores between them.
/// @param text The text.
/// @param offset The offset to read at.
/// @param radix Which digits.
/// @param visit Called with each digit's value, in order.
/// @return The offset after the digits, or nothing when no well-formed digit sequence starts at
///         @p offset.
constexpr std::optional<std::size_t> readDigits(std::string_view text,
                                                std::size_t offset,
                                                Radix radix,
                                                auto&& visit)
{
    auto position = offset;
    auto digits = std::size_t { 0 };
    while (position < text.size())
    {
        auto const value = digitValue(text[position], radix);
        if (value)
        {
            visit(*value);
            ++digits;
            ++position;
            continue;
        }
        // An underscore separates two digits, never leads, trails or doubles.
        if (text[position] != '_' || digits == 0 || position + 1 >= text.size()
            || !digitValue(text[position + 1], radix))
            break;
        ++position;
    }
    if (digits == 0)
        return std::nullopt;
    return position;
}

} // namespace isoline::text
