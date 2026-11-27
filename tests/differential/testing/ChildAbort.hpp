// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace isoline::differential::testing
{

/// Ends the process abnormally, as a crash would: SIGABRT on POSIX (ChildAbortPosix.cpp), a fail-fast
/// status (an NTSTATUS error) on Windows (ChildAbortWindows.cpp), where abort() of a Debug C runtime would
/// open a dialog box instead.
[[noreturn]] void endAbnormally();

} // namespace isoline::differential::testing
