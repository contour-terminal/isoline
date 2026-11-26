// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace isoline::component
{

/// A gated feature of the component model: a production or rule the Explainer marks with an emoji
/// ("Gated Features", design/mvp/Explainer.md at the pinned revision f78ac03c).
///
/// The enumerators are the rows of ComponentFeatureTable, in the same order; adding a gate is adding
/// an enumerator here and a row there.
enum class ComponentFeature : uint8_t
{
    Async,
    Map,
    Implements,
    ExternalId,
    Values,
    NestedNamespaces,
    MoreAsyncBuiltins,
    StackfulAsync,
    Threading,
    SharedThreading,
    FixedLengthLists,
    ErrorContext,
    CanonicalInterfaceNames,
    Memory64,
    GettersSetters,
    Forward,
};

/// The last ComponentFeature enumerator; it must name whichever enumerator is listed last above.
inline constexpr auto LastComponentFeature = ComponentFeature::Forward;

/// The number of ComponentFeature enumerators, as LastComponentFeature counts them.
inline constexpr auto ComponentFeatureCount = static_cast<std::size_t>(LastComponentFeature) + 1;

/// How far Isoline implements a gated feature: the decoder admits a form of a feature that is
/// Decoded or Implemented, the validator only one that is Implemented (decodeAdmits(),
/// validationAdmits()). The order rises with support, so the refusal is zero.
enum class Implementation : uint8_t
{
    Refused,     ///< Neither decoded nor validated: refused wherever its forms appear.
    Decoded,     ///< Decoded into the AST, and refused by validation.
    Implemented, ///< Decoded, validated and run.
};

/// One row of ComponentFeatureTable: what is known about a single gated feature.
struct ComponentFeatureRow
{
    ComponentFeature feature; ///< The feature this row describes; equal to the row's index.
    std::string_view gate;    ///< The Explainer's emoji for it.
    std::string_view name;    ///< Its name, as wasm-tools' `--features` flag spells it.
    Implementation p11;       ///< How far Isoline implements it, as of P11.
    /// The text a refusal of one of its forms reports, wasm-tools' own where it has one.
    std::string_view refusal;
};

/// Every gated feature, one row per ComponentFeature enumerator, in enumerator order.
///
/// P11 implements what WASI adopted up to 0.3.2 bar concurrency: map, implements and external-id.
/// Async is decoded and refused by validation until P12; every other gate is refused. wasm-tools
/// gates external-id under cm-implements, and so words its refusal as that of implements; Isoline
/// keeps the two apart, as the Explainer lists them as separate productions.
inline constexpr auto ComponentFeatureTable = std::to_array<ComponentFeatureRow>({
    { .feature = ComponentFeature::Async,
      .gate = "🔀",
      .name = "cm-async",
      .p11 = Implementation::Decoded,
      .refusal = "requires the component model async feature" },
    { .feature = ComponentFeature::Map,
      .gate = "🗺️",
      .name = "cm-map",
      .p11 = Implementation::Implemented,
      .refusal = "requires the component model map feature" },
    { .feature = ComponentFeature::Implements,
      .gate = "🏷️",
      .name = "cm-implements",
      .p11 = Implementation::Implemented,
      .refusal = "the `cm-implements` feature is not active" },
    { .feature = ComponentFeature::ExternalId,
      .gate = "🏷️",
      .name = "cm-external-id",
      .p11 = Implementation::Implemented,
      .refusal = "the `cm-implements` feature is not active" },
    { .feature = ComponentFeature::Values,
      .gate = "🪙",
      .name = "cm-values",
      .p11 = Implementation::Refused,
      .refusal = "support for component model `value`s is not enabled" },
    { .feature = ComponentFeature::NestedNamespaces,
      .gate = "🪺",
      .name = "cm-nested-names",
      .p11 = Implementation::Refused,
      .refusal = "requires the component model nested names feature" },
    { .feature = ComponentFeature::MoreAsyncBuiltins,
      .gate = "🚝",
      .name = "cm-more-async-builtins",
      .p11 = Implementation::Refused,
      .refusal = "requires the component model more async builtins feature" },
    { .feature = ComponentFeature::StackfulAsync,
      .gate = "🚟",
      .name = "cm-async-stackful",
      .p11 = Implementation::Refused,
      .refusal = "requires the component model async stackful feature" },
    { .feature = ComponentFeature::Threading,
      .gate = "🧵",
      .name = "cm-threading",
      .p11 = Implementation::Refused,
      .refusal = "requires the component model threading feature" },
    { .feature = ComponentFeature::SharedThreading,
      .gate = "🧵②",
      .name = "cm-shared-threading",
      .p11 = Implementation::Refused,
      .refusal = "requires the component model shared threading feature" },
    { .feature = ComponentFeature::FixedLengthLists,
      .gate = "🔧",
      .name = "cm-fixed-length-lists",
      .p11 = Implementation::Refused,
      .refusal = "requires the component model fixed-length lists feature" },
    { .feature = ComponentFeature::ErrorContext,
      .gate = "📝",
      .name = "cm-error-context",
      .p11 = Implementation::Refused,
      .refusal = "requires the component model error-context feature" },
    { .feature = ComponentFeature::CanonicalInterfaceNames,
      .gate = "🔗",
      .name = "cm-canon-names",
      .p11 = Implementation::Refused,
      .refusal = "the `cm-canon-names` feature is not active" },
    { .feature = ComponentFeature::Memory64,
      .gate = "🐘",
      .name = "cm64",
      .p11 = Implementation::Refused,
      .refusal = "requires the component model 64-bit feature" },
    { .feature = ComponentFeature::GettersSetters,
      .gate = "📡",
      .name = "cm-accessors",
      .p11 = Implementation::Refused,
      .refusal = "requires the component model accessors feature" },
    { .feature = ComponentFeature::Forward,
      .gate = "➡️",
      .name = "cm-forward",
      .p11 = Implementation::Refused,
      .refusal = "requires the component model forward feature" },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: ComponentFeatureTable)
        {
            if (static_cast<std::size_t>(row.feature) != index)
                return false;
            ++index;
        }
        return index == ComponentFeatureCount;
    }(),
    "ComponentFeatureTable must hold one row per ComponentFeature, at the index of its enumerator");

