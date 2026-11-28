// SPDX-License-Identifier: Apache-2.0
// The page protection of an address on Linux: its mapping's row of /proc/self/maps.

#include <bit>
#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

#include <isoline/execution/testing/PageProtection.hpp>

namespace isoline::execution::testing
{

std::optional<std::string> protectionOf(void const* address)
{
    auto const at = std::bit_cast<std::uintptr_t>(address);
    auto maps = std::ifstream { "/proc/self/maps" };
    auto line = std::string {};
    while (std::getline(maps, line))
    {
        // "start-end perms offset device inode path", the addresses in hexadecimal.
        auto row = std::istringstream { line };
        auto start = std::uintptr_t { 0 };
        auto end = std::uintptr_t { 0 };
        auto dash = char {};
        auto permissions = std::string {};
        row >> std::hex >> start >> dash >> end >> permissions;
        if (at < start || at >= end || permissions.size() < 3)
            continue;
        return permissions.substr(0, 3);
    }
    return std::nullopt;
}

} // namespace isoline::execution::testing
