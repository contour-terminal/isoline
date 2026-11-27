// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include <isoline/binary/Types.hpp>
#include <isoline/meta/EnumRows.hpp>
#include <isoline/runtime/Value.hpp>
#include <isoline/validate/Subtyping.hpp>

// The tables checkValues() reads (runtime/ValueCheck.cpp), a header of their own so that the reflection
// checks reach them (meta/EnumTables.def). Installed with isoline_core's headers, as every header the
// module surface includes is, and C++23 like each of them.

namespace isoline::runtime
{

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
    std::optional<binary::AbstractHeapType> top;
    Referent referent; ///< What its address names, and so how it is checked.
};

/// Every reference kind, one row per RefKind enumerator in enumerator order: what checkValues()
/// requires of a reference of that kind.
inline constexpr auto ReferenceTable = std::to_array<ReferenceRow>({
    { .kind = RefKind::Null, .top = std::nullopt, .referent = Referent::Nothing },
    { .kind = RefKind::Func, .top = binary::AbstractHeapType::Func, .referent = Referent::Function },
    { .kind = RefKind::Extern, .top = binary::AbstractHeapType::Extern, .referent = Referent::Host },
    { .kind = RefKind::Exn, .top = binary::AbstractHeapType::Exn, .referent = Referent::Exception },
});

static_assert(meta::firstMisplacedRow<LastRefKind>(ReferenceTable, &ReferenceRow::kind)
                  == meta::NoMisplacedRow,
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
    validate::HeapRank rank; ///< The rank; equal to the row's index.
    Acceptance acceptance;   ///< The non-null references a heap of that rank accepts.
};

/// Every HeapRank, one row per enumerator in enumerator order.
inline constexpr auto RankTable = std::to_array<RankRow>({
    { .rank = validate::HeapRank::Top, .acceptance = Acceptance::All },
    { .rank = validate::HeapRank::Defined, .acceptance = Acceptance::ByFunctionType },
    { .rank = validate::HeapRank::Bottom, .acceptance = Acceptance::None },
    { .rank = validate::HeapRank::BelowAll, .acceptance = Acceptance::None },
});

static_assert(meta::firstMisplacedRow<validate::LastHeapRank>(RankTable, &RankRow::rank)
                  == meta::NoMisplacedRow,
              "RankTable must hold one row per HeapRank, at the index of its enumerator");

} // namespace isoline::runtime
