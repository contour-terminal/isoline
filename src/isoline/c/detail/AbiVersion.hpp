// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace isoline::c::detail
{

/// An ABI version of the C API, as ISOLINE_ABI_VERSION packs it: (major << 16) | minor.
struct AbiVersion
{
    std::uint32_t major; ///< Changes when a release breaks programs built for the one before; 0 before 1.0.
    std::uint32_t minor; ///< Changes when a release adds a symbol or a layout.

    /// The version packed in @p packed, as isoline_abi_version() returns it.
    [[nodiscard]] static constexpr AbiVersion unpack(std::uint32_t packed) noexcept
    {
        return AbiVersion { .major = packed >> 16U, .minor = packed & 0xFFFFU };
    }
};

/// Whether a library of ABI @p library serves a program compiled against the header of ABI @p header
/// (plan P17a, D8): the majors are equal and the library's minor is at least the header's, and before 1.0,
/// where every minor may break the one before it, the minors are equal.
/// @return True when the program may use the library.
[[nodiscard]] constexpr bool compatible(AbiVersion library, AbiVersion header) noexcept
{
    if (header.major != library.major)
        return false;
    if (library.major == 0)
        return header.minor == library.minor;
    return header.minor <= library.minor;
}

} // namespace isoline::c::detail
