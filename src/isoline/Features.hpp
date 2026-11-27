// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <string>
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

/// The oldest standard a proposal is part of, oldest first, so that a standard's features are the
/// rows of its enumerator and of every one before it.
enum class Standard : uint8_t
{
    Wasm2,    ///< Part of WebAssembly 2.0.
    Wasm3,    ///< Added by WebAssembly 3.0.
    Proposal, ///< In no released standard yet.
};

/// A feature switched on without a feature it builds on (FeatureRow::prerequisites).
struct UnmetPrerequisite
{
    Feature feature;      ///< The feature that is on.
    Feature prerequisite; ///< The feature it builds on, which is off.
};

/// An immutable set of enabled features, handed to an engine part at construction.
///
/// A set may be built in any order with with() and without(); whether it is closed over every
/// enabled feature's prerequisites is checkPrerequisites()'s question, which decodeModule asks
/// before it reads a byte.
class FeatureSet
{
  public:
    /// The empty set, as none() spells it.
    constexpr FeatureSet() noexcept = default;

    /// The empty set: the WebAssembly 1.0 MVP.
    /// @return A set with every feature off.
    [[nodiscard]] static constexpr FeatureSet none() noexcept { return FeatureSet {}; }

    /// WebAssembly 2.0: every feature whose FeatureTable row is of Standard::Wasm2.
    /// @return A set with exactly the WebAssembly 2.0 features on.
    [[nodiscard]] static constexpr FeatureSet wasm2() noexcept { return upTo(Standard::Wasm2); }

    /// WebAssembly 3.0: every feature of Standard::Wasm2 or Standard::Wasm3.
    /// @return A set with exactly the WebAssembly 3.0 features on.
    [[nodiscard]] static constexpr FeatureSet wasm3() noexcept { return upTo(Standard::Wasm3); }

    /// Every feature Isoline knows, the proposals of no released standard included.
    /// @return A set with every FeatureTable row on.
    [[nodiscard]] static constexpr FeatureSet all() noexcept { return upTo(Standard::Proposal); }

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

    /// Whether every feature of @p other is on.
    /// @param other The features to query.
    /// @return true when @p other is a subset of this set; always for the empty set.
    [[nodiscard]] constexpr bool includes(FeatureSet other) const noexcept
    {
        return (_bits & other._bits) == other._bits;
    }

    /// This set with every prerequisite of its features on, transitively.
    /// @return The smallest closed set (see checkPrerequisites()) holding this one.
    [[nodiscard]] constexpr FeatureSet withPrerequisites() const noexcept;

    /// Whether every enabled feature's prerequisites are enabled too.
    /// @return Nothing when the set is closed; otherwise the first enabled feature, in FeatureTable
    ///         order, with a prerequisite that is off, and the first such prerequisite.
    [[nodiscard]] constexpr std::expected<void, UnmetPrerequisite> checkPrerequisites() const noexcept;

    /// Two sets are equal when they enable the same features.
    [[nodiscard]] constexpr bool operator==(FeatureSet const&) const noexcept = default;

  private:
    static_assert(FeatureCount <= 32, "FeatureSet stores one bit per Feature in a uint32_t");

    constexpr explicit FeatureSet(uint32_t bits) noexcept: _bits { bits } {}

    /// Every feature whose standard is @p newest or older.
    [[nodiscard]] static constexpr FeatureSet upTo(Standard newest) noexcept;

    [[nodiscard]] static constexpr uint32_t bitOf(Feature feature) noexcept
    {
        return uint32_t { 1 } << static_cast<uint32_t>(feature);
    }

    uint32_t _bits = 0;
};

/// One row of FeatureTable: what is known about a single proposal.
struct FeatureRow
{
    Feature feature;             ///< The proposal this row describes; equal to the row's index.
    std::string_view name;       ///< The proposal's name, as its repository spells it.
    Standard standard;           ///< The oldest standard the proposal is part of.
    FeatureSet prerequisites {}; ///< The proposals it builds on, which must be on whenever it is.
};

