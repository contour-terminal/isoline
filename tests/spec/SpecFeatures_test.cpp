// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "SpecFeatures.hpp"
#include <isoline/Features.hpp>

using isoline::Feature;
using isoline::FeatureSet;
using isoline::spectest::featureSetNamed;

TEST_CASE("SpecFeatures.namesSelectTheirSets")
{
    CHECK(featureSetNamed("wasm2") == FeatureSet::wasm2());
    CHECK(featureSetNamed("wasm3") == FeatureSet::wasm3());
    CHECK(featureSetNamed("all") == FeatureSet::all());
    // Each set is a different one: 3.0 adds to 2.0, and the proposals add to 3.0.
    CHECK(featureSetNamed("wasm3")->has(Feature::ExtendedConst));
    CHECK_FALSE(featureSetNamed("wasm2")->has(Feature::ExtendedConst));
    CHECK(featureSetNamed("all")->has(Feature::WideArithmetic));
    CHECK_FALSE(featureSetNamed("wasm3")->has(Feature::WideArithmetic));
}

TEST_CASE("SpecFeatures.unknownNamesSelectNothing")
{
    CHECK_FALSE(featureSetNamed("wasm4").has_value());
    CHECK_FALSE(featureSetNamed("").has_value());
    CHECK_FALSE(featureSetNamed("WASM3").has_value());
}
