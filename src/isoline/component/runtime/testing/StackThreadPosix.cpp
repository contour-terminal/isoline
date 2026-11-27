// SPDX-License-Identifier: Apache-2.0
#include <core/Assert.hpp>

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>

#include <pthread.h>
#include <unistd.h>

#include <isoline/component/runtime/testing/StackThread.hpp>

namespace isoline::component::runtime::testing
{

namespace
{
    struct Start
    {
        void (*entry)(void*) noexcept;
        void* argument;
    };
} // namespace

void runOnStackThread(std::size_t bytes, void (*entry)(void*) noexcept, void* argument)
{
    auto attributes = pthread_attr_t {};
    Require(pthread_attr_init(&attributes) == 0);
    // macOS refuses a stack size that is not a multiple of the page size (16 KiB on Apple silicon).
    auto const pageSize = sysconf(_SC_PAGESIZE);
    Require(pageSize > 0);
    auto const page = static_cast<std::size_t>(pageSize);
    auto const wanted = std::max<std::size_t>(bytes, PTHREAD_STACK_MIN);
    Require(pthread_attr_setstacksize(&attributes, (wanted + page - 1) / page * page) == 0);
    auto start = Start { .entry = entry, .argument = argument };
    auto thread = pthread_t {};
    Require(pthread_create(
                &thread,
                &attributes,
                [](void* raw) -> void* {
                    auto const* started = static_cast<Start const*>(raw);
                    started->entry(started->argument);
                    return nullptr;
                },
                &start)
            == 0);
    Require(pthread_join(thread, nullptr) == 0);
    pthread_attr_destroy(&attributes);
}

std::uintptr_t stackAddress() noexcept
{
    return reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
}

} // namespace isoline::component::runtime::testing
