// SPDX-License-Identifier: Apache-2.0
// The page protection of an address on macOS: its region as mach_vm_region reports it.

#include <bit>
#include <cstdint>
#include <optional>
#include <string>

#include <isoline/execution/testing/PageProtection.hpp>
#include <mach/mach.h>
#include <mach/mach_vm.h>

namespace isoline::execution::testing
{

std::optional<std::string> protectionOf(void const* address)
{
    auto region = static_cast<mach_vm_address_t>(std::bit_cast<std::uintptr_t>(address));
    auto size = mach_vm_size_t { 0 };
    auto info = vm_region_basic_info_data_64_t {};
    auto count = mach_msg_type_number_t { VM_REGION_BASIC_INFO_COUNT_64 };
    auto object = mach_port_t { MACH_PORT_NULL };
    if (::mach_vm_region(::mach_task_self(),
                         &region,
                         &size,
                         VM_REGION_BASIC_INFO_64,
                         reinterpret_cast<vm_region_info_t>(&info),
                         &count,
                         &object)
            != KERN_SUCCESS
        || region > static_cast<mach_vm_address_t>(std::bit_cast<std::uintptr_t>(address)))
        return std::nullopt;
    return std::string { (info.protection & VM_PROT_READ) != 0 ? 'r' : '-',
                         (info.protection & VM_PROT_WRITE) != 0 ? 'w' : '-',
                         (info.protection & VM_PROT_EXECUTE) != 0 ? 'x' : '-' };
}

} // namespace isoline::execution::testing
