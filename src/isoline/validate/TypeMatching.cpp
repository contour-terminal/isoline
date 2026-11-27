// SPDX-License-Identifier: Apache-2.0
#include <isoline/validate/TypeMatching.hpp>

#include <core/Assert.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <isoline/binary/Module.hpp>
#include <isoline/binary/TypeCanon.hpp>
#include <isoline/binary/Types.hpp>

namespace isoline::validate
{

CoreExternType externTypeOf(binary::Import const& import)
{
    return CoreExternType { .kind = import.kind,
                            .typeIndex = import.typeIndex,
                            .table = import.table,
                            .memory = import.memory,
                            .global = import.global };
}

bool limitsMatch(binary::Limits const& sub, binary::Limits const& super) noexcept
{
    if (sub.addressType != super.addressType || sub.min < super.min)
        return false;
    return !super.max || (sub.max && *sub.max <= *super.max);
}

bool memoryTypeMatches(binary::MemoryType const& sub, binary::MemoryType const& super) noexcept
{
    return sub.pageSizeLog2 == super.pageSizeLog2 && limitsMatch(sub.limits, super.limits);
}

TypeMatcher::TypeMatcher(std::span<binary::FuncType const> sub, std::span<binary::FuncType const> super):
    _subCount { sub.size() }, _ids { binary::jointCanonicalTypeIds(std::array { sub, super }) }
{
}

bool TypeMatcher::matches(uint32_t sub, uint32_t super) const
{
    Require(sub < _subCount && super < _ids.size() - _subCount);
    return _ids[sub] == _ids[_subCount + super];
}

bool TypeMatcher::matchesExtern(CoreExternType const& sub, CoreExternType const& super) const
{
    if (sub.kind != super.kind)
        return false;
    auto const equivalent = [this](uint32_t subIndex, uint32_t superIndex) {
        return matches(subIndex, superIndex);
    };
    static_assert(binary::ExternKindCount == 5, "a new extern kind needs its matching rule here");
    if (sub.kind == binary::ExternKind::Table)
        return tableTypeMatches(sub.table, super.table, equivalent);
    if (sub.kind == binary::ExternKind::Memory)
        return memoryTypeMatches(sub.memory, super.memory);
    if (sub.kind == binary::ExternKind::Global)
        return globalTypeMatches(sub.global, super.global, equivalent);
    // A function's or a tag's type matches only an equivalent one: an exception's payload is read by
    // the tag's type.
    return matches(sub.typeIndex, super.typeIndex);
}

bool matches(std::span<binary::FuncType const> a,
             uint32_t sub,
             std::span<binary::FuncType const> b,
             uint32_t super)
{
    return TypeMatcher { a, b }.matches(sub, super);
}

bool matchesExtern(std::span<binary::FuncType const> a,
                   CoreExternType const& sub,
                   std::span<binary::FuncType const> b,
                   CoreExternType const& super)
{
    return TypeMatcher { a, b }.matchesExtern(sub, super);
}

} // namespace isoline::validate