/// Every proposal Isoline knows, one row per Feature enumerator, in enumerator order.
///
/// The prerequisites are what each proposal builds on: reference types extends bulk memory's segments
/// and table instructions, relaxed SIMD and function references extend SIMD and reference types,
/// exception handling's exnref is a reference type, and wide arithmetic's instructions return two
/// results. Isoline refuses a set that lacks one; Wasmtime v48 checks only relaxed SIMD's.
inline constexpr auto FeatureTable = std::to_array<FeatureRow>({
    { .feature = Feature::SignExtension, .name = "sign-extension", .standard = Standard::Wasm2 },
    { .feature = Feature::NonTrappingFloatToInt,
      .name = "nontrapping-float-to-int",
      .standard = Standard::Wasm2 },
    { .feature = Feature::MultiValue, .name = "multi-value", .standard = Standard::Wasm2 },
    { .feature = Feature::ReferenceTypes,
      .name = "reference-types",
      .standard = Standard::Wasm2,
      .prerequisites = FeatureSet::none().with(Feature::BulkMemory) },
    { .feature = Feature::BulkMemory, .name = "bulk-memory", .standard = Standard::Wasm2 },
    { .feature = Feature::Simd, .name = "simd", .standard = Standard::Wasm2 },
    { .feature = Feature::RelaxedSimd,
      .name = "relaxed-simd",
      .standard = Standard::Wasm3,
      .prerequisites = FeatureSet::none().with(Feature::Simd) },
    { .feature = Feature::TailCall, .name = "tail-call", .standard = Standard::Wasm3 },
    { .feature = Feature::ExtendedConst, .name = "extended-const", .standard = Standard::Wasm3 },
    { .feature = Feature::TypedFunctionReferences,
      .name = "function-references",
      .standard = Standard::Wasm3,
      .prerequisites = FeatureSet::none().with(Feature::ReferenceTypes) },
    { .feature = Feature::MultiMemory, .name = "multi-memory", .standard = Standard::Wasm3 },
    { .feature = Feature::ExceptionHandling,
      .name = "exception-handling",
      .standard = Standard::Wasm3,
      .prerequisites = FeatureSet::none().with(Feature::ReferenceTypes) },
    { .feature = Feature::Memory64, .name = "memory64", .standard = Standard::Wasm3 },
    { .feature = Feature::BranchHinting, .name = "branch-hinting", .standard = Standard::Wasm3 },
    { .feature = Feature::Annotations, .name = "annotations", .standard = Standard::Wasm3 },
    { .feature = Feature::WideArithmetic,
      .name = "wide-arithmetic",
      .standard = Standard::Proposal,
      .prerequisites = FeatureSet::none().with(Feature::MultiValue) },
    { .feature = Feature::CompactImports, .name = "compact-imports", .standard = Standard::Proposal },
    { .feature = Feature::CustomPageSizes, .name = "custom-page-sizes", .standard = Standard::Proposal },
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

/// A refusal's message, followed by the disabled proposal that would have accepted the module, as
/// decode and validation errors spell it: "multiple memories (multi-memory not enabled)". The
/// message comes first, so the spec suite's prefix match sees it unchanged.
/// @param message The refusal's message.
/// @param disabled The proposal; none leaves @p message as it is.
/// @return The message.
[[nodiscard]] inline std::string appendDisabledFeature(std::string message, std::optional<Feature> disabled)
{
    if (!disabled)
        return message;
    return std::format("{} ({} not enabled)", message, featureRow(*disabled).name);
}

constexpr FeatureSet FeatureSet::upTo(Standard newest) noexcept
{
    auto set = none();
    for (auto const& row: FeatureTable)
        if (row.standard <= newest)
            set = set.with(row.feature);
    return set;
}

constexpr FeatureSet FeatureSet::withPrerequisites() const noexcept
{
    // Each pass adds the prerequisites of what is on; a chain is at most FeatureCount long.
    auto closed = *this;
    for ([[maybe_unused]] auto const pass: std::views::iota(std::size_t { 0 }, FeatureCount))
        for (auto const& row: FeatureTable)
            if (closed.has(row.feature))
                closed._bits |= row.prerequisites._bits;
    return closed;
}

constexpr std::expected<void, UnmetPrerequisite> FeatureSet::checkPrerequisites() const noexcept
{
    for (auto const& row: FeatureTable)
    {
        if (!has(row.feature))
            continue;
        for (auto const& prerequisite: FeatureTable)
            if (row.prerequisites.has(prerequisite.feature) && !has(prerequisite.feature))
                return std::unexpected { UnmetPrerequisite { .feature = row.feature,
                                                             .prerequisite = prerequisite.feature } };
    }
    return {};
}

} // namespace isoline
