// SPDX-License-Identifier: Apache-2.0
#include "Hex.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>

namespace isoline::spectest
{

namespace
{
    [[nodiscard]] std::optional<uint8_t> hexDigit(char digit) noexcept
    {
        if (digit >= '0' && digit <= '9')
            return static_cast<uint8_t>(digit - '0');
        if (digit >= 'a' && digit <= 'f')
            return static_cast<uint8_t>(digit - 'a' + 10);
        return std::nullopt;
    }
} // namespace

std::expected<std::string, std::string> decodeHex(std::string_view hex)
{
    if (hex.size() % 2 != 0)
        return std::unexpected { std::format("odd-length hex string '{}'", hex) };
    auto text = std::string {};
    text.reserve(hex.size() / 2);
    for (auto const index: std::views::iota(std::size_t { 0 }, hex.size() / 2))
    {
        auto const high = hexDigit(hex[2 * index]);
        auto const low = hexDigit(hex[(2 * index) + 1]);
        if (!high || !low)
            return std::unexpected { std::format("malformed hex string '{}'", hex) };
        text.push_back(static_cast<char>((*high << 4) | *low));
    }
    return text;
}

} // namespace isoline::spectest
