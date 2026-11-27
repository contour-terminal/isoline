// SPDX-License-Identifier: Apache-2.0
// The child ProcessRunner_test runs. Its first argument says what it does:
// - a number: it prints each argument after the first on a line of its own to standard output, "to
//   standard error" to standard error, and exits with that status;
// - "abort": it ends abnormally (ChildAbort.hpp: a signal on POSIX, a fail-fast status on Windows);
// - "sleep": it sleeps a minute, for the runner to kill it at its deadline.
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <print>
#include <span>
#include <string_view>
#include <thread>

#include "ChildAbort.hpp"

namespace
{
/// What the child does for a named first argument.
struct Mode
{
    std::string_view name;
    void (*run)();
};
constexpr auto Modes = std::to_array<Mode>({
    { .name = "abort", .run = &isoline::differential::testing::endAbnormally },
    { .name = "sleep", .run = [] { std::this_thread::sleep_for(std::chrono::minutes { 1 }); } },
});
} // namespace

int main(int argc, char const* argv[])
{
    auto const arguments = std::span<char const* const> { argv, static_cast<std::size_t>(argc) }.subspan(1);
    auto status = 0;
    if (!arguments.empty())
    {
        auto const first = std::string_view { arguments.front() };
        if (auto const mode = std::ranges::find(Modes, first, &Mode::name); mode != Modes.end())
        {
            mode->run();
            return 0;
        }
        std::from_chars(first.data(), first.data() + first.size(), status);
        for (auto const* argument: arguments.subspan(1))
            std::println("{}", argument);
    }
    std::print(stderr, "to standard error");
    return status;
}
