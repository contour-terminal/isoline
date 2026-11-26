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

TEST_CASE("Features.isConstexpr")
{
    constexpr auto WithTailCall = isoline::FeatureSet::wasm2().with(isoline::Feature::TailCall);
    STATIC_CHECK(WithTailCall.has(isoline::Feature::TailCall));
    STATIC_CHECK(WithTailCall.has(isoline::Feature::SignExtension));
}
