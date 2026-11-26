// SPDX-License-Identifier: Apache-2.0
#include <isoline/runtime/ValueCheck.hpp>

#include <algorithm>
#include <array>
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
    using validate::HeapRank;

    /// What a non-null reference's address must name in the store, and so how it is checked.
    enum class Referent : uint8_t
    {
        Nothing,   ///< A null reference names nothing: the type must admit null, in the null's hierarchy.
        Host,      ///< A host object, whose address is the embedder's: any is accepted.
        Function,  ///< A function, which must exist in the store.
        Exception, ///< An exception, which must be live in the store (Store::isLiveException).
    };

    /// One row of ReferenceTable: a reference kind, the heap hierarchy it belongs to, and its referent.
    struct ReferenceRow
    {
        RefKind kind; ///< The reference kind; equal to the row's index.
        /// The top of the hierarchy whose heaps accept it; nullopt for a null, which belongs to the
        /// hierarchy of its own heap.
        std::optional<AbstractHeapType> top;
        Referent referent; ///< What its address names, and so how it is checked.
    };

    /// Every reference kind, one row per RefKind enumerator in enumerator order: what checkValues()
    /// requires of a reference of that kind.
    constexpr auto ReferenceTable = std::to_array<ReferenceRow>({
        { .kind = RefKind::Null, .top = std::nullopt, .referent = Referent::Nothing },
        { .kind = RefKind::Func, .top = AbstractHeapType::Func, .referent = Referent::Function },
        { .kind = RefKind::Extern, .top = AbstractHeapType::Extern, .referent = Referent::Host },
        { .kind = RefKind::Exn, .top = AbstractHeapType::Exn, .referent = Referent::Exception },
    });

    static_assert(
        [] {
            auto index = std::size_t { 0 };
            for (auto const& row: ReferenceTable)
                if (static_cast<std::size_t>(row.kind) != index++)
                    return false;
            return index == static_cast<std::size_t>(RefKind::Exn) + 1;
        }(),
        "ReferenceTable must hold one row per RefKind, at the index of its enumerator");

    /// Which non-null references of its hierarchy a heap of some rank accepts.
    enum class Acceptance : uint8_t
    {
        None,           ///< None: the bottom holds no reference but null, and bot belongs to no hierarchy.
        All,            ///< Every one: the top.
        ByFunctionType, ///< A function's, when the function's TypeId is the heap's: a concrete type, which
                        ///< is a function type, the only kind of type below func.
    };

    /// One row of RankTable: a heap's rank, and the non-null references it accepts.
    struct RankRow
    {
        HeapRank rank;         ///< The rank; equal to the row's index.
        Acceptance acceptance; ///< The non-null references a heap of that rank accepts.
    };

    /// Every HeapRank, one row per enumerator in enumerator order.
    constexpr auto RankTable = std::to_array<RankRow>({
        { .rank = HeapRank::Top, .acceptance = Acceptance::All },
        { .rank = HeapRank::Defined, .acceptance = Acceptance::ByFunctionType },
        { .rank = HeapRank::Bottom, .acceptance = Acceptance::None },
        { .rank = HeapRank::BelowAll, .acceptance = Acceptance::None },
    });

    static_assert(
        [] {
            auto index = std::size_t { 0 };
            for (auto const& row: RankTable)
                if (static_cast<std::size_t>(row.rank) != index++)
                    return false;
            return index == static_cast<std::size_t>(HeapRank::BelowAll) + 1;
        }(),
        "RankTable must hold one row per HeapRank, at the index of its enumerator");

    /// The hierarchy a value's heap belongs to, which the value may name arbitrarily: nullopt for
    /// bot, which belongs to none, and for a heap no AbstractHeapType names.
    /// @param heap The heap a value claims.
    /// @return The top of its hierarchy, or nullopt.
    [[nodiscard]] std::optional<AbstractHeapType> familyOf(binary::HeapType heap) noexcept
    {
        if (heap.kind == AbstractHeapType::Bot
            || static_cast<std::size_t>(heap.kind) >= validate::HeapTable.size())
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
        auto const acceptance =
            RankTable[static_cast<std::size_t>(validate::heapRow(type.heap.kind).rank)].acceptance;
        if (acceptance == Acceptance::All)
            return {};
        if (acceptance == Acceptance::ByFunctionType
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
