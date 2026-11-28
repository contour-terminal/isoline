// SPDX-License-Identifier: Apache-2.0
#pragma once

// How the system protects the page holding an address, for the code memory tests. The platform's half is
// CMake-selected (PageProtectionLinux.cpp reads /proc/self/maps, PageProtectionMacOS.cpp asks mach_vm_region,
// PageProtectionWindows.cpp VirtualQuery). Tests only.

#include <optional>
#include <string>

namespace isoline::execution::testing
{

/// The protection of the page holding @p address, as /proc/self/maps spells it: three letters, `r`, `w` and
/// `x` where reads, writes and instruction fetch are allowed, `-` where they are not ("r-x").
/// @return It; none when no mapping holds @p address.
[[nodiscard]] std::optional<std::string> protectionOf(void const* address);

} // namespace isoline::execution::testing
