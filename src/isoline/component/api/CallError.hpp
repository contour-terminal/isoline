// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include <isoline/api/Types.hpp>
#include <isoline/component/canonical/Trap.hpp>
#include <isoline/component/canonical/Values.hpp>

namespace isoline::component::api
{

/// How a component call, or a drop of a resource the host holds, failed.
///
/// The enumerators are the rows of ComponentCallFailureTable, in the same order.
enum class ComponentCallFailure : uint8_t
{
    Trap,             ///< The call trapped (detail: the trap).
    Interrupted,      ///< The store's interrupter stopped it (detail: the Interrupted trap, R13).
    HostFailure,      ///< A host function it called failed (detail: the failure, R13).
    LockedDown,       ///< The store was locked down already: nothing ran.
    ArgumentMismatch, ///< The arguments are not of the function's types, or not the host's: nothing ran.
    ForeignFunction,  ///< The function, or the resource, is of another store: nothing ran.
    StoreDeleted,     ///< The store the instance or function belongs to is gone: nothing ran.
    Instantiating,    ///< The store is instantiating (a drop from a start function's host call): nothing ran.
    /// The owns the host holds would pass a cap (detail: the HostOwns or LiveValueBytes trap); the owns
    /// that could not be held are destroyed at the store's end.
    ResourceExhausted,
};

/// The last ComponentCallFailure enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastComponentCallFailure = ComponentCallFailure::ResourceExhausted;

/// What a failure did to the store.
///
/// The enumerators are the rows of StoreEffectTexts, in the same order.
enum class StoreEffect : uint8_t
{
    Unchanged, ///< Nothing: no guest code ran, and the store is as it was.
    LocksDown, ///< It locked the store down (D6): no guest code of it runs again.
};

/// Each StoreEffect's words, in enumerator order, as docs/embedding.md's generated block reads them.
inline constexpr auto StoreEffectTexts = std::to_array<std::string_view>({ "unchanged", "locked down" });

static_assert(StoreEffectTexts.size() == static_cast<std::size_t>(StoreEffect::LocksDown) + 1,
              "StoreEffectTexts must hold one text per StoreEffect");

/// What a ComponentCallError carries beside its failure: nothing, a trap, a host function's failure, or
/// the first argument not of its type. ComponentCallFailureTable names the alternative of each failure.
using ComponentCallDetail =
    std::variant<std::monostate, canonical::Trap, isoline::api::HostFailure, canonical::ValueError>;

/// The index of @p T among ComponentCallDetail's alternatives, which a row derives its detail from.
template <typename T>
[[nodiscard]] consteval std::size_t detailIndexOf() noexcept
{
    return []<std::size_t... Index>(std::index_sequence<Index...>) {
        auto index = std::variant_size_v<ComponentCallDetail>;
        ((std::is_same_v<T, std::variant_alternative_t<Index, ComponentCallDetail>> ? (index = Index) : 0),
         ...);
        return index;
    }(std::make_index_sequence<std::variant_size_v<ComponentCallDetail>> {});
}

/// One row of ComponentCallFailureTable: a failure, the detail it carries, what it does to the store,
/// and its text.
struct ComponentCallFailureRow
{
    ComponentCallFailure failure; ///< The failure; equal to the row's index.
    std::string_view name;        ///< The failure's name, as the embedding guide lists it.
    std::size_t detail;           ///< The index of its ComponentCallDetail alternative (detailIndexOf()).
    std::string_view alternative; ///< That alternative, named.
    StoreEffect effect;           ///< What it does to the store; the api locks the store down by it.
    std::string_view text;        ///< Its text, which message() follows with the detail's.
};

/// Every call failure, one row per ComponentCallFailure enumerator, in enumerator order.
inline constexpr auto ComponentCallFailureTable = std::to_array<ComponentCallFailureRow>({
    { .failure = ComponentCallFailure::Trap,
      .name = "Trap",
      .detail = detailIndexOf<canonical::Trap>(),
      .alternative = "canonical::Trap",
      .effect = StoreEffect::LocksDown,
      .text = "component call trapped" },
    { .failure = ComponentCallFailure::Interrupted,
      .name = "Interrupted",
      .detail = detailIndexOf<canonical::Trap>(),
      .alternative = "canonical::Trap",
      .effect = StoreEffect::LocksDown,
      .text = "component call interrupted" },
    { .failure = ComponentCallFailure::HostFailure,
      .name = "HostFailure",
      .detail = detailIndexOf<isoline::api::HostFailure>(),
      .alternative = "isoline::api::HostFailure",
      .effect = StoreEffect::LocksDown,
      .text = "host function failed" },
    { .failure = ComponentCallFailure::LockedDown,
      .name = "LockedDown",
      .detail = detailIndexOf<std::monostate>(),
      .alternative = "std::monostate",
      .effect = StoreEffect::Unchanged,
      .text = "component store is locked down" },
    { .failure = ComponentCallFailure::ArgumentMismatch,
      .name = "ArgumentMismatch",
      .detail = detailIndexOf<canonical::ValueError>(),
      .alternative = "canonical::ValueError",
      .effect = StoreEffect::Unchanged,
      .text = "arguments do not match the function" },
    { .failure = ComponentCallFailure::ForeignFunction,
      .name = "ForeignFunction",
      .detail = detailIndexOf<std::monostate>(),
      .alternative = "std::monostate",
      .effect = StoreEffect::Unchanged,
      .text = "function or resource of another component store" },
    { .failure = ComponentCallFailure::StoreDeleted,
      .name = "StoreDeleted",
      .detail = detailIndexOf<std::monostate>(),
      .alternative = "std::monostate",
      .effect = StoreEffect::Unchanged,
      .text = "component store is gone" },
    { .failure = ComponentCallFailure::Instantiating,
      .name = "Instantiating",
      .detail = detailIndexOf<std::monostate>(),
      .alternative = "std::monostate",
      .effect = StoreEffect::Unchanged,
      .text = "component store is instantiating" },
    { .failure = ComponentCallFailure::ResourceExhausted,
      .name = "ResourceExhausted",
      .detail = detailIndexOf<canonical::Trap>(),
      .alternative = "canonical::Trap",
      .effect = StoreEffect::LocksDown,
      .text = "component store resources exhausted" },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: ComponentCallFailureTable)
        {
            if (static_cast<std::size_t>(row.failure) != index
                || row.detail >= std::variant_size_v<ComponentCallDetail>)
                return false;
            ++index;
        }
        return index == static_cast<std::size_t>(LastComponentCallFailure) + 1;
    }(),
    "ComponentCallFailureTable must hold one row per ComponentCallFailure, at the index of its enumerator, "
    "each naming an alternative of ComponentCallDetail");

/// The row of @p failure.
/// @param failure A failure.
/// @return Its ComponentCallFailureTable row.
[[nodiscard]] constexpr ComponentCallFailureRow const& componentCallFailureRow(
    ComponentCallFailure failure) noexcept
{
    return ComponentCallFailureTable[static_cast<std::size_t>(failure)];
}

/// A failed component call: how it failed, and what its row says the failure carries.
struct ComponentCallError
{
    ComponentCallFailure failure = ComponentCallFailure::LockedDown; ///< How.
    ComponentCallDetail detail {};                                   ///< The alternative its row names.

    /// The error's text: its row's, then the detail's after a colon (none for std::monostate).
    /// @return It.
    /// @throws std::bad_alloc When the text cannot be stored.
    [[nodiscard]] std::string message() const;

    /// Two errors are equal when their failures and details are.
    bool operator==(ComponentCallError const&) const = default;
};

/// A call error of @p failure whose detail is @p detail, checked against the failure's row.
/// @param failure How the call failed.
/// @param detail What its row says it carries (checked).
/// @return The error.
[[nodiscard]] ComponentCallError callError(ComponentCallFailure failure, ComponentCallDetail detail = {});

} // namespace isoline::component::api
