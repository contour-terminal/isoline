// SPDX-License-Identifier: Apache-2.0
#include <isoline/binary/TypeCanon.hpp>

#include <core/Assert.hpp>

#include <cstdint>
#include <map>
#include <span>
#include <utility>
#include <vector>

#include <isoline/binary/Types.hpp>

namespace isoline::binary
{

std::vector<uint32_t> canonicalTypeIds(std::span<FuncType const> types)
{
    auto ids = std::vector<uint32_t> {};
    ids.reserve(types.size());
    // Each type rewritten to canonical ids, mapped to the first index it was seen at.
    auto firstOf = std::map<FuncType, uint32_t> {};
    for (auto const& type: types)
    {
        auto const index = static_cast<uint32_t>(ids.size());
        auto canonical = mapTypeIndices(type, [&](uint32_t referenced) {
            Require(referenced < index);
            return ids[referenced];
        });
        ids.push_back(firstOf.try_emplace(std::move(canonical), index).first->second);
    }
    return ids;
}

} // namespace isoline::binary
