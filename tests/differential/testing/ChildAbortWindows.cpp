// SPDX-License-Identifier: Apache-2.0
#include <cstdlib>

#include <windows.h>

#include "ChildAbort.hpp"

namespace isoline::differential::testing
{

namespace
{
    /// STATUS_STACK_BUFFER_OVERRUN: the status a fail-fast ends a process with.
    constexpr auto FailFastStatus = UINT { 0xC000'0409 };
} // namespace

void endAbnormally()
{
    TerminateProcess(GetCurrentProcess(), FailFastStatus);
    std::abort();
}

} // namespace isoline::differential::testing
