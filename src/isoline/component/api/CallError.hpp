// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
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
    ArgumentMismatch, ///< The arguments are not of the function's types, or of its store: nothing ran.
    ForeignFunction,  ///< The function, or the resource, is of another store: nothing ran.
};

/// The last ComponentCallFailure enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastComponentCallFailure = ComponentCallFailure::ForeignFunction;

/// What a failure did to the store.
enum class StoreEffect : uint8_t
{
    Unchanged, ///< Nothing: no guest code ran, and the store is as it was.
    LocksDown, ///< It locked the store down (D6): no guest code of it runs again.
};

/// What a ComponentCallError carries beside its failure: nothing, a trap, a host function's failure, or
/// the first argument not of its type. ComponentCallFailureTable names the alternative of each failure.
using ComponentCallDetail =
    std::variant<std::monostate, canonical::Trap, isoline::api::HostFailure, canonical::ValueError>;

/// One row of ComponentCallFailureTable: a failure, the detail it carries, what it does to the store,
/// and its text.
struct ComponentCallFailureRow
{
    ComponentCallFailure failure; ///< The failure; equal to the row's index.
    std::size_t detail;           ///< The index of its ComponentCallDetail alternative.
    std::string_view alternative; ///< That alternative, named.
    StoreEffect effect;           ///< What it does to the store.
    std::string_view text;        ///< Its text, which message() follows with the detail's.
};

/// Every call failure, one row per ComponentCallFailure enumerator, in enumerator order.
inline constexpr auto ComponentCallFailureTable = std::to_array<ComponentCallFailureRow>({
    { .failure = ComponentCallFailure::Trap,
      .detail = 1,
      .alternative = "canonical::Trap",
      .effect = StoreEffect::LocksDown,
      .text = "component call trapped" },
    { .failure = ComponentCallFailure::Interrupted,
      .detail = 1,
      .alternative = "canonical::Trap",
      .effect = StoreEffect::LocksDown,
      .text = "component call interrupted" },
    { .failure = ComponentCallFailure::HostFailure,
      .detail = 2,
      .alternative = "isoline::api::HostFailure",
      .effect = StoreEffect::LocksDown,
      .text = "host function failed" },
    { .failure = ComponentCallFailure::LockedDown,
      .detail = 0,
      .alternative = "std::monostate",
      .effect = StoreEffect::Unchanged,
      .text = "component store is locked down" },
    { .failure = ComponentCallFailure::ArgumentMismatch,
      .detail = 3,
      .alternative = "canonical::ValueError",
      .effect = StoreEffect::Unchanged,
      .text = "arguments do not match the function" },
    { .failure = ComponentCallFailure::ForeignFunction,
      .detail = 0,
      .alternative = "std::monostate",
      .effect = StoreEffect::Unchanged,
      .text = "function or resource of another component store" },
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
