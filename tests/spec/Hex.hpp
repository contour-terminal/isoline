// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <string>
#include <string_view>

namespace isoline::spectest
{

/// Decodes hex-encoded bytes, as .oracle manifests spell reasons, messages and divergences: two
/// lower-case hex digits per byte.
/// @param hex The digits.
/// @return The bytes, or why the digits are malformed (an odd count, or a character no digit).
[[nodiscard]] std::expected<std::string, std::string> decodeHex(std::string_view hex);

} // namespace isoline::spectest
