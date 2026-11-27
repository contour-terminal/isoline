// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <expected>
#include <functional>

#include <windows.h>

#include <isoline/runtime/testing/SmallStackThread.hpp>

namespace isoline::runtime::testing
{

namespace
{
    /// The thread's entry: runs the work its argument points at.
    DWORD WINAPI runWork(LPVOID work)
    {
        (*static_cast<std::function<void()>*>(work))();
        return 0;
    }
} // namespace

std::expected<void, SmallStackError> runOnSmallStack(std::size_t stackBytes,
                                                     std::function<void()> const& work)
{
    // The thread takes its argument as a pointer: a copy of the work this frame owns, alive until the join.
    auto task = work;
    // The size reserves the stack, rather than committing it, so that it is the stack's whole size.
    auto* const thread =
        CreateThread(nullptr, stackBytes, &runWork, &task, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (thread == nullptr)
        return std::unexpected { SmallStackError::Unavailable };
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    return {};
}

} // namespace isoline::runtime::testing
