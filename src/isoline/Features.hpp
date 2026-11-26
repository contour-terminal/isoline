// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace isoline
{

/// A WebAssembly proposal an engine part (decoder, validator) can switch on or off.
///
/// The enumerators are the rows of FeatureTable, in the same order; adding a proposal is adding an
/// enumerator here and a row there.
enum class Feature : uint8_t
{
    SignExtension,
    NonTrappingFloatToInt,
    MultiValue,
    ReferenceTypes,
    BulkMemory,
    Simd,
    RelaxedSimd,
    TailCall,
    ExtendedConst,
    TypedFunctionReferences,
    MultiMemory,
    ExceptionHandling,
    Memory64,
    BranchHinting,
    Annotations,
    WideArithmetic,
    CompactImports,
    CustomPageSizes,
};

/// The last Feature enumerator; it must name whichever enumerator is listed last above.
///
/// This is the one place the enum's extent is spelled. FeatureTable's static_assert and the tests
/// size the table against it, so appending an enumerator means moving this constant along, and a
/// row added without doing so fails to compile.
inline constexpr auto LastFeature = Feature::CustomPageSizes;

/// The number of Feature enumerators, as LastFeature counts them.
inline constexpr auto FeatureCount = static_cast<std::size_t>(LastFeature) + 1;

/// Whether a feature is enabled.
enum class FeatureState : uint8_t
{
    Off,
    On,
};

/// One row of FeatureTable: what is known about a single proposal.
struct FeatureRow
{
    Feature feature;       ///< The proposal this row describes; equal to the row's index.
    std::string_view name; ///< The proposal's name, as its repository spells it.
    FeatureState inWasm2;  ///< On when the proposal is part of WebAssembly 2.0.
};

/// Every proposal Isoline knows, one row per Feature enumerator, in enumerator order.
inline constexpr auto FeatureTable = std::to_array<FeatureRow>({
    { .feature = Feature::SignExtension, .name = "sign-extension", .inWasm2 = FeatureState::On },
    { .feature = Feature::NonTrappingFloatToInt,
      .name = "nontrapping-float-to-int",
      .inWasm2 = FeatureState::On },
    { .feature = Feature::MultiValue, .name = "multi-value", .inWasm2 = FeatureState::On },
    { .feature = Feature::ReferenceTypes, .name = "reference-types", .inWasm2 = FeatureState::On },
    { .feature = Feature::BulkMemory, .name = "bulk-memory", .inWasm2 = FeatureState::On },
    { .feature = Feature::Simd, .name = "simd", .inWasm2 = FeatureState::On },
    { .feature = Feature::RelaxedSimd, .name = "relaxed-simd", .inWasm2 = FeatureState::Off },
    { .feature = Feature::TailCall, .name = "tail-call", .inWasm2 = FeatureState::Off },
    { .feature = Feature::ExtendedConst, .name = "extended-const", .inWasm2 = FeatureState::Off },
    { .feature = Feature::TypedFunctionReferences,
      .name = "function-references",
      .inWasm2 = FeatureState::Off },
    { .feature = Feature::MultiMemory, .name = "multi-memory", .inWasm2 = FeatureState::Off },
    { .feature = Feature::ExceptionHandling, .name = "exception-handling", .inWasm2 = FeatureState::Off },
    { .feature = Feature::Memory64, .name = "memory64", .inWasm2 = FeatureState::Off },
    { .feature = Feature::BranchHinting, .name = "branch-hinting", .inWasm2 = FeatureState::Off },
    { .feature = Feature::Annotations, .name = "annotations", .inWasm2 = FeatureState::Off },
    { .feature = Feature::WideArithmetic, .name = "wide-arithmetic", .inWasm2 = FeatureState::Off },
    { .feature = Feature::CompactImports, .name = "compact-imports", .inWasm2 = FeatureState::Off },
    { .feature = Feature::CustomPageSizes, .name = "custom-page-sizes", .inWasm2 = FeatureState::Off },
});

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: FeatureTable)
        {
            if (static_cast<std::size_t>(row.feature) != index)
                return false;
            ++index;
        }
        return index == FeatureCount;
    }(),
    "FeatureTable must hold one row per Feature, at the index of its enumerator");

/// The table row describing @p feature.
/// @param feature The proposal to look up.
/// @return Its row in FeatureTable.
[[nodiscard]] constexpr FeatureRow const& featureRow(Feature feature) noexcept
{
    return FeatureTable[static_cast<std::size_t>(feature)];
}

/// An immutable set of enabled features, handed to an engine part at construction.
class FeatureSet
{
  public:
    /// The empty set, as none() spells it.
    constexpr FeatureSet() noexcept = default;

    /// The empty set: the WebAssembly 1.0 MVP.
    /// @return A set with every feature off.
    [[nodiscard]] static constexpr FeatureSet none() noexcept { return FeatureSet {}; }

    /// WebAssembly 2.0: every feature whose FeatureTable row has inWasm2 On.
    /// @return A set with exactly the WebAssembly 2.0 features on.
    [[nodiscard]] static constexpr FeatureSet wasm2() noexcept
    {
        auto set = none();
        for (auto const& row: FeatureTable)
            if (row.inWasm2 == FeatureState::On)
                set = set.with(row.feature);
        return set;
    }

    /// This set with one more feature on.
    /// @param feature The feature to enable.
    /// @return A copy of this set with @p feature on.
    [[nodiscard]] constexpr FeatureSet with(Feature feature) const noexcept
    {
        return FeatureSet { _bits | bitOf(feature) };
    }

    /// This set with one feature off.
    /// @param feature The feature to disable.
    /// @return A copy of this set with @p feature off.
    [[nodiscard]] constexpr FeatureSet without(Feature feature) const noexcept
    {
        return FeatureSet { _bits & ~bitOf(feature) };
    }

    /// Whether @p feature is on.
    /// @param feature The feature to query.
    /// @return true when @p feature is in this set.
    [[nodiscard]] constexpr bool has(Feature feature) const noexcept { return (_bits & bitOf(feature)) != 0; }

    /// Two sets are equal when they enable the same features.
    [[nodiscard]] constexpr bool operator==(FeatureSet const&) const noexcept = default;

  private:
    static_assert(FeatureTable.size() <= 32, "FeatureSet stores one bit per Feature in a uint32_t");

    constexpr explicit FeatureSet(uint32_t bits) noexcept: _bits { bits } {}

    [[nodiscard]] static constexpr uint32_t bitOf(Feature feature) noexcept
    {
        return uint32_t { 1 } << static_cast<uint32_t>(feature);
    }

    uint32_t _bits = 0;
};

} // namespace isoline
