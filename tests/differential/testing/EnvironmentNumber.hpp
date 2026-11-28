// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <charconv>
#include <cstdint>
#include <format>
#include <string_view>
#include <system_error>

namespace isoline::differential::testing
{

/// A whole number from the environment variable @p name, or @p fallback when it is unset; a value that is no
/// whole number fails the test.
/// @param name The variable.
/// @param fallback The number when it is unset.
/// @return The number.
[[nodiscard]] inline uint64_t numberFromEnvironment(std::string_view name, uint64_t fallback)
{
    auto const text = core::LiveEnvironment {}.get(name);
    if (!text)
        return fallback;
    auto value = uint64_t {};
    auto const [end, error] = std::from_chars(text->data(), text->data() + text->size(), value);
    if (error != std::errc {} || end != text->data() + text->size())
        FAIL(std::format("{}={} is not a whole number", name, *text));
    return value;
}

} // namespace isoline::differential::testing
