// SPDX-License-Identifier: Apache-2.0
#include <isoline/c/abi.h>

#include <cstdint>

#include <isoline/c/detail/AbiVersion.hpp>

using isoline::c::detail::AbiVersion;

uint32_t ISOLINE_CALL isoline_abi_version() noexcept
{
    return ISOLINE_ABI_VERSION;
}

bool ISOLINE_CALL isoline_abi_compatible(uint32_t headerVersion) noexcept
{
    return isoline::c::detail::compatible(AbiVersion::unpack(ISOLINE_ABI_VERSION),
                                          AbiVersion::unpack(headerVersion));
}
