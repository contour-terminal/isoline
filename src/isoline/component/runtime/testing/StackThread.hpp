// SPDX-License-Identifier: Apache-2.0
#pragma once

// A thread of a chosen stack size, for the tests that nest component calls deep enough to need one, and
// the stack address they measure a nesting's cost with. The platform halves live in StackThreadPosix.cpp
// and StackThreadWindows.cpp, which CMake selects.

#include <cstddef>
#include <cstdint>
#include <exception>
#include <utility>

namespace isoline::component::runtime::testing
{

/// Starts a thread whose stack reserves at least @p bytes, runs @p entry(@p argument) on it and joins it.
/// @param bytes The stack to reserve.
/// @param entry What the thread runs; it must not throw.
/// @param argument Its argument.
void runOnStackThread(std::size_t bytes, void (*entry)(void*) noexcept, void* argument);

/// An address of the calling function's frame: comparing two, taken at two depths of one recursion, gives
/// the stack the recursion took between them.
/// @return The address.
[[nodiscard]] std::uintptr_t stackAddress() noexcept;

/// Runs @p body on a thread whose stack reserves @p bytes and waits for it; what it throws is rethrown here.
/// The body must not use Catch2 assertions, which are not thread-safe: it hands its results back instead.
/// @param bytes The stack to reserve.
/// @param body What runs, kept here while it does.
template <typename F>
void runOnStack(std::size_t bytes, F body)
{
    struct Call
    {
        F* body;
        std::exception_ptr thrown {};
    };
    auto call = Call { .body = &body };
    runOnStackThread(
        bytes,
        [](void* argument) noexcept {
            auto& running = *static_cast<Call*>(argument);
            try
            {
                (*running.body)();
            }
            catch (...)
            {
                running.thrown = std::current_exception();
            }
        },
        &call);
    if (call.thrown)
        std::rethrow_exception(call.thrown);
}

} // namespace isoline::component::runtime::testing
