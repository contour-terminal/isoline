// SPDX-License-Identifier: Apache-2.0
#include <isoline/validate/Subtyping.hpp>

#include <core/Assert.hpp>

#include <cstdint>
#include <span>

#include <isoline/binary/Types.hpp>

namespace isoline::validate
{

bool isSubtype(binary::ValueType sub, binary::ValueType super, std::span<uint32_t const> canonical) noexcept
{
    return isSubtypeWith(sub, super, [canonical](uint32_t subIndex, uint32_t superIndex) {
        Require(subIndex < canonical.size() && superIndex < canonical.size());
        return canonical[subIndex] == canonical[superIndex];
    });
}

bool isStoreSubtype(binary::ValueType sub, binary::ValueType super) noexcept
{
    return isSubtypeWith(
        sub, super, [](uint32_t subIndex, uint32_t superIndex) { return subIndex == superIndex; });
}

} // namespace isoline::validate