/// The table row describing @p feature.
/// @param feature The gated feature to look up.
/// @return Its row in ComponentFeatureTable.
[[nodiscard]] constexpr ComponentFeatureRow const& componentFeatureRow(ComponentFeature feature) noexcept
{
    return ComponentFeatureTable[static_cast<std::size_t>(feature)];
}

/// An immutable set of enabled gated features, handed to the decoder and the validator at
/// construction. The default (ungated) features of the component model are always on.
class ComponentFeatureSet
{
  public:
    /// The empty set, as defaults() spells it.
    constexpr ComponentFeatureSet() noexcept = default;

    /// The default features of the component model alone: no gated feature is on.
    /// @return The empty set.
    [[nodiscard]] static constexpr ComponentFeatureSet defaults() noexcept { return ComponentFeatureSet {}; }

    /// What WASI adopted up to 0.3.2: map, implements, external-id and async.
    /// @return The set of exactly those four features.
    [[nodiscard]] static constexpr ComponentFeatureSet wasi03() noexcept
    {
        return defaults()
            .with(ComponentFeature::Async)
            .with(ComponentFeature::Map)
            .with(ComponentFeature::Implements)
            .with(ComponentFeature::ExternalId);
    }

    /// This set with one more feature on.
    /// @param feature The feature to enable.
    /// @return A copy of this set with @p feature on.
    [[nodiscard]] constexpr ComponentFeatureSet with(ComponentFeature feature) const noexcept
    {
        return ComponentFeatureSet { _bits | bitOf(feature) };
    }

    /// Whether @p feature is on.
    /// @param feature The feature to query.
    /// @return true when @p feature is in this set.
    [[nodiscard]] constexpr bool contains(ComponentFeature feature) const noexcept
    {
        return (_bits & bitOf(feature)) != 0;
    }

    /// Two sets are equal when they enable the same features.
    [[nodiscard]] constexpr bool operator==(ComponentFeatureSet const&) const noexcept = default;

  private:
    static_assert(ComponentFeatureCount <= 32,
                  "ComponentFeatureSet stores one bit per feature in a uint32_t");

    constexpr explicit ComponentFeatureSet(uint32_t bits) noexcept: _bits { bits } {}

    [[nodiscard]] static constexpr uint32_t bitOf(ComponentFeature feature) noexcept
    {
        return uint32_t { 1 } << static_cast<uint32_t>(feature);
    }

    uint32_t _bits = 0;
};

/// Whether the decoder admits a form gated by @p feature: the set enables it, and Isoline decodes it.
/// @param features The features the decoder was given.
/// @param feature The form's gate.
/// @return true when @p features contains @p feature and its row is not Implementation::Refused.
[[nodiscard]] constexpr bool decodeAdmits(ComponentFeatureSet features, ComponentFeature feature) noexcept
{
    return features.contains(feature) && componentFeatureRow(feature).p11 != Implementation::Refused;
}

/// Whether the validator admits a form gated by @p feature: the set enables it, and Isoline
/// implements it.
/// @param features The features the validator was given.
/// @param feature The form's gate.
/// @return true when @p features contains @p feature and its row is Implementation::Implemented.
[[nodiscard]] constexpr bool validationAdmits(ComponentFeatureSet features, ComponentFeature feature) noexcept
{
    return features.contains(feature) && componentFeatureRow(feature).p11 == Implementation::Implemented;
}

} // namespace isoline::component
