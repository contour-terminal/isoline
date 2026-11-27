// SPDX-License-Identifier: Apache-2.0
#include <isoline/binary/TypeCanon.hpp>

#include <core/Assert.hpp>

#include <array>
#include <cstddef>
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
    auto const sections = std::array { types };
    return jointCanonicalTypeIds(sections);
}

std::vector<uint32_t> jointCanonicalTypeIds(std::span<std::span<FuncType const> const> sections)
{
    auto total = std::size_t { 0 };
    for (auto const& section: sections)
        total += section.size();
    auto ids = std::vector<uint32_t> {};
    ids.reserve(total);
    // Each type rewritten to canonical ids, mapped to the first position it was seen at.
    auto firstOf = std::map<FuncType, uint32_t> {};
    for (auto const& section: sections)
    {
        auto const sectionStart = static_cast<uint32_t>(ids.size());
        for (auto const& type: section)
        {
            auto const position = static_cast<uint32_t>(ids.size());
            auto canonical = mapTypeIndices(type, [&](uint32_t referenced) {
                Require(referenced < position - sectionStart);
                return ids[sectionStart + referenced];
            });
            ids.push_back(firstOf.try_emplace(std::move(canonical), position).first->second);
        }
    }
    return ids;
}

} // namespace isoline::binary
