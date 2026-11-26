// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace isoline::component
{

/// The caps the component layer enforces on everything a component may make it hold (spec §7):
/// exceeding one is a refusal at load or a trap at run time, never host memory exhaustion. Each
/// field is one row of LimitTable, which names, describes and words it.
struct ComponentLimits
{
    uint64_t maxComponentBytes = uint64_t { 256 } << 20; ///< Bytes of one encoded component.
    /// Scopes open at once: components, component and instance types, nested scopes (binary and text).
    uint32_t maxNesting = 100;
    uint32_t maxTypeDepth = 100;      ///< Value-type nesting.
    uint32_t maxTypeSize = 1'000'000; ///< Effective type size per component (wasm-tools' rule, saturating).
    uint32_t maxTypes = 1'000'000;    ///< Per component, all index spaces of sort type.
    uint32_t maxDefinitions = 1'000'000;        ///< Per component, all sections.
    uint32_t maxNameBytes = 100'000;            ///< Bytes of one name.
    uint32_t maxCoreInstances = 10'000;         ///< Per ComponentStore.
    uint32_t maxComponentInstances = 10'000;    ///< Per ComponentStore.
    uint32_t maxInstantiationSteps = 1'000'000; ///< Plan steps executed by one instantiate().
    uint32_t maxResourceTypes = 100'000;        ///< Runtime resource types per ComponentStore (generative).
    /// Per ComponentStore, all handle tables together; each table also holds at most 2^28 - 1.
    uint32_t maxHandles = 1'000'000;
    /// Per ComponentStore: the host bytes of every live Values.
    uint64_t maxLiveValueBytes = uint64_t { 256 } << 20;

    /// Two sets of limits are equal when every limit is.
    bool operator==(ComponentLimits const&) const = default;
};

/// One cap of ComponentLimits, by name. The enumerators are the rows of LimitTable, in the same
/// order; adding a cap is adding a field there, an enumerator here and a row to the table.
enum class ComponentLimit : uint8_t
{
    ComponentBytes,
    Nesting,
    TypeDepth,
    TypeSize,
    Types,
    Definitions,
    NameBytes,
    CoreInstances,
    ComponentInstances,
    InstantiationSteps,
    ResourceTypes,
    Handles,
    LiveValueBytes,
};

/// The last ComponentLimit enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastComponentLimit = ComponentLimit::LiveValueBytes;

/// The number of ComponentLimit enumerators, as LastComponentLimit counts them.
inline constexpr auto ComponentLimitCount = static_cast<std::size_t>(LastComponentLimit) + 1;

/// One row of LimitTable: a cap, what it counts, and how its refusal reads.
struct ComponentLimitRow
{
    ComponentLimit limit;  ///< The cap this row describes; equal to the row's index.
    std::string_view name; ///< Its ComponentLimits field.
    std::string_view rule; ///< What it counts, and where it is enforced.
    /// The refusal's text, a std::format string of the cap's value; wasm-tools' own where it has one.
    std::string_view message;
    uint64_t (*value)(ComponentLimits const&) noexcept; ///< Reads the cap from a ComponentLimits.
};

/// Every cap, one row per ComponentLimit enumerator, in enumerator order.
inline constexpr auto LimitTable = std::to_array<ComponentLimitRow>({
    { .limit = ComponentLimit::ComponentBytes,
      .name = "maxComponentBytes",
      .rule = "bytes of one encoded component, refused before the decoder copies it",
      .message = "component size exceeds the limit of {} bytes",
      .value = [](ComponentLimits const& limits) noexcept -> uint64_t { return limits.maxComponentBytes; } },
    { .limit = ComponentLimit::Nesting,
      .name = "maxNesting",
      .rule = "scopes open at once, the outermost component counted: nested components, component and "
              "instance types, nested scopes, in the binary and the text format",
      .message = "nesting exceeds the limit of {} levels",
      .value = [](ComponentLimits const& limits) noexcept -> uint64_t { return limits.maxNesting; } },
    { .limit = ComponentLimit::TypeDepth,
      .name = "maxTypeDepth",
      .rule = "nesting of one value type",
      .message = "type nesting is too deep",
      .value = [](ComponentLimits const& limits) noexcept -> uint64_t { return limits.maxTypeDepth; } },
    { .limit = ComponentLimit::TypeSize,
      .name = "maxTypeSize",
      .rule = "effective type size per component, summed as wasm-tools sums it, saturating",
      .message = "effective type size exceeds the limit of {}",
      .value = [](ComponentLimits const& limits) noexcept -> uint64_t { return limits.maxTypeSize; } },
    { .limit = ComponentLimit::Types,
      .name = "maxTypes",
      .rule = "entries of every index space of sort type, per component",
      .message = "types count exceeds limit of {}",
      .value = [](ComponentLimits const& limits) noexcept -> uint64_t { return limits.maxTypes; } },
    { .limit = ComponentLimit::Definitions,
      .name = "maxDefinitions",
      .rule = "definitions of every section, custom sections included, per component",
      .message = "definitions count exceeds limit of {}",
      .value = [](ComponentLimits const& limits) noexcept -> uint64_t { return limits.maxDefinitions; } },
    { .limit = ComponentLimit::NameBytes,
      .name = "maxNameBytes",
      .rule = "bytes of one name, attribute or custom section name, refused before it is copied",
      .message = "string size out of bounds",
      .value = [](ComponentLimits const& limits) noexcept -> uint64_t { return limits.maxNameBytes; } },
    { .limit = ComponentLimit::CoreInstances,
      .name = "maxCoreInstances",
      .rule = "core instances of one ComponentStore",
      .message = "core instances count exceeds limit of {}",
      .value = [](ComponentLimits const& limits) noexcept -> uint64_t { return limits.maxCoreInstances; } },
    { .limit = ComponentLimit::ComponentInstances,
      .name = "maxComponentInstances",
      .rule = "component instances of one ComponentStore",
      .message = "component instances count exceeds limit of {}",
      .value = [](ComponentLimits const& limits) noexcept -> uint64_t {
          return limits.maxComponentInstances;
      } },
    { .limit = ComponentLimit::InstantiationSteps,
      .name = "maxInstantiationSteps",
      .rule = "plan steps one instantiate() executes",
      .message = "instantiation steps exceed the limit of {}",
      .value = [](ComponentLimits const& limits) noexcept -> uint64_t {
          return limits.maxInstantiationSteps;
      } },
    { .limit = ComponentLimit::ResourceTypes,
      .name = "maxResourceTypes",
      .rule = "runtime resource types of one ComponentStore, one per resource definition executed",
      .message = "resource types count exceeds limit of {}",
      .value = [](ComponentLimits const& limits) noexcept -> uint64_t { return limits.maxResourceTypes; } },
    { .limit = ComponentLimit::Handles,
      .name = "maxHandles",
      .rule = "handles of every handle table of one ComponentStore together; one table holds at most "
              "2^28 - 1",
      .message = "handles count exceeds limit of {}",
      .value = [](ComponentLimits const& limits) noexcept -> uint64_t { return limits.maxHandles; } },
    { .limit = ComponentLimit::LiveValueBytes,
      .name = "maxLiveValueBytes",
      .rule = "host bytes of every live Values of one ComponentStore",
      .message = "live value bytes exceed the limit of {}",
      .value = [](ComponentLimits const& limits) noexcept -> uint64_t { return limits.maxLiveValueBytes; } },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: LimitTable)
        {
            if (static_cast<std::size_t>(row.limit) != index)
                return false;
            ++index;
        }
        return index == ComponentLimitCount;
    }(),
    "LimitTable must hold one row per ComponentLimit, at the index of its enumerator");

/// The table row describing @p limit.
/// @param limit The cap to look up.
/// @return Its row in LimitTable.
[[nodiscard]] constexpr ComponentLimitRow const& limitRow(ComponentLimit limit) noexcept
{
    return LimitTable[static_cast<std::size_t>(limit)];
}

/// The value of one cap.
/// @param limits The caps.
/// @param limit The cap to read.
/// @return Its value in @p limits.
[[nodiscard]] constexpr uint64_t limitValue(ComponentLimits const& limits, ComponentLimit limit) noexcept
{
    return limitRow(limit).value(limits);
}

/// The text of a refusal of @p limit.
/// @param limit The cap exceeded.
/// @param value The cap's value, which the text names where its row's message does.
/// @return The row's message, formatted with @p value.
[[nodiscard]] inline std::string limitMessage(ComponentLimit limit, uint64_t value)
{
    return std::vformat(limitRow(limit).message, std::make_format_args(value));
}

} // namespace isoline::component
