// SPDX-License-Identifier: Apache-2.0
#include <core/Assert.hpp>

#include <cstddef>
#include <cstdint>

#include <intrin.h>
#include <process.h>
#include <windows.h>

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

    unsigned __stdcall startThread(void* raw)
    {
        auto const* started = static_cast<Start const*>(raw);
        started->entry(started->argument);
        return 0;
    }
} // namespace

void runOnStackThread(std::size_t bytes, void (*entry)(void*) noexcept, void* argument)
{
    auto start = Start { .entry = entry, .argument = argument };
    auto const handle = _beginthreadex(nullptr,
                                       static_cast<unsigned>(bytes),
                                       &startThread,
                                       &start,
                                       STACK_SIZE_PARAM_IS_A_RESERVATION,
                                       nullptr);
    Require(handle != 0);
    auto* const thread = reinterpret_cast<HANDLE>(handle);
    Require(WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0);
    CloseHandle(thread);
}

std::uintptr_t stackAddress() noexcept
{
    return reinterpret_cast<std::uintptr_t>(_AddressOfReturnAddress());
}

} // namespace isoline::component::runtime::testing
