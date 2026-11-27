// SPDX-License-Identifier: Apache-2.0
//
// Contour's contract case installsNoSignalHandlers, on POSIX systems, where signal dispositions exist:
// CMake adds this file to the unit tests there (src/isoline/CMakeLists.txt), so no source tests for the
// platform.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <csignal>
#include <cstddef>
#include <optional>
#include <ranges>

#include <isoline/api/CallError.hpp>
#include <isoline/api/Engine.hpp>
#include <isoline/api/Store.hpp>
#include <isoline/api/testing/Contract.hpp>

using isoline::api::CallFailure;
using isoline::api::testing::contract::callI32;
using isoline::api::testing::contract::instanceModule;
using isoline::api::testing::contract::onPreparedThread;
using isoline::api::testing::contract::Runtime;

namespace
{

/// The signals whose disposition an engine must leave alone: those a compiling engine's trap handler
/// would take (an out-of-bounds access, an illegal instruction, an arithmetic fault), and the ones a
/// runtime might take to wake a thread.
constexpr auto GuardedSignals = std::to_array({ SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGUSR1, SIGUSR2 });

/// What a disposition is compared by: its handler and its flags.
struct Disposition
{
    void (*handler)(int, siginfo_t*, void*); ///< sa_sigaction, which shares its storage with sa_handler.
    int flags;                               ///< sa_flags.

    /// Equal when handler and flags are.
    bool operator==(Disposition const&) const = default;
};

/// The disposition of every guarded signal, in GuardedSignals' order. It asserts nothing, so that it
/// may run on a thread other than the test thread.
/// @return The dispositions; nothing when a sigaction() query failed.
std::optional<std::array<Disposition, GuardedSignals.size()>> dispositions()
{
    auto result = std::array<Disposition, GuardedSignals.size()> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, GuardedSignals.size()))
    {
        struct sigaction action {};
        if (sigaction(GuardedSignals.at(index), nullptr, &action) != 0)
            return std::nullopt;
        result.at(index) = Disposition { .handler = action.sa_sigaction, .flags = action.sa_flags };
    }
    return result;
}

} // namespace

TEST_CASE("Contract.installsNoSignalHandlers", "[contract]")
{
    // No trap handler for out-of-bounds accesses, no wake-up signal: an engine, a store, an
    // instantiation, a trapping call and a prepared thread leave the process's (or a sanitizer's)
    // handlers in charge.
    auto const before = dispositions();
    REQUIRE(before.has_value());
    {
        auto runtime = Runtime {};
        CHECK(dispositions() == before);
        auto const instance = runtime.instantiate(runtime.load(instanceModule()));
        CHECK(dispositions() == before);
        auto const trapped = callI32(instance, "recurse");
        REQUIRE_FALSE(trapped.has_value());
        CHECK(trapped.error().failure() == CallFailure::Trap);
        CHECK(dispositions() == before);
        auto const prepared = onPreparedThread(runtime.engine(), [] { return dispositions(); });
        REQUIRE(prepared.has_value());
        CHECK(*prepared == before);
        auto const preparation = runtime.engine().prepareThread();
        REQUIRE(preparation.has_value());
        CHECK(dispositions() == before);
    }
    CHECK(dispositions() == before);
}
