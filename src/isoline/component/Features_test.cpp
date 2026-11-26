// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <set>
#include <string_view>

#include <isoline/component/Features.hpp>

using isoline::component::ComponentFeature;
using isoline::component::ComponentFeatureCount;
using isoline::component::ComponentFeatureSet;
using isoline::component::ComponentFeatureTable;
using isoline::component::decodeAdmits;
using isoline::component::Implementation;
using isoline::component::validationAdmits;

TEST_CASE("Features.tableCoversEveryEnumerator")
{
    CHECK(ComponentFeatureTable.size() == ComponentFeatureCount);
    CHECK(ComponentFeatureCount == 16);
    auto index = std::size_t { 0 };
    auto names = std::set<std::string_view> {};
    for (auto const& row: ComponentFeatureTable)
    {
        CHECK(static_cast<std::size_t>(row.feature) == index);
        CHECK_FALSE(row.gate.empty());
        CHECK_FALSE(row.refusal.empty());
        names.insert(row.name);
        ++index;
    }
    CHECK(names.size() == ComponentFeatureCount);

    CHECK(ComponentFeatureSet::wasi03()
          == ComponentFeatureSet::defaults()
                 .with(ComponentFeature::Map)
                 .with(ComponentFeature::Implements)
                 .with(ComponentFeature::ExternalId)
                 .with(ComponentFeature::Async));
    for (auto const& row: ComponentFeatureTable)
        CHECK_FALSE(ComponentFeatureSet::defaults().contains(row.feature));
}

TEST_CASE("Features.p11Support")
{
    // P11 implements what WASI 0.3.2 adopted bar concurrency, decodes async, and refuses the rest.
    for (auto const& row: ComponentFeatureTable)
    {
        auto const expected = [&] {
            switch (row.feature)
            {
                case ComponentFeature::Map:
                case ComponentFeature::Implements:
                case ComponentFeature::ExternalId: return Implementation::Implemented;
                case ComponentFeature::Async: return Implementation::Decoded;
                default: return Implementation::Refused;
            }
        }();
        CHECK(row.p11 == expected);
    }
    CHECK(ComponentFeatureTable[static_cast<std::size_t>(ComponentFeature::Values)].gate == "🪙");
    CHECK(ComponentFeatureTable[static_cast<std::size_t>(ComponentFeature::Implements)].refusal
          == "the `cm-implements` feature is not active");
}

TEST_CASE("Features.admission")
{
    auto const wasi = ComponentFeatureSet::wasi03();
    // Decoded: the decoder admits async, the validator does not.
    CHECK(decodeAdmits(wasi, ComponentFeature::Async));
    CHECK_FALSE(validationAdmits(wasi, ComponentFeature::Async));
    // Implemented: both admit it, when the set enables it.
    CHECK(decodeAdmits(wasi, ComponentFeature::Map));
    CHECK(validationAdmits(wasi, ComponentFeature::Map));
    CHECK_FALSE(decodeAdmits(ComponentFeatureSet::defaults(), ComponentFeature::Map));
    CHECK_FALSE(validationAdmits(ComponentFeatureSet::defaults(), ComponentFeature::Map));
    // Refused: neither admits it, even when the set enables it.
    auto const values = ComponentFeatureSet::defaults().with(ComponentFeature::Values);
    CHECK(values.contains(ComponentFeature::Values));
    CHECK_FALSE(decodeAdmits(values, ComponentFeature::Values));
    CHECK_FALSE(validationAdmits(values, ComponentFeature::Values));
}
