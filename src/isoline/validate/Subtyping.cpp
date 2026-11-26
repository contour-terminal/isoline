// SPDX-License-Identifier: Apache-2.0
#include <isoline/validate/Subtyping.hpp>

#include <core/Assert.hpp>

#include <cstdint>
#include <span>

#include <isoline/binary/Types.hpp>

namespace isoline::validate
{

namespace
{
    using binary::AbstractHeapType;
    using binary::ValueKind;
    using binary::ValueType;

    /// Whether @p sub is a subtype of @p super, two concrete types being compared by
    /// @p equivalent(sub index, super index).
    template <typename Equivalent>
    [[nodiscard]] bool isSubtypeBy(ValueType const& sub,
                                   ValueType const& super,
                                   Equivalent const& equivalent) noexcept
    {
        if (sub.kind != ValueKind::Ref || super.kind != ValueKind::Ref)
            return sub.kind == super.kind;
        if (sub.nullable == binary::Nullable::Yes && super.nullable == binary::Nullable::No)
            return false;
        auto const& subRow = heapRow(sub.heap.kind);
        auto const& superRow = heapRow(super.heap.kind);
        if (subRow.rank == HeapRank::BelowAll)
            return true;
        if (subRow.top != superRow.top)
            return false;
        if (sub.heap.kind == AbstractHeapType::Concrete && super.heap.kind == AbstractHeapType::Concrete)
            return equivalent(sub.heap.typeIndex, super.heap.typeIndex);
        return sub.heap.kind == super.heap.kind || superRow.rank == HeapRank::Top
               || subRow.rank == HeapRank::Bottom;
    }
} // namespace

bool isSubtype(ValueType sub, ValueType super, std::span<uint32_t const> canonical) noexcept
{
    return isSubtypeBy(sub, super, [canonical](uint32_t subIndex, uint32_t superIndex) {
        Require(subIndex < canonical.size() && superIndex < canonical.size());
        return canonical[subIndex] == canonical[superIndex];
    });
}

bool isStoreSubtype(ValueType sub, ValueType super) noexcept
{
    return isSubtypeBy(
        sub, super, [](uint32_t subIndex, uint32_t superIndex) { return subIndex == superIndex; });
}

} // namespace isoline::validate
