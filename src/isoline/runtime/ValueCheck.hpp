// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <expected>
#include <optional>
#include <span>

#include <isoline/binary/Types.hpp>
#include <isoline/runtime/Abrupt.hpp>
#include <isoline/runtime/Store.hpp>
#include <isoline/runtime/Value.hpp>

namespace isoline::runtime
{

/// What a non-null reference's address must name in the store, and so how it is checked.
enum class Referent : uint8_t
{
    Nothing,   ///< A null reference names nothing: the type must admit null, in the null's hierarchy.
    Host,      ///< A host object, whose address is the embedder's: any is accepted.
    Function,  ///< A function, which must exist in the store; a concrete type heap names its TypeId.
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
/// requires of a reference of that kind. A non-null reference is accepted by its hierarchy's top, and
/// by a concrete type below it whose TypeId is its function's; never by the bottom.
inline constexpr auto ReferenceTable = std::to_array<ReferenceRow>({
    { .kind = RefKind::Null, .top = std::nullopt, .referent = Referent::Nothing },
    { .kind = RefKind::Func, .top = binary::AbstractHeapType::Func, .referent = Referent::Function },
    { .kind = RefKind::Extern, .top = binary::AbstractHeapType::Extern, .referent = Referent::Host },
    { .kind = RefKind::Exn, .top = binary::AbstractHeapType::Exn, .referent = Referent::Exception },
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

/// Whether @p values are of @p types in @p store: what the host boundary checks of arguments, host
/// results and exception payloads, so that a value of the wrong kind, a reference outside its type's
/// heap, or an address the store does not hold is refused rather than reaching a precondition.
///
/// A count mismatch is reported first, at the first position the shorter list lacks: values that
/// do not pair up with their types are not compared one by one. Otherwise the first value that is
/// not of its kind, or a reference that ReferenceTable's row refuses, is reported at its position.
/// A reference carries no store identity, so a reference of another store whose address happens to
/// exist in this one is not told apart; it is still within this store, so using it stays safe.
///
/// Precondition (checked): every reference type in @p types is in store form, its heap not bot.
/// @param store The store the values are for, whose functions and exceptions references name.
/// @param values The values.
/// @param types The types they must be of, in store form (concrete heaps name TypeIds).
/// @return Nothing, or the first mismatch.
[[nodiscard]] std::expected<void, ValueMismatch> checkValues(
    Store const& store, std::span<Value const> values, std::span<binary::ValueType const> types) noexcept;

} // namespace isoline::runtime
