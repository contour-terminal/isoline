// SPDX-License-Identifier: Apache-2.0
#include <isoline/runtime/ValueCheck.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <ranges>
#include <span>
#include <variant>

#include <isoline/binary/Types.hpp>
#include <isoline/runtime/Abrupt.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/validate/Subtyping.hpp>

namespace isoline::runtime
{

namespace
{
    using binary::AbstractHeapType;

    /// The hierarchy a value's heap belongs to, which the value may name arbitrarily: nullopt for
    /// bot, which belongs to none, and for a heap no AbstractHeapType names.
    /// @param heap The heap a value claims.
    /// @return The top of its hierarchy, or nullopt.
    [[nodiscard]] std::optional<AbstractHeapType> familyOf(binary::HeapType heap) noexcept
    {
        if (heap.kind == AbstractHeapType::Bot || heap.kind > AbstractHeapType::Concrete)
            return std::nullopt;
        return validate::topOf(heap.kind);
    }

    /// Whether the referent at @p address exists in @p store.
    /// @param store The store.
    /// @param referent What the address names.
    /// @param address The address.
    /// @return Whether the store holds it; a host object always is.
    [[nodiscard]] bool exists(Store const& store, Referent referent, uint32_t address) noexcept
    {
        if (referent == Referent::Function)
            return address < store.functionCount();
        if (referent == Referent::Exception)
            return store.isLiveException(address);
        return true;
    }

    /// Why @p reference is not of @p type in @p store, if it is not.
    /// @param store The store.
    /// @param reference The reference.
    /// @param type A reference type, in store form.
    /// @return Nothing, or the mismatch's kind.
    [[nodiscard]] std::expected<void, MismatchKind> checkReference(Store const& store,
                                                                   Ref const& reference,
                                                                   binary::ValueType const& type) noexcept
    {
        if (static_cast<std::size_t>(reference.kind) >= ReferenceTable.size())
            return std::unexpected { MismatchKind::Kind };
        auto const& row = ReferenceTable[static_cast<std::size_t>(reference.kind)];
        auto const typeTop = validate::topOf(type.heap.kind);
        if (row.referent == Referent::Nothing)
        {
            if (type.nullable == binary::Nullable::No)
                return std::unexpected { MismatchKind::NotNullable };
            if (familyOf(reference.heap) != typeTop)
                return std::unexpected { MismatchKind::Heap };
            return {};
        }
        if (row.top != typeTop)
            return std::unexpected { MismatchKind::Heap };
        if (!exists(store, row.referent, reference.address))
            return std::unexpected { MismatchKind::ForeignReference };
        auto const rank = validate::heapRow(type.heap.kind).rank;
        if (rank == validate::HeapRank::Top)
            return {};
        // A concrete type is a function type, the only one below func, so the reference is a function's:
        // it is of the type when its TypeId is the one the heap names. Below that lies only the bottom,
        // which holds no reference but null.
        if (rank == validate::HeapRank::Defined
            && store.function(reference.address).typeId == type.heap.typeIndex)
            return {};
        return std::unexpected { MismatchKind::Heap };
    }

    /// Why @p value is not of @p type in @p store, if it is not.
    /// @param store The store.
    /// @param value The value.
    /// @param type Its type, in store form.
    /// @return Nothing, or the mismatch's kind.
    [[nodiscard]] std::expected<void, MismatchKind> checkValue(Store const& store,
                                                               Value const& value,
                                                               binary::ValueType const& type) noexcept
    {
        if (kindOf(value) != type.kind)
            return std::unexpected { MismatchKind::Kind };
        if (auto const* reference = std::get_if<Ref>(&value))
            return checkReference(store, *reference, type);
        return {};
    }

} // namespace

std::expected<void, ValueMismatch> checkValues(Store const& store,
                                               std::span<Value const> values,
                                               std::span<binary::ValueType const> types) noexcept
{
    if (values.size() != types.size())
        return std::unexpected { ValueMismatch {
            .kind = MismatchKind::Count,
            .index = static_cast<uint32_t>(std::min(values.size(), types.size())) } };
    for (auto const index: std::views::iota(std::size_t { 0 }, values.size()))
        if (auto const checked = checkValue(store, values[index], types[index]); !checked)
            return std::unexpected { ValueMismatch { .kind = checked.error(),
                                                     .index = static_cast<uint32_t>(index) } };
    return {};
}

} // namespace isoline::runtime
