// SPDX-License-Identifier: Apache-2.0
// The child ProcessRunner_test runs: it prints each argument after the first on a line of its own to
// standard output, "to standard error" to standard error, and exits with the status its first argument
// names.
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <print>
#include <span>
#include <string_view>

int main(int argc, char const* argv[])
{
    auto const arguments = std::span<char const* const> { argv, static_cast<std::size_t>(argc) }.subspan(1);
    auto status = 0;
    if (!arguments.empty())
    {
        auto const first = std::string_view { arguments.front() };
        std::from_chars(first.data(), first.data() + first.size(), status);
        for (auto const* argument: arguments.subspan(1))
            std::println("{}", argument);
    }
    std::print(stderr, "to standard error");
    return status;
}
