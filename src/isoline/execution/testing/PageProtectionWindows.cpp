// SPDX-License-Identifier: Apache-2.0
// The page protection of an address on Windows: its region as VirtualQuery reports it.

#include <optional>
#include <string>

#include <windows.h>

#include <isoline/execution/testing/PageProtection.hpp>

namespace isoline::execution::testing
{

std::optional<std::string> protectionOf(void const* address)
{
    auto information = MEMORY_BASIC_INFORMATION {};
    if (::VirtualQuery(address, &information, sizeof(information)) == 0 || information.State != MEM_COMMIT)
        return std::nullopt;
    auto const protection = information.Protect & 0xffU;
    auto const isExecutable = protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ
                              || protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
    auto const isWritable = protection == PAGE_READWRITE || protection == PAGE_WRITECOPY
                            || protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
    return std::string { protection != PAGE_NOACCESS && protection != PAGE_EXECUTE ? 'r' : '-',
                         isWritable ? 'w' : '-',
                         isExecutable ? 'x' : '-' };
}

} // namespace isoline::execution::testing
