// SPDX-License-Identifier: Apache-2.0
#include <core/Utils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>

#include <isoline/Features.hpp>

TEST_CASE("Features.tableIsWellFormed")
{
    for (auto const [index, row]: core::views::enumerate(isoline::FeatureTable))
        CHECK(static_cast<std::size_t>(row.feature) == index);
    CHECK(isoline::FeatureTable.size() == isoline::FeatureCount);
}

TEST_CASE("Features.namesAreTheProposalNames")
{
    CHECK(isoline::featureRow(isoline::Feature::SignExtension).name == "sign-extension");
    CHECK(isoline::featureRow(isoline::Feature::NonTrappingFloatToInt).name == "nontrapping-float-to-int");
    CHECK(isoline::featureRow(isoline::Feature::TypedFunctionReferences).name == "function-references");
    CHECK(isoline::featureRow(isoline::Feature::CustomPageSizes).name == "custom-page-sizes");
}

TEST_CASE("Features.wasm2HasExactlyTheSixWasm2Proposals")
{
    auto const set = isoline::FeatureSet::wasm2();
    CHECK(set.has(isoline::Feature::BulkMemory));
    CHECK(set.has(isoline::Feature::Simd));
    CHECK_FALSE(set.has(isoline::Feature::TailCall));

    auto enabled = std::size_t { 0 };
    for (auto const& row: isoline::FeatureTable)
    {
        CHECK(set.has(row.feature) == (row.standard == isoline::Standard::Wasm2));
        if (set.has(row.feature))
            ++enabled;
    }
    CHECK(enabled == 6);
}

TEST_CASE("Features.standards")
{
    using isoline::Feature;
    using isoline::FeatureSet;
    CHECK(FeatureSet::wasm3().has(Feature::TailCall));
    CHECK(FeatureSet::wasm3().has(Feature::Simd));
    CHECK_FALSE(FeatureSet::wasm3().has(Feature::WideArithmetic));
    for (auto const& row: isoline::FeatureTable)
    {
        CHECK(FeatureSet::all().has(row.feature));
        CHECK(FeatureSet::wasm3().has(row.feature) == (row.standard != isoline::Standard::Proposal));
    }
    CHECK(FeatureSet::all()
          == FeatureSet::wasm3()
                 .with(Feature::WideArithmetic)
                 .with(Feature::CompactImports)
                 .with(Feature::CustomPageSizes));
}

TEST_CASE("Features.noneHasNothing")
{
    auto const set = isoline::FeatureSet::none();
    for (auto const& row: isoline::FeatureTable)
        CHECK_FALSE(set.has(row.feature));
    CHECK(set != isoline::FeatureSet::wasm2());
}

TEST_CASE("Features.withWithout")
{
    auto const s = isoline::FeatureSet::none().with(isoline::Feature::Memory64);
    CHECK(s.has(isoline::Feature::Memory64));
    CHECK_FALSE(s.without(isoline::Feature::Memory64).has(isoline::Feature::Memory64));
    CHECK(s.without(isoline::Feature::Memory64) == isoline::FeatureSet::none());
    CHECK(s.with(isoline::Feature::Memory64) == s);
    CHECK_FALSE(s.has(isoline::Feature::CustomPageSizes));
}

TEST_CASE("Features.includes")
{
    using isoline::Feature;
    using isoline::FeatureSet;
    auto const both = FeatureSet::none().with(Feature::Memory64).with(Feature::CustomPageSizes);
    CHECK(FeatureSet::all().includes(both));
    CHECK(both.includes(FeatureSet::none().with(Feature::Memory64)));
    CHECK(both.includes(FeatureSet::none()));
    CHECK_FALSE(FeatureSet::wasm3().includes(both));
    CHECK_FALSE(FeatureSet::none().with(Feature::CustomPageSizes).includes(both));
}

TEST_CASE("Features.isConstexpr")
{
    constexpr auto WithTailCall = isoline::FeatureSet::wasm2().with(isoline::Feature::TailCall);
    STATIC_CHECK(WithTailCall.has(isoline::Feature::TailCall));
    STATIC_CHECK(WithTailCall.has(isoline::Feature::SignExtension));
}

TEST_CASE("Features.everyStandardSetIsClosedOverPrerequisites")
{
    using isoline::FeatureSet;
    for (auto const set: { FeatureSet::none(), FeatureSet::wasm2(), FeatureSet::wasm3(), FeatureSet::all() })
        CHECK(set.checkPrerequisites().has_value());
    // A prerequisite is never of a newer standard than the feature needing it, so every upTo set is
    // closed by construction.
    for (auto const& row: isoline::FeatureTable)
        for (auto const& prerequisite: isoline::FeatureTable)
            if (row.prerequisites.has(prerequisite.feature))
                CHECK(prerequisite.standard <= row.standard);
}

TEST_CASE("Features.relaxedSimdRequiresSimd")
{
    using isoline::Feature;
    using isoline::FeatureSet;
    CHECK(isoline::featureRow(Feature::RelaxedSimd).prerequisites == FeatureSet::none().with(Feature::Simd));
    auto const unclosed = FeatureSet::wasm2().with(Feature::RelaxedSimd).without(Feature::Simd);
    auto const checked = unclosed.checkPrerequisites();
    REQUIRE_FALSE(checked.has_value());
    CHECK(checked.error().feature == Feature::RelaxedSimd);
    CHECK(checked.error().prerequisite == Feature::Simd);
}

TEST_CASE("Features.prerequisiteRows")
{
    using isoline::Feature;
    using isoline::FeatureSet;
    auto const needs = [](Feature feature, Feature prerequisite) {
        return isoline::featureRow(feature).prerequisites.has(prerequisite);
    };
    CHECK(needs(Feature::ReferenceTypes, Feature::BulkMemory));
    CHECK(needs(Feature::TypedFunctionReferences, Feature::ReferenceTypes));
    CHECK(needs(Feature::ExceptionHandling, Feature::ReferenceTypes));
    CHECK(needs(Feature::WideArithmetic, Feature::MultiValue));
    CHECK(isoline::featureRow(Feature::Simd).prerequisites == FeatureSet::none());
    CHECK(isoline::featureRow(Feature::Memory64).prerequisites == FeatureSet::none());
}

TEST_CASE("Features.withPrerequisitesClosesTheSet")
{
    using isoline::Feature;
    using isoline::FeatureSet;
    // Transitively: function references need reference types, which need bulk memory.
    auto const closed = FeatureSet::none().with(Feature::TypedFunctionReferences).withPrerequisites();
    CHECK(closed
          == FeatureSet::none()
                 .with(Feature::TypedFunctionReferences)
                 .with(Feature::ReferenceTypes)
                 .with(Feature::BulkMemory));
    CHECK(closed.checkPrerequisites().has_value());
    CHECK(FeatureSet::wasm3().withPrerequisites() == FeatureSet::wasm3());
    STATIC_CHECK(FeatureSet::none().with(Feature::RelaxedSimd).withPrerequisites().has(Feature::Simd));
}
